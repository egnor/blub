#pragma once

#include <stdint.h>

template <typename T>
class __attribute__((packed)) BigEndianStorage {
 public:
  BigEndianStorage(T value) : value(convert(value)) {}
  operator T() const { return convert(value); }
 private:
  T value;
  static constexpr T convert(T in) {
    T out;
    uint8_t* inb = reinterpret_cast<uint8_t*>(&in);
    uint8_t* outb = reinterpret_cast<uint8_t*>(&out);
    for (int i = 0; i < sizeof(T); ++i) outb[i] = inb[sizeof(T) - i - 1];
    return out;
  }
};

using uint16_be = BigEndianStorage<uint16_t>;
using uint32_be = BigEndianStorage<uint32_t>;

using int16_be = BigEndianStorage<int16_t>;
using int32_be = BigEndianStorage<int32_t>;
