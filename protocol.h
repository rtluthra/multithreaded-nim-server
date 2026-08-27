#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

/* A complete NGP message is at most 104 bytes: the five byte prefix "0|NN|"
   plus the declared length, which is two decimal digits and so at most 99. */
#define MAX_MSG        104
#define MAX_NAME       72
#define MSG_HDR_LEN    10   /* "0|NN|TYPE|" */
#define MAX_FIELDS     3    /* OVER carries the most fields */
#define CONN_BUF_SIZE  1024 /* holds several messages glued into one read */

typedef enum {
    MSG_OPEN,
    MSG_WAIT,
    MSG_NAME,
    MSG_PLAY,
    MSG_MOVE,
    MSG_OVER,
    MSG_FAIL
} message_type;

/* Status codes returned by msg_parse(), conn_fill_nonblocking() and
   recv_msg().  A malformed message is reported separately from a closed
   connection because the protocol treats them differently: a framing error
   gets FAIL "10 Invalid" and a close, while a close during a game is a
   forfeit. */
#define NGP_OK          1
#define NGP_INCOMPLETE  0
#define NGP_CLOSED    (-1)
#define NGP_MALFORMED (-2)

typedef struct {
    message_type type;
    char *fields[MAX_FIELDS];   /* owned by the Message; NULL past fieldCount */
    int   fieldCount;
} Message;

/* Receive state for a single socket.  Every connection owns one of these, so
   two connections never share parser state. */
typedef struct {
    int    fd;
    size_t len;
    char   buf[CONN_BUF_SIZE];
} Conn;

void conn_init(Conn *c, int fd);

/* Put a Message into a known-empty state so free_message() is always safe. */
void msg_init(Message *m);
void free_message(Message *m);

/* Parse one message out of the bytes already buffered.  Never touches the
   socket, so this is how buffered messages are consumed without blocking. */
int msg_parse(Conn *c, Message *m);

/* Append whatever the socket has available right now, without blocking. */
int conn_fill_nonblocking(Conn *c);

/* Return the next complete message, reading only when the buffered bytes do
   not already contain one. */
int recv_msg(Conn *c, Message *m);

/* Write a complete message, looping until all bytes are sent. */
int send_msg(int fd, const char *msg);

/* Builders.  buf must have room for MAX_MSG + 1 bytes.  Each returns the
   number of bytes written, or -1 if the message would not fit. */
int build_open(char *buf, const char *name);
int build_wait(char *buf);
int build_name(char *buf, int player, const char *opponent);
int build_play(char *buf, int nextPlayer, const int *piles);
int build_over(char *buf, int winner, const int *piles, int forfeit);
int build_fail(char *buf, int code, const char *error);

#endif
