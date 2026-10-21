# Notification sounds

`Mention.wav`, `Error.wav`, and `Topic.wav` are synthesized for CupidChat by
`scripts/gen_notification_sounds.py`. Run that script to regenerate them.

The other WAV files are the existing chat sounds. The client embeds each sound
listed in `scripts/gen_sounds.py`; it does not need this directory at runtime.
