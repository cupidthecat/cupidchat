#ifndef SERVER_LOG_H
#define SERVER_LOG_H

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level_t;

void log_set_level(log_level_t lvl);
void log_emit(log_level_t lvl, const char *file, int line, const char *fmt, ...);

#define LOG_DEBUG(...) log_emit(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  log_emit(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  log_emit(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) log_emit(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif
