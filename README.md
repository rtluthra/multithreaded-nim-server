CS 214 Project IV: Nim (nimd)

Authors
  RL857 - Ritika Luthra
  SND84 - Sanika Deshmukh

Level attempted
  Concurrent games (section 4.2) AND the extra credit (section 4.3).

  nimd runs every game on its own threads, so any number of games proceed at
  the same time and no game waits on another.  Within a game, each player has
  a dedicated reader thread, so the server reacts to whichever player sends a
  message first.  It does not wait for the player whose turn it is.  That is
  what makes the two extra-credit cases work:

    - If it is player 1's turn and player 2 sends MOVE, player 2 immediately
      receives FAIL "31 Impatient".  Player 1's move is not required first.
    - If player 2 disconnects while player 1 is still thinking, player 1 is
      immediately declared the winner by forfeit.

1. Building and running

  make            builds nimd and the test program
  ./nimd <port>   starts the server, for example ./nimd 4000
  ./test          runs the unit tests (prints "N checks, 0 failures")
  make clean      removes the binaries and object files

  nimd logs connections, moves and results to STDOUT.

  Note on testing by hand: nimd sends exactly the bytes the protocol
  defines and does not append a newline, because a stray byte would break the
  message length used for framing.  This means that with plain nc the replies
  arrive without line breaks and several may appear run together on one line,
  which is correct.  nc also appends a newline to whatever you type; nimd
  tolerates the extra byte at the start of the next message only if you send
  well-formed messages, so for exact testing prefer a client that does not
  add a newline:

    printf '0|11|OPEN|Alice|' | nc localhost 4000

2. Architecture

  nimd.c          listening socket, OPEN/WAIT handshake, the lobby of
                  unmatched players, the registry of connected players, and
                  the pairing of players into games
  game_thread.c   the reader threads, the game manager thread, and the
                  message queue that connects them
  game.c          the Nim rules: the board, legal moves, and end of game
  protocol.c      message framing: parsing received bytes into messages and
                  building messages to send
  test.c          unit tests for framing and the Nim rules

  Threads
    - One thread per connection performs the handshake, so a slow or silent
      client never holds up the listening socket.
    - Each game has one manager thread and two reader threads.  The readers
      only turn bytes into messages and push them onto a bounded queue; the
      manager pops from that queue and is the only thread that touches the
      board and the turn.  This keeps the game rules in one place and means
      no lock is needed on the game state itself.
    - The queue is guarded by a mutex with one condition variable for "not
      empty" and one for "not full".

  Shared state
    - The list of connected players is guarded by its own mutex.  Testing a
      name and inserting it happen under a single lock, so two clients
      connecting at once cannot both claim the same name.
    - The lobby of unmatched players is guarded by its own mutex.
    - Locks are always taken lobby-then-players, never the other way, so
      there is no possibility of deadlock.

  Memory ownership
    - Every connection owns its own receive buffer, so no parser state is
      shared between connections.
    - A player's name and receive buffer are created by the handshake, handed
      to the lobby, and then handed to the game.  The game manager frees them
      when the game ends.
    - A received message is freed in exactly one place: the manager's loop,
      after it has been handled.
    - At the end of a game the manager closes the queue, shuts down both
      sockets to wake the readers, joins both readers, and only then frees
      the game.  Nothing is freed while a reader could still be using it.

3. Protocol

  Messages are 0|LL|TYPE|field|...|, where LL is two decimal digits giving the
  number of bytes after "0|LL|".  A complete message is therefore 5 + LL bytes
  and always ends with a vertical bar.

  Framing is validated both ways, as section 3.4.2 requires: the stated length
  must be exactly two digits, the type code must be four known characters, and
  the message must contain exactly the number of bar-terminated fields that
  its type requires, with the last bar as the final byte.  Anything else is
  answered with FAIL "10 Invalid" and the connection is closed.

  Invalid byte sequences are rejected as early as possible rather than by
  waiting for more data.  For example "0|1|" is refused after four bytes,
  because the length field is not two digits.

  Because TCP does not preserve message boundaries, the receiver keeps a
  buffer per connection and handles all four cases:
    - a partial message: keep the bytes and read more
    - one complete message: consume exactly 5 + LL bytes
    - several complete messages from one read: each is returned in turn, and
      no read() is made while a complete message is still buffered
    - a complete message followed by a partial one: the complete message is
      returned and the remainder is kept for later

  Bytes that arrive before a player is matched are kept and carried into the
  game, so nothing is lost at the handshake boundary.

  Error codes sent
    10 Invalid          message cannot be parsed or is misframed      (closes)
    21 Long Name        player name longer than 72 characters         (closes)
    22 Already Playing  that name is already connected                (closes)
    23 Already Open     OPEN sent a second time                       (closes)
    24 Not Playing      MOVE sent before the game began               (closes)
    31 Impatient        MOVE sent during the opponent's turn
    32 Pile Index       pile number outside 1-5
    33 Quantity         asked to remove too many or too few stones

  Sessions
    - A client that disconnects after OPEN but before NAME is dropped and is
      not matched with anyone.
    - A client that disconnects after NAME but before OVER forfeits, and the
      remaining player is sent OVER with "Forfeit".
    - After OVER the server closes the connection, and the player's name
      becomes available again.

4. Nim

  Every game starts with five piles of 1, 3, 5, 7 and 9 stones.  Players take
  turns removing at least one stone from a single pile.  Whoever removes the
  last stone wins.  Player 1 moves first.

5. Test plan

  Automated unit tests (./test), 69 checks:
    - every message builder reproduces the exact examples in the write-up,
      including OVER with an empty final field and OVER with "Forfeit"
    - a 72-character name fits; an oversized one is refused instead of
      overrunning the buffer
    - parsing OPEN, WAIT, and OVER, including an empty field
    - a message delivered a few bytes at a time is reported as incomplete
      until the last byte arrives, then parsed correctly
    - two whole messages plus the start of a third from one read: both are
      returned and the partial bytes are kept
    - malformed input is rejected: wrong version, missing version bar, a
      one-digit length, a non-numeric length, a length too small to hold a
      type, an unknown type, an unterminated type code, a field not
      terminated inside the stated length, an extra field inside the stated
      length, fields running past the stated length, and a name containing a
      vertical bar
    - input that is merely short is reported as incomplete, not invalid
    - bytes past the stated length belong to the next message
    - over a real socket: send_msg puts exactly the framed bytes on the wire,
      two messages written separately are both recovered, and a closed peer
      is reported as closed rather than as a framing error
    - the Nim rules: starting board, pile and quantity range checks, removing
      from the right pile, and detecting the end of the game

  End-to-end tests against a running server, 31 checks, all passing:
    - both players receive WAIT after their own OPEN
    - both receive NAME with the right player number and opponent name,
      followed immediately by the first PLAY
    - a valid move updates the board, both players are sent the new PLAY, and
      the turn changes
    - out-of-turn MOVE is answered with 31 Impatient at once, without waiting
      for the other player (extra credit)
    - pile 9 gives 32 Pile Index; removing 5 from a pile of 1 gives 33
      Quantity; the connection stays open in both cases
    - two MOVE messages written as one block are both processed
    - playing to the last stone gives OVER with the winner and an empty third
      field
    - a duplicate name gives 22, a 73-character name gives 21, "0|1|" and an
      unknown type give 10, MOVE before OPEN gives 24, a second OPEN gives 23
    - an OPEN split across two writes is reassembled
    - a player disconnecting mid-game gives the other OVER with "Forfeit"
    - a player who leaves while waiting is dropped, and the next two live
      players are matched with each other
    - two games run at once: a move in one game is processed while the other
      game sits idle, and both stay responsive
    - a name can be reused after its game ends

  Concurrency and memory checks:
    - built and run under ThreadSanitizer: no data races reported across
      concurrent games, disconnects and forfeits
    - built and run under AddressSanitizer: no invalid accesses reported
    - 250 sequential games plus rejected and abandoned connections plus a
      burst of 80 clients connecting at once (40 simultaneous games): all 80
      were matched, memory stayed flat, and the server ended holding fewer
      open descriptors than a freshly started one, so sockets and threads are
      not accumulating
