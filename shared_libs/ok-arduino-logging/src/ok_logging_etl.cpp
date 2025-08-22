// Adapter for the Embedded Template Library (ETL) to use ok_logging.h macros
// Only active if ETL_LOG_ERRORS is defined for the project

#ifdef ETL_LOG_ERRORS

#include "Embedded_Template_Library.h"
#include "etl/error_handler.h"

#include "ok_logging.h"

static void etl_error_handler(etl::exception const& e) {
  ok_log(
      "ETL", OK_FATAL_LEVEL, "%s\n  at: %s:%d",
      e.what(), e.file_name(), e.line_number());
}

static int register_etl_error_handler() {
  etl::error_handler::set_callback<etl_error_handler>();
  return 0;
}

// Relies on etl::error_handler::set_callback being global initializer safe
static int global_init = register_etl_error_handler();

#endif
