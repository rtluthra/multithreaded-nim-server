#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "game.h"
#include "game_thread.h"
#include "protocol.h"

/* One manager thread and two reader threads make up a game.  The readers do
   nothing but turn bytes into messages, so the manager can respond to a
   message from either player as soon as it arrives instead of blocking on the
   player whose turn it happens to be. */

static int other(int player) { return player == 1 ? 2 : 1; }
static int idx(int player)   { return player - 1; }

/* ------------------------------------------------------------------ queue */

void queue_init(struct game_queue *q)
{
    q->head = q->tail = q->count = 0;
    q->closed = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->has_message, NULL);
    pthread_cond_init(&q->has_space, NULL);
}

/* Wake anyone blocked on the queue and make further pushes discard.  Without
   this a reader blocked on a full queue would never notice the game had
   ended and the manager's join would hang. */
void queue_close(struct game_queue *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = 1;
    pthread_cond_broadcast(&q->has_message);
    pthread_cond_broadcast(&q->has_space);
    pthread_mutex_unlock(&q->lock);
}

void queue_destroy(struct game_queue *q)
{
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->has_message);
    pthread_cond_destroy(&q->has_space);
}

static void queue_push(struct game_queue *q, struct player_message pm)
{
    pthread_mutex_lock(&q->lock);
    while (!q->closed && q->count == QUEUE_SIZE)
        pthread_cond_wait(&q->has_space, &q->lock);

    if (q->closed) {
        pthread_mutex_unlock(&q->lock);
        free_message(&pm.msg);      /* nothing will consume it */
        return;
    }

    q->queue[q->tail] = pm;
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;
    pthread_cond_signal(&q->has_message);
    pthread_mutex_unlock(&q->lock);
}

/* Returns 0 on success, -1 if the queue is closed and empty. */
static int queue_pop(struct game_queue *q, struct player_message *out)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->has_message, &q->lock);

    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    *out = q->queue[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;
    pthread_cond_signal(&q->has_space);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/* Free anything the manager never got around to reading. */
static void queue_drain(struct game_queue *q)
{
    pthread_mutex_lock(&q->lock);
    while (q->count > 0) {
        free_message(&q->queue[q->head].msg);
        q->head = (q->head + 1) % QUEUE_SIZE;
        q->count--;
    }
    pthread_mutex_unlock(&q->lock);
}

/* ---------------------------------------------------------- reader thread */

void *player_reader_thread(void *arg)
{
    struct reader_args *ra = arg;
    struct game_state *gs = ra->gs;
    Conn *conn = gs->conn[idx(ra->player_id)];

    for (;;) {
        struct player_message pm;

        pm.player_id = ra->player_id;
        msg_init(&pm.msg);
        pm.status = recv_msg(conn, &pm.msg);

        queue_push(&gs->mq, pm);    /* takes ownership of pm.msg */

        if (pm.status != NGP_OK)
            break;                  /* closed or unparseable: nothing more */
    }
    return NULL;
}

/* --------------------------------------------------------- manager thread */

/* Send to one player, remembering if the connection has gone away. */
static void send_to(struct game_state *gs, int player, const char *msg)
{
    if (!gs->alive[idx(player)])
        return;
    if (send_msg(gs->fd[idx(player)], msg) < 0)
        gs->alive[idx(player)] = 0;
}

/* Stop using a connection after an error that requires closing it.  The
   shutdown also unblocks that player's reader thread. */
static void drop_player(struct game_state *gs, int player)
{
    gs->alive[idx(player)] = 0;
    shutdown(gs->fd[idx(player)], SHUT_RDWR);
}

static void send_fail(struct game_state *gs, int player, int code,
                      const char *text)
{
    char buf[MAX_MSG + 1];
    if (build_fail(buf, code, text) > 0)
        send_to(gs, player, buf);
}

static void announce_turn(struct game_state *gs)
{
    char buf[MAX_MSG + 1];
    if (build_play(buf, gs->turn, gs->piles) > 0) {
        send_to(gs, 1, buf);
        send_to(gs, 2, buf);
    }
}

/* The surviving player wins by forfeit.  Only they are told, since the other
   connection is already gone. */
static void forfeit(struct game_state *gs, int winner)
{
    char buf[MAX_MSG + 1];
    if (build_over(buf, winner, gs->piles, 1) > 0)
        send_to(gs, winner, buf);
    printf("game over: %s (player %d) wins by forfeit\n",
           gs->name[idx(winner)], winner);
    fflush(stdout);
    gs->game_over = 1;
}

/* Strict integer field: the protocol only ever sends plain decimal digits, and
   atoi would silently turn garbage into 0. */
static int parse_int(const char *s, int *out)
{
    int value = 0;
    size_t i;

    if (!s || s[0] == '\0' || strlen(s) > 3)
        return -1;
    for (i = 0; s[i]; i++) {
        if (!isdigit((unsigned char)s[i]))
            return -1;
        value = value * 10 + (s[i] - '0');
    }
    *out = value;
    return 0;
}

static void handle_move(struct game_state *gs, int player, Message *m)
{
    char buf[MAX_MSG + 1];
    int pile, qty;

    /* Out of turn is reported but does not end the game or the connection. */
    if (player != gs->turn) {
        send_fail(gs, player, 31, "Impatient");
        return;
    }

    if (parse_int(m->fields[0], &pile) < 0) {
        send_fail(gs, player, 32, "Pile Index");
        return;
    }
    if (parse_int(m->fields[1], &qty) < 0) {
        send_fail(gs, player, 33, "Quantity");
        return;
    }

    switch (check_move(gs->piles, pile, qty)) {
    case MOVE_BAD_PILE:
        send_fail(gs, player, 32, "Pile Index");
        return;
    case MOVE_BAD_QTY:
        send_fail(gs, player, 33, "Quantity");
        return;
    default:
        break;
    }

    move(gs->piles, pile, qty);
    printf("%s (player %d) removes %d from pile %d -> %d %d %d %d %d\n",
           gs->name[idx(player)], player, qty, pile,
           gs->piles[0], gs->piles[1], gs->piles[2],
           gs->piles[3], gs->piles[4]);
    fflush(stdout);

    /* Whoever took the last stone wins. */
    if (is_game_over(gs->piles)) {
        if (build_over(buf, player, gs->piles, 0) > 0) {
            send_to(gs, 1, buf);
            send_to(gs, 2, buf);
        }
        printf("game over: %s (player %d) wins\n",
               gs->name[idx(player)], player);
        fflush(stdout);
        gs->game_over = 1;
        return;
    }

    gs->turn = other(gs->turn);
    announce_turn(gs);
}

static void handle_message(struct game_state *gs, struct player_message *pm)
{
    int player = pm->player_id;

    if (pm->status == NGP_MALFORMED) {
        /* A framing error is not recoverable: report it and close. */
        send_fail(gs, player, 10, "Invalid");
        printf("%s (player %d) sent an unreadable message\n",
               gs->name[idx(player)], player);
        fflush(stdout);
        drop_player(gs, player);
        forfeit(gs, other(player));
        return;
    }

    if (pm->status == NGP_CLOSED) {
        printf("%s (player %d) disconnected\n",
               gs->name[idx(player)], player);
        fflush(stdout);
        gs->alive[idx(player)] = 0;
        forfeit(gs, other(player));
        return;
    }

    switch (pm->msg.type) {
    case MSG_MOVE:
        handle_move(gs, player, &pm->msg);
        break;
    case MSG_OPEN:
        send_fail(gs, player, 23, "Already Open");
        drop_player(gs, player);
        forfeit(gs, other(player));
        break;
    case MSG_FAIL:
        /* A client is allowed to send FAIL, so it gets no FAIL in reply; it
           is reporting a problem, which ends the session. */
        printf("%s (player %d) reported: %s\n", gs->name[idx(player)],
               player, pm->msg.fields[0]);
        fflush(stdout);
        drop_player(gs, player);
        forfeit(gs, other(player));
        break;
    default:
        /* WAIT, NAME, PLAY and OVER are only ever sent by the server, so
           receiving one is an inappropriate message. */
        send_fail(gs, player, 10, "Invalid");
        drop_player(gs, player);
        forfeit(gs, other(player));
        break;
    }
}

void *game_manager_thread(void *arg)
{
    struct game_state *gs = arg;
    struct reader_args ra[2];
    pthread_t reader[2];
    int started[2] = { 0, 0 };
    char buf[MAX_MSG + 1];
    int i;

    printf("game start: %s (player 1) vs %s (player 2)\n",
           gs->name[0], gs->name[1]);
    fflush(stdout);

    /* ra lives on this stack, which is safe because the readers are joined
       before this function returns. */
    for (i = 0; i < 2; i++) {
        ra[i].gs = gs;
        ra[i].player_id = i + 1;
        started[i] = (pthread_create(&reader[i], NULL,
                                     player_reader_thread, &ra[i]) == 0);
        if (!started[i]) {
            perror("pthread_create");
            gs->alive[i] = 0;
            gs->game_over = 1;
        }
    }

    if (!gs->game_over) {
        /* Tell each player their number and their opponent, then start the
           first turn immediately. */
        if (build_name(buf, 1, gs->name[1]) > 0)
            send_to(gs, 1, buf);
        if (build_name(buf, 2, gs->name[0]) > 0)
            send_to(gs, 2, buf);
        announce_turn(gs);
    }

    while (!gs->game_over) {
        struct player_message pm;
        if (queue_pop(&gs->mq, &pm) < 0)
            break;
        handle_message(gs, &pm);
        free_message(&pm.msg);      /* single place where a message is freed */
    }

    /* Shut down in a fixed order so nothing is freed while a reader could
       still touch it: stop the queue, unblock the sockets, join the readers,
       and only then release the game. */
    queue_close(&gs->mq);
    for (i = 0; i < 2; i++)
        shutdown(gs->fd[i], SHUT_RDWR);
    for (i = 0; i < 2; i++) {
        if (started[i])
            pthread_join(reader[i], NULL);
    }

    queue_drain(&gs->mq);
    queue_destroy(&gs->mq);

    for (i = 0; i < 2; i++) {
        close(gs->fd[i]);
        players_remove(gs->name[i]);
        free(gs->name[i]);
        free(gs->conn[i]);
    }
    free(gs);
    return NULL;
}
