#!/usr/bin/env python3
"""Grant admin access to existing accounts while the chat service is stopped."""
import argparse
from pathlib import Path
import sqlite3
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('database', type=Path)
    parser.add_argument('accounts', nargs='+')
    args = parser.parse_args()
    connection = sqlite3.connect(args.database.resolve().as_uri() + '?mode=rw', uri=True)
    try:
        connection.execute('BEGIN IMMEDIATE')
        accounts = []
        for name in args.accounts:
            row = connection.execute(
                'SELECT id, username FROM users WHERE username=? COLLATE NOCASE', (name,)
            ).fetchone()
            if row is None:
                raise ValueError(f'Account does not exist: {name}')
            accounts.append(row)
        for account, name in accounts:
            connection.execute(
                'INSERT INTO account_controls(user_id,admin) VALUES(?,1) '
                'ON CONFLICT(user_id) DO UPDATE SET admin=1', (account,)
            )
            connection.execute(
                'INSERT INTO admin_actions(actor_id,target_id,action,reason,created_at) '
                'VALUES(0,?,?,?,?)', (account, 'grant', 'Local server operator', int(time.time()))
            )
        connection.commit()
        print('Admins:', ', '.join(name for _, name in accounts))
    except Exception:
        connection.rollback()
        raise
    finally:
        connection.close()


if __name__ == '__main__':
    main()
