#include <Arduino.h>
#include <etl/chrono.h>

#if defined(ARDUINO_ARCH_RP2040)
extern "C" ETL_CHRONO_STEADY_CLOCK_DURATION::rep etl_get_steady_clock() {
  return time_us_64();
}
#endif
