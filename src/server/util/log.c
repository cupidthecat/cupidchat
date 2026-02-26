/*
 * src/server/util/log.c -  structured logging for cupid-chatd
 *
 * Format (stderr / journald-friendly):
 *   2026-02-25T12:34:56Z [LEVEL] key=val key=val message
 *
 * Levels: DEBUG INFO WARN ERROR
 */

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "server/log.h"

static log_level_t g_min_level = LOG_INFO;

void log_set_level(log_level_t lvl) {
    g_min_level = lvl;
}

static const char *level_str(log_level_t l) {
    switch (l) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO ";
        case LOG_WARN:  return "WARN ";
        case LOG_ERROR: return "ERROR";
        default:        return "?????";
    }
}

void log_emit(log_level_t lvl, const char *file, int line, const char *fmt, ...) {
    if (lvl < g_min_level) return;

    /* timestamp */
    time_t now = time(NULL);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

    /* message */
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s [%s] %s:%d %s\n", ts, level_str(lvl), file, line, msg);
}
