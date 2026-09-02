#include <Arduino.h>
#include <etl/chrono.h>

extern "C" ETL_CHRONO_STEADY_CLOCK_DURATION::rep etl_get_steady_clock() {
  return rp2040.getCycleCount64();
}
