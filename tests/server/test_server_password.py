#!/usr/bin/env python3
"""Exercise the account gate over TCP, including failed registration side effects."""
from contextlib import contextmanager
import os
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
SERVER = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / 'cupid-chatd'
ENV = {k: v for k, v in os.environ.items() if k != 'CUPIDCHAT_SERVER_PASSWORD_FILE'}


def tlv(tag, value):
    return struct.pack('!HH', tag, len(value)) + value


def send(connection, kind, payload=b''):
    connection.sendall(struct.pack('!IHHI', len(payload), kind, 2, 1) + payload)


def exact(connection, count):
    data = b''
    while len(data) < count:
        chunk = connection.recv(count - len(data))
        assert chunk, 'server unexpectedly closed connection'
        data += chunk
    return data


def receive(connection):
    size, kind, _, _ = struct.unpack('!IHHI', exact(connection, 12))
    payload = exact(connection, size)
    fields = {}
    while payload:
        tag, length = struct.unpack('!HH', payload[:4])
        fields[tag] = payload[4:4 + length]
        payload = payload[4 + length:]
    return kind, fields


def expect(connection, kind):
    # Account flags, board status and buddy snapshots may follow successful auth.
    for _ in range(16):
        actual, fields = receive(connection)
        if actual == kind:
            return fields
        assert actual in (0x8060, 0x8070, 0x8040), hex(actual)
    raise AssertionError('missing response')


def auth(connection, kind, secret, account=b'account_check', password=b'account-password'):
    payload = tlv(0x10, account) + tlv(0x14, password)
    if secret is not None:
        payload += tlv(0x85, secret)
    send(connection, kind, payload)


def hello(port, locked):
    connection = socket.create_connection(('127.0.0.1', port), timeout=3)
    send(connection, 1, tlv(1, struct.pack('!H', 1)))
    fields = expect(connection, 0x8001)
    caps = int.from_bytes(fields[5], 'big')
    assert bool(caps & 0x100) == locked
    assert caps & 8, 'account requirement not advertised'
    return connection


@contextmanager
def running(directory, secret=None, use_env=False):
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    arguments = [str(SERVER), '--host', '127.0.0.1', '--port', str(port),
                 '--db', str(directory / 'chat.db'), '--verbose',
                 '--rate-burst', '100', '--rate-msgs', '100']
    env = dict(ENV)
    if secret is not None:
        path = directory / 'server-password'
        path.write_bytes(secret + b'\r\n')
        path.chmod(0o600)
        if use_env:
            env['CUPIDCHAT_SERVER_PASSWORD_FILE'] = str(path)
        else:
            arguments += ['--server-password-file', str(path)]
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(arguments, stdout=log, stderr=log, env=env)
        try:
            deadline = time.monotonic() + 8
            while True:
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                        break
                except OSError:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        log.seek(0)
                        raise AssertionError(log.read().decode())
                    time.sleep(0.02)
            yield port
        finally:
            process.terminate()
            process.wait(timeout=5)
            log.seek(0)
            output = log.read()
            assert process.returncode == 0, output
            assert b'AddressSanitizer' not in output and b'runtime error:' not in output, output
            assert b'account-password' not in output, 'account secret in verbose log'
            assert b'61 63 63 6f 75 6e 74 2d 70 61 73 73' not in output, 'auth payload dumped'
            if secret:
                assert secret not in output, 'server secret in verbose log'


def test_gate(directory, secret):
    with running(directory, secret) as port:
        with hello(port, True) as connection:
            for kind in (3, 4):
                for invalid in (None, b'', b'wrong', secret[:-1], secret + b'x',
                                secret + b'\0', b'x' * 256):
                    auth(connection, kind, invalid)
                    fields = expect(connection, 0x8002)
                    assert int.from_bytes(fields[0x40], 'big') == 13
            with sqlite3.connect(directory / 'chat.db') as database:
                assert database.execute('SELECT count(*) FROM users').fetchone()[0] == 0
            # Knowing the shared key alone cannot browse or send anything as a guest.
            for kind in (0x10, 0x12, 0x13, 0x20, 0x21, 0x42, 0x50, 0x71, 0x60):
                send(connection, kind)
                fields = expect(connection, 0x8002)
                assert int.from_bytes(fields[0x40], 'big') == 4
            auth(connection, 3, secret)
            expect(connection, 0x8003)
            send(connection, 0x20)
            expect(connection, 0x8020)
            send(connection, 5)
            expect(connection, 0x8005)
            auth(connection, 4, None)
            expect(connection, 0x8002)
            auth(connection, 4, secret, password=b'incorrect-account-password')
            fields = expect(connection, 0x8002)
            assert fields[0x41] == b'incorrect username or password'
            auth(connection, 4, secret)
            expect(connection, 0x8004)
    # Existing account is preserved, and environment configuration enforces the same gate.
    with running(directory, secret, use_env=True) as port:
        with hello(port, True) as connection:
            auth(connection, 4, None)
            expect(connection, 0x8002)
            auth(connection, 4, secret)
            expect(connection, 0x8004)
    # Removing the optional lock restores the ordinary account-only sign-in.
    with running(directory) as port:
        with hello(port, False) as connection:
            auth(connection, 4, None)
            expect(connection, 0x8004)


def test_bad_files(directory):
    path = directory / 'bad-password'
    for content in (b'', b'\n', b'a\0b', b'a\nb', b'a\rb', b'x' * 256, b'x' * 258):
        path.write_bytes(content)
        result = subprocess.run([str(SERVER), '--server-password-file', str(path)],
                                env=ENV, capture_output=True, timeout=5)
        assert result.returncode == 1, (content, result.stderr)
    path.unlink()
    for target in (path, directory):
        result = subprocess.run([str(SERVER), '--server-password-file', str(target)],
                                env=ENV, capture_output=True, timeout=5)
        assert result.returncode == 1
    os.mkfifo(path)
    result = subprocess.run([str(SERVER), '--server-password-file', str(path)],
                            env=ENV, capture_output=True, timeout=5)
    assert result.returncode == 1, 'FIFO should fail without blocking'


def main():
    with tempfile.TemporaryDirectory(prefix='cupidchat-password-') as temporary:
        directory = Path(temporary)
        test_gate(directory, b'shared-server-password')
        with sqlite3.connect(directory / 'chat.db') as database:
            database.execute('DELETE FROM users')
        test_gate(directory, b'x' * 255)
        test_bad_files(directory)
    print('PASS server password: registration, login, guest denial, restart, optional lock, '
          'invalid files, maximum length, and secret-free logs')


if __name__ == '__main__':
    main()
