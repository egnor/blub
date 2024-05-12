#include "tagged_logging.h"

#include <Arduino.h>

static Print* log_output = nullptr;

#ifndef TAGGED_LOGGING_CONFIG
#define TAGGED_LOGGING_CONFIG note
#endif

#define STRINGIFY2(x...) #x
#define STRINGIFY(x) STRINGIFY2(x)
static char const* const config = STRINGIFY(TAGGED_LOGGING_CONFIG);
#undef STRINGIFY
#undef STRINGIFY2

static TaggedLoggingLevel level_for_tag(char const*);

TaggedLoggingContext::TaggedLoggingContext(char const* tag)
  : tag(tag), level(level_for_tag(tag)) {}

void set_logging_output(Print* output) { log_output = output; }

void tagged_log(char const* tag, TaggedLoggingLevel lev, char const* f, ...) {
  if (log_output == nullptr) return;

  log_output->print(millis() * 1e-3f, 3);

  switch (lev) {
    case TL_FATAL_LEVEL: log_output->print(" 💥 "); break;
    case TL_PROBLEM_LEVEL: log_output->print(" 🔥 "); break;
    case TL_NOTICE_LEVEL: log_output->print(" "); break;
    case TL_SPAM_LEVEL: log_output->print(" 🕸️ "); break;
  }

  if (tag != nullptr && tag[0] != '\0') {
    log_output->print("[");
    log_output->print(tag);
    log_output->print("] ");
  }

  if (lev == TL_FATAL_LEVEL) log_output->print("FATAL ");

  char stack_buf[128];
  char* buf = stack_buf;
  int buf_size = sizeof(stack_buf);
  while (true) {
    va_list args;
    va_start(args, f);
    auto len = vsnprintf(buf, buf_size, f, args);
    va_end(args);

    if (len >= buf_size) {
      if (buf != stack_buf) delete[] buf;
      buf_size = len + 1;
      buf = new char[buf_size];
    } else {
      if (len < 0) strncpy(buf, "[log message formatting error]", buf_size);
      while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) --len;
      break;
    }
  }

  log_output->println(buf);
  if (buf != stack_buf) delete[] buf;
  if (lev == TL_FATAL_LEVEL) {
    log_output->println("  🚨 REBOOT IN 1 SEC 🚨\n");
    log_output->flush();
    delay(1000);
    rp2040.reboot();
  }
}

static TaggedLoggingLevel level_for_tag(char const* tag) {
  // Find the start of the "tag=level,..." (or just "level,...")
  // part of the logging config that matches the given tag
  char const* part = config;
  do {
    char const* part_end = part + strcspn(part, "=,");
    if (part_end == part) {
      tagged_log(
          "tagged_logging", TL_PROBLEM_LEVEL,
          "Empty spec in config: %s", config);
      part = part_end + 1;
      continue;
    }

    // entry without a tag is a default level
    if (*part_end != '=') break;

    // this is a tag pattern; check if it matches
    if (strncasecmp(part, tag, part_end - part) == 0) break;

    // no match; look for the comma
    part = part_end + 1 + strcspn(part_end + 1, ",");
    if (*part == ',') ++part;
  } while (*part);

  if (!*part) {
    tagged_log(
        "tagged_logging", TL_PROBLEM_LEVEL,
        "No config entry for \"%s\" (and no default):\n  %s", tag, config);
    return TL_FATAL_LEVEL;
  }

  // for "file_substring=level", skip to the "level" part
  char const* part_end = part + strcspn(part, "=,");
  if (*part_end == '=') {
    part = part_end + 1;
    part_end = part + strcspn(part, ",");
  }

  // parse the log level
  int const len = part_end - part;
  if ((len == 3 && strncasecmp(part, "all", 3) == 0) ||
      (len == 4 && strncasecmp(part, "spam", 4) == 0) ||
      (len == 5 && strncasecmp(part, "debug", 5) == 0) ||
      (len == 7 && strncasecmp(part, "verbose", 7) == 0)) {
    return TL_SPAM_LEVEL;
  }

  if ((len == 4 && strncasecmp(part, "default", 7) == 0) ||
      (len == 4 && strncasecmp(part, "info", 4) == 0) ||
      (len == 4 && strncasecmp(part, "normal", 6) == 0) ||
      (len == 4 && strncasecmp(part, "note", 4) == 0) ||
      (len == 6 && strncasecmp(part, "notice", 6) == 0) ||
      (len == 7 && strncasecmp(part, "notable", 7) == 0)) {
    return TL_NOTICE_LEVEL;
  }

  if ((len == 7 && strncasecmp(part, "problem", 7) == 0) ||
      (len == 7 && strncasecmp(part, "warning", 7) == 0) ||
      (len == 5 && strncasecmp(part, "error", 5) == 0)) {
    return TL_PROBLEM_LEVEL;
  }

  if ((len == 5 && strncasecmp(part, "fatal", 5) == 0) ||
      (len == 5 && strncasecmp(part, "panic", 5) == 0) ||
      (len == 4 && strncasecmp(part, "none", 4) == 0)) {
    return TL_PROBLEM_LEVEL;
  }

  tagged_log(
      "tagged_logging", TL_PROBLEM_LEVEL,
      "Bad level \"%.*s\" in config: %s", len, part, config);
  return TL_SPAM_LEVEL;
}
