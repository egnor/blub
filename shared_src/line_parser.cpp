// See line_parser.h. ILLUSTRATIVE STUB -- delete or replace.

#include "line_parser.h"

LineParser::LineParser(char* buffer, size_t capacity)
    : buffer_(buffer), capacity_(capacity) {
    buffer_[0] = '\0';
}

void LineParser::reset() {
    pending_ = 0;
    pending_truncated_ = false;
}

bool LineParser::feed(char byte) {
    if (byte != '\n') {
        if (pending_ + 1 < capacity_) {
            buffer_[pending_++] = byte;
        } else {
            pending_truncated_ = true;
        }
        return false;
    }

    // Strip the CR of a CRLF pair, but leave a lone CR mid-line alone.
    if (pending_ > 0 && buffer_[pending_ - 1] == '\r') --pending_;

    buffer_[pending_] = '\0';
    length_ = pending_;
    truncated_ = pending_truncated_;
    pending_ = 0;
    pending_truncated_ = false;
    return true;
}
