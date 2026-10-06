// REST API and the small web page.
//
//   GET  /api/fans        -> [0,50,0,100]       speed of each fan in percent
//   POST /api/fans/2      body "75"             set fan 2 to 75 %
//   POST /api/fans/all    body "30"             set every fan to 30 %
//        ...?fade=2000                          ramp time in ms for 0..100 %, default FAN_FADE_MS
//   POST /api/wifi        body "ssid\npassword" store WiFi credentials and reboot
//   GET  /api/wifi/scan   -> [{"ssid":"home","rssi":-52}, ...]  networks in range
//   GET  /api/vpn         -> {"state":"up","tunnel":"10.0.0.2/24 via vpn.example.com:51820"}
//   POST /api/vpn         body = WireGuard client config   store it, the tunnel switches over
//   DELETE /api/vpn                             remove the VPN config and the tunnel
//   GET  /api/ota         -> {"version":"1.2","board":"fans8","slot":"ota_0"}   the running firmware
//   POST /api/ota         body = firmware file (.bin)   write it to the other slot and reboot into it
//   GET  /                                      test page (index.html) with sliders, WiFi, VPN and update

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "mbedtls/platform_util.h"
#include "config.h"
#include "fan.h"
#include "http.h"
#include "ota.h"
#include "vpn.h"
#include "wifi.h"

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

// Answers and reboots. A reboot right behind the answer loses it when the
// link is slow, so the client is asked to close the connection and that is
// waited for, 5 s at most.
static esp_err_t reply_and_reboot(httpd_req_t *req, const char *text)
{
    int sock = httpd_req_to_sockfd(req);
    struct timeval timeout = { .tv_sec = 5 };
    char byte;

    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, text);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    recv(sock, &byte, 1, 0);
    esp_restart();
    return ESP_OK;
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

    return reply_and_reboot(req, "saved, rebooting\n");
}

static esp_err_t vpn_get(httpd_req_t *req)
{
    char buf[256];

    // Neither text has characters that JSON needs escaped.
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"tunnel\":\"%s\"}", vpn_state(), vpn_tunnel());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t vpn_post(httpd_req_t *req)
{
    if (!same_origin(req)) {
        return ESP_OK;
    }
    char *conf_text = malloc(VPN_CONF_MAX);
    if (!conf_text) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    const char *problem = read_body(req, conf_text, VPN_CONF_MAX) > 0 ? vpn_config_save(conf_text)
                        : "body must be a WireGuard config shorter than " STR(VPN_CONF_MAX) " bytes";
    mbedtls_platform_zeroize(conf_text, VPN_CONF_MAX);  // it holds the private key
    free(conf_text);

    if (problem) {
        return httpd_resp_send_err(req, problem == vpn_store_failed ? HTTPD_500_INTERNAL_SERVER_ERROR
                                                                    : HTTPD_400_BAD_REQUEST, problem);
    }
    return httpd_resp_sendstr(req, "saved\n");
}

static esp_err_t vpn_delete(httpd_req_t *req)
{
    if (!same_origin(req)) {
        return ESP_OK;
    }
    const char *problem = vpn_config_save(NULL);
    if (problem) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, problem);
    }
    return httpd_resp_sendstr(req, "removed\n");
}

static esp_err_t ota_get(httpd_req_t *req)
{
    char buf[120];

    // The version is what git describe says, with no characters that JSON needs escaped.
    snprintf(buf, sizeof(buf), "{\"version\":\"%s\",\"board\":\"" BOARD_TYPE "\",\"slot\":\"%s\"}",
             esp_app_get_description()->version, ota_slot());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!same_origin(req)) {
        return ESP_OK;
    }
    if (req->content_len > ota_size_max()) {    // not worth receiving
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "the file is larger than a firmware slot");
        return ESP_FAIL;    // closes the connection
    }
    // The file is passed on in full chunks. After a problem the rest of it is
    // still received, a browser reads the answer only when its upload is through.
    static uint8_t chunk[OTA_CHUNK];
    const char *problem = NULL;
    bool begun = false;
    size_t left = req->content_len, fill = 0;
    while (left > 0) {
        int n = httpd_req_recv(req, (char *)chunk + fill, MIN(left, OTA_CHUNK - fill));
        if (n <= 0) {       // connection lost or stalled
            ota_abort();
            return ESP_FAIL;
        }
        fill += n;
        left -= n;
        if (fill == OTA_CHUNK || left == 0) {
            if (!problem) {
                problem = begun ? ota_write(chunk, fill) : ota_begin(chunk, fill);
            }
            begun = true;
            fill = 0;
        }
    }
    if (!problem) {
        problem = begun ? ota_end() : "body must be the firmware file (.bin)";
    }
    if (problem) {
        ota_abort();
        return httpd_resp_send_err(req, problem == ota_failed ? HTTPD_500_INTERNAL_SERVER_ERROR
                                                              : HTTPD_400_BAD_REQUEST, problem);
    }
    return reply_and_reboot(req, "stored, rebooting. Open the page or send a request within " STR(OTA_CONFIRM_MIN)
                                 " minutes, else the previous firmware comes back\n");
}

// A client that connects to the web server shows that this firmware can be
// reached, which ends its trial after an update (ota.c).
static esp_err_t client_open(httpd_handle_t server, int sockfd)
{
    ota_confirm();
    return ESP_OK;
}

void http_start(void)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = index_get },
        { .uri = "/api/fans",    .method = HTTP_GET,  .handler = fans_get },
        { .uri = "/api/fans/*",  .method = HTTP_POST, .handler = fans_post },
        { .uri = "/api/wifi",    .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get },
        { .uri = "/api/vpn",     .method = HTTP_GET,  .handler = vpn_get },
        { .uri = "/api/vpn",     .method = HTTP_POST, .handler = vpn_post },
        { .uri = "/api/vpn",     .method = HTTP_DELETE, .handler = vpn_delete },
        { .uri = "/api/ota",     .method = HTTP_GET,  .handler = ota_get },
        { .uri = "/api/ota",     .method = HTTP_POST, .handler = ota_post },
    };
    httpd_handle_t server;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 1;
    config.stack_size = 6144;           // checking a new firmware left 920 bytes of the default 4096
    config.open_fn = client_open;
    config.lru_purge_enable = true;     // drop the oldest idle connection when all slots are taken
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = sizeof(routes) / sizeof(routes[0]);
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}
