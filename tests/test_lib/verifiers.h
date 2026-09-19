#pragma once

#include <etl/string.h>
#include <ok_logging.h>

#define VERIFY_TRUE(v) ({  \
    bool const _t = (v);  \
    if (!_t) OK_LOG_SOURCE(OK_CONTEXT, OK_ERROR_LEVEL, "#TEST-FAIL# %s", #v);  \
    _t;  \
  })

#define VERIFY_A_OP_B_STR(a, op, b) ({  \
    etl::string_view const _av(a), _bv(b);  \
    bool const _t = (_av op _bv);  \
    if (!_t) OK_LOG_SOURCE(  \
      OK_CONTEXT, OK_ERROR_LEVEL, \
      "#TEST-FAIL# %s %s %s\n  %s (%db) = [%.*s]\n  %s (%db) = [%.*s]",  \
      #a, #op, #b, \
      #a, _av.size(), _av.size(), _av.data(), \
      #b, _bv.size(), _bv.size(), _bv.data()  \
    );  \
    _t;  \
  })

#define VERIFY_A_OP_B_INT(a, op, b) ({  \
    long long int const _av(a), _bv(b);  \
    bool const _t = (_av op _bv);  \
    if (!_t) OK_LOG_SOURCE(  \
      OK_CONTEXT, OK_ERROR_LEVEL, \
      "#TEST-FAIL# %s %s %s\n  %s = %lld\n  %s = %lld",  \
      #a, #op, #b, #a, _av, #b, _bv \
    );  \
    _t;  \
  })

#define VERIFY_A_OP_B_FP(a, op, b) ({  \
    double const _av(a), _bv(b);  \
    bool const _t = (_av op _bv);  \
    if (!_t) OK_LOG_SOURCE(  \
      OK_CONTEXT, OK_ERROR_LEVEL, \
      "#TEST-FAIL# %s %s %s\n  %s = %g\n  %s = %g",  \
      #a, #op, #b, #a, _av, #b, _bv \
    );  \
    _t;  \
  })

#define VERIFY_A_NEAR_B_FP(a, b, within) ({  \
    double const _av(a), _bv(b), _wi(within);  \
    bool const _t = (fabs(_av - _bv) <= _wi);  \
    if (!_t) OK_LOG_SOURCE(  \
      OK_CONTEXT, OK_ERROR_LEVEL, \
      "#TEST-FAIL# %s ~= %s (+/- %g)\n  %s = %g\n  %s = %g",  \
      #a, #b, _wi, #a, _av, #b, _bv \
    );  \
    _t;  \
  })
