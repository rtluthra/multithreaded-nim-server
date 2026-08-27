#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

/* This file does the parsing and formatting of NGP messages.  Every message
   has the form
       0|LL|TYPE|field|...|
   where LL is the number of bytes following "0|LL|", so a complete message is
   always 5 + LL bytes long and always ends with a vertical bar. */

/* How many fields each message type carries. */
static int type_fields(message_type t)
{
    switch (t) {
    case MSG_OPEN: return 1;
    case MSG_WAIT: return 0;
    case MSG_NAME: return 2;
    case MSG_PLAY: return 2;
    case MSG_MOVE: return 2;
    case MSG_OVER: return 3;
    case MSG_FAIL: return 1;
    }
    return -1;
}

/* Match the four byte type code.  Returns 0 if it is not a known type. */
static int type_from_code(const char *code, message_type *out)
{
    if (memcmp(code, "OPEN", 4) == 0)      *out = MSG_OPEN;
    else if (memcmp(code, "WAIT", 4) == 0) *out = MSG_WAIT;
    else if (memcmp(code, "NAME", 4) == 0) *out = MSG_NAME;
    else if (memcmp(code, "PLAY", 4) == 0) *out = MSG_PLAY;
    else if (memcmp(code, "MOVE", 4) == 0) *out = MSG_MOVE;
    else if (memcmp(code, "OVER", 4) == 0) *out = MSG_OVER;
    else if (memcmp(code, "FAIL", 4) == 0) *out = MSG_FAIL;
    else return 0;
    return 1;
}

void conn_init(Conn *c, int fd)
{
    c->fd = fd;
    c->len = 0;
}

void msg_init(Message *m)
{
    int i;
    m->type = MSG_FAIL;
    m->fieldCount = 0;
    for (i = 0; i < MAX_FIELDS; i++)
        m->fields[i] = NULL;
}

void free_message(Message *m)
{
    int i;
    if (!m)
        return;
    for (i = 0; i < MAX_FIELDS; i++) {
        free(m->fields[i]);
        m->fields[i] = NULL;
    }
    m->fieldCount = 0;
}

/* ---------------------------------------------------------------- parsing */

int msg_parse(Conn *c, Message *m)
{
    const char *b = c->buf;
    size_t n = c->len;
    size_t pos, total;
    unsigned declared;
    message_type type;
    int nfields, i;

    msg_init(m);

    /* Protocol version: a single digit, always 0 for this project.  Each
       check is written so that an impossible byte is rejected as soon as it
       arrives, rather than waiting for the rest of the message. */
    if (n < 1) return NGP_INCOMPLETE;
    if (b[0] != '0') return NGP_MALFORMED;
    if (n < 2) return NGP_INCOMPLETE;
    if (b[1] != '|') return NGP_MALFORMED;

    /* Message length: exactly two decimal digits followed by a bar.  This is
       what makes "0|1|" detectably invalid after only four bytes. */
    if (n < 3) return NGP_INCOMPLETE;
    if (!isdigit((unsigned char)b[2])) return NGP_MALFORMED;
    if (n < 4) return NGP_INCOMPLETE;
    if (!isdigit((unsigned char)b[3])) return NGP_MALFORMED;
    if (n < 5) return NGP_INCOMPLETE;
    if (b[4] != '|') return NGP_MALFORMED;

    declared = (unsigned)(b[2] - '0') * 10 + (unsigned)(b[3] - '0');
    if (declared < 5)
        return NGP_MALFORMED;   /* too short to hold a type code and its bar */
    total = 5 + declared;

    /* Message type: four characters followed by a bar.  An unrecognised type
       means the number of fields cannot be determined, which the spec treats
       as a framing error. */
    if (n < MSG_HDR_LEN) return NGP_INCOMPLETE;
    if (b[9] != '|') return NGP_MALFORMED;
    if (!type_from_code(b + 5, &type)) return NGP_MALFORMED;
    nfields = type_fields(type);

    if (n < total) {
        /* Fewer bytes than the stated length.  If every field is already
           present and terminated then the message is too short for its own
           length field; otherwise we simply need to read more. */
        int found = 0;
        pos = MSG_HDR_LEN;
        while (found < nfields) {
            const char *bar = memchr(b + pos, '|', n - pos);
            if (!bar)
                break;
            pos = (size_t)(bar - b) + 1;
            found++;
        }
        return (found >= nfields) ? NGP_MALFORMED : NGP_INCOMPLETE;
    }

    /* Every field must be present and terminated within the stated length,
       and the last bar must be the final byte of the message. */
    pos = MSG_HDR_LEN;
    for (i = 0; i < nfields; i++) {
        const char *bar = memchr(b + pos, '|', total - pos);
        if (!bar)
            return NGP_MALFORMED;   /* field runs past the stated length */
        pos = (size_t)(bar - b) + 1;
    }
    if (pos != total)
        return NGP_MALFORMED;       /* extra bytes or bars inside the length */

    /* Copy the fields out.  memchr is used rather than strtok because empty
       fields are legal (the third field of OVER is empty on a normal win)
       and strtok would silently merge the adjacent bars. */
    m->type = type;
    pos = MSG_HDR_LEN;
    for (i = 0; i < nfields; i++) {
        const char *bar = memchr(b + pos, '|', total - pos);
        size_t flen = (size_t)(bar - (b + pos));
        char *f = malloc(flen + 1);
        if (!f) {
            free_message(m);
            return NGP_CLOSED;
        }
        memcpy(f, b + pos, flen);
        f[flen] = '\0';
        m->fields[i] = f;
        m->fieldCount++;
        pos = (size_t)(bar - b) + 1;
    }

    /* Drop the message we just consumed, keeping any glued remainder. */
    memmove(c->buf, c->buf + total, c->len - total);
    c->len -= total;
    return NGP_OK;
}

int conn_fill_nonblocking(Conn *c)
{
    ssize_t n;

    if (c->len >= sizeof(c->buf))
        return NGP_MALFORMED;

    n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len, MSG_DONTWAIT);
    if (n > 0) {
        c->len += (size_t)n;
        return NGP_OK;
    }
    if (n == 0)
        return NGP_CLOSED;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return NGP_INCOMPLETE;
    return NGP_CLOSED;
}

int recv_msg(Conn *c, Message *m)
{
    for (;;) {
        ssize_t n;
        int r = msg_parse(c, m);

        if (r != NGP_INCOMPLETE)
            return r;

        /* Only an incomplete first message can be left in the buffer here, so
           if it already spans a full message's worth of bytes it can never
           become valid.  This also guarantees there is room to read. */
        if (c->len >= MAX_MSG)
            return NGP_MALFORMED;

        n = read(c->fd, c->buf + c->len, sizeof(c->buf) - c->len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return NGP_CLOSED;
        }
        if (n == 0)
            return NGP_CLOSED;
        c->len += (size_t)n;
    }
}

/* ---------------------------------------------------------------- sending */

int send_msg(int fd, const char *msg)
{
    const char *p = msg;
    size_t left = strlen(msg);

    /* No terminator is added: the message length and the trailing bar are the
       only framing the protocol defines, so an extra byte would corrupt it. */
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

/* --------------------------------------------------------------- builders */

static void board_string(char *out, size_t cap, const int *piles)
{
    snprintf(out, cap, "%d %d %d %d %d",
             piles[0], piles[1], piles[2], piles[3], piles[4]);
}

/* Every builder computes the declared length the same way: four bytes of type
   code, the bar after it, then each field and its bar. */
static int framed(char *buf, int len, int written)
{
    if (len < 5 || len > 99 || written < 0 || written > MAX_MSG) {
        buf[0] = '\0';
        return -1;
    }
    return written;
}

int build_open(char *buf, const char *name)
{
    int len = 4 + 1 + (int)strlen(name) + 1;
    return framed(buf, len,
                  snprintf(buf, MAX_MSG + 1, "0|%02d|OPEN|%s|", len, name));
}

int build_wait(char *buf)
{
    int len = 4 + 1;
    return framed(buf, len,
                  snprintf(buf, MAX_MSG + 1, "0|%02d|WAIT|", len));
}

int build_name(char *buf, int player, const char *opp)
{
    char pbuf[8];
    int len;

    snprintf(pbuf, sizeof(pbuf), "%d", player);
    len = 4 + 1 + (int)strlen(pbuf) + 1 + (int)strlen(opp) + 1;
    return framed(buf, len,
                  snprintf(buf, MAX_MSG + 1, "0|%02d|NAME|%s|%s|",
                           len, pbuf, opp));
}

int build_play(char *buf, int nextPlayer, const int *piles)
{
    char state[64];
    char pbuf[8];
    int len;

    board_string(state, sizeof(state), piles);
    snprintf(pbuf, sizeof(pbuf), "%d", nextPlayer);
    len = 4 + 1 + (int)strlen(pbuf) + 1 + (int)strlen(state) + 1;
    return framed(buf, len,
                  snprintf(buf, MAX_MSG + 1, "0|%02d|PLAY|%s|%s|",
                           len, pbuf, state));
}

int build_over(char *buf, int winner, const int *piles, int forfeit)
{
    char state[64];
    char wbuf[8];
    const char *extra = forfeit ? "Forfeit" : "";
    int len;

    board_string(state, sizeof(state), piles);
    snprintf(wbuf, sizeof(wbuf), "%d", winner);
    len = 4 + 1 + (int)strlen(wbuf) + 1 + (int)strlen(state) + 1
            + (int)strlen(extra) + 1;
    return framed(buf, len,
                  snprintf(buf, MAX_MSG + 1, "0|%02d|OVER|%s|%s|%s|",
                           len, wbuf, state, extra));
}

int build_fail(char *buf, int code, const char *error)
{
    char temp[96];
    int len;

    snprintf(temp, sizeof(temp), "%d %s", code, error);
    len = 4 + 1 + (int)strlen(temp) + 1;
    return framed(buf, len,
                  snprintf(buf, MAX_MSG + 1, "0|%02d|FAIL|%s|", len, temp));
}
