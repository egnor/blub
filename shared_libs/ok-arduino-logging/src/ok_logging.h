// Simple, robust printf logging for Arduino-ish code.

#pragma once

#include <Arduino.h>
#include <cstdarg>
#include <cstdint>

enum OkLoggingLevel : int8_t {
  OK_DETAIL_LEVEL,
  OK_NOTICE_LEVEL,
  OK_ERROR_LEVEL,
  OK_FATAL_LEVEL,
};

// Logging macros expect an OkLoggingContext named OK_CONTEXT in scope
struct OkLoggingContext {
  char const* tag;
  OkLoggingLevel min;
  OkLoggingContext(char const* tag);  // Initializes tag and minimum level
};

// Define this global constant as a comma-separated list of "tag=level"
// selectors with "*" wildcards, eg. "mymodule=spam,subsystem.*=problem,*=note".
extern char const* const ok_logging_config;

// TODO: add numeric detail levels with contextual adjustment?
#define OK_DETAIL(fmt, ...) OK_REPORT(OK_DETAIL_LEVEL, fmt, ##__VA_ARGS__)
#define OK_NOTICE(fmt, ...) OK_REPORT(OK_NOTICE_LEVEL, fmt, ##__VA_ARGS__)
#define OK_ERROR(fmt, ...) OK_REPORT(OK_ERROR_LEVEL, fmt, ##__VA_ARGS__)
#define OK_FATAL(fmt, ...) OK_REPORT_SOURCE(OK_FATAL_LEVEL, fmt, ##__VA_ARGS__)
#define OK_FATAL_IF(c) if (c) OK_FATAL("%s", #c) else {}
#define OK_ERROR_IF(c) \
    ({const auto e = c; if (e) OK_REPORT_SOURCE(OK_ERROR_LEVEL, "%s", #c); e;})

#define OK_REPORT(lev, fmt, ...) if ((lev) >= OK_CONTEXT.min) ok_log( \
    OK_CONTEXT.tag, (lev), "" fmt, ##__VA_ARGS__); else {}
#define OK_REPORT_SOURCE(lev, fmt, ...) if (lev >= OK_CONTEXT.min) ok_log( \
    OK_CONTEXT.tag, (lev), "" fmt "\n  at: %s:%d\n  in: %s", \
    ##__VA_ARGS__, __FILE__, __LINE__, __PRETTY_FUNCTION__); else {}

void set_ok_logging_output(Print*);
OkLoggingLevel swap_ok_logging_minimum(OkLoggingLevel);
void ok_log(char const* tag, OkLoggingLevel, char const* fmt, ...);
void ok_logv(char const* tag, OkLoggingLevel, char const* fmt, va_list args);
