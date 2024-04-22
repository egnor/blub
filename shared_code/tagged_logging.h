#pragma once

namespace arduino { class Print; }

enum TaggedLoggingLevel {
  TL_SPAM_LEVEL,
  TL_NOTICE_LEVEL,
  TL_PROBLEM_LEVEL,
  TL_FATAL_LEVEL,
};

// Logging macros expect a TaggedLoggingContext named TL_CONTEXT in scope
struct TaggedLoggingContext {
  char const* tag;
  TaggedLoggingLevel level;
  TaggedLoggingContext(char const* tag);  // Initializes tag and level
};

#define TL_SPAM(fmt, ...) TL_MAYBE(TL_SPAM_LEVEL, fmt, ##__VA_ARGS__)
#define TL_NOTICE(fmt, ...) TL_MAYBE(TL_NOTICE_LEVEL, fmt, ##__VA_ARGS__)
#define TL_PROBLEM(fmt, ...) TL_MAYBE(TL_PROBLEM_LEVEL, fmt, ##__VA_ARGS__)
#define TL_FATAL(fmt, ...) for(;;) TL_MAYBE(TL_FATAL_LEVEL, fmt, ##__VA_ARGS__)
#define TL_ASSERT(c) if (c) {} else TL_FATAL( \
    "Assertion failed: %s\n  %s:%d\n  %s", \
    #c, __FILE__, __LINE__, __PRETTY_FUNCTION__)

#define TL_MAYBE(l, f, ...) if (l >= TL_CONTEXT.level) \
    tagged_log(TL_CONTEXT.tag, l, (f), ##__VA_ARGS__); else {}

void set_logging_output(arduino::Print*);
void tagged_log(char const* tag, TaggedLoggingLevel, char const* fmt, ...);
