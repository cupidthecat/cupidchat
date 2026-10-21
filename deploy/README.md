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
Windows users can run the Linux client through WSL.

## Data and operations

The database is `/var/lib/cupidchat/cupidchat.db`. Include this guest in
your normal Proxmox backup schedule. For a database-only live backup, use
SQLite's `.backup` command so the WAL contents are included; copying just
the live `.db` file can miss recent messages.

```bash
sudo systemctl status cupidchat
sudo systemctl restart cupidchat
sudo journalctl -u cupidchat -f
```
