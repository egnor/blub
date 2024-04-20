#pragma once

namespace arduino { class Print; }

enum chatty_level {
  CHATTY_SPAM = 0,
  CHATTY_REMARK,
  CHATTY_PROBLEM,
  CHATTY_FATAL,
};

#define CL_SPAM(fmt, ...) CL_MAYBE(CHATTY_SPAM, fmt, ##__VA_ARGS__)
#define CL_REMARK(fmt, ...) CL_MAYBE(CHATTY_REMARK, fmt, ##__VA_ARGS__)
#define CL_PROBLEM(fmt, ...) CL_MAYBE(CHATTY_PROBLEM, fmt, ##__VA_ARGS__)
#define CL_FATAL(fmt, ...) for(;;) chatty_log(CHATTY_FATAL, fmt, ##__VA_ARGS__)
#define CL_ASSERT(c) if (c) {} else CL_FATAL(\
    "Assertion failed: %s\n  %s:%d\n  %s", \
    #c, __FILE__, __LINE__, __PRETTY_FUNCTION__ \
  )

void set_chatty_output(arduino::Print*);
void chatty_log(chatty_level, char const* fmt, ...);
chatty_level get_chatty_file_level(char const* file);

// icky C++ stuff to avoid evaluating the file name more than once

#define CL_MAYBE(level, fmt, ...) do { \
    auto const file_level = cached_chatty_file_level<chatty_id(__FILE__)>(); \
    if ((level) >= file_level) chatty_log((level), (fmt), ##__VA_ARGS__); \
  } while (0)

template <int N>
struct chatty_id {
  using A = char[N];
  A id;
  constexpr chatty_id(A const& v) { for (int i = 0; i < N; ++i) id[i] = v[i]; }
};

template <chatty_id file>
chatty_level cached_chatty_file_level() {
  static auto const file_level = get_chatty_file_level(file.id);
  return file_level;
}
