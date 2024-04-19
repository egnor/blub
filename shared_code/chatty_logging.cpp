#include "chatty_logging.h"

#include <Arduino.h>

static Print* chatty_output = nullptr;

#ifndef CHATTY_CONFIG
#define CHATTY_CONFIG note
#endif

#define STRINGIFY2(x) #x
#define STRINGIFY(x) STRINGIFY2(x)
static char const* const config = STRINGIFY(CHATTY_CONFIG);
#undef STRINGIFY
#undef STRINGIFY2

void set_chatty_output(Print* output) {
  chatty_output = output;
}

void chatty_log(chatty_level level, char const* fmt, ...) {
  if (chatty_output == nullptr) return;

  chatty_output->print(millis() * 1e-3f, 3);

  switch (level) {
    case CHATTY_FATAL: chatty_output->print(" 💥 FATAL (rebooting): "); break;
    case CHATTY_PROBLEM: chatty_output->print(" 🔥 "); break;
    case CHATTY_NOTE: chatty_output->print(" "); break;
    case CHATTY_SPAM: chatty_output->print(" 🕸️ "); break;
  }

  char stack_buf[128];
  char* buf = stack_buf;
  int buf_size = sizeof(stack_buf);
  while (true) {
    va_list args;
    va_start(args, fmt);
    auto len = vsnprintf(buf, buf_size, fmt, args);
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

  chatty_output->println(buf);
  if (buf != stack_buf) delete[] buf;
  if (level == CHATTY_FATAL) {
    chatty_output->flush();
    delay(1000);
    rp2040.reboot();
  }
}

// returns the start of the "file_substring=level,..." (or just "level,...")
// part of the chatty config that matches the given file
static char const* find_spec_part(char const* file) {
  char const* part = config;
  do {
    char const* part_end = part + strcspn(part, "=,");
    if (part_end == part) {
      chatty_log(CHATTY_PROBLEM, "Empty spec in chatty config: %s", config);
      part = part_end + 1;
      continue;
    }

    // entry without a file pattern is a default level
    if (*part_end != '=') return part;

    // this is a file substring to match against
    for (char const* m = strchr(file, part[0]); m; m = strchr(m + 1, part[0])) {
      if (strncmp(m, part, part_end - part) == 0) return part;
    }

    // no match; look for the comma
    part = part_end + 1 + strcspn(part_end + 1, ",");
    if (*part == ',') ++part;
  } while (*part);

  chatty_log(
      CHATTY_PROBLEM, "No match for file (%s) in chatty config: %s",
      file, config
  );
  return nullptr;
}

static chatty_level find_file_level(char const* file) {
  char const* part = find_spec_part(file);
  if (part == nullptr) return CHATTY_FATAL;

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
    return CHATTY_SPAM;
  }

  if ((len == 4 && strncasecmp(part, "note", 4) == 0) ||
             (len == 6 && strncasecmp(part, "notice", 6) == 0) ||
             (len == 7 && strncasecmp(part, "notable", 7) == 0) ||
             (len == 4 && strncasecmp(part, "info", 4) == 0)) {
    return CHATTY_NOTE;
  }

  if ((len == 7 && strncasecmp(part, "problem", 7) == 0) ||
             (len == 7 && strncasecmp(part, "warning", 7) == 0) ||
             (len == 5 && strncasecmp(part, "error", 5) == 0)) {
    return CHATTY_PROBLEM;
  }

  if ((len == 5 && strncasecmp(part, "fatal", 5) == 0) ||
             (len == 5 && strncasecmp(part, "panic", 5) == 0) ||
             (len == 4 && strncasecmp(part, "none", 4) == 0)) {
    return CHATTY_FATAL;
  }

  chatty_log(
      CHATTY_PROBLEM, "Bad log level \"%.*s\" in chatty config: %s",
      len, part, config
  );
  return CHATTY_SPAM;
}

chatty_level get_chatty_file_level(char const* file) {
  auto const level = find_file_level(file);
  if (chatty_output != nullptr) {
    chatty_output->print(millis() * 1e-3f, 3);
    switch (level) {
      case CHATTY_FATAL: chatty_output->print(" 🕳️ [no log] "); break;
      case CHATTY_PROBLEM: chatty_output->print(" 🛎️ [log error+] "); break;
      case CHATTY_NOTE: chatty_output->print(" 💬 [log note+] "); break;
      case CHATTY_SPAM: chatty_output->print(" 🗯️ [log all] "); break;
    }
    chatty_output->println(file);
  }
  return level;
}
