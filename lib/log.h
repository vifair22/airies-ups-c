/*
Purpose:
  Header-only logging helper for C and C++ with timestamps and optional colors.
Repository:
  https://git.airies.net/vifair22/c-log

Quick use (C99):
  #include "log.h"
  int main(void) {
      log_msg("INFO", "Hello");
      log_msg("ERROR", "Something failed");
      return 0;
  }

Quick use (C++17):
  #include "log.h"
  #include <string>
  #include <string_view>
  int main() {
      log_msg("INFO", "Hello");
      log_msg(std::string("WARN"), std::string("Be careful"));
      log_msg(std::string_view("DEBUG"), std::string_view("Details"));
      return 0;
  }

Log levels (case-insensitive):
  DEBUG, INFO, WARN, ERROR

Output format:
  YYYY-MM-DD HH:MM:SS.sss [function] message
  - Timestamp in blue, message colored by level (unless LOG_NO_COLOR is defined).
  - WARN/ERROR go to stderr; INFO/DEBUG go to stdout.

Build options:
  - Define LOG_NO_COLOR to disable ANSI colors.
  - Windows consoles: VT is enabled automatically on first log call.

License (MIT):
  Copyright (c) 2026 Vincent Fairfield
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/


/* ---------------------------------------------------------------------------  */

#pragma once
#ifndef LOG_H
#define LOG_H

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>
#if !defined(_MSC_VER)
#include <strings.h>
#endif
#if !defined(_WIN32)
#include <sys/time.h>
#if !defined(_MSC_VER)
struct tm;
struct tm* localtime_r(const time_t* timer, struct tm* result);
void flockfile(FILE* stream);
void funlockfile(FILE* stream);
#endif
#endif
#if defined(_WIN32)
#include <windows.h>
#else
typedef void* HANDLE;
#endif

// Define LOG_NO_COLOR to disable ANSI color output.
#if defined(LOG_NO_COLOR)
#define LOG_COLOR_RESET ""
#define LOG_COLOR_BLUE  ""
#define LOG_COLOR_RED   ""
#define LOG_COLOR_YELLOW ""
#define LOG_COLOR_CYAN  ""
#else
#define LOG_COLOR_RESET "\x1b[0m"
#define LOG_COLOR_BLUE  "\x1b[34m"
#define LOG_COLOR_RED   "\x1b[31m"
#define LOG_COLOR_YELLOW "\x1b[33m"
#define LOG_COLOR_CYAN  "\x1b[36m"
#endif

typedef enum LogLevel {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_UNKNOWN,
    LOG_LEVEL_COUNT
} LogLevel;

typedef struct LogLevelEntry {
    const char* name;
    LogLevel level;
} LogLevelEntry;

static inline int log_strcasecmp(const char* a, const char* b) {
#if defined(_MSC_VER)
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

static inline LogLevel log_parse_level(const char* level) {
    static const LogLevelEntry map[] = {
        { "DEBUG", LOG_LEVEL_DEBUG },
        { "INFO",  LOG_LEVEL_INFO  },
        { "WARN",  LOG_LEVEL_WARN  },
        { "ERROR", LOG_LEVEL_ERROR }
    };
    if (level == NULL) return LOG_LEVEL_INFO;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (log_strcasecmp(level, map[i].name) == 0) {
            return map[i].level;
        }
    }
    return LOG_LEVEL_UNKNOWN;
}

static inline const char* log_level_color(LogLevel level) {
    static const char* colors[LOG_LEVEL_COUNT] = {
        LOG_COLOR_CYAN,   // DEBUG
        LOG_COLOR_RESET,  // INFO
        LOG_COLOR_YELLOW, // WARN
        LOG_COLOR_RED,    // ERROR
        LOG_COLOR_RESET   // UNKNOWN
    };
    if (level < 0 || level >= LOG_LEVEL_COUNT) return LOG_COLOR_RESET;
    return colors[level];
}

// Enable ANSI color output on older Windows consoles.
static inline int log_enable_vt_handle(HANDLE out) {
#if defined(LOG_NO_COLOR)
    (void)out;
    return 1;
#elif defined(_WIN32)
    if (out == INVALID_HANDLE_VALUE) return 0;
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode)) return 0;
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    return SetConsoleMode(out, mode) ? 1 : 0;
#else
    (void)out;
    return 1;
#endif
}

static inline int log_enable_vt(void) {
#if defined(LOG_NO_COLOR)
    return 1;
#elif defined(_WIN32)
    int ok_out = log_enable_vt_handle(GetStdHandle(STD_OUTPUT_HANDLE));
    int ok_err = log_enable_vt_handle(GetStdHandle(STD_ERROR_HANDLE));
    return (ok_out && ok_err) ? 1 : 0;
#else
    return 1;
#endif
}

static inline void log_init_vt_once(void) {
#if defined(LOG_NO_COLOR)
    return;
#elif defined(_WIN32)
    static volatile LONG vt_inited = 0;
    if (InterlockedCompareExchange(&vt_inited, 1, 0) == 0) {
        (void)log_enable_vt();
    }
#endif
}

static inline void log_stream_lock(FILE* out) {
#if defined(_MSC_VER)
    _lock_file(out);
#elif !defined(_WIN32)
    flockfile(out);
#else
    (void)out;
#endif
}

static inline void log_stream_unlock(FILE* out) {
#if defined(_MSC_VER)
    _unlock_file(out);
#elif !defined(_WIN32)
    funlockfile(out);
#else
    (void)out;
#endif
}

static inline void log_msg_impl(const char* message, const char* level, const char* func) {
    if (message == NULL) message = "(null)";
#if defined(_WIN32) && !defined(LOG_NO_COLOR)
    log_init_vt_once();
#endif
    // Build timestamp in YYYY-MM-DD HH:MM:SS.sss.
    char time_buf[24];
    struct timespec ts;
    struct tm tm_buf;
    int ms = 0;
    int ok = 1;
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && defined(TIME_UTC)
    if (timespec_get(&ts, TIME_UTC) == 0) {
        ok = 0;
    } else {
        ms = (int)(ts.tv_nsec / 1000000);
    }
#elif defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0) && defined(CLOCK_REALTIME)
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ok = 0;
    } else {
        ms = (int)(ts.tv_nsec / 1000000);
    }
#elif defined(_WIN32)
    ts.tv_sec = time(NULL);
    ts.tv_nsec = 0;
#else
    {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) != 0) {
            ok = 0;
        } else {
            ts.tv_sec = (time_t)tv.tv_sec;
            ts.tv_nsec = (long)tv.tv_usec * 1000L;
            ms = (int)(ts.tv_nsec / 1000000);
        }
    }
#endif

#if defined(_MSC_VER)
    if (!ok || localtime_s(&tm_buf, &ts.tv_sec) != 0) {
        ok = 0;
    } else {
        int n = snprintf(
            time_buf,
            sizeof(time_buf),
            "%04d-%02d-%02d %02d:%02d:%02d.%03d",
            tm_buf.tm_year + 1900,
            tm_buf.tm_mon + 1,
            tm_buf.tm_mday,
            tm_buf.tm_hour,
            tm_buf.tm_min,
            tm_buf.tm_sec,
            ms
        );
        if (n < 0 || n >= (int)sizeof(time_buf)) ok = 0;
    }
#else
    if (!ok || localtime_r(&ts.tv_sec, &tm_buf) == NULL) {
        ok = 0;
    } else {
        int n = snprintf(
            time_buf,
            sizeof(time_buf),
            "%04d-%02d-%02d %02d:%02d:%02d.%03d",
            tm_buf.tm_year + 1900,
            tm_buf.tm_mon + 1,
            tm_buf.tm_mday,
            tm_buf.tm_hour,
            tm_buf.tm_min,
            tm_buf.tm_sec,
            ms
        );
        if (n < 0 || n >= (int)sizeof(time_buf)) ok = 0;
    }
#endif
    if (!ok) {
        (void)snprintf(time_buf, sizeof(time_buf), "0000-00-00 00:00:00.000");
    }

    LogLevel parsed_level = log_parse_level(level);
    const char* level_color = log_level_color(parsed_level);
    FILE* out = (parsed_level == LOG_LEVEL_WARN || parsed_level == LOG_LEVEL_ERROR) ? stderr : stdout;

    // time in blue, [function] default, message colored by level
    // Progress lines use \r on stderr without a trailing \n.  Clear any
    // active progress text so the log message starts on a clean line.
    log_stream_lock(out);
    if (out == stderr)
        (void)fprintf(stderr, "\r%-80s\r", "");
    (void)fprintf(
        out,
        LOG_COLOR_BLUE "%s" LOG_COLOR_RESET " "
        "[%s]" " "
        "%s%s" LOG_COLOR_RESET "\n",
        time_buf,
        func ? func : "?",
        level_color,
        message
    );
    (void)fflush(out);
    log_stream_unlock(out);
}

// Helper macro to auto-pass the calling function name in C.
#define log_msg(level, message) log_msg_impl((message), (level), __func__)

#ifdef __cplusplus
#include <string>
#include <string_view>
#undef log_msg

inline void log_msg_cpp(const char* level, const char* message, const char* func) {
    log_msg_impl(message, level, func);
}

inline void log_msg_cpp(std::string_view level, std::string_view message, const char* func) {
    std::string msg(message);
    std::string lvl(level);
    log_msg_impl(msg.c_str(), lvl.c_str(), func);
}

inline void log_msg_cpp(const std::string& level, const std::string& message, const char* func) {
    log_msg_impl(message.c_str(), level.c_str(), func);
}

#define log_msg(level, message) log_msg_cpp((level), (message), __func__)
#endif

#endif // LOG_H
