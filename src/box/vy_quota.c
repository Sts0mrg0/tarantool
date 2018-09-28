/*
 * Copyright 2010-2018, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "vy_quota.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <tarantool_ev.h>

#include "fiber.h"
#include "fiber_cond.h"
#include "say.h"
#include "trivia/util.h"

/**
 * Return true if the requested amount of memory may be consumed
 * right now, false if consumers have to wait.
 */
static inline bool
vy_quota_may_use(struct vy_quota *q, size_t size)
{
	if (q->used + size > q->limit)
		return false;
	return true;
}

/**
 * Consume the given amount of memory without checking the limit.
 */
static inline void
vy_quota_do_use(struct vy_quota *q, size_t size)
{
	q->used += size;
}

/**
 * Return the given amount of memory without waking blocked fibers.
 * This function is an exact opposite of vy_quota_do_use().
 */
static inline void
vy_quota_do_unuse(struct vy_quota *q, size_t size)
{
	assert(q->used >= size);
	q->used -= size;
}

/**
 * Invoke the registered callback in case memory usage exceeds
 * the configured limit.
 */
static inline void
vy_quota_check_limit(struct vy_quota *q)
{
	if (q->used > q->limit)
		q->quota_exceeded_cb(q);
}

/**
 * Wake up the first consumer in the line waiting for quota.
 */
static void
vy_quota_signal(struct vy_quota *q)
{
	fiber_cond_signal(&q->cond);
}

void
vy_quota_create(struct vy_quota *q, vy_quota_exceeded_f quota_exceeded_cb)
{
	q->limit = SIZE_MAX;
	q->used = 0;
	q->too_long_threshold = TIMEOUT_INFINITY;
	q->quota_exceeded_cb = quota_exceeded_cb;
	fiber_cond_create(&q->cond);
}

void
vy_quota_destroy(struct vy_quota *q)
{
	fiber_cond_broadcast(&q->cond);
	fiber_cond_destroy(&q->cond);
}

void
vy_quota_set_limit(struct vy_quota *q, size_t limit)
{
	q->limit = limit;
	vy_quota_check_limit(q);
	vy_quota_signal(q);
}

void
vy_quota_force_use(struct vy_quota *q, size_t size)
{
	vy_quota_do_use(q, size);
	vy_quota_check_limit(q);
}

void
vy_quota_release(struct vy_quota *q, size_t size)
{
	vy_quota_do_unuse(q, size);
	vy_quota_signal(q);
}

int
vy_quota_use(struct vy_quota *q, size_t size, double timeout)
{
	if (vy_quota_may_use(q, size)) {
		vy_quota_do_use(q, size);
		return 0;
	}

	/* Wait for quota. */
	double wait_start = ev_monotonic_now(loop());
	double deadline = wait_start + timeout;

	do {
		/*
		 * If the requested amount of memory cannot be
		 * consumed due to the configured limit, notify
		 * the caller before going to sleep so that it
		 * can start memory reclaim immediately.
		 */
		if (q->used + size > q->limit)
			q->quota_exceeded_cb(q);
		if (fiber_cond_wait_deadline(&q->cond, deadline) != 0)
			return -1; /* timed out */
	} while (!vy_quota_may_use(q, size));

	double wait_time = ev_monotonic_now(loop()) - wait_start;
	if (wait_time > q->too_long_threshold) {
		say_warn("waited for %zu bytes of vinyl memory quota "
			 "for too long: %.3f sec", size, wait_time);
	}

	vy_quota_do_use(q, size);
	/*
	 * Blocked consumers are awaken one by one to preserve
	 * the order they were put to sleep. It's a responsibility
	 * of a consumer that managed to acquire the requested
	 * amount of quota to wake up the next one in the line.
	 */
	vy_quota_signal(q);
	return 0;
}

void
vy_quota_adjust(struct vy_quota *q, size_t reserved, size_t used)
{
	if (reserved > used) {
		vy_quota_do_unuse(q, reserved - used);
		vy_quota_signal(q);
	}
	if (reserved < used) {
		vy_quota_do_use(q, used - reserved);
		vy_quota_check_limit(q);
	}
}
