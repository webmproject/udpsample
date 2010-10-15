/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */


#ifdef WINDOWS
#include <windows.h>
#include <mmsystem.h>
unsigned int get_time(void)
{
    LARGE_INTEGER pf;
    long long now;
    QueryPerformanceCounter((LARGE_INTEGER *) &now);
    QueryPerformanceFrequency(&pf);
    return (unsigned int)(now * 1000 /  pf.LowPart);
}
#else
#include <time.h>
#include <sys/time.h>

unsigned int get_time(void)
{
    struct timespec  ts;
    unsigned long long tv;

#if _POSIX_TIMERS > 0
    clock_gettime(CLOCK_REALTIME, &ts);
#else
struct timeval tv2;
gettimeofday(&tv2, NULL);
ts.tv_sec = tv2.tv_sec;
ts.tv_nsec = tv2.tv_usec * 1000;
#endif
    tv = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return tv & 0xffffffff;
}
int _kbhit(void)
{
    struct timeval tv;
    fd_set read_fd;

    tv.tv_sec = 0;
    tv.tv_usec = 0;
    FD_ZERO(&read_fd);
    FD_SET(0, &read_fd);

    if (select(1, &read_fd, NULL, NULL, &tv) == -1)
        return 0;

    if (FD_ISSET(0, &read_fd))
        return 1;

    return 0;
}


#endif

#include "tctypes.h"
#include "rtp.h"
#include <stdarg.h>
#include <stdio.h>

unsigned int start_time = 0;

void vpxlog_dbg_no_head(int level, const tc8 *format, ...)
{
    va_list list;

    if (!(level & LOG_MASK))
        return;

    va_start(list, format);

    vprintf(format, list);
    va_end(list);
}

void vpxlog_dbg(int level, const tc8 *format, ...)
{
    va_list list;

    if (!(level & LOG_MASK))
        return;

    if (start_time == 0)
        start_time = get_time();

    printf("%8d ", get_time() - start_time);
    va_start(list, format);

    vprintf(format, list);
    va_end(list);
}

