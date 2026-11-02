# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Consumer sdkconfig lint. Included by the IDF build system for every project
# that has espos_core in its component list, after sdkconfig has been
# generated and before any component CMakeLists.txt runs; the CONFIG_* values
# are plain CMake variables here (build.cmake includes sdkconfig.cmake first,
# the same way IDF's own partition_table/project_include.cmake reads
# CONFIG_PARTITION_TABLE_CUSTOM).
#
# A firmware that includes cmake/espos_project.cmake inherits every value
# checked below from sdkconfig.d/espos.defaults and never trips this, unless
# it raises a limit a check is sized by (the socket floor below). It is
# for the project that installs espOS's components from the registry and
# writes its own CMakeLists.txt: without the shared defaults its sdkconfig is
# IDF's, and the settings below are the ones whose absence does not show
# at build time but as a device that wedges or overflows a stack in the
# field. Each failure names the exact line to add.
#
# Every check skips when its value is not in this sdkconfig at all (another
# target, a component not in the build) rather than guess.

if(NOT DEFINED CONFIG_IDF_TARGET OR CONFIG_IDF_TARGET STREQUAL "linux")
    # Host tests run on pthread stacks and have no radio; nothing here applies.
    return()
endif()

set(_espos_lint_fix
    "Add the line to your sdkconfig.defaults and delete the stale sdkconfig so the "
    "defaults are re-applied — or include espOS's cmake/espos_project.cmake, whose "
    "espos_project_prologue() puts sdkconfig.d/espos.defaults on SDKCONFIG_DEFAULTS "
    "for you.")
string(CONCAT _espos_lint_fix ${_espos_lint_fix})

# Findings accumulate and are reported together at the end. Failing on the
# first one makes a consumer without the prologue fix a setting, reconfigure,
# and be told about the next -- three or four rounds before the build starts.
# Captured at include time: inside a function or later in the file
# CMAKE_CURRENT_LIST_DIR is whatever is being processed then, not this file.
set(ESPOS_CORE_PARTITIONS "${CMAKE_CURRENT_LIST_DIR}/partitions" CACHE INTERNAL
    "espOS's bundled partition tables, shipped inside espos_core")

set(_espos_lint_problems "")
set(_espos_lint_lines "")
macro(_espos_lint_report why line)
    string(APPEND _espos_lint_problems "  - ${why}\n")
    string(APPEND _espos_lint_lines "    ${line}\n")
endmacro()

# WiFi/IP events and FreeRTOS timers run the state machine, JSON building and
# SSE sends; the IDF defaults (2304 / 2048) are too tight (espos.defaults).
foreach(_espos_stack CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH)
    if(DEFINED ${_espos_stack} AND ${_espos_stack} LESS 4096)
        _espos_lint_report(
            "${_espos_stack}=${${_espos_stack}}: espOS runs its WiFi/SignalK state machines, JSON building and SSE sends on that task and needs at least 4096 (a smaller stack overflows on the first busy event, not at boot)."
            "${_espos_stack}=4096")
    endif()
endforeach()

# tcp_transport builds the whole WebSocket upgrade request in this buffer and
# refuses to dial when it does not fit; a bearer token of up to
# ESPOS_SK_TOKEN_MAX (1024) plus the other headers does not fit IDF's 1024
# (espos.defaults).
if(DEFINED CONFIG_WS_BUFFER_SIZE AND CONFIG_WS_BUFFER_SIZE LESS 2048)
    _espos_lint_report(
        "CONFIG_WS_BUFFER_SIZE=${CONFIG_WS_BUFFER_SIZE}: the WebSocket upgrade request (bearer token up to 1024 bytes, User-Agent and the other headers) is built in this buffer, and a request that does not fit is never sent, so a device with a long token can never open the SignalK stream."
        "CONFIG_WS_BUFFER_SIZE=2048")
endif()

# One socket table serves espos_httpd and every connection the device opens
# itself. At IDF's 10 a browser holding a few connections to the web UI
# leaves none for the calls to the server (espos.defaults). Two of the
# consumers are sized by Kconfig, so the floor follows them: 11 fixed
# (espos_httpd's 4 base clients and 3 of its own, the portal DNS responder,
# the delta stream, the BLE control socket, an OTA download) plus the
# event-stream clients and espos_sk's concurrent HTTP calls, which is 16 at
# the defaults. A component not in the build counts at its default.
set(_espos_sse_clients 3)
if(DEFINED CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS)
    set(_espos_sse_clients ${CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS})
endif()
set(_espos_sk_http 2)
if(DEFINED CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT)
    set(_espos_sk_http ${CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT})
endif()
math(EXPR _espos_httpd_clients "4 + ${_espos_sse_clients}")
math(EXPR _espos_min_sockets "11 + ${_espos_sse_clients} + ${_espos_sk_http}")
if(DEFINED CONFIG_LWIP_MAX_SOCKETS AND CONFIG_LWIP_MAX_SOCKETS LESS ${_espos_min_sockets})
    _espos_lint_report(
        "CONFIG_LWIP_MAX_SOCKETS=${CONFIG_LWIP_MAX_SOCKETS}: espos_httpd may hold ${_espos_httpd_clients} clients (4 + CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS) and 3 sockets of its own, the setup portal's DNS responder holds 1, and the delta stream, espOS's HTTP calls to the server (up to CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT=${_espos_sk_http}), the BLE gateway's control socket and an OTA download share the same table: ${_espos_min_sockets} in all. Below that a browser with the web UI open can take the sockets the device's own calls need: \"Failed to create socket\" in the log, and accept() failing with ENFILE."
        "CONFIG_LWIP_MAX_SOCKETS=${_espos_min_sockets}")
endif()

if(CONFIG_IDF_TARGET STREQUAL "esp32p4")
    # From espos.defaults.esp32p4: without PSRAM, esp_hosted's startup
    # allocations leave so little internal RAM that FreeRTOS cannot allocate
    # the timer task's stack when the scheduler starts, and every boot panics
    # within seconds. Seen on a Waveshare P4 PoE board.
    if(NOT CONFIG_SPIRAM)
        _espos_lint_report(
            "CONFIG_SPIRAM is off on the ESP32-P4. esp_hosted's startup allocations leave internal RAM so short that FreeRTOS cannot allocate its timer task's stack, and the board panics within seconds of every boot (\"assert failed: vApplicationGetTimerTaskMemory port_common.c:97\"). CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM is dropped without it as well."
            "CONFIG_SPIRAM=y")
    endif()

    # From espos.defaults.esp32p4: "Keep the hosted transport mempool — the
    # transport's large DMA buffer pool — out of internal RAM [...]
    # CONSTRAINT: only safe with 64-byte L2 cache lines — the 1600-byte
    # transport stride is 64-aligned but NOT 128-aligned, so with
    # CONFIG_CACHE_L2_CACHE_LINE_128B the SDIO driver rejects PSRAM buffers
    # (ESP_ERR_INVALID_ARG; esp-hosted-mcu#219)."
    if(CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM AND DEFINED CONFIG_CACHE_L2_CACHE_LINE_64B
       AND NOT CONFIG_CACHE_L2_CACHE_LINE_64B)
        _espos_lint_report(
            "CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM=y without CONFIG_CACHE_L2_CACHE_LINE_64B=y. The hosted transport's 1600-byte buffer stride is 64-aligned but not 128-aligned, so with 128-byte L2 cache lines the SDIO driver rejects the PSRAM buffers (ESP_ERR_INVALID_ARG, esp-hosted-mcu#219) and the co-processor link wedges. A 256 KB L2 cache forces 128-byte lines; use 128 KB."
            "CONFIG_CACHE_L2_CACHE_LINE_64B=y")
    endif()

    # esp_hosted 3.x made the co-processor heartbeat opt-in and defaults it
    # off. It is espos_wifi's only evidence that the radio link is alive, so
    # without it wedge detection compiles out -- see hosted_watchdog.c. Checked
    # here as well as there so a consumer gets the line to add rather than a
    # #warning buried in a build log.
    if(CONFIG_ESP_HOSTED AND NOT CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT)
        _espos_lint_report(
            "CONFIG_ESP_HOSTED is on but CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT is not. The heartbeat is the only signal espos_wifi has that the co-processor link is alive -- esp_hosted reports faults it detects itself but has no liveness query -- so a silently wedged SDIO link becomes undetectable and the device sits unreachable until it is power-cycled."
            "CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT=y")
    endif()

    # esp_hosted 3.x renamed most host-side keys and REMOVED the 2.x
    # spellings. IDF leaves KCONFIG_WARN_UNDEF_ASSIGN off, so an assignment to
    # a symbol that no longer exists is dropped in silence: the SDIO pins fall
    # back to the component's built-in defaults, the co-processor never
    # answers, and the only symptom is a P4 with no WiFi. Nothing survives
    # into CONFIG_* for a lint to test, so scan the defaults files themselves.
    # The pairs are the ones espOS set through 0.13.x; docs/wifi.md has the
    # full table.
    set(_espos_hosted_renames
        "ESP_HOSTED_ENABLED|ESP_HOSTED"
        "ESP_HOSTED_SDIO_HOST_INTERFACE|ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO"
        "ESP_HOSTED_SDIO_4_BIT_BUS|ESP_HOSTED_HOST_SDIO_BUS_WIDTH_4"
        "ESP_HOSTED_SDIO_PIN_CLK|ESP_HOSTED_HOST_SDIO_PIN_CLK"
        "ESP_HOSTED_SDIO_PIN_CMD|ESP_HOSTED_HOST_SDIO_PIN_CMD"
        "ESP_HOSTED_SDIO_PIN_D0|ESP_HOSTED_HOST_SDIO_PIN_D0"
        "ESP_HOSTED_SDIO_PIN_D1|ESP_HOSTED_HOST_SDIO_PIN_D1"
        "ESP_HOSTED_SDIO_PIN_D2|ESP_HOSTED_HOST_SDIO_PIN_D2"
        "ESP_HOSTED_SDIO_PIN_D3|ESP_HOSTED_HOST_SDIO_PIN_D3"
        "ESP_HOSTED_SDIO_GPIO_RESET_SLAVE|ESP_HOSTED_HOST_RESET_GPIO"
        "ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ|ESP_HOSTED_HOST_SDIO_CLK_KHZ"
        "ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE|ESP_HOSTED_HOST_SDIO_RX_STREAMING_MODE"
        "ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP|ESP_HOSTED_HOST_CP_RESET_STRATEGY_ALWAYS"
        "ESP_HOSTED_MEMPOOL_PREFER_SPIRAM|EH_HOST_PORT_DMA_PREFER_SPIRAM"
        "ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE|ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE"
        "ESP_HOSTED_ENABLE_BT_BLUEDROID|ESP_HOSTED_HOST_FEAT_BT"
        "ESP_HOSTED_ENABLE_BT_NIMBLE|ESP_HOSTED_HOST_FEAT_BT")

    # Removed in 3.x with no successor, so "set this instead" is the wrong
    # advice -- and the two families are removed for different reasons, so they
    # carry different messages. Reset polarity is no longer configurable: 3.x
    # always drives park-high / 10 ms low / park-high (eh_host_port_power.c,
    # "CP reset is its EN pin: LOW asserts reset, HIGH runs it"). That is the
    # waveform 2.x produced from ACTIVE_HIGH, so a board on ACTIVE_HIGH is
    # unaffected -- but a board that needed the other polarity has no knob any
    # more and its co-processor will not come out of reset, which is a hardware
    # fact no sdkconfig line can fix.
    set(_espos_hosted_gone_reset
        "ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH"
        "ESP_HOSTED_SDIO_RESET_ACTIVE_LOW"
        "ESP_HOSTED_SPI_RESET_ACTIVE_HIGH"
        "ESP_HOSTED_SPI_RESET_ACTIVE_LOW"
        "ESP_HOSTED_SPI_HD_RESET_ACTIVE_HIGH"
        "ESP_HOSTED_SPI_HD_RESET_ACTIVE_LOW"
        "ESP_HOSTED_UART_RESET_ACTIVE_HIGH"
        "ESP_HOSTED_UART_RESET_ACTIVE_LOW")
    set(_espos_hosted_gone_reset_why
        "esp_hosted 3.x removed it: reset polarity is now fixed at park-high / 10 ms low / park-high, which is what 2.x produced from ACTIVE_HIGH. A board that needed the opposite polarity will not bring its co-processor out of reset, and no sdkconfig line can change that.")

    # The HCI-transport knobs went a different way: 3.x keeps the core a
    # stack-agnostic HCI byte pipe and the application binds a stack with
    # esp_hosted_bt_host_stack_setup(), which picks it from the IDF BT Kconfig.
    # CONFIG_BT_{BLUEDROID,NIMBLE}_ENABLED is that selector, not a replacement
    # for these -- a project already sets it.
    set(_espos_hosted_gone_hci
        "ESP_HOSTED_BLUEDROID_HCI_VHCI"
        "ESP_HOSTED_NIMBLE_HCI_VHCI")
    set(_espos_hosted_gone_hci_why
        "esp_hosted 3.x removed it: the HCI binding moved into esp_hosted_bt_host_stack_setup(), which takes the stack from the IDF BT Kconfig the project already sets. Enable CONFIG_ESP_HOSTED_HOST_FEAT_BT and call that function after esp_hosted_connect_to_slave() (docs/ble.md).")

    # The defaults files to scan. Taken from the SDKCONFIG_DEFAULTS build
    # PROPERTY, not the same-named variable: project.cmake has already made
    # every entry absolute by then (`get_filename_component(... ABSOLUTE)`) and
    # folded in the $ENV{SDKCONFIG_DEFAULTS} and project-default cases, whereas
    # the variable is whatever the caller wrote and may be relative. A relative
    # path here would simply not exist from this scope, and the whole scan would
    # skip in silence -- a lint that lints nothing. Falls back to the variable
    # and then to the project's own file, each made absolute, in case a future
    # IDF stops setting the property.
    idf_build_get_property(_espos_defaults_seed SDKCONFIG_DEFAULTS)
    if(NOT _espos_defaults_seed)
        if(SDKCONFIG_DEFAULTS)
            set(_espos_defaults_seed "${SDKCONFIG_DEFAULTS}")
        else()
            set(_espos_defaults_seed "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
        endif()
    endif()
    # kconfig.cmake pairs each entry with its .<target> sibling; mirror that.
    set(_espos_defaults_files)
    foreach(_f IN LISTS _espos_defaults_seed)
        get_filename_component(_f "${_f}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
        list(APPEND _espos_defaults_files "${_f}" "${_f}.${IDF_TARGET}")
    endforeach()

    foreach(_f IN LISTS _espos_defaults_files)
        if(NOT EXISTS "${_f}")
            continue()
        endif()
        file(STRINGS "${_f}" _lines REGEX "^CONFIG_[A-Z0-9_]+=")
        foreach(_fam "reset" "hci")
            foreach(_gone IN LISTS _espos_hosted_gone_${_fam})
                foreach(_line IN LISTS _lines)
                    if(_line MATCHES "^CONFIG_${_gone}=")
                        _espos_lint_report(
                            "${_f} sets CONFIG_${_gone}. ${_espos_hosted_gone_${_fam}_why}"
                            "(delete the CONFIG_${_gone} line)")
                    endif()
                endforeach()
            endforeach()
        endforeach()

        foreach(_pair IN LISTS _espos_hosted_renames)
            string(REPLACE "|" ";" _p "${_pair}")
            list(GET _p 0 _old)
            list(GET _p 1 _new)
            foreach(_line IN LISTS _lines)
                if(_line MATCHES "^CONFIG_${_old}=")
                    _espos_lint_report(
                        "${_f} sets CONFIG_${_old}, an esp_hosted 2.x key. 3.x removed it and IDF drops assignments to undefined symbols without a word, so the setting is simply lost."
                        "CONFIG_${_new}=<the value CONFIG_${_old} had>   (and delete the CONFIG_${_old} line)")
                endif()
            endforeach()
        endforeach()
    endforeach()

    # From espos.defaults.esp32p4: "IDF defaults this to 6, but silently
    # raises it to 16 as soon as a project enables PSRAM
    # (SPIRAM_TRY_ALLOCATE_WIFI_LWIP) [...]. A window of 16 overruns the
    # SDIO Rx path on this transport: it wedges under sustained inbound TCP
    # [...] (espressif/esp-hosted-mcu#184). Measured on a Waveshare 7B with
    # repeated ~30 KB HTTP reads: at 16 the link wedged after 85 requests /
    # 77 s; at 6 it survived 400 consecutive requests over 283 s."
    if(CONFIG_SPIRAM AND DEFINED CONFIG_WIFI_RMT_RX_BA_WIN AND NOT CONFIG_WIFI_RMT_RX_BA_WIN EQUAL 6)
        _espos_lint_report(
            "CONFIG_SPIRAM=y with CONFIG_WIFI_RMT_RX_BA_WIN=${CONFIG_WIFI_RMT_RX_BA_WIN}. Enabling PSRAM makes IDF raise the remote radio's receive block-ack window to 16, which overruns the SDIO Rx path to the C6 under sustained inbound TCP: the link wedges (\"H_SDIO_DRV: task still writing Rx data to queue!\") and WiFi stays dead until reboot (esp-hosted-mcu#184). Measured: 16 wedged after 85 requests, 6 survived 400. The knob is WIFI_RMT_*, not ESP_WIFI_RX_BA_WIN — the radio is remote."
            "CONFIG_WIFI_RMT_RX_BA_WIN=6")
    endif()
endif()

# Settings that are not a judgement call: espOS's own code or build depends on
# each, and a consumer without the prologue gets none of them. Kept in one
# table so it can be diffed against sdkconfig.d/espos.defaults, which
# tools/check_sdkconfig_lint.py does in CI -- the lint and the defaults are two
# expressions of one truth and must not drift apart.
#
# CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH is the reason this table exists: without
# it, espos_httpd's api_coredump.c fails to find esp_core_dump.h roughly 900
# files into the build, a long way from anything that names the cause.
foreach(_espos_need
        CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
        CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT
        CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
        CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME
        CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES)
    if(NOT ${_espos_need})
        if(_espos_need STREQUAL "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH")
            _espos_lint_report(
                "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH is off. espos_httpd serves the stored core dump at /api/v1/system/coredump and includes esp_core_dump.h, which IDF only provides when this is on; without it the build fails deep in espos_httpd rather than here."
                "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y")
        elseif(_espos_need STREQUAL "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE")
            _espos_lint_report(
                "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is off. espos_ota marks an image valid only after it has run; without rollback a bad update is permanent and the device needs a USB flash."
                "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y")
        else()
            _espos_lint_report(
                "${_espos_need} is off. espOS signs every image so a device accepts an OTA only from the key it was flashed with; with signing off, the firmware installs over USB and then rejects every update, on the device, long after the build went green."
                "${_espos_need}=y")
        endif()
    endif()
endforeach()

# The UI, the config store and OTA all need somewhere to live. A table without
# them builds and then fails at runtime or at the first update, so say it here.
if(CONFIG_PARTITION_TABLE_SINGLE_APP OR CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE)
    # Name the table's real path. It ships inside this component, so the same
    # message works from a checkout and from managed_components/ -- telling a
    # registry consumer to look in "partitions/" was advice for a directory
    # they do not have, and they met a bare ninja error instead.
    #
    # The consumer has to name it themselves: CONFIG_PARTITION_TABLE_CUSTOM_FILENAME
    # is read while sdkconfig is generated, which is before any component CMake
    # runs, so this file can point at a table but cannot select one.
    _espos_lint_report(
        "the partition table is IDF's single-app default: one app slot and no 'storage'. espOS needs two OTA slots (an update stages into the passive one) and a storage partition for the web UI. Copy a bundled table next to your CMakeLists.txt -- ${ESPOS_CORE_PARTITIONS}/{4,8,16}mb.csv -- and name it below. Match CONFIG_ESPTOOLPY_FLASHSIZE_* to the table you pick, or the image will not fit the chip."
        "CONFIG_PARTITION_TABLE_CUSTOM=y\n    CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions.csv\"")
endif()

# A table and a flash size that disagree produce an image the chip cannot
# hold, and the flash fails partway through writing it. IDF catches it later
# and says nothing about espOS's tables, so say it here where the table was
# just chosen.
#
# espOS's tables leave offsets blank (gen_esp32part assigns them) and size in
# K/M, so sum the sizes rather than reading an end offset -- an earlier
# version of this check looked for hex offsets, matched nothing, and silently
# never fired.
if(CONFIG_PARTITION_TABLE_CUSTOM AND CONFIG_PARTITION_TABLE_CUSTOM_FILENAME AND CONFIG_ESPTOOLPY_FLASHSIZE)
    get_filename_component(_espos_table "${CONFIG_PARTITION_TABLE_CUSTOM_FILENAME}"
                           ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(EXISTS "${_espos_table}")
        file(STRINGS "${_espos_table}" _espos_rows REGEX "^[^#]+,")
        set(_espos_total 0)
        foreach(_espos_row IN LISTS _espos_rows)
            string(REPLACE " " "" _espos_row "${_espos_row}")
            # Match the 5th field directly. Splitting on "," and counting does
            # NOT work: list(LENGTH) drops empty elements (CMP0007), and these
            # tables leave the offset column blank, so every row looked like it
            # had four fields and the check silently never fired.
            if(_espos_row MATCHES "^[^,]*,[^,]*,[^,]*,[^,]*,([^,]+)")
                set(_espos_size "${CMAKE_MATCH_1}")
                set(_espos_bytes 0)
                if(_espos_size MATCHES "^([0-9]+)K$")
                    math(EXPR _espos_bytes "${CMAKE_MATCH_1} * 1024")
                elseif(_espos_size MATCHES "^([0-9]+)M$")
                    math(EXPR _espos_bytes "${CMAKE_MATCH_1} * 1048576")
                elseif(_espos_size MATCHES "^0x[0-9a-fA-F]+$")
                    math(EXPR _espos_bytes "${_espos_size}")
                elseif(_espos_size MATCHES "^[0-9]+$")
                    set(_espos_bytes ${_espos_size})
                endif()
                math(EXPR _espos_total "${_espos_total} + ${_espos_bytes}")
            endif()
        endforeach()
        # The table itself starts at CONFIG_PARTITION_TABLE_OFFSET (0x8000 by
        # default): bootloader and table sit below the first partition.
        set(_espos_base 32768)
        if(CONFIG_PARTITION_TABLE_OFFSET)
            set(_espos_base ${CONFIG_PARTITION_TABLE_OFFSET})
        endif()
        math(EXPR _espos_end "${_espos_total} + ${_espos_base} + 4096")
        string(REGEX REPLACE "MB$" "" _espos_mb "${CONFIG_ESPTOOLPY_FLASHSIZE}")
        if(_espos_total GREATER 0 AND _espos_mb MATCHES "^[0-9]+$")
            math(EXPR _espos_cap "${_espos_mb} * 1024 * 1024")
            if(_espos_end GREATER _espos_cap)
                math(EXPR _espos_need "(${_espos_end} + 1048575) / 1048576")
                # Round up to a flash size that exists.
                set(_espos_pick 0)
                foreach(_espos_try 2 4 8 16 32)
                    if(_espos_pick EQUAL 0 AND NOT _espos_try LESS _espos_need)
                        set(_espos_pick ${_espos_try})
                    endif()
                endforeach()
                _espos_lint_report(
                    "${CONFIG_PARTITION_TABLE_CUSTOM_FILENAME} needs about ${_espos_need} MB but CONFIG_ESPTOOLPY_FLASHSIZE is ${CONFIG_ESPTOOLPY_FLASHSIZE}, so the table runs past the end of the chip and the flash fails partway through writing it."
                    "CONFIG_ESPTOOLPY_FLASHSIZE_${_espos_pick}MB=y")
            endif()
        endif()
    endif()
endif()

if(_espos_lint_problems)
    message(FATAL_ERROR
        "espos_core: this project's sdkconfig is missing settings espOS depends on.\n\n"
        "${_espos_lint_problems}\n"
        "Add these lines:\n\n"
        "${_espos_lint_lines}\n"
        "${_espos_lint_fix}")
endif()

unset(_espos_hosted_renames)
unset(_espos_hosted_gone_reset)
unset(_espos_hosted_gone_reset_why)
unset(_espos_hosted_gone_hci)
unset(_espos_hosted_gone_hci_why)
unset(_espos_defaults_files)
unset(_espos_defaults_seed)
unset(_espos_lint_fix)
unset(_espos_lint_problems)
unset(_espos_lint_lines)
unset(_espos_need)
unset(_espos_stack)
unset(_espos_sse_clients)
unset(_espos_sk_http)
unset(_espos_httpd_clients)
unset(_espos_min_sockets)
unset(_espos_table)
unset(_espos_rows)
unset(_espos_cols)
unset(_espos_end)

# The component-name helpers live in their own file so every component that
# needs them can include it without depending on espos_core being in the build
# (espOS #138 review: espos_ota, espos_flow, espos_power, espos_n2k and
# espos_prov all call them and none requires espos_core).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/espos_components.cmake")
