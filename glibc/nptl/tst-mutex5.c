/* Copyright (C) 2002-2019 Free Software Foundation, Inc.
   This file is part of the GNU C Library.
   Contributed by Ulrich Drepper <drepper@redhat.com>, 2002.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <http://www.gnu.org/licenses/>.  */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>
#include <config.h>
#include <support/check.h>
#include <support/timespec.h>


#ifndef TYPE
# define TYPE PTHREAD_MUTEX_NORMAL
#endif


static int
do_test (void)
{
  pthread_mutex_t m;
  pthread_mutexattr_t a;

  TEST_COMPARE (pthread_mutexattr_init (&a), 0);
  TEST_COMPARE (pthread_mutexattr_settype (&a, TYPE), 0);

#ifdef ENABLE_PI
  TEST_COMPARE (pthread_mutexattr_setprotocol (&a, PTHREAD_PRIO_INHERIT), 0);
#endif

  int err = pthread_mutex_init (&m, &a);
  if (err != 0)
    {
#ifdef ENABLE_PI
      if (err == ENOTSUP)
        FAIL_UNSUPPORTED ("PI mutexes unsupported");
#endif
      FAIL_EXIT1 ("mutex_init failed");
    }

  TEST_COMPARE (pthread_mutexattr_destroy (&a), 0);
  TEST_COMPARE (pthread_mutex_lock (&m), 0);
  if (pthread_mutex_trylock (&m) == 0)
    FAIL_EXIT1 ("mutex_trylock succeeded");

  /* Wait 2 seconds.  */
  struct timespec ts_timeout = timespec_add (xclock_now (CLOCK_REALTIME),
                                             make_timespec (2, 0));

  TEST_COMPARE (pthread_mutex_timedlock (&m, &ts_timeout), ETIMEDOUT);
  TEST_TIMESPEC_BEFORE_NOW (ts_timeout, CLOCK_REALTIME);

  /* The following makes the ts value invalid.  */
  ts_timeout.tv_nsec += 1000000000;

  TEST_COMPARE (pthread_mutex_timedlock (&m, &ts_timeout), EINVAL);
  TEST_COMPARE (pthread_mutex_unlock (&m), 0);

  const struct timespec ts_start = xclock_now (CLOCK_REALTIME);

  /* Wait 2 seconds.  */
  ts_timeout = timespec_add (ts_start, make_timespec (2, 0));

  TEST_COMPARE (pthread_mutex_timedlock (&m, &ts_timeout), 0);

  const struct timespec ts_end = xclock_now (CLOCK_REALTIME);

  /* Check that timedlock didn't delay.  We use a limit of 0.1 secs.  */
  TEST_TIMESPEC_BEFORE (ts_end,
                        timespec_add (ts_start, make_timespec (0, 100000000)));

  TEST_COMPARE (pthread_mutex_unlock (&m), 0);
  TEST_COMPARE (pthread_mutex_destroy (&m), 0);

  return 0;
}

#include <support/test-driver.c>
