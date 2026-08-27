#ifndef GAME_THREAD_H
#define GAME_THREAD_H

#include <pthread.h>

#include "game.h"
#include "protocol.h"

#define QUEUE_SIZE 16

/* One message as delivered by a reader thread.  status is NGP_OK, NGP_CLOSED
   or NGP_MALFORMED; msg only carries fields when status is NGP_OK. */
struct player_message {
    int     player_id;      /* 1 or 2 */
    int     status;
    Message msg;
};

/* Bounded queue used by the two reader threads to hand messages to the game
   manager, so the manager can react to whichever player speaks first. */
struct game_queue {
    struct player_message queue[QUEUE_SIZE];
    int head, tail, count;
    int closed;                     /* set at end of game so readers can exit */
    pthread_mutex_t lock;
    pthread_cond_t  has_message;    /* manager waits here when empty */
    pthread_cond_t  has_space;      /* readers wait here when full */
};

struct game_state {
    int   fd[2];            /* index 0 is player 1 */
    char *name[2];          /* owned by the game */
    Conn *conn[2];          /* owned by the game; may hold buffered bytes */
    int   piles[NIM_PILES];
    int   turn;             /* 1 or 2 */
    int   alive[2];         /* cleared once a connection is unusable */
    int   game_over;
    struct game_queue mq;
};

struct reader_args {
    struct game_state *gs;
    int player_id;
};

void queue_init(struct game_queue *q);
void queue_close(struct game_queue *q);
void queue_destroy(struct game_queue *q);

void *player_reader_thread(void *arg);  /* reads one socket into the queue */
void *game_manager_thread(void *arg);   /* owns the rules and the turn order */

/* Registry of players currently connected, implemented in nimd.c.
   players_add tests and inserts under one lock so two connections cannot
   both claim the same name. */
int  players_add(const char *name);     /* 0 on success, -1 if already present */
void players_remove(const char *name);

#endif
