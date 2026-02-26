/*
 * src/client/sound.c -  non-blocking WAV notification sounds
 *
 * WAV data is compiled directly into the binary (via sounds_data.c generated
 * by scripts/gen_sounds.py at build time).  At play time the raw bytes are
 * written to an anonymous in-memory file (memfd_create) and aplay is given
 * the path /proc/self/fd/N -  no files on disk are ever created, so the
 * binary can run from any working directory.
 *
 * Uses aplay (ALSA) via fork()+execl() so the TUI is never blocked.
 * SIGCHLD is ignored so zombie children are automatically reaped by the
 * kernel without an explicit waitpid() on the hot path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/syscall.h>    /* SYS_memfd_create (Linux ≥ 3.17) */

#include "client/sound.h"
#include "client/sounds_data.h"

/* State */

static int g_enabled = 1;   /* cleared to 0 if aplay is absent */

/* Map sound_id_t -> embedded blob. */
static const struct {
    const uint8_t *data;
    size_t         size;
} SOUNDS[] = {
    [SND_WELCOME]   = { snd_welcome_data,   snd_welcome_size   },
    [SND_GOODBYE]   = { snd_goodbye_data,   snd_goodbye_size   },
    [SND_DM]        = { snd_dm_data,        snd_dm_size        },
    [SND_IM]        = { snd_im_data,        snd_im_size        },
    [SND_BUDDY_IN]  = { snd_buddy_in_data,  snd_buddy_in_size  },
    [SND_BUDDY_OUT] = { snd_buddy_out_data, snd_buddy_out_size },
};

#define N_SOUNDS  ((int)(sizeof(SOUNDS) / sizeof(SOUNDS[0])))

/* Helpers */

/*
 * Write `len` bytes of WAV data to an anonymous in-memory file (memfd) and
 * fork aplay to play it.  The fd is created *without* MFD_CLOEXEC so it is
 * inherited across fork() and survives execl() in the child.  aplay receives
 * the path /proc/self/fd/<N>, which the kernel resolves against the child's
 * own fd table -  it opens the memfd and reads it as a normal WAV file.
 *
 * If sync != 0 the parent waits for aplay to exit (used for SND_GOODBYE so
 * the sound completes before process teardown).
 */
static void play_mem(const uint8_t *data, size_t len, int sync)
{
    if (!g_enabled || !data || len == 0) return;

    /* Anonymous in-memory file -  no O_CLOEXEC so the fd is inherited by
     * the child and survives execl(). */
    int mfd = (int)syscall(SYS_memfd_create, "snd", 0U);
    if (mfd < 0) return;

    /* Write the WAV bytes into the memfd. */
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(mfd, data + written, len - written);
        if (n <= 0) { close(mfd); return; }
        written += (size_t)n;
    }

    /* /proc/self/fd/N -  resolved by the kernel relative to the calling
     * process's fd table, so valid in the child even after execl(). */
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", mfd);

    pid_t pid = fork();
    if (pid < 0) { close(mfd); return; }

    if (pid == 0) {
        /* child: silence aplay output so ncurses is not disturbed. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl("/usr/bin/aplay", "aplay", "-q", path, (char *)NULL);
        /* execl failed (aplay missing / wrong path) -  exit silently */
        _exit(1);
    }

    /* parent: close our copy; child still holds the fd open */
    close(mfd);

    if (sync) {
        int status;
        waitpid(pid, &status, 0);
    }
    /* async: SIGCHLD SIG_IGN causes the kernel to auto-reap the child */
}

/* Public API */

void sound_init(const char *sounds_dir)
{
    /* sounds_dir is accepted for API compatibility but ignored -  all WAV
     * data is compiled into the binary via sounds_data.c. */
    (void)sounds_dir;

    /* Ignore SIGCHLD so async aplay children are auto-reaped without
     * accumulating zombies.  Safe -  our only other child use is
     * sound_play_sync() which calls waitpid() directly. */
    signal(SIGCHLD, SIG_IGN);

    /* Disable sounds if aplay is not available rather than filling the
     * process table with failed fork()s. */
    if (access("/usr/bin/aplay", X_OK) != 0)
        g_enabled = 0;
}

void sound_play(sound_id_t id)
{
    if (!g_enabled) return;
    if (id < 0 || id >= N_SOUNDS) return;
    play_mem(SOUNDS[id].data, SOUNDS[id].size, 0 /* async */);
}

void sound_play_sync(sound_id_t id)
{
    if (!g_enabled) return;
    if (id < 0 || id >= N_SOUNDS) return;
    play_mem(SOUNDS[id].data, SOUNDS[id].size, 1 /* sync */);
}
