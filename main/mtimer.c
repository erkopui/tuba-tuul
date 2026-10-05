#include "mtimer.h"

static uint32_t (*mtimer_timer_get)(uint16_t *msec);

void mtimer_init(uint32_t (*timer_get)(uint16_t *msec))
{
	mtimer_timer_get = timer_get;
}

uint32_t mtimer_get(mtimer_t *mtimer)
{
	uint16_t msec;
	uint32_t sec;

	sec = mtimer_timer_get(&msec);
	if (mtimer != ((void *)0)) {
		mtimer->sec = sec;
		mtimer->msec = msec;
	}
	return sec;
}

void mtimer_timeout_set(mtimer_t *mtimer, uint32_t msec)
{
	if (!msec) {
		mtimer->sec = 0;
		mtimer->msec = 0;
		return;
	}
	mtimer_get(mtimer);
	mtimer->sec += (msec / 1000);
	mtimer->msec += (msec % 1000);
	if (mtimer->msec >= 1000) {
		mtimer->msec -= 1000;
		mtimer->sec++;
	}
}

void mtimer_timeout_add(mtimer_t *mtimer, uint32_t msec)
{
	if (!msec)
		return;

	if (!mtimer->sec && !mtimer->msec)
		mtimer_get(mtimer);

	mtimer->sec += (msec / 1000);
	mtimer->msec += (msec % 1000);
	if (mtimer->msec >= 1000) {
		mtimer->msec -= 1000;
		mtimer->sec++;
	}
}

uint8_t mtimer_timeout(mtimer_t *mtimer)
{
	mtimer_t t;

	if (!mtimer->sec && !mtimer->msec)
		return 1;

	mtimer_get(&t);
	if (mtimer->sec > t.sec)
		return 0;
	if (mtimer->sec < t.sec)
		return 1;
	if (mtimer->msec > t.msec)
		return 0;

	return 1;
}

uint32_t mtimer_elapsed(mtimer_t *before, mtimer_t *after)
{
	if (before->sec > after->sec)
		return 0;

	if (before->sec == after->sec) {
		if (before->msec > after->msec)
			return 0;

		return (after->msec - before->msec);
	}
	if (before->msec > after->msec)
		return (((1000 - before->msec) + after->msec) + (1000 * (after->sec - before->sec - 1)));

	return ((after->msec - before->msec) + (1000 * (after->sec - before->sec)));
}

int mtimer_cleared(mtimer_t *mtimer)
{
	return ((mtimer->sec == 0) && (mtimer->msec == 0));
}
