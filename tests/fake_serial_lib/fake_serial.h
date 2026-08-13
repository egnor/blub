#pragma once

#include <Arduino.h>
#include <etl/circular_buffer.h>
#include <etl/memory.h>
#include <stdint.h>

class FakeSerial: public arduino::HardwareSerial {
 public:
  FakeSerial(int write_size, int read_size);
  virtual ~FakeSerial() = default;

  void begin(unsigned long baud) override { this->baud = baud; }
  void begin(unsigned long baud, uint16_t conf) override { this->baud = baud; }
  void end() override { this->baud = 0; }
  int available() override { return read_buf.size(); }
  int peek() override { return read_buf.empty() ? -1 : read_buf.front(); }
  int read() override { auto v = peek(); if (v >= 0) read_buf.pop(); return v; }
  size_t write(uint8_t c) override { write_buf.push(c); return 1; }
  void flush() override {}
  int availableForWrite() override { return write_buf.available(); }
  operator bool() { return baud != 0; }

  unsigned long baud = 0;
  etl::circular_buffer_ext<uint8_t> read_buf;
  etl::circular_buffer_ext<uint8_t> write_buf;

 private:
  etl::unique_ptr<uint8_t[]> read_storage;
  etl::unique_ptr<uint8_t[]> write_storage;
};
