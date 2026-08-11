// Incremental line splitter for byte streams from serial devices.
//
// ILLUSTRATIVE STUB: this exists to give tests/test_line_parser something to
// test while the real client library is being written. Delete or replace it.

#pragma once

#include <stddef.h>

// Accumulates bytes and emits complete lines. Lines end with "\n" or "\r\n";
// the terminator is not included in the returned line. Uses a caller-supplied
// buffer and never allocates.
//
// Lines longer than the buffer are truncated rather than overflowing, and the
// truncation is reported so callers don't act on a partial line as if it were
// whole.
class LineParser {
  public:
    // Bytes are accumulated into buffer[0 .. capacity-1], with one byte
    // reserved for the NUL terminator, so the longest reportable line is
    // capacity - 1 characters.
    LineParser(char* buffer, size_t capacity);

    // Feeds one byte. Returns true if a complete line is now available from
    // line(); that line stays valid until the next feed().
    bool feed(char byte);

    // The most recently completed line, NUL terminated. Empty before the
    // first complete line.
    const char* line() const { return buffer_; }

    // Length of line(), not counting the NUL.
    size_t length() const { return length_; }

    // True if the most recently completed line dropped characters because it
    // did not fit in the buffer.
    bool truncated() const { return truncated_; }

    // Discards any partially accumulated line.
    void reset();

  private:
    char* const buffer_;
    const size_t capacity_;
    size_t pending_ = 0;         // bytes accumulated for the line in progress
    size_t length_ = 0;          // length of the last completed line
    bool pending_truncated_ = false;
    bool truncated_ = false;
};
