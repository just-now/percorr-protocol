#include "sm.h"
#include <stddef.h> /* NULL */
#include <stdio.h> /* fprintf */
#include <time.h>      /* syscall, getpid */


static bool sm_is_locked(const struct sm *m)
{
	return ERGO(m->is_locked, m->is_locked(m));
}

int sm_state(const struct sm *m)
{
	PRE(sm_is_locked(m));
	return m->state;
}

static inline void sm_obs(const struct sm *m)
{
	struct timespec ts = {0};
	struct tm tm;

	clock_gettime(CLOCK_REALTIME, &ts);
	gmtime_r(&ts.tv_sec, &tm);

	fprintf(stderr,
		"LIBDQLITE[%6.6u] %04d-%02d-%02dT%02d:%02d:%02d.%09lu "
		"%s pid: %lu sm_id: %lu %s |\n",
		111,

		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		tm.tm_min, tm.tm_sec, (unsigned long)ts.tv_nsec,

		m->name, 111UL, m->id, m->conf[sm_state(m)].name);
}

void sm_to_sm_obs(const struct sm *from, const struct sm *to)
{
	struct timespec ts = {0};
	struct tm tm;

	clock_gettime(CLOCK_REALTIME, &ts);
	gmtime_r(&ts.tv_sec, &tm);

	fprintf(stderr,
		"LIBDQLITE[%6.6u] %04d-%02d-%02dT%02d:%02d:%02d.%09lu "
		"%s-to-%s opid: %lu dpid: %lu id: %lu id: %lu |\n",
		111,

		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		tm.tm_min, tm.tm_sec, (unsigned long)ts.tv_nsec,

		from->name, to->name, 111UL, 111UL, from->id, to->id);
}

void sm_init(struct sm *m,
	     bool (*invariant)(const struct sm *, int),
	     bool (*is_locked)(const struct sm *),
	     const struct sm_conf *conf,
	     int state)
{
	static unsigned long int id = 0;

	PRE(conf[state].flags & SM_INITIAL);

	m->conf = conf;
	m->state = state;
	m->invariant = invariant;
	m->is_locked = is_locked;
	m->id = ++id;

	POST(m->invariant != NULL && m->invariant(m, SM_PREV_NONE));
	sm_obs(m);
}

void sm_fini(struct sm *m)
{
	PRE(m->invariant != NULL && m->invariant(m, SM_PREV_NONE));
	PRE(m->conf[sm_state(m)].flags & SM_FINAL);
}

void sm_move(struct sm *m, int next_state)
{
	int prev = sm_state(m);

	printf("SM_MOVE %s => %s\n", m->conf[prev].name, m->conf[next_state].name);

	PRE(sm_is_locked(m));
	PRE(m->conf[sm_state(m)].allowed & BITS(next_state));
	m->state = next_state;
	sm_obs(m);
	POST(m->invariant != NULL && m->invariant(m, prev));
}

void sm_fail(struct sm *m, int fail_state, int rc)
{
	int prev = sm_state(m);
	PRE(sm_is_locked(m));
	PRE(rc != 0 && m->rc == 0);
	PRE(m->conf[fail_state].flags & SM_FAILURE);
	PRE(m->conf[sm_state(m)].allowed & BITS(fail_state));

	m->rc = rc;
	m->state = fail_state;
	POST(m->invariant != NULL && m->invariant(m, prev));
}
