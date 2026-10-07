idf_component_register(SRCS "main.c" "display.c" "fan.c" "http.c" "led.c" "modbus.c" "mtimer.c" "ota.c" "stat.c" "vpn.c" "wifi.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "index.html")

# vpn.c uses the lwIP-level API of esp_wireguard (wireguardif.h), which the
# component keeps in its private src directory.
idf_component_get_property(wireguard_dir trombik__esp_wireguard COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${wireguard_dir}/src")

# vpn.c puts its own routing hook in front of the one in ESP-IDF's lwIP port.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ip4_route_src_hook")
