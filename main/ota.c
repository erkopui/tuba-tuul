// Firmware update over the air (OTA).
//
// The flash has two firmware slots (partitions.csv). A new firmware is
// written to the slot that is not running and started at the next reboot. It
// then runs on trial until ota_confirm() tells that it can be reached, and so
// can be updated again. Without that the board goes back to the previous
// firmware: after OTA_CONFIRM_MIN minutes, or at once when it crashes or is
// reset before.
//
// Before anything is written the start of the file is checked: it must be
// this project and for this board type. That the image is complete and
// undamaged ESP-IDF checks at the end, and the bootloader again at every start.

#include <stdatomic.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "config.h"
#include "ota.h"

#ifndef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "the trial of a new firmware needs CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, see sdkconfig.defaults"
#endif

// Our own description of the firmware. The linker puts it right behind the
// one of ESP-IDF (esp_app_desc_t), near the start of the firmware file.
typedef struct {
    uint32_t magic;
    char type[16];      // board this firmware is for
} board_desc_t;

#define BOARD_DESC_MAGIC    0x44524f42      // "BORD"
#define APP_DESC_OFFSET     (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

_Static_assert(sizeof(BOARD_TYPE) <= sizeof(((board_desc_t *)0)->type), "BOARD_TYPE is too long");
_Static_assert(APP_DESC_OFFSET + sizeof(esp_app_desc_t) + sizeof(board_desc_t) <= OTA_CHUNK,
               "the first piece of a firmware file must hold both descriptions");

static const __attribute__((section(".rodata_custom_desc"))) board_desc_t board_desc = {
    .magic = BOARD_DESC_MAGIC,
    .type = BOARD_TYPE,
};

const char ota_failed[] = "cannot store the firmware";

static atomic_bool on_trial;    // the running firmware is new and not confirmed yet
static esp_ota_handle_t handle;

void ota_init(void)
{
    esp_ota_img_states_t state;

    on_trial = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK
            && state == ESP_OTA_IMG_PENDING_VERIFY;
}

// Confirming and giving up can come from two tasks at the same moment, the
// one that takes on_trial away is the one that acts.
void ota_confirm(void)
{
    if (atomic_exchange(&on_trial, false)) {
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

void ota_poll(void)
{
    if (esp_timer_get_time() > OTA_CONFIRM_MIN * 60000000LL && atomic_exchange(&on_trial, false)) {
        // Comes back only when there is no previous firmware to go to, then this one stays.
        esp_ota_mark_app_invalid_rollback_and_reboot();
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

const char *ota_slot(void)
{
    return esp_ota_get_running_partition()->label;
}

size_t ota_size_max(void)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    return next ? next->size : 0;
}

const char *ota_begin(const uint8_t *image_start, size_t size)
{
    const esp_image_header_t *image = (const void *)image_start;
    const esp_app_desc_t *app = (const void *)(image_start + APP_DESC_OFFSET);
    const board_desc_t *board = (const void *)(app + 1);

    if (size < APP_DESC_OFFSET + sizeof(*app) + sizeof(*board) || image->magic != ESP_IMAGE_HEADER_MAGIC
            || image->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID || app->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "not a firmware file for this chip";
    }
    if (strncmp(app->project_name, esp_app_get_description()->project_name, sizeof(app->project_name)) != 0) {
        return "firmware of another project";
    }
    if (board->magic != BOARD_DESC_MAGIC || strncmp(board->type, board_desc.type, sizeof(board->type)) != 0) {
        return "firmware for another board, this is " BOARD_TYPE;
    }
    // Erasing piece by piece as it is written, not the whole slot before.
    if (esp_ota_begin(esp_ota_get_next_update_partition(NULL), OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        return ota_failed;
    }
    return ota_write(image_start, size);
}

const char *ota_write(const uint8_t *data, size_t size)
{
    return esp_ota_write(handle, data, size) == ESP_OK ? NULL : ota_failed;
}

const char *ota_end(void)
{
    esp_err_t err = esp_ota_end(handle);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        return "the firmware file is damaged or not complete";
    }
    return err == ESP_OK && esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL)) == ESP_OK
           ? NULL : ota_failed;
}

void ota_abort(void)
{
    esp_ota_abort(handle);      // does nothing when no firmware is being written
}
