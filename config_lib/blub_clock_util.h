// Utilities for working with etl::chrono clocks.

#pragma once 

#include <etl/chrono.h>

using millis64 = etl::chrono::duration<int64_t, etl::milli>;
using secf = etl::chrono::duration<float>;
using secd = etl::chrono::duration<double>;

template <typename TToDuration, typename TRep, typename TPeriod>
auto raw_count(etl::chrono::duration<TRep, TPeriod> d) {
  return etl::chrono::duration_cast<TToDuration>(d).count();
}

template <typename TToDuration, typename TClock, typename TDuration>
auto raw_count(etl::chrono::time_point<TClock, TDuration> tp) {
  return raw_count<TToDuration>(tp.time_since_epoch());
}
