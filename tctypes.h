/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef __TCTYPES_H__
#define __TCTYPES_H__

/* basic types */

typedef char tc8;
typedef unsigned char tcu8;
typedef short tc16;
typedef unsigned short tcu16;
typedef int tc32;
typedef unsigned int tcu32;

#if defined(LINUX)
# define TC64 "lld"
typedef long long tc64;
#elif defined(WIN32) || defined(_WIN32_WCE)
# define TC64 "I64d"
typedef __int64 tc64;
#elif defined(VXWORKS) || defined(NDS_NITRO)
# define TC64 "lld"
typedef long long tc64;
#elif defined(__uClinux__) && defined(CHIP_DM642)
# include <lddk.h>
# define TC64 "lld"
typedef long tc64;
#elif defined(__SYMBIAN32__)&&!defined(__WINS__)
#include "e32std.h"
#define TC64 "lld"
typedef TInt64 tc64;
#elif defined(__SYMBIAN32__)&&defined(__WINS__)
#define TC64 "lld"
typedef long long tc64;
#else
#define TC64 "lld"
typedef long long tc64;
#endif

#ifndef __SYMBIAN32__
/*not carrying over the I64INT as it actually has the potential to change the
  calculation as it effectively casts to int*/
# define I64REAL(x) ((double)(x))
#endif

#define tcFalse 0
#define tcTrue  1

/* END - basic types */

/* TrueCast return codes; common to all libs */

enum eTCRV
{
    TC_FILE_NOT_FOUND                       = -404,

    TC_BUFFER_UNDERRUN                      = -203,
    TC_BUFFER_EMPTY                         = -202,
    TC_BUFFER_FULL                          = -201,

    TC_MSG_TOO_LARGE                        = -102,
    TC_TIMEDOUT                             = -101,
    TC_WOULDBLOCK                           = -100,

    TC_INVALID_VERSION                      = -8,
    TC_INPROGRESS                           = -7,
    TC_INVALID_STATE_CHANGE                 = -6,
    TC_NO_PLUGIN                            = -5,
    TC_RESOURCE_LOCKED                      = -4,
    TC_NO_MEM                               = -3,
    TC_INVALID_PARAMS                       = -2,
    TC_ERROR                                = -1,
    TC_OK                                   = 0,

    TC_DONE                                 = 1
};
typedef enum eTCRV TCRV;

#endif //__TCTYPES_H__
