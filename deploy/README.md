# Linux server deployment

Use a dedicated Debian or Ubuntu guest on Proxmox. An unprivileged Debian
container with one CPU, 512 MiB RAM, and an 8 GiB disk is sufficient for a
small chat server. Enable startup at boot for the guest.

## Build and install

The restored `include/` directory is required for both client and server.
Several protocol constants were absent from the uploaded source and have
been assigned explicitly in `include/proto/types.h`. Rebuild every client
from this source; compatibility with previously built clients is unknown.

Inside the Linux guest:

```bash
sudo apt-get update
sudo apt-get install -y build-essential libsqlite3-dev libcrypt-dev
make SAN= cupid-chatd
sudo bash scripts/install-server.sh
```

`SAN=` omits the development sanitizers. Use a fresh build directory when
switching build options; make does not track command-line flag changes.
The installer creates an unprivileged `cupidchat` service account, installs
the server, and enables automatic startup and restart after failure.

Configuration: `/etc/cupidchat/server.env`:

```ini
CUPIDCHAT_HOST=127.0.0.1
CUPIDCHAT_PORT=5555
```

The initial bind address only accepts local connections. To receive a TCP
proxy from the Proxmox host, set `CUPIDCHAT_HOST` to the guest's private IPv4
address. Use a fixed guest IP or a DHCP reservation so the proxy target
does not change. Restart after editing:

```bash
sudo systemctl restart cupidchat
sudo journalctl -u cupidchat -n 50 --no-pager
```

## Tailscale connection

Either install Tailscale inside the guest and bind to its Tailscale IPv4
address, or forward a port on the Proxmox host's Tailscale IPv4 address to
the private guest address. Keep the TCP listener limited to the intended
interface. CupidChat uses a binary TCP protocol, so an HTTP reverse proxy
or the Proxmox web URL cannot serve as the chat endpoint.

Clients must be connected to Tailscale and allowed by the tailnet's access
policy. They connect with:

```bash
./cupid-chat --host SERVER_TAILSCALE_IPV4 --port 5555 --nick alice
```

For clients, install `libncurses-dev` and `python3` as well, then run
`make SAN= cupid-chat`. Notification sounds optionally use `alsa-utils`.
Windows users can run the Linux client through WSL. WSLg audio uses `paplay`
from `pulseaudio-utils`; `/sound status` reports audio availability.

Update the server with the client for `/query` and room-scoped user lists.
User-list responses include the room name so a delayed response cannot replace
the sidebar for a different room. Older clients ignore the extra field.

## Data and operations

### Accounts and profiles

Chat requires an account. Upgrade clients along with the server to get the
sign-in/create-account screen and profile editor. Older clients can sign in
with `/login` or `/register`, but cannot browse rooms or chat before doing so.
There is no guest mode. Existing accounts and passwords remain valid.

Startup creates `user_profiles` if it is missing. Its optional public fields
reference `users.id`; existing users get an empty profile until they save one.
The upgrade preserves rooms, messages, topics, accounts, and buddy lists.
Unfinished sign-ins close after five minutes. Verbose frame logs omit login
and registration payloads.

### First admins

After the upgraded service has initialized its database, stop it and make a
SQLite backup. Grant access only to account names you have verified:

```bash
sudo systemctl stop cupidchat
sudo python3 scripts/grant-admin.py /var/lib/cupidchat/cupidchat.db verified_account another_verified_account
sudo systemctl start cupidchat
```

The helper requires an existing database and existing accounts. All names are
checked before any role is changed. It preserves mute and ban settings and
records the grants. Sign in again to load the admin badge. Admins can use
`/admin grant` and `/admin revoke` for later changes; the last admin is protected.

The schema adds `account_controls`, `server_settings`, `admin_actions`, `polls`,
`poll_options`, and `poll_votes`. Include them in database backups. Bans, mutes,
roles, MOTD, banners, poll options, and votes survive restarts. Individual vote
choices remain visible to server operators through the database.

### BBS storage

Startup adds `bbs_threads`, `bbs_posts`, and `bbs_reads`. Include all three in
SQLite backups along with the existing tables. Thread and post authors refer
to persistent accounts, and read markers are saved per account and thread.
Board upgrades preserve existing rooms, messages, profiles, polls, and roles.

Clients advertise board support through the server capability response. Use
an updated client for F9 and `/board`; older clients can still use live chat.
No additional listener, port, or service is needed.

### Ownership migration

Back up the database with SQLite's backup API before upgrading. The schema adds
`rooms.owner_account_id`, which references `users.id`. Existing `owner_uid`
values remain in the database for review but do not grant permissions. Existing
rooms, topics, and messages remain available. Guest access is disabled; account
ownership persists.

To assign a legacy room after confirming its owner, stop the service and use a
SQLite transaction. Substitute the verified account and room names below:

```sql
BEGIN IMMEDIATE;
UPDATE rooms
SET owner_account_id = (SELECT id FROM users WHERE username = 'verified_account' COLLATE NOCASE)
WHERE name = 'verified_room' AND owner_account_id IS NULL;
SELECT name, owner_uid, owner_account_id FROM rooms WHERE name = 'verified_room';
COMMIT;
```

Check the result before committing; use `ROLLBACK` if it is wrong. Restart the
service to reload the assignment. Do not map an old `owner_uid` directly to an
account ID: the two number spaces are unrelated.

The database is `/var/lib/cupidchat/cupidchat.db`. Include this guest in
your normal Proxmox backup schedule. For a database-only live backup, use
SQLite's `.backup` command so the WAL contents are included; copying just
the live `.db` file can miss recent messages.

```bash
sudo systemctl status cupidchat
sudo systemctl restart cupidchat
sudo journalctl -u cupidchat -f
```
