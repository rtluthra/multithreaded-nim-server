#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "game.h"
#include "game_thread.h"
#include "protocol.h"

/* nimd listens for connections, completes the OPEN/WAIT handshake with each
   client, pairs waiting clients into games, and runs every game on its own
   set of threads so that games never block one another.
   Each connection is handled by its own thread from the moment it is
   accepted, so the listening socket is never held up by a handshake. */

/* ------------------------------------------------- active player registry */

struct active_player {
    char *name;
    struct active_player *next;
};

static struct active_player *players = NULL;
static pthread_mutex_t players_lock = PTHREAD_MUTEX_INITIALIZER;

/* Testing and inserting happen under the same lock so two connections racing
   with the same name cannot both be admitted. */
int players_add(const char *name)
{
    struct active_player *p, *node;

    pthread_mutex_lock(&players_lock);
    for (p = players; p; p = p->next) {
        if (strcmp(p->name, name) == 0) {
            pthread_mutex_unlock(&players_lock);
            return -1;
        }
    }

    node = malloc(sizeof(*node));
    if (!node) {
        pthread_mutex_unlock(&players_lock);
        return -1;
    }
    node->name = strdup(name);
    if (!node->name) {
        free(node);
        pthread_mutex_unlock(&players_lock);
        return -1;
    }
    node->next = players;
    players = node;
    pthread_mutex_unlock(&players_lock);
    return 0;
}

void players_remove(const char *name)
{
    struct active_player *p, *prev = NULL;

    pthread_mutex_lock(&players_lock);
    p = players;
    while (p) {
        if (strcmp(p->name, name) == 0) {
            if (prev)
                prev->next = p->next;
            else
                players = p->next;
            free(p->name);
            free(p);
            break;
        }
        prev = p;
        p = p->next;
    }
    pthread_mutex_unlock(&players_lock);
}

/* ------------------------------------------------------------------ lobby */

/* A client that has finished the handshake and is waiting to be matched.  Its
   Conn goes on to the game, so any bytes that arrived early are not lost. */
struct waiting_player {
    int   fd;
    char *name;
    Conn *conn;
    struct waiting_player *next;
};

static struct waiting_player *lobby = NULL;
static pthread_mutex_t lobby_lock = PTHREAD_MUTEX_INITIALIZER;

/* Release a waiting client, optionally telling it why.  Called with
   lobby_lock held; players_remove only takes players_lock, so the lock order
   is always lobby then players and cannot deadlock. */
static void discard_waiting(struct waiting_player *w, int code,
                            const char *text)
{
    char buf[MAX_MSG + 1];

    if (text && build_fail(buf, code, text) > 0)
        send_msg(w->fd, buf);
    close(w->fd);
    players_remove(w->name);
    free(w->name);
    free(w->conn);
    free(w);
}

/* Decide whether a waiting client can still be matched.  A client that
   disconnected before receiving NAME must simply be dropped, and anything it
   sends before the game starts is out of place.  Returns 1 if the client is
   still waiting normally, or 0 after dropping it. */
static int waiting_ok(struct waiting_player *w)
{
    for (;;) {
        Message m;
        int r = msg_parse(w->conn, &m);

        if (r == NGP_OK) {
            message_type t = m.type;
            free_message(&m);
            if (t == MSG_OPEN)
                discard_waiting(w, 23, "Already Open");
            else if (t == MSG_MOVE)
                discard_waiting(w, 24, "Not Playing");
            else if (t == MSG_FAIL)
                discard_waiting(w, 0, NULL);    /* clients may send FAIL */
            else
                discard_waiting(w, 10, "Invalid");
            return 0;
        }
        if (r == NGP_MALFORMED) {
            discard_waiting(w, 10, "Invalid");
            return 0;
        }

        /* Incomplete: look for more bytes, but never block the lobby. */
        r = conn_fill_nonblocking(w->conn);
        if (r == NGP_OK)
            continue;               /* more arrived, try parsing again */
        if (r == NGP_INCOMPLETE)
            return 1;               /* nothing pending, still a good client */

        printf("player \"%s\" left before being matched\n", w->name);
        fflush(stdout);
        discard_waiting(w, 0, NULL);
        return 0;
    }
}

/* All lobby helpers below are called with lobby_lock held. */

static void lobby_append(struct waiting_player *w)
{
    struct waiting_player **pp = &lobby;
    while (*pp)
        pp = &(*pp)->next;
    w->next = NULL;
    *pp = w;
}

static void lobby_push_front(struct waiting_player *w)
{
    w->next = lobby;
    lobby = w;
}

/* Take the longest-waiting client that is still usable, dropping any that
   are not. */
static struct waiting_player *lobby_take_ready(void)
{
    while (lobby) {
        struct waiting_player *w = lobby;
        lobby = w->next;
        w->next = NULL;
        if (waiting_ok(w))
            return w;
    }
    return NULL;
}

static int start_game(struct waiting_player *a, struct waiting_player *b)
{
    struct game_state *gs;
    pthread_t manager;
    int i;

    gs = malloc(sizeof(*gs));
    if (!gs) {
        discard_waiting(a, 0, NULL);
        discard_waiting(b, 0, NULL);
        return -1;
    }

    /* Ownership of the name and the Conn moves from the lobby to the game;
       the game manager frees them when the game ends. */
    gs->fd[0]   = a->fd;    gs->fd[1]   = b->fd;
    gs->name[0] = a->name;  gs->name[1] = b->name;
    gs->conn[0] = a->conn;  gs->conn[1] = b->conn;
    init_piles(gs->piles);
    gs->turn = 1;
    gs->alive[0] = gs->alive[1] = 1;
    gs->game_over = 0;
    queue_init(&gs->mq);
    free(a);
    free(b);

    if (pthread_create(&manager, NULL, game_manager_thread, gs) != 0) {
        perror("pthread_create");
        queue_destroy(&gs->mq);
        for (i = 0; i < 2; i++) {
            close(gs->fd[i]);
            players_remove(gs->name[i]);
            free(gs->name[i]);
            free(gs->conn[i]);
        }
        free(gs);
        return -1;
    }
    pthread_detach(manager);
    return 0;
}

/* Pair off as many waiting clients as possible. */
static void try_match(void)
{
    for (;;) {
        struct waiting_player *a, *b;

        a = lobby_take_ready();
        if (!a)
            return;
        b = lobby_take_ready();
        if (!b) {
            lobby_push_front(a);    /* keep a waiting for the next client */
            return;
        }
        if (start_game(a, b) < 0)
            return;
    }
}

/* -------------------------------------------------------------- handshake */

static void reject(int fd, Conn *conn, int code, const char *text)
{
    char buf[MAX_MSG + 1];

    if (text && build_fail(buf, code, text) > 0)
        send_msg(fd, buf);
    close(fd);
    free(conn);
}

/* Runs the OPEN/WAIT exchange for one connection and then parks the client in
   the lobby. */
static void *handshake_thread(void *arg)
{
    int fd = *(int *)arg;
    Conn *conn;
    Message m;
    char buf[MAX_MSG + 1];
    char *name;
    struct waiting_player *w;
    int r;

    free(arg);

    conn = malloc(sizeof(*conn));
    if (!conn) {
        close(fd);
        return NULL;
    }
    conn_init(conn, fd);

    r = recv_msg(conn, &m);
    if (r != NGP_OK) {
        free_message(&m);
        if (r == NGP_MALFORMED)
            reject(fd, conn, 10, "Invalid");
        else
            reject(fd, conn, 0, NULL);
        return NULL;
    }

    /* The only message a client may open with is OPEN.  A MOVE at this point
       is specifically a move before the game began. */
    if (m.type != MSG_OPEN) {
        message_type t = m.type;
        free_message(&m);
        if (t == MSG_MOVE)
            reject(fd, conn, 24, "Not Playing");   /* a move before the game */
        else if (t == MSG_FAIL)
            reject(fd, conn, 0, NULL);             /* clients may send FAIL */
        else
            reject(fd, conn, 10, "Invalid");
        return NULL;
    }

    if (strlen(m.fields[0]) > MAX_NAME) {
        free_message(&m);
        reject(fd, conn, 21, "Long Name");
        return NULL;
    }

    name = strdup(m.fields[0]);
    free_message(&m);
    if (!name) {
        reject(fd, conn, 0, NULL);
        return NULL;
    }

    /* A name already in use means that player is connected already. */
    if (players_add(name) < 0) {
        free(name);
        reject(fd, conn, 22, "Already Playing");
        return NULL;
    }

    /* Both players receive WAIT in response to their own OPEN. */
    if (build_wait(buf) < 0 || send_msg(fd, buf) < 0) {
        players_remove(name);
        free(name);
        reject(fd, conn, 0, NULL);
        return NULL;
    }

    printf("player \"%s\" connected and is waiting for a match\n", name);
    fflush(stdout);

    w = malloc(sizeof(*w));
    if (!w) {
        players_remove(name);
        free(name);
        reject(fd, conn, 0, NULL);
        return NULL;
    }
    w->fd = fd;
    w->name = name;
    w->conn = conn;
    w->next = NULL;

    pthread_mutex_lock(&lobby_lock);
    lobby_append(w);
    try_match();
    pthread_mutex_unlock(&lobby_lock);
    return NULL;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    int listener, port, opt = 1;
    struct sockaddr_in addr;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "%s: invalid port \"%s\"\n", argv[0], argv[1]);
        return 1;
    }

    /* Writing to a client that has already gone away must not kill the
       server; send_msg reports the error instead. */
    signal(SIGPIPE, SIG_IGN);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listener);
        return 1;
    }
    if (listen(listener, 16) < 0) {
        perror("listen");
        close(listener);
        return 1;
    }

    printf("nimd listening on port %d (concurrent games)\n", port);
    fflush(stdout);

    for (;;) {
        pthread_t t;
        int *fdp;
        int fd = accept(listener, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        fdp = malloc(sizeof(*fdp));
        if (!fdp) {
            close(fd);
            continue;
        }
        *fdp = fd;

        if (pthread_create(&t, NULL, handshake_thread, fdp) != 0) {
            perror("pthread_create");
            free(fdp);
            close(fd);
            continue;
        }
        pthread_detach(t);
    }

    close(listener);
    return 0;
}
