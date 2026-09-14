// ETL configuration for blub code.
// ETL includes "eth_profile.h" guarded by __has_include_,
// so it will NOT find this in a sketch dir but WILL find it in a library.

#pragma once

#define ETL_CHECK_PUSH_POP 1
#define ETL_LOG_ERRORS 1
#define ETL_USE_OK_LOGGING 1
#define ETL_VERBOSE_ERRORS 1

#if defined(F_CPU)
// see rp2040_etl_clock.cpp, which uses time_us_64()
#define ETL_CHRONO_STEADY_CLOCK_DURATION  \
    etl::chrono::duration<int64_t, etl::micro>
#endif
