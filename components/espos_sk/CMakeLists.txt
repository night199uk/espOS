# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
idf_build_get_property(target IDF_TARGET)
set(srcs src/token_sm.c src/sk_store.c src/sk_http.c src/sk_http_api.c src/sk_delta.c src/sk_parse.c src/sk_inbound.c src/sk_ws.c src/espos_sk.c src/api_sk.c src/sk_tls_policy.c src/sk_tls.c src/sk_migrate.c src/sk_time.c src/sk_ota_url.c src/sk_select.c)
# esp-tls/mbedtls are named unconditionally even though only an
# ESPOS_SK_TLS build compiles anything that uses them. A `if(CONFIG_...)`
# here would be empty on the first pass -- component CMakeLists are
# evaluated during early expansion, before sdkconfig exists -- and would
# then drop the requirement while the #if'd sources compile on the second,
# failing on a missing esp_crt_bundle.h. Same trap as espos_ble's `bt`.
# Unused objects cost nothing: the linker drops them.
set(priv_reqs espressif__cjson esp_http_client nvs_flash tcp_transport esp_system esp-tls mbedtls espos_event)
if(target STREQUAL "linux")
    list(APPEND srcs src/discovery_sim.c)
else()
    # discovery.c browses with the responder espos_net owns; mdns.h and the
    # espressif/mdns dependency come through espos_net's REQUIRES.
    list(APPEND srcs src/discovery.c)
    list(APPEND priv_reqs esp_netif lwip)
endif()
# espos_net, not espos_wifi: "is the network up", the hostname and the device
# id come from the seam, so a SignalK device may carry its link on any
# transport — or, on a chip without WiFi, on none the radio provides.
# WHOLE_ARCHIVE, and src/sk_ota_url.c is the reason.
#
# That file exists only to provide the STRONG definition of a hook espos_ota
# declares weak -- the override direction avoids espos_ota having to depend on
# espos_sk for one convenience. But nothing CALLS into it, and a linker pulls
# an object out of a static archive only when that resolves an undefined
# symbol. It never would, so the weak stub would win and
# ota.manifest_src = "signalk" would report "this firmware has no SignalK
# client" on a firmware that plainly has one.
#
# Exactly the bug espos_time shipped with (/system/info reporting no clock
# while /time reported sntp, #49): both halves compile, both link, and the
# disagreement only shows on a running device.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    REQUIRES espos_config espos_httpd espos_net
    PRIV_REQUIRES espos_health espos_time
    PRIV_REQUIRES ${priv_reqs}
    WHOLE_ARCHIVE)

espos_config_add_descriptor(config/sk.json)
