#!/usr/bin/env python3
"""
seed_history.py — insert a large batch of realistic test messages
                  into the #general room so scroll/history can be tested.

Usage:
    python3 scripts/seed_history.py [--db cupidchat.db] [--count 300]
"""

import sqlite3
import time
import random
import argparse
import sys

# ---------------------------------------------------------------------------
# Realistic IRC-style message corpus
# ---------------------------------------------------------------------------
NICKS = ["alice", "bob", "charlie", "diana", "eduardo", "frank",
         "grace", "hank", "iris", "jake"]

MESSAGES = [
    "hey everyone!",
    "what's up?",
    "just got back from the store",
    "anyone seen the new build?",
    "the server log looks clean today",
    "did you try the /me command yet?",
    "lol that topic bar is actually pretty cool",
    "brb grabbing coffee",
    "ok back",
    "has anyone tested DMs with more than 2 people?",
    "the nick colors are a nice touch",
    "i keep forgetting PgUp to scroll lol",
    "is the scrollback working for you?",
    "try sending like 200 messages and see",
    "yeah the ring buffer handles it fine",
    "i hit 500 messages and it wrapped cleanly",
    "the timestamps look good",
    "does /topic persist across restarts?",
    "it does - it's in the DB",
    "sweet, that's proper IRC behavior",
    "i noticed the join/part messages look different",
    "yeah the *** prefix renders in yellow",
    "much better than the old magenta-for-everything",
    "who set the room topic last?",
    "pretty sure it was alice",
    "testing a slightly longer message here to see how it wraps on small terminals and whatnot",
    "yeah it truncates cleanly with waddnstr",
    "no overflow crashes at all",
    "asan is quiet which is a good sign",
    "the rate limiter is set to 10/s",
    "that's fine for normal chatting",
    "burst cap of 25 means you can paste a paragraph",
    "true, won't get rate-limited in practice",
    "room deletion worked fine for me",
    "only the owner can delete, right?",
    "yeah, owner_uid check on the server",
    "what happens if the owner disconnects?",
    "room stays, uid persists in the db",
    "makes sense - registered accounts would fix that properly",
    "alright back to testing - pressing PgUp now",
    "nice, scrolled back 28 lines in one hit",
    "the 'N earlier messages' indicator at top is helpful",
    "and the 'N more below' at bottom is clear",
    "pressing PgDn brings me back to the latest",
    "sending a message also snaps to bottom automatically",
    "/me tests the action message here",
    "looks great - italic magenta per-nick color",
    "or not italic on all terminals, but distinct",
    "ncurses A_ITALIC support is terminal-dependent",
    "good point, keeping it as just per-nick color is robust",
    "the djb2 hash is consistent - alice is always the same color",
    "yeah I tested it on two clients simultaneously",
    "both showed alice in cyan and bob in yellow",
    "that's the palette order",
    "7 colors cycling - enough for a busy room",
    "once you get 8+ people some share a color but that's expected",
    "true mirc only had 16 fixed colors anyway",
    "this feels very authentic tbh",
    "the topic bar really sells it",
    "Topic: Come chat and chill | set by alice",
    "lol nice topic",
    "does anyone remember mIRC's !seen command?",
    "haha yes the bots were the soul of IRC",
    "^this so much",
    "anyway this scrollback test is going well",
    "200+ messages loaded with no issues",
    "the ring buffer math checks out",
    "oldest_idx = (head - len + cap*2) % cap is correct",
    "yeah I traced through it manually",
    "no off-by-one on wrap",
    "the history replay from server on join also works",
    "server sends last 500 messages now, right?",
    "yeah bumped DEFAULT_HISTORY_SIZE from 50",
    "50 was way too small for any real conversation",
    "500 is still a subset of what's in the DB",
    "for a chat app that's fine - full log is server-side",
    "ok I think we've thoroughly tested this",
    "the feature is solid",
    "agreed - let's ship it",
    "++",
    "LGTM",
    "nice work everyone",
    "alright grabbing lunch",
    "enjoy!",
    "o/",
    "oh wait one more thing",
    "does scroll position persist when you switch rooms and come back?",
    "yes! it does, per-room scroll_offset on cli_room_t",
    "so if you scroll back in #general then go to #random, coming back to #general preserves position",
    "that's correct IRC behavior, nice",
    "ok NOW grabbing lunch",
    "lol go eat",
    "o/",
    "back - had a sandwich",
    "good sandwich?",
    "decent, a bit dry",
    "rip",
    "anyway - all tests green?",
    "asan clean, no warnings",
    "scrolling works great",
    "history replay works",
    "topic bar shows correctly",
    "per-nick colors are consistent",
    "system messages look distinct",
    "/me actions render properly",
    "DMs still work",
    "room deletion still works",
    "auto-refresh fires on join/leave events",
    "all good here",
    "ok let's move on to the next feature",
    "what's on the list?",
    "notification sounds, maybe?",
    "or unread badges in the sidebar",
    "the unread count is already there",
    "oh right the number next to room names",
    "yeah, resets when you open the room",
    "neat",
    "what about mentioning nicks with @alice ?",
    "highlight/beep would be useful",
    "definitely on the list",
    "ok for now this is a solid base",
    "agreed",
    "see you all tomorrow",
    "later o/",
]

# ---------------------------------------------------------------------------

def seed(db_path: str, room: str, count: int) -> None:
    con = sqlite3.connect(db_path)
    cur = con.cursor()

    # Ensure the room row exists (db_seed_defaults uses INSERT OR IGNORE so it
    # should already be there after the first server run).
    cur.execute(
        "INSERT OR IGNORE INTO rooms (name, topic, owner_uid, created_at) "
        "VALUES (?, '', 0, ?)",
        (room, int(time.time() * 1000))
    )

    # Space messages evenly over the past 24 hours.
    end_ts   = int(time.time() * 1000)
    start_ts = end_ts - 24 * 60 * 60 * 1000          # 24 h ago
    interval = (end_ts - start_ts) // max(count, 1)

    rng = random.Random(42)
    corpus = MESSAGES * (count // len(MESSAGES) + 1)
    rng.shuffle(corpus)

    rows = []
    for i in range(count):
        nick    = rng.choice(NICKS)
        text    = corpus[i]
        ts_ms   = start_ts + i * interval + rng.randint(0, interval // 2)
        user_id = NICKS.index(nick) + 100   # fake uid, non-zero
        rows.append((room, user_id, nick, text, ts_ms))

    cur.executemany(
        "INSERT INTO messages (room_name, user_id, nick, text, ts_ms) "
        "VALUES (?, ?, ?, ?, ?)",
        rows
    )
    con.commit()
    con.close()
    print(f"Inserted {count} messages into #{room} in {db_path}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Seed chat history for testing")
    ap.add_argument("--db",    default="cupidchat.db", help="SQLite database path")
    ap.add_argument("--room",  default="general",       help="Room name")
    ap.add_argument("--count", default=300, type=int,   help="Number of messages")
    args = ap.parse_args()

    seed(args.db, args.room, args.count)
