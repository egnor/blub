// Conveniently update text on small mono screens (via U8g2)

#pragma once

#include <memory>

#include "U8g2lib.h"

class LittleStatus {
  public:
    static constexpr int MIN_SIZE = 6;
    static constexpr int MAX_SIZE = 15;

    virtual ~LittleStatus() = default;
    virtual void line_printf(int line, char const* format, ...) = 0;
    virtual void set_line_size(int line, int size) = 0;
    virtual U8G2* raw_display() const = 0;
};

std::unique_ptr<LittleStatus> make_little_status(std::unique_ptr<U8G2> display);
