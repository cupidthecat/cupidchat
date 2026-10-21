#!/usr/bin/env bash
# Install an already-built server inside a Debian/Ubuntu systemd guest.
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
if (( EUID != 0 )); then
    echo "Run as root after building: make SAN= cupid-chatd" >&2
    exit 1
fi
if command -v pveversion >/dev/null 2>&1; then
    echo "Install inside a Linux guest, not on the Proxmox hypervisor." >&2
    exit 1
fi
if [[ ! -x ./cupid-chatd ]]; then
    echo "Build the server first: make SAN= cupid-chatd" >&2
    exit 1
fi
if ! getent passwd cupidchat >/dev/null; then
    useradd --system --user-group --home-dir /var/lib/cupidchat \
        --shell /usr/sbin/nologin cupidchat
fi
install -d -o cupidchat -g cupidchat -m 0750 /var/lib/cupidchat
install -d -m 0755 /etc/cupidchat /usr/local/bin
if [[ ! -e /etc/cupidchat/server.env ]]; then
    printf '%s\n' 'CUPIDCHAT_HOST=127.0.0.1' 'CUPIDCHAT_PORT=5555' \
        > /etc/cupidchat/server.env
    chmod 0644 /etc/cupidchat/server.env
fi
install -m 0755 cupid-chatd /usr/local/bin/cupid-chatd.new
mv -f /usr/local/bin/cupid-chatd.new /usr/local/bin/cupid-chatd
install -m 0644 deploy/cupidchat.service /etc/systemd/system/cupidchat.service
systemctl daemon-reload
systemctl enable cupidchat.service
systemctl restart cupidchat.service
systemctl --no-pager status cupidchat.service
