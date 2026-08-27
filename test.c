#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "game.h"
#include "protocol.h"

/* Unit tests for the pieces that are hard to exercise by hand: message
   framing, the TCP cases where a read does not line up with a message
   boundary, and the Nim rules. */

static int failures = 0;
static int checks = 0;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("FAIL: %s\n  got  \"%s\"\n  want \"%s\"\n", what, got, want);
    }
}

/* Put bytes into a connection buffer without involving a socket. */
static void feed(Conn *c, const char *s)
{
    size_t n = strlen(s);
    memcpy(c->buf + c->len, s, n);
    c->len += n;
}

/* ------------------------------------------------------------- builders */

/* The expected strings are the examples printed in the specification. */
static void test_builders(void)
{
    char buf[MAX_MSG + 1];
    int piles[NIM_PILES];
    char longname[MAX_NAME + 40];

    check(build_open(buf, "John Nim-Example") > 0, "build_open succeeds");
    check_str(buf, "0|22|OPEN|John Nim-Example|", "OPEN example");

    build_wait(buf);
    check_str(buf, "0|05|WAIT|", "WAIT example");

    build_name(buf, 1, "Alice Opponentson");
    check_str(buf, "0|25|NAME|1|Alice Opponentson|", "NAME example");

    init_piles(piles);
    build_play(buf, 1, piles);
    check_str(buf, "0|17|PLAY|1|1 3 5 7 9|", "PLAY example");

    memset(piles, 0, sizeof(piles));
    build_over(buf, 2, piles, 0);
    check_str(buf, "0|18|OVER|2|0 0 0 0 0||", "OVER example (normal win)");

    build_over(buf, 1, piles, 1);
    check_str(buf, "0|25|OVER|1|0 0 0 0 0|Forfeit|", "OVER with forfeit");

    build_fail(buf, 22, "Already Playing");
    check_str(buf, "0|24|FAIL|22 Already Playing|", "FAIL example");

    /* A name at the limit must still fit; anything far past it is refused
       rather than overrunning the caller's buffer. */
    memset(longname, 'x', MAX_NAME);
    longname[MAX_NAME] = '\0';
    check(build_name(buf, 2, longname) > 0, "72 character name fits");

    memset(longname, 'x', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    check(build_name(buf, 2, longname) < 0, "oversized name is refused");
}

/* --------------------------------------------------------------- framing */

static void test_valid_parse(void)
{
    Conn c;
    Message m;

    conn_init(&c, -1);
    feed(&c, "0|22|OPEN|John Nim-Example|");
    check(msg_parse(&c, &m) == NGP_OK, "OPEN parses");
    check(m.type == MSG_OPEN, "OPEN type");
    check(m.fieldCount == 1, "OPEN field count");
    check_str(m.fields[0], "John Nim-Example", "OPEN name field");
    check(c.len == 0, "buffer emptied");
    free_message(&m);

    /* WAIT has no fields at all. */
    conn_init(&c, -1);
    feed(&c, "0|05|WAIT|");
    check(msg_parse(&c, &m) == NGP_OK, "WAIT parses");
    check(m.fieldCount == 0, "WAIT has no fields");
    free_message(&m);

    /* The last field of a normal OVER is empty, which must survive parsing. */
    conn_init(&c, -1);
    feed(&c, "0|18|OVER|2|0 0 0 0 0||");
    check(msg_parse(&c, &m) == NGP_OK, "OVER parses");
    check(m.fieldCount == 3, "OVER field count");
    check_str(m.fields[1], "0 0 0 0 0", "OVER board field");
    check_str(m.fields[2], "", "OVER empty forfeit field");
    free_message(&m);
}

static void test_partial(void)
{
    Conn c;
    Message m;

    /* A message delivered a few bytes at a time must not be reported until it
       is complete, and must not be treated as an error. */
    conn_init(&c, -1);
    feed(&c, "0");
    check(msg_parse(&c, &m) == NGP_INCOMPLETE, "partial: version only");
    feed(&c, "|09|MO");
    check(msg_parse(&c, &m) == NGP_INCOMPLETE, "partial: half a type code");
    feed(&c, "VE|3|");
    check(msg_parse(&c, &m) == NGP_INCOMPLETE, "partial: one field short");
    feed(&c, "5|");
    check(msg_parse(&c, &m) == NGP_OK, "partial: completes");
    check(m.type == MSG_MOVE, "reassembled type");
    check_str(m.fields[0], "3", "reassembled pile");
    check_str(m.fields[1], "5", "reassembled quantity");
    free_message(&m);
}

static void test_multiple(void)
{
    Conn c;
    Message m;

    /* Two whole messages plus the start of a third, all from one read. */
    conn_init(&c, -1);
    feed(&c, "0|09|MOVE|1|1|0|09|MOVE|2|3|0|09|MOV");

    check(msg_parse(&c, &m) == NGP_OK, "glued: first message");
    check_str(m.fields[0], "1", "glued: first pile");
    free_message(&m);

    check(msg_parse(&c, &m) == NGP_OK, "glued: second message");
    check_str(m.fields[0], "2", "glued: second pile");
    free_message(&m);

    check(msg_parse(&c, &m) == NGP_INCOMPLETE, "glued: trailing partial");
    check(c.len == strlen("0|09|MOV"), "glued: partial bytes retained");
}

static void expect_malformed(const char *bytes, const char *what)
{
    Conn c;
    Message m;

    conn_init(&c, -1);
    feed(&c, bytes);
    check(msg_parse(&c, &m) == NGP_MALFORMED, what);
    free_message(&m);
}

static void expect_incomplete(const char *bytes, const char *what)
{
    Conn c;
    Message m;

    conn_init(&c, -1);
    feed(&c, bytes);
    check(msg_parse(&c, &m) == NGP_INCOMPLETE, what);
    free_message(&m);
}

static void test_malformed(void)
{
    expect_malformed("1|05|WAIT|",       "wrong protocol version");
    expect_malformed("0x05|WAIT|",       "missing version terminator");
    expect_malformed("0|1|",             "length is not two digits");
    expect_malformed("0|ab|WAIT|",       "non numeric length");
    expect_malformed("0|03|WAI|",        "length too small for a type");
    expect_malformed("0|10|PIZZA|hi|",   "unknown message type");
    expect_malformed("0|09|MOVEx3|5|",   "type code not terminated");
    expect_malformed("0|09|MOVE|3456|",  "field not terminated in the length");
    expect_malformed("0|11|MOVE|3|5|7|", "extra field inside the length");
    expect_malformed("0|07|MOVE|3|5|",   "fields run past the length");
    expect_malformed("0|22|OPEN|a|b|",   "name containing a bar");

    /* These are short rather than wrong: more bytes could still complete
       them, so the parser must ask for more instead of failing. */
    expect_incomplete("0|05|WAIT",       "truncated WAIT is only incomplete");
    expect_incomplete("0|09|MOVE|35|",   "missing final field is incomplete");

    /* The declared length ends the message, so anything after it belongs to
       the next message rather than making this one invalid. */
    {
        Conn c;
        Message m;
        conn_init(&c, -1);
        feed(&c, "0|09|MOVE|3|5|7|");
        check(msg_parse(&c, &m) == NGP_OK, "length bounds the message");
        check(c.len == strlen("7|"), "bytes past the length are kept");
        free_message(&m);
        check(msg_parse(&c, &m) == NGP_MALFORMED, "the leftover is rejected");
        free_message(&m);
    }
}

/* ------------------------------------------------------------ socket path */

static void test_socket_roundtrip(void)
{
    int sv[2];
    Conn c;
    Message m;
    char buf[MAX_MSG + 1];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        printf("FAIL: socketpair\n");
        failures++;
        return;
    }

    conn_init(&c, sv[0]);

    /* send_msg must put exactly the framed bytes on the wire, with nothing
       appended, or the receiver's length arithmetic breaks. */
    build_open(buf, "Alice");
    check(send_msg(sv[1], buf) == 0, "send_msg succeeds");
    check(recv_msg(&c, &m) == NGP_OK, "recv_msg reads one message");
    check(m.type == MSG_OPEN, "socket: OPEN type");
    check_str(m.fields[0], "Alice", "socket: name survives the round trip");
    free_message(&m);

    /* Two messages written separately must both be recovered, and the second
       must be available without waiting for more data. */
    build_open(buf, "Bob");
    send_msg(sv[1], buf);
    build_wait(buf);
    send_msg(sv[1], buf);
    check(recv_msg(&c, &m) == NGP_OK, "socket: first of two");
    check_str(m.fields[0], "Bob", "socket: first payload");
    free_message(&m);
    check(recv_msg(&c, &m) == NGP_OK, "socket: second of two");
    check(m.type == MSG_WAIT, "socket: second type");
    free_message(&m);

    /* A closed peer is reported as closed, not as a framing error. */
    close(sv[1]);
    check(recv_msg(&c, &m) == NGP_CLOSED, "socket: close is detected");
    free_message(&m);
    close(sv[0]);
}

/* --------------------------------------------------------------- nim rules */

static void test_game(void)
{
    int piles[NIM_PILES];

    init_piles(piles);
    check(piles[0] == 1 && piles[4] == 9, "initial board is 1 3 5 7 9");
    check(!is_game_over(piles), "fresh board is not over");

    check(check_move(piles, 0, 1) == MOVE_BAD_PILE, "pile 0 is out of range");
    check(check_move(piles, 6, 1) == MOVE_BAD_PILE, "pile 6 is out of range");
    check(check_move(piles, 1, 0) == MOVE_BAD_QTY,  "removing 0 is invalid");
    check(check_move(piles, 1, 2) == MOVE_BAD_QTY,  "removing too many");
    check(check_move(piles, 3, 5) == MOVE_OK,       "emptying a pile is legal");

    move(piles, 3, 5);
    check(piles[2] == 0, "stones are removed from the right pile");
    check(check_move(piles, 3, 1) == MOVE_BAD_QTY, "empty pile rejects moves");

    memset(piles, 0, sizeof(piles));
    check(is_game_over(piles), "empty board is over");
}

int main(void)
{
    test_builders();
    test_valid_parse();
    test_partial();
    test_multiple();
    test_malformed();
    test_socket_roundtrip();
    test_game();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
