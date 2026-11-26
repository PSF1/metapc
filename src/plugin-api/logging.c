#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "config.h"
#include "paths.h"
#include "plugin.h"
#include "fs-utils.h"
#include "logging-internal.h"

void (*_savenvr)();
void (*_dumppic)();
void (*_dumpregs)();

/* The log is written in every build, so problems on other computers can be diagnosed. Each
   process writes its own file in the logs folder (<machine>.log, or manager.log for the machine
   manager), named by pclog_open() once the paths and the machine are known; messages logged
   before that only go to the console. Every line is flushed, so the log is complete even if the
   process hangs or is killed, and the file is capped so a noisy device cannot fill the disc. */

#define LOG_SIZE_LIMIT (16 * 1024 * 1024)

FILE *pclogf = NULL;
static char log_file[1024];
static int log_reopen = 0;
static long log_size = 0;
static int log_full = 0;

#ifdef _WIN32
static SRWLOCK log_lock = SRWLOCK_INIT;
#define LOG_LOCK() AcquireSRWLockExclusive(&log_lock)
#define LOG_UNLOCK() ReleaseSRWLockExclusive(&log_lock)
#else
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOG_LOCK() pthread_mutex_lock(&log_lock)
#define LOG_UNLOCK() pthread_mutex_unlock(&log_lock)
#endif

/* Called with log_lock held. */
static int log_open_file() {
        if (pclogf)
                return 1;
        if (!log_file[0] || log_full)
                return 0;
        /* The first open starts a new log; reopening after pclog_end() appends to it. */
        pclogf = fopen(log_file, log_reopen ? "at" : "wt");
        if (!pclogf) {
                fprintf(stderr, "Could not open log file %s for writing: %s\n", log_file, strerror(errno));
                log_file[0] = 0;
                return 0;
        }
        log_reopen = 1;
        return 1;
}

/* Called with log_lock held. */
static void log_write(const char *s) {
        if (!log_open_file())
                return;
        size_t len = strlen(s);
        if (log_size + (long)len > LOG_SIZE_LIMIT) {
                fputs("\n[log size limit reached, further messages are not written]\n", pclogf);
                fflush(pclogf);
                log_full = 1;
                return;
        }
        fputs(s, pclogf);
        fflush(pclogf);
        log_size += len;
}

void pclog_open(const char *name) {
        char header[256];
        time_t now = time(NULL);

        LOG_LOCK();
        if (pclogf) {
                fclose(pclogf);
                pclogf = NULL;
        }
        if (!pcem_dir_exists(logs_path))
                pcem_create_directory(logs_path);
        strncpy(log_file, logs_path, sizeof(log_file) - 1);
        log_file[sizeof(log_file) - 1] = 0;
        put_backslash(log_file);
        strncat(log_file, (name && name[0]) ? name : "pcem", sizeof(log_file) - strlen(log_file) - 5);
        strcat(log_file, ".log");
        log_reopen = 0;
        log_size = 0;
        log_full = 0;
        strftime(header, sizeof(header), "MetaPC log started %Y-%m-%d %H:%M:%S\n", localtime(&now));
        log_write(header);
        LOG_UNLOCK();
}

uint8_t pclog_start() {
        LOG_LOCK();
        int ret = log_open_file();
        LOG_UNLOCK();
        return ret;
}

void pclog_flush() {
        LOG_LOCK();
        if (pclogf)
                fflush(pclogf);
        LOG_UNLOCK();
}

void pclog_end() {
        LOG_LOCK();
        if (pclogf) {
                fflush(pclogf);
                fclose(pclogf);
                pclogf = NULL;
        }
        LOG_UNLOCK();
}

static void log_vprintf(FILE *console, const char *prefix, const char *format, va_list ap) {
        char buf[2048];
        size_t plen = strlen(prefix);

        if (log_full && !console)
                return;
        strcpy(buf, prefix);
        vsnprintf(buf + plen, sizeof(buf) - plen, format, ap);
        if (console)
                fputs(buf, console);
        LOG_LOCK();
        log_write(buf);
        LOG_UNLOCK();
}

void error(const char *format, ...) {
        va_list ap;
        va_start(ap, format);
        log_vprintf(stderr, "", format, ap);
        va_end(ap);
}

void fatal(const char *format, ...) {
        va_list ap;
        va_start(ap, format);
        log_vprintf(stderr, "FATAL: ", format, ap);
        va_end(ap);

        _savenvr();
        _dumppic();
        _dumpregs();
        pclog_end();
        exit(-1);
}

void warning(const char *format, ...) {
        va_list ap;
        va_start(ap, format);
        log_vprintf(stderr, "Warning: ", format, ap);
        va_end(ap);
}

void pclog(const char *format, ...) {
        va_list ap;
        va_start(ap, format);
        log_vprintf(stdout, "", format, ap);
        va_end(ap);
}
