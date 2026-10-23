# CupidChat

Terminal-first chat system written in C11. A lightweight server (`cupid-chatd`) and a self-contained ncurses client (`cupid-chat`) that run on Linux.

## Features

**Server**

- `epoll`-based event loop with `timerfd` keepalive
- Batched TCP output and connection scans that visit connected clients only
- Persistent room operators, account bans, topic locks, and room invitations
- Persistent BBS threads and replies, unread markers, and clickable author profiles
- Persistent account moderation, admin roles, editable MOTD and banners, and server polls
- Required account sign-in; guest connections cannot access chat
- SQLite WAL persistence (rooms, users, profiles, buddies, per-room history, topics)
- Token-bucket rate limiting per connection
- Per-connection output backpressure with priority frame dropping
- Keepalive ping/pong with configurable timeout
- TLS listener (build-time opt-in via `TLS=1`)

**Client**

- ncurses TUI: room sidebar, topic bar, chat pane, status bar, user list
- Rooms and direct messages (DMs) with separate scroll state per conversation
- Keyboard input wakes the client immediately; draft edits redraw only the input line
- Cached message wrapping for scrolling through retained history
- Nonblocking outgoing queue capped at 256 KiB, with partial-write handling
- Clickable account profiles and a public profile editor (F7 or `/profile`)
- Password-masked sign-in and account creation forms
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

For everyday use, build both programs without sanitizers in a separate object directory:

```bash
make SAN= BUILDDIR=build-release all
```

The separate directory avoids mixing objects compiled with different sanitizer settings.
Rebuild the client as well as the server after an update to receive client performance changes.

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

Sign in or choose **Create account** on the first screen. `--nick` prefills the
account name; it does not sign you in. Tab moves between fields and buttons,
Enter continues, and passwords are masked. After signing in, the client joins
`#general`. Type a message and press Enter to send.

Accounts are required for room lists, history, messages, and profiles. Existing
accounts keep their passwords and saved data. Use the current client for the
sign-in form; an older client must send `/login` or `/register` before chatting.

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
--nick NICK          Initial account name (optional)
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

Each account can join up to 64 rooms at once. Leave one before joining another
when the limit is reached. Join requests use the connection rate limit.

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
| `/logout` | Leave rooms, clear local chats and drafts, and return to sign-in |
| `/profile [nick or @account]` | View a profile; without a name, view and edit your own |
| `/whois <nick>` | Look up an online nickname or account |

Rooms created while logged in belong to that account across reconnects and
server restarts. Logging out removes your account's deletion permission until
you log in again. Renaming keeps your account identity. Each account can have
one active login.

Guest access is disabled. Old guest-created rooms and their history remain
available after sign-in, but they do not acquire an owner automatically.
Account names are reserved: `/nick` cannot take another account's name.

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
| `F6` | Open or refresh buddies; Enter opens a DM, P opens the selected profile |
| `F7` | Open your profile |
| `F8` | Open the latest server poll |
| `F9` | Open the BBS board |
| `F11` | Open controls for the active room |
| `F10` / `Ctrl+Q` | Open quit confirmation; Enter quits, Esc cancels |
| `Ctrl+L` | Redraw the terminal |

Click a chat in the sidebar to select it. The mouse wheel scrolls the chat
under the pointer. The footer has Close, Help, Latest, and Quit buttons;
clicking the input line moves the cursor. Click a user, a live message name,
a buddy, or the DM header to view that account's profile. The top bar and
Profile footer button open your own profile. The user list also scrolls with
the mouse wheel. The most recent status stays on the line above the footer.
Mouse support depends on your
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
select a buddy, Enter opens a DM, P or a click opens their profile (including
offline buddies), R refreshes, and Esc closes the window.

The list follows account names across nickname changes. A buddy appears online
while logged in; their current display nickname appears beside the account if
it differs. Watching is one-way: adding a buddy does not add you to their list.
Your list is sent only to your own connection. Online and offline transitions
play separate tones; initial list loading and away updates stay quiet.

Messages sent from an existing DM or `/reply` include its peer's session ID.
The server rejects a message if someone else has taken that nickname. Reopen
the DM after a buddy reconnects. Explicit `/dm <nick> <message>` still addresses
the current holder of that nickname.

### IRC room tools

F11, `/room`, the Room footer button, or a click on the room's topic bar opens
its controls. `/roominfo <room>` opens another room's controls. Account and
Reason fields keep their text while you view a profile. Tab changes focus;
Left/Right on the action selector chooses an available action. Run or Enter
submits it. Refresh updates the access list, and Esc closes the window.
Click an account in the list, choose Profile, or use F7 to view its profile.
The window shows up to 64 access entries; commands can target any account by name.

| Command | Action |
|---------|--------|
| `/op <account>` | Give the account operator rights in this room |
| `/deop <account>` | Remove assigned operator rights |
| `/mode +t` | Let only operators, the owner, and server admins change the topic |
| `/mode -t` | Let all room members change the topic |
| `/kick <account> [reason]` | Remove an online member; it can join again |
| `/ban <account> [reason]` | Save a room ban and remove the account if present |
| `/unban <account>` | Remove the room ban |
| `/invite <account> [room]` | Invite an account to the current or named room |
| `/decline [room]` | Clear a pending invitation for the current or named room |

Room owners and server admins grant or remove operators and set topic locks.
Operators can invite accounts, kick ordinary members, and ban or unban them.
A room operator cannot kick or ban another operator; the owner or a server
admin can. Room owners and server admins retain their rights and are protected
from room kicks and bans. Moderation targets persistent account names, so a
nickname change or reconnect does not bypass a ban or lose operator rights.
Room bans affect that room; the account can continue chatting elsewhere.

Rooms stay in the public directory. Invitations point accounts to a room, and
room bans still apply. A new online invitation plays its own tone. Offline
accounts receive a quiet reminder when they sign in. Pending invitations stay
until the account joins or declines. An account can have up to 64 pending
invitations; repeating a pending invitation has a 30-second cooldown. Muted
accounts cannot send invitations.

On another account's profile, Invite sends an invitation to your active joined
room when you have permission. The card shows that room's name. The sidebar
uses `!` for invited rooms, `@` for rooms where you are an operator, and `X` for
rooms you cannot join because of a ban. Your owned rooms keep the `~` marker.

Operator rights, room bans, ban reasons, and topic locks survive server
restarts. Ordinary accounts can see the operator list; managers can also see
bans, reasons, and pending invitations. Selecting a banned account loads its
saved reason into the Reason field. Reasons allow 256 UTF-8 bytes. Topics allow
255 UTF-8 bytes; malformed or oversized room targets and topics are rejected.

### BBS board

F9, `/board`, `/bbs`, or the BBS footer button opens the board. It requires an
account, and posts stay on the server after everyone disconnects. The top bar
shows how many threads have unread posts. `*` marks unread threads, `P` marks
pinned threads, and `L` marks locked threads. Each row also shows its post count.
Pinned threads appear first; other threads follow their most recent post.

Use arrows to select a thread, then Enter or a click to open it. Left/Right or
Prev/Next switches pages of 20 threads. In a thread, Up/Down selects a post,
PgUp/PgDn or the mouse wheel scrolls, and Older/Newer loads another page of 20
posts. Threads open at their most recent page. Opening a page marks posts read
through the last post on that page; older pages do not move your read marker
backward. Read markers belong to your account and survive reconnects.

N starts a thread. A or Reply answers the open thread. Enter moves from the
subject to the body and then to Post; Tab moves between fields and buttons.
Ctrl+S posts directly. Subjects allow 128 UTF-8 bytes and message bodies allow
2048. Bodies are single paragraphs that wrap in the editor and reader.
Left/Right, Home/End, Backspace/Delete, Ctrl+U, and Ctrl+K edit text without
splitting UTF-8 characters. Wait five seconds between posts. If saving fails,
your draft stays available. Esc or Cancel asks before discarding it.

Click a post's author, use U, or choose Author to open their account profile,
including offline authors. Esc closes the profile and returns to the board.
F7 also opens your own profile from the composer. The board and profiles
preserve your live-chat draft. Closing the board keeps its current view.

D or Remove asks before clearing a post's body. Authors can remove their own
posts; admins can remove any post. A placeholder remains with the author and
date, and the thread subject stays available so replies keep their context.
Admins can press P to pin or unpin a thread and L to lock or unlock replies.
Locked threads remain readable. Muted accounts can read and remove their own
posts but cannot create threads or replies.

A separate tone announces a new post from another account. `/sound off` mutes
it. Notifications do not switch chats or move the board while you are reading;
R or Refresh loads updates. Initial board loading, removals, pins, and locks
stay quiet. Saving a post uses the existing save chime.

### Server messages, polls, and admins

`/motd` and `/banner` show the current server messages. A banner stays in the
topic bar until an admin clears it. MOTD updates reach connected users and are
shown to new sign-ins. Banners, MOTD changes, and poll openings and closings play a short notice
tone; `/sound off` mutes it with the other notifications.

F8 or `/poll` opens the latest server poll. Arrows or 1-4 select a choice;
Enter or Vote submits it. R refreshes, PgUp/PgDn reads wrapped question lines,
and Esc or Close returns to chat. Opening or refreshing a poll also puts its
full question and choices in scrollback. `/vote <1-4>` submits a choice from
the command line. Each account has one vote and can change it while the poll
is open. Closed polls retain their totals. Only one poll can be open at a time.
Votes survive reconnects and server restarts. Other clients receive counts;
they do not receive individual voters' choices.

Admins have a badge in the top bar and on their profile. `/admin help` lists
the commands. Account names in moderation commands refer to persistent
accounts, including users who have changed their nickname.

| Command | Action |
|---------|--------|
| `/admin kick <account> [reason]` | Disconnect an online account; it may reconnect |
| `/admin ban <account> [reason]` | Disconnect the account and block future logins |
| `/admin unban <account> [reason]` | Allow the account to log in again |
| `/admin mute <account> [reason]` | Block room messages, DMs, topics, typing, and votes |
| `/admin unmute <account> [reason]` | Restore those actions |
| `/admin grant <account>` | Give an existing account admin access |
| `/admin revoke <account>` | Remove admin access; the last admin cannot be removed |
| `/admin banner <text>` | Save and broadcast a banner, up to 512 UTF-8 bytes |
| `/admin banner clear` | Remove the banner |
| `/admin motd <text>` | Save the MOTD, up to 1800 UTF-8 bytes; use `|` between lines |
| `/admin motd clear` | Empty the MOTD |
| `/admin poll Question \| Choice one \| Choice two [\| more]` | Create a poll with 2-4 choices |
| `/admin pollclose` | Close the current poll and retain its results |

Revoke an admin's role before kicking, muting, or banning that account.
Moderation reasons are limited to 256 UTF-8 bytes. Account bans do not prevent
someone from registering another account. Moderation and role changes are
recorded in the server database. Server operators can appoint the first admin
using the [deployment instructions](deploy/README.md).

### Public profiles

F7 or `/profile` opens your account card. Choose Edit to set a display name,
status, bio, pronouns, location, and website. Every field is optional and visible
to signed-in users. The account name stays visible beside the display name.
Profiles also show the join date, current nickname, and online or away status.

In the editor, Tab moves between fields and Save/Cancel. Left/Right, Home/End,
Backspace/Delete, Ctrl+U, and Ctrl+K edit text without splitting UTF-8 characters.
Long fields scroll horizontally; smaller terminals show the focused field.
Limits are 64 bytes for display name, 128 for status, 512 for bio, 32 for
pronouns, 64 for location, and 160 for website. Save waits for server confirmation
and plays a short chime. Cancel asks before discarding changes.

Use `/profile nick`, `/whois nick`, or `/profile @account` to look someone up.
The account form also works while they are offline. A card offers Message,
Add buddy/Unbuddy, Refresh, and Close. Tab selects buttons and Enter activates
them; arrows and the mouse wheel scroll the card. Opening a profile preserves
your chat draft. Website fields display as text.

Historical messages retain old connection IDs, so their names do not open a
current user's card. Use an account name or the current user list instead.

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
