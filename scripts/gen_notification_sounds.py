#!/usr/bin/env python3
"""Generate CupidChat's short notification tones using only the Python standard library."""

import math
from pathlib import Path
import struct
import wave


def write_tone(path: Path, notes: list[tuple[float, float]]) -> None:
    rate = 22050
    samples = bytearray()
    for frequency, seconds in notes:
        count = round(rate * seconds)
        for index in range(count):
            # Fade each note in and out to avoid clicks.
            envelope = min(1.0, index / (rate * 0.01), (count - 1 - index) / (rate * 0.02))
            value = round(6000 * envelope * math.sin(2 * math.pi * frequency * index / rate))
            samples.extend(struct.pack("<h", value))
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(samples)


def main() -> None:
    sounds = Path(__file__).resolve().parent.parent / "sounds"
    write_tone(sounds / "Mention.wav", [(880, 0.09), (1174.66, 0.14)])
    write_tone(sounds / "Error.wav", [(330, 0.10), (220, 0.13)])
    write_tone(sounds / "Topic.wav", [(659.25, 0.07), (783.99, 0.10)])
    write_tone(sounds / "BuddyOnline.wav", [(523.25, 0.07), (659.25, 0.07), (783.99, 0.12)])
    write_tone(sounds / "ProfileSaved.wav", [(659.25, 0.06), (987.77, 0.12)])
    write_tone(sounds / "BuddyOffline.wav", [(783.99, 0.07), (659.25, 0.07), (523.25, 0.12)])


if __name__ == "__main__":
    main()
