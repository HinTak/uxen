/*
 * Copyright 2012-2015, Bromium, Inc.
 * Author: Christian Limpach <Christian.Limpach@gmail.com>
 * SPDX-License-Identifier: ISC
 */

#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <stdint.h>
#include <stdio.h>

#include "clock.h"

#define dprintf(fmt, ...) debug_printf(fmt, ## __VA_ARGS__)

#define plog(fmt, ...) debug_printf("%07"PRId64": %s:%d " fmt "\n",     \
                                    os_get_clock_ms() % 10000000,	\
                                    __FUNCTION__, __LINE__,             \
                                    ## __VA_ARGS__)

#ifndef LIBIMG
void __attribute__ ((__format__ (printf, 1, 0)))
debug_vprintf(const char *fmt, va_list ap);
void __attribute__ ((__format__ (printf, 1, 2)))
debug_printf(const char *fmt, ...);
void __attribute__ ((__format__ (printf, 1, 2)))
error_printf(const char *fmt, ...);
#else
#define debug_vprintf(fmt, ap) fvprintf(stderr, fmt, ap)
#define debug_printf(...) fprintf(stderr, __VA_ARGS__)
#define error_printf(...) fprintf(stderr, __VA_ARGS__)
#endif

void logstyle_set(const char *logstyle);

#endif	/* _DEBUG_H_ */
