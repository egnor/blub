#include "fake_serial.h"

#include <ok_logging.h>

static OkLoggingContext OK_CONTEXT("fake_serial");

FakeSerial::FakeSerial(int read_size, int write_size)
  : read_buf(read_size), write_buf(write_size) {
  read_storage.reset(new uint8_t[read_size + 1]);
  write_storage.reset(new uint8_t[write_size + 1]);
  read_buf.set_buffer(read_storage.get());
  write_buf.set_buffer(write_storage.get());
}
