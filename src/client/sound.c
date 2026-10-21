/* Embedded WAV notifications. Playback workers use desktop audio first,
 * then ALSA, and keep player output away from the terminal UI. */
#include "client/sound.h"
#include "client/sounds_data.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const struct {
    const uint8_t *data;
    size_t size;
} SOUNDS[] = {
    [SND_WELCOME] = {snd_welcome_data, snd_welcome_size},
    [SND_GOODBYE] = {snd_goodbye_data, snd_goodbye_size},
    [SND_DM] = {snd_dm_data, snd_dm_size},
    [SND_IM] = {snd_im_data, snd_im_size},
    [SND_BUDDY_IN] = {snd_buddy_in_data, snd_buddy_in_size},
    [SND_BUDDY_OUT] = {snd_buddy_out_data, snd_buddy_out_size},
    [SND_MENTION] = {snd_mention_data, snd_mention_size},
    [SND_ERROR] = {snd_error_data, snd_error_size},
    [SND_TOPIC] = {snd_topic_data, snd_topic_size},
    [SND_BUDDY_ONLINE] = {snd_buddy_online_data, snd_buddy_online_size},
    [SND_BUDDY_OFFLINE] = {snd_buddy_offline_data, snd_buddy_offline_size},
    [SND_PROFILE_SAVED] = {snd_profile_saved_data, snd_profile_saved_size},
};

static char players[3][PATH_MAX];
static int player_count;
static pid_t workers[4];
static int muted, failed;
static uint64_t last_play_ms;

static pid_t wait_worker(pid_t pid, int *status, int flags) {
    pid_t rc;
    do {
        rc = waitpid(pid, status, flags);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

static void reap_workers(void) {
    for (size_t i = 0; i < sizeof(workers) / sizeof(workers[0]); i++) {
        if (!workers[i])
            continue;
        int status = 0;
        pid_t rc = wait_worker(workers[i], &status, WNOHANG);
        if (rc > 0) {
            failed = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
            workers[i] = 0;
        } else if (rc < 0 && errno == ECHILD) {
            workers[i] = 0;
        }
    }
}

static void find_player(const char *name) {
    const char *env = getenv("PATH");
    char *paths = strdup(env ? env : "/usr/local/bin:/usr/bin:/bin");
    if (!paths)
        return;
    char *save = NULL;
    for (char *dir = strtok_r(paths, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char candidate[PATH_MAX];
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
        if (n > 0 && n < (int)sizeof(candidate) && access(candidate, X_OK) == 0) {
            snprintf(players[player_count++], PATH_MAX, "%s", candidate);
            break;
        }
    }
    free(paths);
}

void sound_init(const char *sounds_dir) {
    (void)sounds_dir;
    reap_workers();
    player_count = 0;
    failed = 0;
    last_play_ms = 0;
    /* WSLg normally exports this. Recover it for shells that omit the variable. */
    if (!getenv("PULSE_SERVER") && access("/mnt/wslg/PulseServer", F_OK) == 0)
        setenv("PULSE_SERVER", "unix:/mnt/wslg/PulseServer", 0);
    find_player("paplay");
    find_player("pw-play");
    find_player("aplay");
}

void sound_set_enabled(int enabled) {
    muted = !enabled;
}

const char *sound_status(void) {
    reap_workers();
    if (muted)
        return "Sounds muted. /sound on enables them.";
    if (!player_count)
        return "No audio player. Install pulseaudio-utils (WSLg) or alsa-utils.";
    if (failed)
        return "Audio playback failed. Run cupid-chat --sound-test outside the TUI.";
    return "Sounds on: desktop audio with ALSA fallback. /sound test plays a preview.";
}

/* A worker waits for each player in turn so an unavailable desktop server can
 * fall back to the next backend. Its children inherit only the WAV descriptor. */
static void playback_worker(int mfd, int diagnostic) {
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd < 0)
        _exit(1);
    dup2(nullfd, STDIN_FILENO);
    if (!diagnostic) {
        dup2(nullfd, STDOUT_FILENO);
        dup2(nullfd, STDERR_FILENO);
    }
    if (nullfd != mfd && nullfd > STDERR_FILENO)
        close(nullfd);
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (mfd > 3 && close_range(3, (unsigned)mfd - 1, 0) < 0) {
        for (int fd = 3; fd < mfd; fd++)
            close(fd);
    }
    if (close_range((unsigned)mfd + 1, ~0U, 0) < 0) {
        for (long fd = mfd + 1; fd < maxfd; fd++)
            close((int)fd);
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", mfd);
    for (int i = 0; i < player_count; i++) {
        if (diagnostic)
            fprintf(stderr, "Playing with %s\n", players[i]);
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGALRM, SIG_DFL);
            alarm(8);
            execl(players[i], players[i], path, (char *)NULL);
            _exit(127);
        }
        int status = 0;
        if (pid > 0 && wait_worker(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0)
            _exit(0);
    }
    _exit(1);
}

static int play_mem(sound_id_t id, int sync, int diagnostic) {
    reap_workers();
    if (muted || !player_count || id < 0 || id >= SND_COUNT)
        return -1;
    int slot = -1;
    for (int i = 0; i < 4; i++)
        if (!workers[i]) {
            slot = i;
            break;
        }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    if (!sync && (slot < 0 || (last_play_ms && ms - last_play_ms < 200)))
        return -1;
    int mfd = (int)syscall(SYS_memfd_create, "cupidchat-sound", 0U);
    if (mfd < 0)
        return -1;
    size_t written = 0;
    while (written < SOUNDS[id].size) {
        ssize_t n = write(mfd, SOUNDS[id].data + written, SOUNDS[id].size - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(mfd);
            return -1;
        }
        written += (size_t)n;
    }
    pid_t pid = fork();
    if (pid == 0)
        playback_worker(mfd, diagnostic);
    close(mfd);
    if (pid < 0)
        return -1;
    last_play_ms = ms;
    if (sync) {
        int status = 0;
        failed =
            wait_worker(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
        return failed ? -1 : 0;
    }
    workers[slot] = pid;
    return 0;
}

void sound_play(sound_id_t id) {
    (void)play_mem(id, 0, 0);
}

void sound_play_sync(sound_id_t id) {
    (void)play_mem(id, 1, 0);
}

int sound_diagnose(void) {
    fprintf(stderr, "%s\n", sound_status());
    const char *server = getenv("PULSE_SERVER");
    fprintf(stderr, "PULSE_SERVER=%s\n", server ? server : "(desktop default)");
    return play_mem(SND_WELCOME, 1, 1);
}
