// Modbus TCP server on port 502, using the esp-modbus component.
//
// Holding registers 0..FAN_COUNT-1 are the fan speeds in percent (functions
// 3, 6 and 16). The register memory is fan_percent[] of fan.c: the Modbus
// driver reads and writes it by itself, modbus_task is told afterwards and
// puts a written speed on the output.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "mbcontroller.h"
#include "config.h"
#include "fan.h"
#include "modbus.h"
#include "stat.h"

static void *slave;

static void modbus_task(void *arg)
{
    for (;;) {
        // The driver queues a note about every access, reads too. Take them
        // all, so the queue never fills up, and act on the writes.
        mb_param_info_t info;
        if (mbc_slave_get_param_info(slave, &info, 1000) != ESP_OK || !(info.type & MB_EVENT_HOLDING_REG_WR)) {
            continue;
        }
        stat_begin(STAT_MODBUS);
        // The driver has already stored the new speeds in fan_percent[],
        // fan_set() limits them to 100 and puts them on the outputs.
        for (int i = info.mb_offset; i < info.mb_offset + (int)info.size && i < FAN_COUNT; i++) {
            mbc_slave_lock(slave);
            int percent = fan_percent[i];
            mbc_slave_unlock(slave);
            fan_set(i, percent, FAN_FADE_MS);
        }
        stat_end(STAT_MODBUS);
    }
}

void modbus_start(void)
{
    mb_communication_info_t comm = {
        .tcp_opts.mode = MB_TCP,
        .tcp_opts.port = MODBUS_PORT,
        .tcp_opts.addr_type = MB_IPV4,
        .tcp_opts.ip_netif_ptr = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"),
    };
    ESP_ERROR_CHECK(mbc_slave_create_tcp(&comm, &slave));

    // Tell the driver where the holding registers live.
    mb_register_area_descriptor_t area = {
        .type = MB_PARAM_HOLDING,
        .start_offset = 0,
        .address = fan_percent,
        .size = sizeof(fan_percent),
        .access = MB_ACCESS_RW,
    };
    ESP_ERROR_CHECK(mbc_slave_set_descriptor(slave, area));
    ESP_ERROR_CHECK(mbc_slave_start(slave));

    xTaskCreatePinnedToCore(modbus_task, "modbus", 4096, NULL, 5, NULL, 1);
}
