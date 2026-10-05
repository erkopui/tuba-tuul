#ifndef _MTIMER_H_
#define _MTIMER_H_

#include <stdint.h>

#define mtimer_timeout_clear(mtimer) mtimer_timeout_set((mtimer), 0)

struct mtimer_t {
	uint32_t sec;
	uint16_t msec;
};

typedef struct mtimer_t mtimer_t;

/**
 * mtimer initialization
 *
 * Initializes the library with a hardware timer function pointer.
 *
 * @param  timer_get hardware timer function
 */
void mtimer_init(uint32_t (*timer_get)(uint16_t *msec));
uint32_t mtimer_get(mtimer_t *mtimer);
void mtimer_timeout_set(mtimer_t *mtimer, uint32_t msec);
void mtimer_timeout_add(mtimer_t *mtimer, uint32_t msec);
uint8_t mtimer_timeout(mtimer_t *mtimer);
uint32_t mtimer_elapsed(mtimer_t *before, mtimer_t *after);
int mtimer_cleared(mtimer_t *mtimer);

#endif

