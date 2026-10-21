/* CupidChat public interface. */
#pragma once

typedef enum {
    SND_WELCOME,
    SND_GOODBYE,
    SND_DM,
    SND_IM,
    SND_BUDDY_IN,
    SND_BUDDY_OUT,
    SND_MENTION,
    SND_ERROR,
    SND_TOPIC,
    SND_BUDDY_ONLINE,
    SND_BUDDY_OFFLINE,
    SND_COUNT
} sound_id_t;

void sound_init(const char *sounds_dir);
void sound_play(sound_id_t id);
void sound_play_sync(sound_id_t id);
void sound_set_enabled(int enabled);
const char *sound_status(void);
int sound_diagnose(void);
