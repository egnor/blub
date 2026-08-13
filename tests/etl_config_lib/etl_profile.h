// ETL configuration for blub tests.
//
// ETL includes "eth_profile.h" guarded by __has_include_,
// so it will NOT find this in a sketch dir but WILL find it in a library.

#pragma once

#define ETL_LOG_ERRORS 1
#define ETL_VERBOSE_ERRORS 1
#define ETL_USE_OK_LOGGING 1
