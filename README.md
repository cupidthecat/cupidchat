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
- Desktop audio via `paplay` or `pw-play`, with `aplay` fallback and bounded background playback

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
- `paplay` (PulseAudio/WSLg), `pw-play` (PipeWire), or `aplay` (ALSA) for notifications

### Ubuntu/Debian

```bash
sudo apt update
sudo apt install -y build-essential libsqlite3-dev libncurses-dev libssl-dev python3 pulseaudio-utils alsa-utils
```

### Arch Linux

```bash
sudo pacman -S base-devel sqlite ncurses openssl python libpulse alsa-utils
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

Builds include AddressSanitizer and UBSan (`-fsanitize=address,undefined`) by default. Pass `SAN=` for a build without sanitizers.

## Quick start

For a persistent Linux service on a Proxmox guest and client access over
Tailscale, see [server deployment](deploy/README.md).

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
| `/msg <nick> <message>` | IRC alias for `/dm` |
| `/query <nick>` | Open a DM with an online user without sending a message |
| `/reply <message>` or `/r <message>` | Reply to the last incoming DM |
| `/unread` | Jump to the next conversation with unread messages |
| `/clear` | Clear this conversation's local buffer; server history remains |
| `/closedm <nick>` | Remove a DM from the sidebar |
| `/close` | Close the active DM or leave the active room |
| `/latest` | Return the active chat to its newest messages |
| `/emoji` | Open the emoji picker; arrows choose, Enter inserts, Esc closes |
| `/buddies` | Open your account's buddy list |
| `/buddy add <account>` | Watch an existing account's online and away status |
| `/buddy remove <account>` | Remove an account from your buddy list |

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

Rooms created while logged in belong to that account across reconnects and
server restarts. Logging out removes your account's deletion permission until
you log in again. Renaming keeps your account identity. Each account can have
one active login.

A guest owns a room only for the connection that created it. The room, topic,
and history remain after the guest disconnects, but ordinary clients cannot
delete it. Logging in does not adopt rooms created earlier as a guest.

Rooms saved before account ownership was introduced become unowned on upgrade.
Their old connection IDs cannot reliably identify an account. Operators can
assign those rooms after checking ownership; see [deployment](deploy/README.md).

**Other**

| Command | Description |
|---------|-------------|
| `/quit` or `/q` | Exit the client |
| `/help` | Open the command help modal |
| `/sound on\|off\|test\|status` | Control notification audio |

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| `Enter` | Send message; if input is empty, confirm sidebar selection |
| `Tab` / `Shift+Tab` | Navigate the sidebar when input is empty; Tab completes nicknames while typing |
| `Up` / `Down` | Sidebar navigation (when input empty) or command history (when typing) |
| `PgUp` / `PgDn` | Scroll chat pane |
| `Ctrl+A` / `Home` | Move cursor to start of input |
| `Ctrl+E` / `End` | Move cursor to end of input |
| `Ctrl+K` | Delete from cursor to end of line |
| `Left` / `Right` | Move input cursor |
| `Delete` | Delete character under cursor |
| `Esc` | Dismiss help modal |
| `F1` | Open help; Up/Down scroll it |
| `F2` | Open the emoji picker |
| `F3` / `Ctrl+W` | Close the active chat |
| `F4` | Return to the newest messages |
| `F5` | Jump to the next unread chat |
| `F6` | Open or refresh the buddy list; Enter opens a DM with the selected online buddy |
| `F10` / `Ctrl+Q` | Open quit confirmation; Enter quits, Esc cancels |
| `Ctrl+L` | Redraw the terminal |

Click a chat in the sidebar to select it. The mouse wheel scrolls the chat
under the pointer. The footer has Close, Help, Latest, and Quit buttons;
clicking the input line moves the cursor. Mouse support depends on your
terminal. Hold Shift while dragging if your terminal uses that to select text.

Each room and DM retains up to 500 messages during the session. Page Up,
Page Down, and the mouse wheel move through wrapped lines. New messages
keep your place while you read older history. F4 or `/latest` returns to
live chat. This buffer is separate from the history replay sent by the server.

Unread counts appear before chat names in the sidebar. F5 or `/unread` visits
the next unread room or DM. `/query <nick>` opens a conversation without sending
an initial message; `/reply <message>` answers the last person who sent you a DM.

IRC aliases include `/msg`, `/part`, `/list`, `/names`, `/j`, and `/r`. You can
join a channel as `/join #general` or `/join general`. Tab completes a unique
nickname from the current room or DM; ambiguous matches extend to their common
prefix. Login and registration commands are excluded from input history and
nickname completion.

The user sidebar hides below 100 columns; the chat sidebar hides below 60.
Below 40 columns or 8 rows, a resize notice replaces the panes. Your draft,
selected chat, and history remain available when the terminal grows again.

### Buddy list

Log in or register before using buddies. Your list is stored with your account
on the server and survives reconnects, nickname changes, and server restarts.
Use `/buddy add <account>` to add an existing account, then F6 or `/buddies` to
see who is online, away, or offline. The list holds up to 64 accounts. Arrows
select a buddy, Enter opens a DM, R refreshes, and Esc closes the window.

The list follows account names, not guest nicknames. A buddy appears online
while logged in; their current display nickname appears beside the account if
it differs. Watching is one-way: adding a buddy does not add you to their list.
Your list is sent only to your own connection. Online and offline transitions
play separate tones; initial list loading and away updates stay quiet.

Messages sent from an existing DM or `/reply` include its peer's session ID.
The server rejects a message if someone else has taken that nickname. Reopen
the DM after a buddy reconnects. Explicit `/dm <nick> <message>` still addresses
the current holder of that nickname.

### Emoji and Unicode

Use a UTF-8 locale, such as `C.UTF-8`, and a terminal font with emoji glyphs.
You can paste emoji, type them with your OS picker, or use F2, `/emoji`, or the
Emoji footer button. Shortcodes such as `:smile:`, `:heart:`, `:thumbsup:`,
`:party:`, and `:rocket:` expand in room messages, actions, and DMs.

The client links against wide ncurses. Editing moves through UTF-8 characters
without splitting their bytes; scrolling and wrapping use terminal cell widths.
Combined emoji sequences, variation selectors, and color rendering depend on
the terminal and font. Editing operates on code points, so a combined sequence
may need more than one Backspace.

## Tests

Run unit tests:

```bash
make check
```

Test suite:

- `tests/proto/test_frame.c` - frame encode/decode
- `tests/proto/test_tlv.c` - TLV read/write
- `tests/server/test_rate_limit.c` - token-bucket rate limiter
- `tests/client/test_ui.c` - layout, scrollback, conversation selection, mouse controls, and resize recovery
- `tests/client/test_terminal.py` - resize signals and mouse input through a terminal pair

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
    sound.c             - notification sound playback via memfd and desktop audio
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

- Use `/sound status` to check audio, `/sound test` for a preview, and `/sound on` or `/sound off` to control notifications.
- The client binary is fully self-contained after build. The `sounds/` directory is not needed at runtime.
- The server allocates its connection table based on the process's `RLIMIT_NOFILE` (open file descriptor limit) rather than `--max-clients`, so file descriptor numbers never exceed the array bounds regardless of OS assignment.
- SQLite is opened in WAL mode with a 5-second busy timeout, so brief lock contention between the server and external tools does not cause crashes.
- The database file (`cupidchat.db`) is created automatically on first run if it does not exist.

## WSL audio

Install `pulseaudio-utils` in the WSL distribution that runs the client. WSLg
normally provides `PULSE_SERVER=unix:/mnt/wslg/PulseServer`; CupidChat also looks
for that socket when the variable is absent. A PulseAudio daemon inside the
distribution is not required for WSLg playback.

Run `./cupid-chat --sound-test` to play the embedded welcome sound without
connecting. It prints the audio server, attempted player paths, and player errors.
WSL installations without WSLg need a configured audio server.

### Playback finishes but nothing is audible

`paplay` can complete successfully while Windows routes WSLg audio to the wrong
device. The Windows session can appear as **Microsoft Remote Desktop** or
**RemoteApp Audio**. Its output can differ from the system's default speakers.

1. Run `./cupid-chat --sound-test` so Windows can show the audio session.
2. Open **Windows Settings > System > Sound > Volume mixer**.
3. Find the Remote Desktop audio session, unmute it, and select the speakers or
   headset you are using. Check the device assigned to that application, even
   if other Windows applications already play through the correct speakers.
4. Run the sound diagnostic again and confirm that you can hear it. A successful
   player exit alone does not confirm audible output.

If the session stays on the previous device, save work in Linux GUI applications
before reconnecting WSLg's Remote Desktop bridge. From Windows PowerShell:

```powershell
Get-Process msrdc -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq "$env:ProgramFiles\WSL\msrdc.exe" } |
    Stop-Process
```

WSLg restarts its bridge and opens a new audio session. This disconnects WSLg
GUI windows; terminal and background Linux processes continue running. Run
`./cupid-chat --sound-test` again after the bridge reconnects.

The output-device setting belongs to Windows on each client PC. Updating the
chat server or pulling this repository does not change that setting.

The client tries `paplay`, `pw-play`, then `aplay`. Playback workers are limited
to four at once, with a 200 ms notification interval. Each player has an eight
second timeout. `/sound off` mutes future notifications for this session.

The first incoming DM uses the mail sound; later replies use the IM sound.
Mentions, server errors, and topic changes have separate short tones generated
by `scripts/gen_notification_sounds.py`. History replay and ignored messages do
not trigger mention sounds.
