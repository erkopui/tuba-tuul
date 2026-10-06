#pragma once

#include <stddef.h>
#include <stdint.h>

#define OTA_CHUNK   4096    // a firmware file is passed in pieces of this size, only the last one is shorter

void ota_init(void);
void ota_poll(void);        // call from the main loop

// Tells that the running firmware can be reached, which ends its trial after
// an update. Without it the board goes back to the previous firmware.
void ota_confirm(void);

const char *ota_slot(void);     // slot of the running firmware: "ota_0" or "ota_1"
size_t ota_size_max(void);      // largest firmware file that fits a slot

// Writing a new firmware: ota_begin() with the first piece of the file, which
// is checked to be a firmware for this board, ota_write() with every further
// piece, then ota_end(), which checks the whole image and makes it the one to
// start at the next reboot. Each returns NULL, or what is wrong: with the
// file, or ota_failed when the board could not store it. After a problem,
// or when the file does not arrive completely, call ota_abort().
const char *ota_begin(const uint8_t *image_start, size_t size);
const char *ota_write(const uint8_t *data, size_t size);
const char *ota_end(void);
void ota_abort(void);
extern const char ota_failed[];
