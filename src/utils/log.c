#include "utils/log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static int log_level = LOG_INFO;
static const char *log_ident = "ultrabalancer";

void log_init(const char *ident, int level) {
    if (ident) log_ident = ident;
    log_level = level;
}

static void log_write(int level, const char *fmt, va_list ap) {
    if (level > log_level)
        return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);

    const char *level_str[] = {
        "EMERG", "ALERT", "CRIT", "ERROR",
        "WARN", "NOTICE", "INFO", "DEBUG"
    };

    fprintf(stderr, "[%s] %s %s: ", timestamp, log_ident, level_str[level]);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write(LOG_ERR, fmt, ap);
    va_end(ap);
}

void log_warning(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write(LOG_WARNING, fmt, ap);
    va_end(ap);
}

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write(LOG_INFO, fmt, ap);
    va_end(ap);
}

void log_debug(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write(LOG_DEBUG, fmt, ap);
    va_end(ap);
}