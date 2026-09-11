#pragma once

#include <etl/string.h>
#include <ok_logging.h>

#define VERIFY_A_OP_B_STR(a, op, b) ({  \
    etl::string_view const _av(a), _bv(b);  \
    bool const _result = (_av op _bv);  \
    if (!_result) OK_REPORT_SOURCE(  \
      OK_ERROR_LEVEL, \
      "#TEST-FAIL# %s %s %s\n  %s (%db) = [%.*s]\n  %s (%db) = [%.*s]",  \
      #a, #op, #b, \
      #a, _av.size(), _av.size(), _av.data(), \
      #b, _bv.size(), _bv.size(), _bv.data()  \
    );  \
    _result;  \
  })

#define VERIFY_A_OP_B_INT(a, op, b) ({  \
    long long int const _av(a), _bv(b);  \
    bool const _result = (_av op _bv);  \
    if (!_result) OK_REPORT_SOURCE(  \
      OK_ERROR_LEVEL, "#TEST-FAIL# %s %s %s\n  %s = %lld\n  %s = %lld",  \
      #a, #op, #b, #a, _av, #b, _bv \
    );  \
    _result;  \
  })
