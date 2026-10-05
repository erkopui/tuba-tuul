// REST API and the small web page.
//
//   GET  /api/fans        -> [0,50,0,100]       speed of each fan in percent
//   POST /api/fans/2      body "75"             set fan 2 to 75 %
//   POST /api/fans/all    body "30"             set every fan to 30 %
//        ...?fade=2000                          ramp time in ms for 0..100 %, default FAN_FADE_MS
//   POST /api/wifi        body "ssid\npassword" store WiFi credentials and reboot
//   GET  /api/wifi/scan   -> [{"ssid":"home","rssi":-52}, ...]  networks in range
//   GET  /                                      test page (index.html) with sliders and WiFi setup

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "config.h"
#include "fan.h"
#include "http.h"
#include "wifi.h"

#define STR_(x) #x
#define STR(x) STR_(x)

// main/index.html, embedded by CMakeLists.txt
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t index_get(httpd_req_t *req)
{
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t fans_get(httpd_req_t *req)
{
    char buf[8 * FAN_COUNT], *p = buf;
    for (int i = 0; i < FAN_COUNT; i++) {
        p += sprintf(p, "%c%d", i ? ',' : '[', fan_get(i));
    }
    strcpy(p, "]");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

// Read the whole (small) request body as a string.
static int read_body(httpd_req_t *req, char *buf, size_t size)
{
    int len = 0;
    if (req->content_len >= size) {
        return -1;
    }
    while (len < req->content_len) {
        int n = httpd_req_recv(req, buf + len, req->content_len - len);
        if (n <= 0) {
            return -1;
        }
        len += n;
    }
    buf[len] = 0;
    return len;
}

// Browsers send an Origin header with cross-site POSTs. Refuse those, so a
// web page somewhere else cannot change the fans or the WiFi behind our back.
// The page itself must be opened by our name or by IP address (with or
// without a port), any other name could be a foreign domain pointed at us
// (DNS rebinding).
static bool same_origin(httpd_req_t *req)
{
    char origin[80], host[64];

    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_ERR_NOT_FOUND) {
        return true;    // curl and friends
    }
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK
            && strncmp(origin, "http://", 7) == 0 && strcmp(origin + 7, host) == 0
            && (strcmp(host, HOSTNAME ".local") == 0 || strcmp(host, HOSTNAME) == 0
                || host[strspn(host, "0123456789.:")] == 0)) {
        return true;
    }
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "cross-site request");
    return false;
}

static esp_err_t fans_post(httpd_req_t *req)
{
    char body[8], query[32], val[8], *end;
    long fade = FAN_FADE_MS;

    if (!same_origin(req)) {
        return ESP_OK;
    }
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK
            && httpd_query_key_value(query, "fade", val, sizeof(val)) == ESP_OK) {
        fade = strtol(val, &end, 10);
        if (end == val || *end || fade < 0 || fade > FAN_FADE_MAX_MS) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fade must be 0.." STR(FAN_FADE_MAX_MS) " ms");
        }
    }
    // Fan number or "all" from the path, a query string is ignored.
    const char *id = req->uri + strlen("/api/fans/");
    bool all = strncmp(id, "all", 3) == 0 && (id[3] == 0 || id[3] == '?');

    if (read_body(req, body, sizeof(body)) <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body must be speed 0..100");
    }
    long pct = strtol(body, &end, 10);
    if (end == body || end[strspn(end, "\r\n")] || pct < 0 || pct > 100) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body must be speed 0..100");
    }

    long n = strtol(id, &end, 10);
    if (all) {
        for (int i = 0; i < FAN_COUNT; i++) {
            fan_set(i, pct, fade);
        }
    } else if (end != id && (*end == 0 || *end == '?') && n >= 1 && n <= FAN_COUNT) {
        fan_set(n - 1, pct, fade);
    } else {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such fan");
    }
    return fans_get(req);
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    static wifi_network_t networks[15];
    char buf[100];

    int count = wifi_scan(networks, sizeof(networks) / sizeof(networks[0]));
    if (count < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed, try again");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < count; i++) {
        char *p = buf + sprintf(buf, "%s{\"ssid\":\"", i ? "," : "");
        // JSON string: escape quote and backslash, drop control characters.
        for (const char *s = networks[i].ssid; *s; s++) {
            if (*s == '"' || *s == '\\') {
                *p++ = '\\';
            }
            if ((unsigned char)*s >= 0x20) {
                *p++ = *s;
            }
        }
        sprintf(p, "\",\"rssi\":%d}", networks[i].rssi);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char body[32 + 64 + 8];     // ssid + password + line ends

    if (!same_origin(req)) {
        return ESP_OK;
    }
    if (read_body(req, body, sizeof(body)) <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body must be ssid\\npassword");
    }
    // Split "ssid\npassword" in place, tolerating \r\n and a trailing newline.
    char *pass = body + strcspn(body, "\r\n");
    if (*pass) {
        *pass++ = 0;
        pass += *pass == '\n';
        pass[strcspn(pass, "\r\n")] = 0;
    }
    esp_err_t err = wifi_set_credentials(body, pass);
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid must be 1..32 and password 0..64 bytes");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    httpd_resp_sendstr(req, "saved, rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

void http_start(void)
{
    httpd_handle_t server;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 1;
    config.lru_purge_enable = true;     // drop the oldest idle connection when all slots are taken
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    static const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = index_get },
        { .uri = "/api/fans",    .method = HTTP_GET,  .handler = fans_get },
        { .uri = "/api/fans/*",  .method = HTTP_POST, .handler = fans_post },
        { .uri = "/api/wifi",    .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get },
    };
    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}
