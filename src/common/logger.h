#pragma once

#include <cstdio>
#include <cstdarg>

namespace ugdr{
namespace common{

enum class LogLevel : int {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
};

static LogLevel g_log_level = LogLevel::INFO;

inline const char* log_level_to_str(LogLevel level){
    switch(level){
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARN: return "WARN";
        case LogLevel::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

inline void internal_log(LogLevel level, const char* file, const char* func, int line, const char* fmt, ...){
    if(level < g_log_level) return;

    fprintf(stderr, "[%s][%s][%s:%d] ", log_level_to_str(level), func, file, line);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
}

#define UGDR_LOG_DEBUG(fmt, ...) \
    ::ugdr::common::internal_log(::ugdr::common::LogLevel::DEBUG, __FILE__, __func__, __LINE__, fmt, ##__VA_ARGS__)

#define UGDR_LOG_INFO(fmt, ...) \
    ::ugdr::common::internal_log(::ugdr::common::LogLevel::INFO, __FILE__, __func__, __LINE__, fmt, ##__VA_ARGS__)

#define UGDR_LOG_WARN(fmt, ...) \
    ::ugdr::common::internal_log(::ugdr::common::LogLevel::WARN, __FILE__, __func__, __LINE__, fmt, ##__VA_ARGS__)

#define UGDR_LOG_ERROR(fmt, ...) \
    ::ugdr::common::internal_log(::ugdr::common::LogLevel::ERROR, __FILE__, __func__, __LINE__, fmt, ##__VA_ARGS__)

}
}