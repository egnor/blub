#pragma once

#include <etl/string.h>
#include <ok_logging.h>

#define VERIFY_A_OP_B_STR(a, op, b) if (!(a op b)) {  \
    etl::string_view const av(a), bv(b);  \
    OK_REPORT_SOURCE(  \
      OK_ERROR_LEVEL, "VERIFY-FAIL: %s %s %s\n  %s = [%.*s]\n  %s = [%.*s]",  \
      #a, #op, #b, #a, av.size(), av.data(), #b, bv.size(), bv.data()  \
    )  \
  } else {}
