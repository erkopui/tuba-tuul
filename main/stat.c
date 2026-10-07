// Pass times of our tasks, for GET /api/tasks.
//
// The time is the one on the clock, so a pass that was interrupted by a task
// of higher priority (WiFi, TCP/IP) or by an interrupt counts with that.

#include <stdint.h>
#include <stdio.h>
#include <sys/param.h>
#include "esp_timer.h"
#include "stat.h"

static const char *const names[STAT_COUNT] = { "main loop", "LED", "web request", "Modbus write", "VPN", "display" };

static struct {
    int64_t start_us;
    uint32_t last_us, max_us, max_at_ms, gap_us, gap_max_us, gap_max_at_ms, count;
} stats[STAT_COUNT];

void stat_begin(int stat_idx)
{
    int64_t now_us = esp_timer_get_time();

    if (stats[stat_idx].start_us) {
        // More than 71 minutes do not fit, the largest number stands for them.
        stats[stat_idx].gap_us = MIN(now_us - stats[stat_idx].start_us, UINT32_MAX);
        if (stats[stat_idx].gap_us > stats[stat_idx].gap_max_us) {
            stats[stat_idx].gap_max_us = stats[stat_idx].gap_us;
            stats[stat_idx].gap_max_at_ms = now_us / 1000;
        }
    }
    stats[stat_idx].start_us = now_us;
}

void stat_end(int stat_idx)
{
    int64_t now_us = esp_timer_get_time();
    uint32_t us = now_us - stats[stat_idx].start_us;

    stats[stat_idx].last_us = us;
    if (us > stats[stat_idx].max_us) {
        stats[stat_idx].max_us = us;
        stats[stat_idx].max_at_ms = now_us / 1000;
    }
    stats[stat_idx].count++;
}

void stat_json(char *buf, size_t buf_size)
{
    int len = 0;

    for (int i = 0; i < STAT_COUNT && len < (int)buf_size; i++) {
        len += snprintf(buf + len, buf_size - len,
                        "%c{\"name\":\"%s\",\"last_us\":%u,\"max_us\":%u,\"max_at_ms\":%u,"
                        "\"gap_us\":%u,\"gap_max_us\":%u,\"gap_max_at_ms\":%u,\"count\":%u}",
                        i ? ',' : '[', names[i], (unsigned)stats[i].last_us, (unsigned)stats[i].max_us,
                        (unsigned)stats[i].max_at_ms, (unsigned)stats[i].gap_us, (unsigned)stats[i].gap_max_us,
                        (unsigned)stats[i].gap_max_at_ms, (unsigned)stats[i].count);
    }
    if (len < (int)buf_size) {
        snprintf(buf + len, buf_size - len, "]");
    }
}
