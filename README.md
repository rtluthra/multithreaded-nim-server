# Multithreaded TCP Nim Server

A concurrent TCP server written in C that supports multiple simultaneous Nim games using POSIX threads, thread-safe message queues, custom TCP protocol framing, and synchronized client management.

This project was developed for Rutgers University CS 214 Project IV and implements the concurrent game architecture (Section 4.2) and extra-credit requirements (Section 4.3).

## Highlights

- Supports multiple Nim games concurrently
- Uses POSIX threads for connection handling and game execution
- Implements a dedicated reader thread for each player
- Uses bounded, thread-safe message queues to coordinate reader threads and game managers
- Implements custom TCP message framing and protocol validation
- Handles partial and multiple messages within a TCP stream
- Detects malformed input and applies protocol-specific error codes
- Handles out-of-turn moves without blocking on the current player
- Detects client disconnects and awards forfeits
- Uses per-connection receive buffers to prevent shared parser state
- Protects the player registry and matchmaking lobby with mutexes
- Includes automated unit and end-to-end tests

## Architecture

Multiple games run independently at the same time. Each active game has its own game manager, message queue, and pair of reader threads.

```text
              incoming TCP connections
                         |
                         v
              +---------------------+
              |  nimd accept() loop |
              +----------+----------+
                         |
        one handshake thread per connection
                         |
                         v
              +---------------------+
              |  matchmaking lobby  |
              +----------+----------+
                         |
    pairs waiting players into independent games
                         |
     +---------+---------+---------+---------+
     |         |         |         |         |
  Game 1    Game 2    Game 3    Game 4      ...
```

Each game is a self-contained set of three threads:

```text
   client A socket       client B socket
          |                     |
          v                     v
   +--------------+      +--------------+
   | Reader (P1)  |      | Reader (P2)  |
   +------+-------+      +------+-------+
          |                     |
          +----------+----------+
                     |
                     v
          +---------------------+
          | bounded queue       |
          | mutex + 2 cond vars |
          +----------+----------+
                     |
                     v
          +---------------------+
          | game manager thread |
          +----------+----------+
                     |
                     v
          +---------------------+
          | board + turn state  |
          +---------------------+
```

### Server

`nimd.c` is responsible for:

- Listening for TCP connections
- Processing the initial `OPEN` handshake
- Maintaining the connected-player registry
- Managing the matchmaking lobby
- Pairing players into games
- Starting game threads

### Game threads

Each active game uses one game manager thread and one reader thread per player.

Reader threads receive and parse messages from their respective TCP connections and place them into a bounded message queue.

The game manager is the only thread that modifies the board and turn state. This keeps game-state updates serialized without requiring a separate lock around the game state itself.

## Concurrency

Games are managed independently, allowing multiple games to progress concurrently without one game blocking another.

Within a game, reader threads process messages independently of whose turn it is. This allows the server to immediately respond to an out-of-turn move instead of waiting for the current player.

For example:

```text
player 2 sends MOVE (out of turn)
            |
            v
     Reader Thread 2          <- never waits for player 1
            |
            v
     Message Queue
            |
            v
     Game Manager             <- not player 2's turn
            |
            v
     FAIL 31 Impatient        <- replied immediately
```

This architecture also allows a player disconnecting during the opponent's turn to be detected and handled as a forfeit.

## Synchronization

The server uses POSIX synchronization primitives to coordinate shared state safely.

### Message queue

Each game has a bounded message queue protected by:

- `pthread_mutex_t`
- Condition variables for queue availability
- A closed-state flag for coordinated shutdown

Reader threads push received messages into the queue while the game manager consumes them.

### Player registry

The server maintains a registry of connected player names protected by a mutex.

Checking whether a name is already in use and adding the name occur under the same lock, preventing two simultaneous connections from claiming the same name.

### Matchmaking lobby

The lobby of unmatched players is also protected by a mutex. Lock ordering is kept consistent to avoid deadlock between shared server components.

## Connection and memory management

Each TCP connection maintains its own receive buffer. This prevents multiple reader threads from sharing parser state and allows independent connections to be processed safely.

At the end of a game, shutdown follows an explicit order:

1. Close the message queue
2. Shut down both client sockets
3. Wake blocked reader threads
4. Join the reader threads
5. Release game resources

This prevents reader threads from accessing memory after the game state has been freed.

Received protocol messages are dynamically allocated and released after processing.

## TCP protocol

The server implements a custom application-layer protocol using the following framing format:

```text
0|LL|TYPE|field1|field2|...|
```

`LL` is a two-digit length field representing the number of bytes following the length prefix. A complete message therefore contains `5 + LL` bytes and ends with a vertical bar.

### Supported message types

| Type | Sent by | Fields |
| --- | --- | --- |
| `OPEN` | client | player name |
| `WAIT` | server | none |
| `NAME` | server | player number, opponent name |
| `PLAY` | server | next player, board state |
| `MOVE` | client | pile, quantity |
| `FAIL` | either | error code and message |
| `OVER` | server | winner, final board, forfeit flag |

### Framing

The protocol implementation validates:

- Protocol version
- Two-digit length field
- Message length
- Message type
- Required field count
- Field termination
- Final message delimiter

Malformed messages are rejected with the appropriate protocol error.

### TCP stream handling

Because TCP is a byte stream rather than a message-oriented protocol, a single `read()` may contain:

- Part of a message
- Exactly one message
- Multiple complete messages
- A complete message followed by part of another message

The receiver maintains buffered data for each connection and processes complete messages without requiring additional reads.

For example:

```text
one read() may return:

  +------------------+------------------+-----------+
  | complete message | complete message |  partial  |
  +------------------+------------------+-----------+
   ^                  ^                  ^
   processed now      processed now      buffered until
                                         more bytes arrive
```

## Error handling

The server implements protocol-specific error codes for invalid client behavior.

| Code | `FAIL` text | Meaning | Connection |
| --- | --- | --- | --- |
| 10 | `10 Invalid` | Invalid or misframed message | Closed |
| 21 | `21 Long Name` | Player name longer than 72 characters | Closed |
| 22 | `22 Already Playing` | Name is already connected | Closed |
| 23 | `23 Already Open` | `OPEN` sent more than once | Closed |
| 24 | `24 Not Playing` | `MOVE` sent before the game began | Closed |
| 31 | `31 Impatient` | `MOVE` sent during the opponent's turn | Remains open |
| 32 | `32 Pile Index` | Pile index outside 1-5 | Remains open |
| 33 | `33 Quantity` | Quantity too large or too small | Remains open |

Malformed protocol input is handled separately from client disconnects. A malformed message results in an appropriate `FAIL` response, while a mid-game disconnect results in a forfeit and an `OVER` message to the remaining player.

## Nim game logic

Each game starts with five piles:

```text
1 3 5 7 9
```

Players alternate turns and may remove one or more stones from a single pile. Player 1 moves first. The player who removes the final stone wins.

The game logic validates:

- Pile indices
- Move quantities
- Turn order
- Remaining stones
- End-of-game conditions
- Winner determination

## Project structure

| File | Description |
| --- | --- |
| `nimd.c` | TCP server, handshake, matchmaking, and player management |
| `game_thread.c` | Reader threads, game manager, and message queue |
| `game_thread.h` | Threading, queue, and game-state definitions |
| `game.c` | Nim game rules and move validation |
| `game.h` | Nim game interface |
| `protocol.c` | TCP message parsing and message construction |
| `protocol.h` | Protocol definitions and interfaces |
| `test.c` | Automated unit and integration tests |
| `Makefile` | Build configuration |
| `AUTHOR` | Project authors |

## Building

Build the server and test program:

```bash
make
```

Run the automated test suite:

```bash
./test
```

Clean compiled binaries and object files:

```bash
make clean
```

## Running the server

Start the server on a specified TCP port:

```bash
./nimd 4000
```

The server listens for incoming TCP client connections and automatically matches available players into games.

## Manual testing

A basic two-player connection can be tested with `nc`:

```bash
printf '0|11|OPEN|Alice|' | nc localhost 4000
```

and from another terminal:

```bash
printf '0|09|OPEN|Bob|' | nc localhost 4000
```

The protocol does not append a newline to responses because message boundaries are determined by the protocol's length field. For exact protocol testing, clients should send only the framed bytes shown above.

## Testing

The project includes automated tests covering protocol parsing, TCP framing, game logic, concurrency, error handling, and connection management.

### Unit tests

The unit test suite contains 69 checks covering:

- Message construction
- Message parsing
- TCP framing
- Partial messages
- Multiple messages in a single read
- Messages split across multiple writes
- Malformed frames
- Invalid length fields
- Unknown message types
- Missing and extra fields
- Empty fields
- Long player names
- TCP disconnects
- Nim game rules
- Move validation
- Game completion

### End-to-end tests

The integration test suite contains 31 checks covering:

- Player matchmaking
- Duplicate-name rejection
- Complete handshake
- Valid gameplay
- Out-of-turn moves
- Invalid pile indices
- Invalid quantities
- Multiple messages in one TCP write
- Client disconnects and forfeits
- Name reuse after a game ends
- Multiple simultaneous games
- Partial `OPEN` messages
- Protocol violations

### Concurrency and memory testing

The server was additionally tested using:

- ThreadSanitizer for data-race detection
- AddressSanitizer for invalid memory access detection
- Sequential game execution
- Multiple simultaneous games
- Abandoned and rejected connections
- High-volume concurrent connections

## Technologies

C, POSIX threads (pthread), TCP/IP sockets, mutexes, condition variables, thread-safe queues, a custom application-layer protocol, dynamic memory management, Make, AddressSanitizer, ThreadSanitizer, and Git.

Rutgers University, Computer Science 214.
