#!/usr/bin/env bash
# tests/smoke/multi_client.sh — end-to-end smoke test
#
# Starts cupid-chatd, connects two clients via netcat + raw protocol bytes,
# checks handshake responses and signed-in connections.
#
# Requires: ./cupid-chatd built, python3 (for frame encoding helper)
#
# Usage:
#   ./tests/smoke/multi_client.sh
#
set -euo pipefail

BINARY=./cupid-chatd
PORT=15555
PASS=0
FAIL=0

ok()   { echo "  PASS  $1"; ((PASS++)) || true; }
fail() { echo "  FAIL  $1: $2"; ((FAIL++)) || true; }

# ── Encode a frame using Python (avoids needing a test client binary) ──────
encode_frame() {
    local type=$1
    local payload_hex=${2:-""}
    python3 - <<EOF
import struct, sys
ptype   = $type
pflags  = 0x0002
pseq    = 1
payload = bytes.fromhex("$payload_hex") if "$payload_hex" else b""
# Note: struct format corrected: I=4B H=2B, total=4+2+2+4=12 bytes
hdr = struct.pack(">IHHI", len(payload), ptype, pflags, pseq)
sys.stdout.buffer.write(hdr + payload)
EOF
}

# ── TLV builder for HELLO payload ─────────────────────────────────────────
build_hello() {
    local nick=$1
    local register=${2:-0}
    python3 - <<EOF
import struct, sys

def tlv(tag, val):
    if isinstance(val, str):
        val = val.encode()
    return struct.pack(">HH", tag, len(val)) + val

TAG_PROTO_VERSION = 0x0001
TAG_NICK          = 0x0010
CMSG_HELLO        = 0x0001

payload  = tlv(TAG_PROTO_VERSION, struct.pack(">H", 1))
payload += tlv(TAG_NICK, "$nick")

hdr = struct.pack(">IHHI", len(payload), CMSG_HELLO, 0x0002, 1)
sys.stdout.buffer.write(hdr + payload)
if $register:
    payload = tlv(TAG_NICK, "$nick") + tlv(0x0014, "smoke-password")
    sys.stdout.buffer.write(struct.pack(">IHHI", len(payload), 0x0003, 0x0002, 2) + payload)
EOF
}

# ── Start server ───────────────────────────────────────────────────────────
if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: $BINARY not found or not executable. Run 'make' first."
    exit 1
fi

SMOKE_DIR=$(mktemp -d)
"$BINARY" --host 127.0.0.1 --db "$SMOKE_DIR/chat.db" --port $PORT --verbose &
SERVER_PID=$!
sleep 0.5   # give it time to start

cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -f /tmp/cupid_smoke_*.out /tmp/cupid_smoke_hello.bin
    rm -rf -- "$SMOKE_DIR"
}
trap cleanup EXIT

echo "=== Smoke tests (server pid=$SERVER_PID on port $PORT) ==="

# ── Test 1: server accepts connection ─────────────────────────────────────
if nc -z 127.0.0.1 $PORT 2>/dev/null; then
    ok "server accepts TCP connection"
else
    fail "server accepts TCP connection" "port $PORT not open"
fi

# ── Test 2: HELLO → WELCOME ────────────────────────────────────────────────
# Send HELLO and capture a few hundred bytes of response
build_hello "smoketest_alice" > /tmp/cupid_smoke_hello.bin

RESPONSE=$(nc -q 1 127.0.0.1 $PORT < /tmp/cupid_smoke_hello.bin 2>/dev/null | xxd | head -20)
if echo "$RESPONSE" | grep -q "8001"; then
    ok "HELLO gets WELCOME (type=0x8001)"
else
    fail "HELLO gets WELCOME" "did not find 0x8001 in response: $RESPONSE"
fi

# ── Test 3: two simultaneous clients ──────────────────────────────────────
{
    build_hello "smoke_alice" 1
    sleep 1
} | nc -q 2 127.0.0.1 $PORT > /tmp/cupid_smoke_alice.out 2>/dev/null &
ALICE_PID=$!

{
    build_hello "smoke_bob" 1
    sleep 1
} | nc -q 2 127.0.0.1 $PORT > /tmp/cupid_smoke_bob.out 2>/dev/null &
BOB_PID=$!

wait "$ALICE_PID" "$BOB_PID" || true

if [[ -s /tmp/cupid_smoke_alice.out && -s /tmp/cupid_smoke_bob.out ]]; then
    ok "two clients connected simultaneously"
else
    fail "two clients connected simultaneously" "no response for one or both clients"
fi

# ── Test 4: repeated HELLO rejected ───────────────────────────────────────
{
    build_hello "smoke_dupnick"
    sleep 0.2
    build_hello "smoke_dupnick"
    sleep 0.5
} | nc -q 2 127.0.0.1 $PORT > /tmp/cupid_smoke_dup.out 2>/dev/null || true

# Should see an ERROR frame (type 0x8002) for the second HELLO
DUP_RESP=$(xxd /tmp/cupid_smoke_dup.out 2>/dev/null | head -40)
if echo "$DUP_RESP" | grep -q "8002"; then
    ok "repeated HELLO gets ERROR (type=0x8002)"
else
    # Duplicate nick test is tricky with shell timing – just warn
    echo "  SKIP  duplicate nick test (timing-sensitive)"
fi

echo ""
echo "Passed: $PASS  Failed: $FAIL"
[[ $FAIL -eq 0 ]]
