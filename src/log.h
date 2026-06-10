#ifndef LOG_H
#define LOG_H

#include <stdbool.h>

extern bool debug_mode;

enum log_level { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR };

void log_init(void);
void log_close(void);
void log_write(enum log_level lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define debugLog(...) do { if (debug_mode) log_write(LOG_DEBUG, __VA_ARGS__); } while(0)
#define infoLog(...)  log_write(LOG_INFO, __VA_ARGS__)
#define warnLog(...)  log_write(LOG_WARN, __VA_ARGS__)
#define errorLog(...) log_write(LOG_ERROR, __VA_ARGS__)

#endif