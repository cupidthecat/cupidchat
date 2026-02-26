# CupidChat

Terminal-first chat system written in C11. A lightweight server (`cupid-chatd`) and a self-contained ncurses client (`cupid-chat`) that run on Linux.

## Features

**Server**

- `epoll`-based event loop with `timerfd` keepalive
- SQLite WAL persistence (rooms, users, per-room history, topics)
- Token-bucket rate limiting per connection
- Per-connection output backpressure with priority frame dropping
- Keepalive ping/pong with configurable timeout
- TLS listener (build-time opt-in via `TLS=1`)

**Client**

- ncurses TUI: room sidebar, topic bar, chat pane, status bar, user list
- Rooms and direct messages (DMs) with separate scroll state per conversation
- Typing indicators, nick-mention highlights, away status
- Per-nick color assignment, IRC-style `/me` actions, date separators
- Command history (Up/Down), sidebar navigation (Tab / arrow keys)
- Notification sounds embedded directly in the binary - no `sounds/` directory needed at runtime
- All sounds played via `aplay` (ALSA) in a forked child; TUI is never blocked

**Protocol**

- Binary framing (`frame`) with TLV-encoded payloads
- Priority flags and sequence numbers on every frame
- Shared implementation used by both client and server

## Requirements

**Build time**

- Linux
- `gcc` with C11 support
- `make`
- `python3` (generates embedded sound data at build time)
- `libsqlite3-dev`
- `libncurses-dev`
- Optional TLS: `libssl-dev` (`libssl`, `libcrypto`)

**Runtime**

- Linux kernel >= 3.17 (uses `memfd_create` for in-memory sound playback)
- `aplay` (ALSA userspace tool) for notification sounds - sounds are silently skipped if absent

### Ubuntu/Debian

```bash
sudo apt update
sudo apt install -y build-essential libsqlite3-dev libncurses-dev libssl-dev python3 alsa-utils
```

### Arch Linux

```bash
sudo pacman -S base-devel sqlite ncurses openssl python alsa-utils
```

## Build

```bash
make
```

Build with TLS support:

```bash
make TLS=1
```

Clean everything including generated files:

```bash
make clean
```

Build outputs:

- `./cupid-chatd` - server binary
- `./cupid-chat` - client binary (sounds embedded, fully self-contained)
- `build/` - object files, dependency files, test binaries

The build runs `scripts/gen_sounds.py` automatically to embed the WAV files in `sounds/` as C byte arrays compiled into the client binary. The `sounds/` directory is only needed at build time.

All builds include AddressSanitizer and UBSan (`-fsanitize=address,undefined`) for early bug detection.

## Quick start

### 1. Start the server

```bash
./cupid-chatd --port 5555 --verbose
```

### 2. Connect clients

```bash
./cupid-chat --host 127.0.0.1 --port 5555 --nick alice
```

```bash
./cupid-chat --host 127.0.0.1 --port 5555 --nick bob
```

The client auto-joins `#general` on connect. Type a message and press Enter to send.

## Server options

```
--host HOST          Listen address          (default: 0.0.0.0)
--port PORT          Plaintext port          (default: 5555)
--tls-port PORT      TLS port                (TLS=1 builds only)
--cert FILE          TLS certificate file
--key FILE           TLS private key file
--ca FILE            TLS CA certificate
--max-clients N      Max simultaneous connections  (default: 1024, clamped 1-65535)
--ping-interval S    Keepalive ping interval in seconds  (default: 30, min: 1)
--ping-timeout S     Seconds before idle client is dropped  (must be < ping-interval)
--rate-msgs N        Token-bucket refill rate (msgs/sec)
--rate-burst N       Maximum burst size  (min: 1)
--obuf-limit BYTES   Per-connection output buffer cap  (min: 4096)
--history N          Per-room history to replay on join  (1-10000, default: 50)
--db PATH            SQLite database path  (default: cupidchat.db)
--verbose            Log every frame and event to stderr
-h, --help
```

## Client options

```
--host HOST          Server hostname  (default: 127.0.0.1)
--port PORT          Server port      (default: 5555)
--nick NICK          Initial nickname (default: guest<NNNN>)
--tls                Use TLS connection
--verbose            Debug output
-h, --help
```

## In-client commands

Type `/help` to open the command reference modal. All commands start with `/`.

**Rooms**

| Command | Description |
|---------|-------------|
| `/join <room>` | Join or create a room |
| `/leave [room]` | Leave current or named room |
| `/rooms` | Refresh and show room list |
| `/topic [text]` | Show or set the room topic |
| `/delroom [name]` | Delete a room you own |

**Messaging**

| Command | Description |
|---------|-------------|
| `/me <text>` | Send an action message (* nick text) |
| `/dm <nick> <message>` | Send a direct message |
| `/closedm <nick>` | Remove a DM from the sidebar |

**Users**

| Command | Description |
|---------|-------------|
| `/users [room]` | List users in current or named room |
| `/nick <newnick>` | Change display name |
| `/away [message]` | Set away status with optional message |
| `/back` | Clear away status |
| `/ignore <nick>` | Add nick to ignore list (or list ignored nicks) |
| `/unignore <nick>` | Remove nick from ignore list |

**Account**

| Command | Description |
|---------|-------------|
| `/register <nick> <password>` | Create a persistent account |
| `/login <nick> <password>` | Log in to an existing account |
| `/logout` | Return to a guest session |

**Other**

| Command | Description |
|---------|-------------|
| `/quit` or `/q` | Exit the client |
| `/help` | Open the command help modal |

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| `Enter` | Send message; if input is empty, confirm sidebar selection |
| `Tab` / `Shift+Tab` | Move sidebar cursor down/up |
| `Up` / `Down` | Sidebar navigation (when input empty) or command history (when typing) |
| `PgUp` / `PgDn` | Scroll chat pane |
| `Ctrl+A` / `Home` | Move cursor to start of input |
| `Ctrl+E` / `End` | Move cursor to end of input |
| `Ctrl+K` | Delete from cursor to end of line |
| `Left` / `Right` | Move input cursor |
| `Delete` | Delete character under cursor |
| Any key | Dismiss help modal |

## Tests

Run unit tests:

```bash
make check
```

Test suite:

- `tests/proto/test_frame.c` - frame encode/decode
- `tests/proto/test_tlv.c` - TLV read/write
- `tests/server/test_rate_limit.c` - token-bucket rate limiter

Smoke test (requires a running server and `python3`/`nc`):

```bash
./tests/smoke/multi_client.sh
```

## Seeding sample data

Populate a database with realistic room history for testing:

```bash
python3 scripts/seed_history.py --db cupidchat.db --count 300 --room general
```

## Project layout

```
src/
  server/
    main.c              - startup, signal handling, resource cleanup
    core/
      loop.c            - epoll event loop, accept, I/O dispatch
      conn.c            - per-connection read/write state
      dispatch.c        - server message handler (all CMSG_* types)
      state.c           - shared server state (rooms, connections)
      room.c            - room membership helpers
      keepalive.c       - ping/pong timeout tracking
      rate_limit.c      - token-bucket rate limiter
      backpressure.c    - output buffer pressure tracking
    db/
      db.c              - SQLite persistence (users, rooms, history)
    util/
      config.c          - config file + command-line parsing
      log.c             - levelled logging

  client/
    main.c              - event loop, frame dispatch, command handling
    net/
      client_conn.c     - TCP connect, frame send/receive
    state/
      model.c           - rooms, DMs, users, messages (client model)
      history.c         - local input history helpers
    ui/
      layout.c          - ncurses window creation and resize
      render.c          - all pane rendering (topbar, rooms, chat, input)
      input.c           - keyboard input, sidebar navigation
    sound.c             - notification sound playback via memfd + aplay
    sounds_data.c       - auto-generated: WAV blobs as C arrays (do not edit)

  shared/
    proto/
      frame.c           - binary frame encode/decode
      tlv.c             - TLV field builder and reader
    net/
      transport_posix.c - plain TCP transport
      transport_tls.c   - OpenSSL TLS transport
      transport_tls_stub.c - no-op stub for non-TLS builds

include/               - public headers mirroring src/ layout
tests/                 - unit tests and smoke test script
scripts/
  gen_sounds.py        - embeds sounds/*.wav into sounds_data.c at build time
  seed_history.py      - populates DB with sample chat history
sounds/                - WAV source files (used at build time only)
```

## Install

```bash
sudo make install
```

Installs to:

- `/usr/local/bin/cupid-chatd`
- `/usr/local/bin/cupid-chat`

## Notes

- Sound playback requires `aplay` (ALSA). If it is absent, sounds are silently disabled - the client runs normally without them.
- The client binary is fully self-contained after build. The `sounds/` directory is not needed at runtime.
- The server allocates its connection table based on the process's `RLIMIT_NOFILE` (open file descriptor limit) rather than `--max-clients`, so file descriptor numbers never exceed the array bounds regardless of OS assignment.
- SQLite is opened in WAL mode with a 5-second busy timeout, so brief lock contention between the server and external tools does not cause crashes.
- The database file (`cupidchat.db`) is created automatically on first run if it does not exist.
