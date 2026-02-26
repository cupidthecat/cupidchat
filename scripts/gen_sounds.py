#!/usr/bin/env python3
"""
scripts/gen_sounds.py — embed WAV files as C byte arrays at build time.

Usage:
    gen_sounds.py <sounds_dir> <out_c> <out_h>

Reads each WAV file listed in SOUNDS, emits hex literals into <out_c> and
extern declarations into <out_h>.  Both files are fully overwritten.
"""

import os
import sys

# Ordered so that the array index matches sound_id_t from include/client/sound.h:
#   SND_WELCOME=0, SND_GOODBYE=1, SND_DM=2, SND_IM=3,
#   SND_BUDDY_IN=4, SND_BUDDY_OUT=5
SOUNDS = [
    ("Welcome.wav",         "welcome"),
    ("Goodbye.wav",         "goodbye"),
    ("You've Got Mail.wav", "dm"),
    ("IM.wav",              "im"),
    ("BuddyIn.wav",         "buddy_in"),
    ("BuddyOut.wav",        "buddy_out"),
]

HEADER_BANNER = """\
/*
 * include/client/sounds_data.h — auto-generated; do not edit.
 *
 * WAV audio blobs embedded at build time by scripts/gen_sounds.py.
 * Regenerated whenever a WAV file in sounds/ changes.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

"""

SOURCE_BANNER = """\
/*
 * src/client/sounds_data.c — auto-generated; do not edit.
 *
 * WAV audio blobs embedded at build time by scripts/gen_sounds.py.
 */
#include "client/sounds_data.h"
"""


def main() -> None:
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <sounds_dir> <out_c> <out_h>",
              file=sys.stderr)
        sys.exit(1)

    sounds_dir = sys.argv[1]
    out_c      = sys.argv[2]
    out_h      = sys.argv[3]

    # ── Validate all inputs before writing any output ──────────────────────
    blobs: list[tuple[str, str, bytes]] = []
    for filename, name in SOUNDS:
        path = os.path.join(sounds_dir, filename)
        try:
            data = open(path, "rb").read()
        except OSError as e:
            print(f"gen_sounds.py: cannot read {path}: {e}", file=sys.stderr)
            sys.exit(1)
        blobs.append((filename, name, data))

    # ── Header ─────────────────────────────────────────────────────────────
    with open(out_h, "w") as fh:
        fh.write(HEADER_BANNER)
        for _, name, data in blobs:
            # Use #define for sizes so they are integer constant expressions
            # and can be used in static struct initialisers in sound.c.
            fh.write(f"/* {name} — {len(data)} bytes */\n")
            fh.write(f"extern const uint8_t snd_{name}_data[];\n")
            fh.write(f"#define              snd_{name}_size   {len(data)}U\n\n")

    # ── Source ─────────────────────────────────────────────────────────────
    with open(out_c, "w") as fc:
        fc.write(SOURCE_BANNER)
        for filename, name, data in blobs:
            fc.write(f"\n/* ── {filename} ({len(data)} bytes) ── */\n")
            fc.write(f"const uint8_t snd_{name}_data[] = {{\n")
            for i in range(0, len(data), 16):
                chunk = data[i:i + 16]
                fc.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
            fc.write("};\n")
            # snd_{name}_size is a #define in the header — no variable needed

    total = sum(len(d) for _, _, d in blobs)
    print(f"gen_sounds.py: wrote {len(blobs)} sounds "
          f"({total} bytes raw) → {out_c}, {out_h}")


if __name__ == "__main__":
    main()
