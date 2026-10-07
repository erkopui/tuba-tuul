#pragma once

#include <stddef.h>

// How long the work of each of our tasks takes: the last pass and the longest
// one since boot. A pass is what a task does between two waits. Also how
// often a task gets its turn: the number of passes, and the time from the
// start of one to the start of the next (the gap), the last and the longest.
// For each longest value also when it ended, in ms since boot.
enum {
    STAT_MAIN,      // one round of the main loop: WiFi, update trial
    STAT_LED,       // one look at the LED, about every 30 ms at the lowest priority
    STAT_HTTP,      // one request of the web server, from its headers read to the answer
    STAT_MODBUS,    // one Modbus write put on the fan outputs
    STAT_VPN,       // one connect attempt, or one look at the tunnel
    STAT_DISPLAY,   // one look at the displays, a changed picture is sent
    STAT_COUNT
};

// stat_idx: STAT_MAIN..STAT_DISPLAY. Call both from the task that is measured.
void stat_begin(int stat_idx);
void stat_end(int stat_idx);
// Writes all of them as JSON:
// [{"name":"main loop","last_us":40,"max_us":900,"max_at_ms":1500,"gap_us":20000,"gap_max_us":21000,
//   "gap_max_at_ms":1500,"count":1234}, ...]
void stat_json(char *buf, size_t buf_size);
