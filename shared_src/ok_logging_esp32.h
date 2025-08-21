// Helpers for ok_logging.h for ESP-IDF error codes

#pragma once

#ifdef ESP32

  #include <esp_err.h>

  #include "ok_logging.h"

  #define OK_LOG_ESP_ERRORS(x) ({ \
      esp_err_t const rc_ = (x); \
      if (rc_ != ESP_OK) OK_REPORT_SOURCE(  \
          OK_ERROR_LEVEL, "%s (%d)\nfrom: %s", esp_err_to_name(rc_), rc_, #x); \
      rc_; \
    })

  #define OK_FATAL_ESP_ERRORS(x) ({ \
      esp_err_t const rc_ = (x); \
      if (rc_ != ESP_OK) \
        OK_FATAL("%s (%d)\nfrom: %s", esp_err_to_name(rc_), rc_, #x); \
    })

#endif  // ESP32
