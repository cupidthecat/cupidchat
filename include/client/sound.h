#ifndef CLIENT_SOUND_H
#define CLIENT_SOUND_H

typedef enum {
    SND_WELCOME = 0,
    SND_GOODBYE,
    SND_DM,
    SND_IM,
    SND_BUDDY_IN,
    SND_BUDDY_OUT
} sound_id_t;

void sound_init(const char *sounds_dir);
void sound_play(sound_id_t id);
void sound_play_sync(sound_id_t id);

#endif
