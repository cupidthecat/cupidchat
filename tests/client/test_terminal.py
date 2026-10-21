#!/usr/bin/env python3
"""Exercise resize signals and mouse packets through a real terminal pair."""
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time


def main():
    root = Path(__file__).resolve().parents[2]
    client = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / 'cupid-chat'
    server = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else root / 'cupid-chatd'
    with tempfile.TemporaryDirectory(prefix='cupidchat-terminal-') as temporary:
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        with open(Path(temporary) / 'server.log', 'w+') as log:
            daemon = subprocess.Popen([str(server), '--host', '127.0.0.1', '--port', str(port),
                                       '--db', str(Path(temporary) / 'chat.db')],
                                      stdout=log, stderr=log)
            process = None
            master = None
            try:
                deadline = time.monotonic() + 8
                while True:
                    try:
                        with socket.create_connection(('127.0.0.1', port), timeout=0.2):
                            break
                    except OSError:
                        if daemon.poll() is not None or time.monotonic() > deadline:
                            log.seek(0)
                            raise AssertionError('server failed to start: ' + log.read())
                        time.sleep(0.05)
                master, slave = pty.openpty()
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
                process = subprocess.Popen([str(client), '--host', '127.0.0.1', '--port', str(port),
                                            '--nick', 'terminal_check'],
                                           stdin=slave, stdout=slave, stderr=slave,
                                           env=dict(os.environ, TERM='xterm-256color', LC_ALL='C.UTF-8'))
                os.close(slave)
                transcript = bytearray()

                def receive_until(expected):
                    start = len(transcript)
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        if expected in transcript[start:]:
                            return
                        if process.poll() is not None:
                            raise AssertionError(f'client exited {process.returncode}: {transcript[-2000:]!r}')
                        if select.select([master], [], [], 0.1)[0]:
                            transcript.extend(os.read(master, 65536))
                    raise AssertionError(f'terminal did not display {expected!r}: {transcript[-2000:]!r}')

                def drain_frame():
                    deadline = time.monotonic() + 2
                    while time.monotonic() < deadline and select.select([master], [], [], 0.2)[0]:
                        transcript.extend(os.read(master, 65536))

                receive_until(b'CupidChat sign in')
                os.write(master, b'\t\t\t\r')
                receive_until(b'Create a CupidChat account')
                os.write(master, b'\rterminal-password\rterminal-password\r')
                receive_until(b'Welcome to CupidChat')
                os.write(master, b'draft_survives_resize')
                receive_until(b'draft_survives_resize')
                fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 4, 12, 0, 0))
                os.kill(process.pid, signal.SIGWINCH)
                receive_until(b'Resize to at')
                fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
                os.kill(process.pid, signal.SIGWINCH)
                receive_until(b'draft_survives_resize')
                # SGR mouse coordinates are one-based; Quit is in the footer.
                os.write(master, b'\x1b[<0;27;23M\x1b[<0;27;23m')
                receive_until(b'Quit CupidChat?')
                drain_frame()
                os.write(master, b'\x1b')
                assert select.select([master], [], [], 5)[0], 'quit dialog did not cancel'
                drain_frame()
                assert process.poll() is None
                os.write(master, b'\x11')
                receive_until(b'Quit CupidChat?')
                os.write(master, b'\r')
                process.wait(timeout=5)
                assert process.returncode == 0
                assert b'AddressSanitizer' not in transcript and b'runtime error:' not in transcript
                print('PASS terminal: resize recovery, draft preservation, mouse quit, cancel, and confirm')
            finally:
                if process is not None and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)
                if master is not None:
                    os.close(master)
                daemon.terminate()
                daemon.wait(timeout=5)


if __name__ == '__main__':
    main()
