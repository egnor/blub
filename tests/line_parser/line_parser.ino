// Device-side test firmware for shared_src/line_parser.{h,cpp}.
//
// Runs under the rp2040js emulator (see tests/emulator/) or on real hardware.
// Prints a structured report to Serial1 (UART0); tests/test_line_parser.py does
// the actual asserting, so matchers stay on the host where they're expressive.
//
// Output protocol:
//   BEGIN-TEST
//   FAIL line=<n> <expression>          (zero or more)
//   ...arbitrary informational lines...
//   END-TEST checks=<n> failures=<n>

#include <Arduino.h>
#include <string.h>

#include "src/line_parser.h"

static int checks = 0;
static int failures = 0;

#define CHECK(expr)                                            \
    do {                                                       \
        ++checks;                                              \
        if (!(expr)) {                                         \
            ++failures;                                        \
            Serial1.printf("TEST-FAIL line=%d %s\n", __LINE__, #expr); \
        }                                                      \
    } while (0)

// Feeds a whole string, returning how many complete lines came out.
static int feed_all(LineParser& parser, const char* text) {
    int lines = 0;
    for (const char* p = text; *p; ++p) {
        if (parser.feed(*p)) ++lines;
    }
    return lines;
}

static void test_simple_line() {
    char buf[32];
    LineParser parser(buf, sizeof(buf));

    CHECK(!parser.feed('a'));
    CHECK(!parser.feed('b'));
    CHECK(parser.feed('\n'));
    CHECK(strcmp(parser.line(), "ab") == 0);
    CHECK(parser.length() == 2);
    CHECK(!parser.truncated());
}

static void test_crlf_and_empty() {
    char buf[32];
    LineParser parser(buf, sizeof(buf));

    CHECK(feed_all(parser, "hello\r\n") == 1);
    CHECK(strcmp(parser.line(), "hello") == 0);
    CHECK(parser.length() == 5);

    CHECK(feed_all(parser, "\n") == 1);
    CHECK(strcmp(parser.line(), "") == 0);
    CHECK(parser.length() == 0);

    // A bare CR in the middle of a line is data, not a terminator.
    CHECK(feed_all(parser, "a\rb\n") == 1);
    CHECK(strcmp(parser.line(), "a\rb") == 0);
    CHECK(parser.length() == 3);
}

static void test_multiple_lines() {
    char buf[32];
    LineParser parser(buf, sizeof(buf));

    CHECK(feed_all(parser, "one\ntwo\r\nthree\n") == 3);
    CHECK(strcmp(parser.line(), "three") == 0);
}

static void test_overflow_truncates() {
    char buf[8];  // longest reportable line is 7 characters
    LineParser parser(buf, sizeof(buf));

    CHECK(feed_all(parser, "0123456789\n") == 1);
    CHECK(parser.truncated());
    CHECK(parser.length() == 7);
    CHECK(strcmp(parser.line(), "0123456") == 0);
    Serial1.printf("INFO truncated_to=%s\n", parser.line());

    // The truncation flag must not leak into the next line.
    CHECK(feed_all(parser, "ok\n") == 1);
    CHECK(!parser.truncated());
    CHECK(strcmp(parser.line(), "ok") == 0);
}

static void test_reset_discards_partial() {
    char buf[32];
    LineParser parser(buf, sizeof(buf));

    feed_all(parser, "partial");
    parser.reset();
    CHECK(feed_all(parser, "clean\n") == 1);
    CHECK(strcmp(parser.line(), "clean") == 0);
}

void setup() {
    Serial1.begin(115200);
    Serial1.println("BEGIN-TEST");

    Serial1.printf("INFO sizeof_pointer=%u\n", (unsigned)sizeof(void*));
    Serial1.printf("INFO sizeof_size_t=%u\n", (unsigned)sizeof(size_t));
    Serial1.printf("INFO sizeof_LineParser=%u\n", (unsigned)sizeof(LineParser));

    test_simple_line();
    test_crlf_and_empty();
    test_multiple_lines();
    test_overflow_truncates();
    test_reset_discards_partial();

    Serial1.printf("END-TEST checks=%d failures=%d\n", checks, failures);
}

void loop() {}
