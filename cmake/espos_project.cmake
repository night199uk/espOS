# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Shared project prologue/epilogue for firmwares built on espOS.
#
# Every espOS firmware needs the same things before `project()`: the supported
# IDF version enforced, espOS's sdkconfig defaults and partition table on
# SDKCONFIG_DEFAULTS, espOS's components on EXTRA_COMPONENT_DIRS, an
# app-signing key, and the re-link-on-key-change workaround below. That was
# ~55 lines of CMake plus a copy of sdkconfig.defaults in each project root,
# which is exactly the code that must not drift: when the signing half of it
# goes wrong the symptom is a device that flashes fine over USB and then
# rejects every OTA, and when the sdkconfig half drifts a P4 consumer gets the
# SDIO block-ack window IDF picks for it and its WiFi wedges under load.
#
# Usage in a project's root CMakeLists.txt:
#
#     cmake_minimum_required(VERSION 3.22)
#     include("${CMAKE_CURRENT_LIST_DIR}/espos/cmake/espos_project.cmake")
#     espos_project_prologue(NAME "ble-gateway"
#                            PARTITIONS "${ESPOS_PARTITIONS_DIR}/16mb.csv")
#     project(ble_gateway)
#     espos_project_ui_partition()
#
# espOS itself includes it as `cmake/espos_project.cmake` — same code path, so
# the shared prologue is exercised by espOS's own CI on every target.
#
# This file includes ESP-IDF's project.cmake itself; do not include both.

include_guard(GLOBAL)

# <espos>/cmake/espos_project.cmake → <espos>
get_filename_component(ESPOS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# The bundled tables live inside espos_core so they reach a firmware that
# installs espOS from the registry, where this file does not exist. One
# definition of the path, used by the prologue and named by espos_core's
# lint when a consumer has no table of its own.
set(ESPOS_PARTITIONS_DIR "${ESPOS_DIR}/components/espos_core/partitions")

if(NOT EXISTS "${ESPOS_DIR}/components/espos_config/CMakeLists.txt")
    message(FATAL_ERROR "espOS: '${ESPOS_DIR}/components' has no espos_config — "
                        "if espOS is a submodule, run `git submodule update --init`.")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/espos_version.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

#
# espos_project_prologue([NAME <label>]
#                        [IDF_VERSION_FILE <path>]
#                        [SIGNING_KEY <path>]
#                        [PROFILE <name>]
#                        [PARTITIONS <csv>]
#                        [COMPONENTS <espos_x> ...])
#
# NAME              label used in messages (default: the project directory name)
# IDF_VERSION_FILE  version pin to enforce (default: the project's own
#                   .idf-version if it has one, else espOS's). When a project
#                   carries its own pin AND espOS's differs, that is a hard
#                   error rather than a silent choice between the two.
# SIGNING_KEY       app-signing key (default: <project>/secure_boot_signing_key.pem)
# PROFILE           an sdkconfig overlay from <espos>/sdkconfig.d/<name>.defaults
#                   ("release", "debug"); -DESPOS_PROFILE=<name> on the idf.py
#                   command line overrides the argument, so a CMakeLists.txt
#                   never has to be edited to cut a release build.
# PARTITIONS        partition-table CSV, absolute or relative to the project
#                   (default: espos_core/partitions/4mb.csv). The bundled
#                   partitions/<n>mb.csv tables also set the flash size.
# COMPONENTS        the OPTIONAL espOS components this firmware uses:
#                   espos_ble, espos_n2k, espos_voice (implies espos_audio),
#                   espos_audio, espos_usb_storage. The core set (config, log, event, health,
#                   httpd, net, wifi, sk, ota, core) is always available;
#                   espos_wifi is excluded on targets without WiFi
#                   (esp32h2/h21/h4). Anything optional and not named is
#                   excluded from the build outright.
#
# A macro, not a function: EXTRA_COMPONENT_DIRS and SDKCONFIG_DEFAULTS have to
# land in the caller's scope, where `project()` will read them.
#
macro(espos_project_prologue)
    cmake_parse_arguments(_ESPOS "" "NAME;IDF_VERSION_FILE;SIGNING_KEY;PROFILE;PARTITIONS" "COMPONENTS" ${ARGN})
    if(_ESPOS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "espos_project_prologue: unknown argument(s): ${_ESPOS_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT _ESPOS_NAME)
        get_filename_component(_ESPOS_NAME "${CMAKE_SOURCE_DIR}" NAME)
    endif()

    _espos_check_idf_version("${_ESPOS_NAME}" "${_ESPOS_IDF_VERSION_FILE}")

    # Sets PROJECT_VER, which project() reads a few lines later. Here rather
    # than in each firmware's root: a consumer wants to know which of ITS
    # builds is on a device for the same reason espOS does, and version.txt
    # alone cannot say.
    espos_project_version()

    # ...and separately, which espOS it is building against. A consumer's own
    # version says nothing about the base underneath it, and 11 of 60 cockpit
    # commits have been `chore: bump espos to <sha>` -- a number nobody can
    # read back off a build.
    _espos_report_base_version("${_ESPOS_NAME}" "${ESPOS_DIR}")

    # In espOS's own tree components/ is already the project's component dir;
    # naming it again would register every component twice.
    if(NOT "${ESPOS_DIR}" STREQUAL "${CMAKE_SOURCE_DIR}")
        list(APPEND EXTRA_COMPONENT_DIRS "${ESPOS_DIR}/components")

        # Consumers build only what main/ (transitively) requires. Without
        # this IDF compiles every component it can see — all of espOS's plus
        # the whole IDF tree — so a gateway without a radio still compiles
        # espos_ble, espos_n2k and espos_voice and links none of them. espOS's
        # own tree deliberately keeps the full set: its CI is what proves that
        # ble/n2k/voice still compile on every target, and MINIMAL_BUILD would
        # silently drop them from that check because the example app does not
        # require them.
        idf_build_set_property(MINIMAL_BUILD ON)

        # Optional espOS components a consumer did not ask for are excluded
        # outright, not just left unrequired: IDF hands the component manager
        # every component it can see BEFORE MINIMAL_BUILD trims the graph, so
        # espos_voice's manifest alone pulls esp-sr/esp-dl/esp-dsp into the
        # lock and compiles ~350 objects nobody links (measured on a headless
        # P4 gateway: 157 MB of esp-dl archives, 0 members linked). Naming
        # what you use is the only place this can be decided before project().
        set(_espos_optional espos_ble espos_eth espos_n2k espos_power espos_prov espos_voice espos_audio
                            espos_usb_storage)
        foreach(_c IN LISTS _ESPOS_COMPONENTS)
            if(NOT "${_c}" IN_LIST _espos_optional)
                message(FATAL_ERROR "${_ESPOS_NAME}: COMPONENTS names '${_c}', which is not an optional "
                                    "espOS component (choose from: ${_espos_optional}); the core set "
                                    "needs no listing.")
            endif()
        endforeach()
        if("espos_voice" IN_LIST _ESPOS_COMPONENTS)
            list(APPEND _ESPOS_COMPONENTS espos_audio)   # voice speaks through the audio contract
        endif()
        foreach(_c IN LISTS _espos_optional)
            if(NOT "${_c}" IN_LIST _ESPOS_COMPONENTS)
                list(APPEND EXCLUDE_COMPONENTS "${_c}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES EXCLUDE_COMPONENTS)
    endif()

    # Targets without WiFi. The 802.15.4-only H-series has no radio esp_wifi
    # can drive and no co-processor for esp_wifi_remote, so espos_wifi cannot
    # build there: exclude it — in a consumer's build and in espOS's own tree
    # alike, a component that cannot build on a target must not be in its
    # graph — and record the decision as a build property. espos_core reads
    # ESPOS_WIFI in early expansion, before sdkconfig exists (a Kconfig symbol
    # could not carry it), and drops its espos_wifi requirement. Everything
    # above the seam (espos_net, espos_sk, espos_ota) builds without WiFi.
    set(_espos_no_wifi_targets esp32h2 esp32h21 esp32h4)
    if(IDF_TARGET IN_LIST _espos_no_wifi_targets)
        list(APPEND EXCLUDE_COMPONENTS espos_wifi)
        list(REMOVE_DUPLICATES EXCLUDE_COMPONENTS)
        idf_build_set_property(ESPOS_WIFI OFF)
        message(STATUS "${_ESPOS_NAME}: ${IDF_TARGET} has no WiFi — espos_wifi excluded, network via espos_net transports only")
    else()
        idf_build_set_property(ESPOS_WIFI ON)
    endif()

    # The command line wins over the argument: `idf.py -DESPOS_PROFILE=release`
    # is how a release gets cut without editing the project.
    if(DEFINED ESPOS_PROFILE)
        set(_ESPOS_PROFILE "${ESPOS_PROFILE}")
    endif()
    _espos_sdkconfig_defaults("${_ESPOS_NAME}" "${_ESPOS_PROFILE}" "${_ESPOS_PARTITIONS}")

    # An explicit SIGNING_KEY has to reach IDF too: its signing step reads
    # CONFIG_SECURE_BOOT_SIGNING_KEY, never this argument (_espos_signing_key).
    if(_ESPOS_SIGNING_KEY)
        get_filename_component(_ESPOS_SIGNING_KEY "${_ESPOS_SIGNING_KEY}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
        set(_espos_signing_explicit TRUE)
    else()
        set(_ESPOS_SIGNING_KEY "${CMAKE_SOURCE_DIR}/secure_boot_signing_key.pem")
        set(_espos_signing_explicit FALSE)
    endif()
    _espos_signing_key("${_ESPOS_NAME}" "${_ESPOS_SIGNING_KEY}" ${_espos_signing_explicit})
endmacro()

#
# Enforce the ESP-IDF version policy (cmake/espos_version.cmake): the pinned
# release is what CI tests, any other release in [ESPOS_IDF_MIN,
# ESPOS_IDF_MAX_EXCL) builds with one warning, and anything outside the range
# is refused unless -DESPOS_ALLOW_IDF_MISMATCH=1 (the pre-split projects each
# spelled this flag differently: BLEGW_/COCKPIT_/ESPOS_ALLOW_IDF_MISMATCH — it
# is ESPOS_ everywhere now).
#
function(_espos_check_idf_version name version_file)
    set(espos_pin "${ESPOS_DIR}/.idf-version")
    set(project_pin "${CMAKE_SOURCE_DIR}/.idf-version")

    if(version_file)
        if(NOT EXISTS "${version_file}")
            message(FATAL_ERROR "${name}: IDF_VERSION_FILE '${version_file}' does not exist.")
        endif()
    elseif(EXISTS "${project_pin}")
        set(version_file "${project_pin}")
        # Two pins that disagree is how a project ends up building against an
        # IDF its espOS was never tested on. Fail rather than pick one.
        if(EXISTS "${espos_pin}" AND NOT "${espos_pin}" STREQUAL "${project_pin}")
            file(READ "${project_pin}" a)
            file(READ "${espos_pin}" b)
            string(STRIP "${a}" a)
            string(STRIP "${b}" b)
            if(NOT a STREQUAL b)
                message(FATAL_ERROR "${name}: .idf-version pins ${a} but the espOS submodule pins ${b}. "
                                    "Align them (espOS's pin is the one its CI tests), or drop this "
                                    "project's .idf-version to follow espOS.")
            endif()
        endif()
    else()
        set(version_file "${espos_pin}")
    endif()

    file(READ "${version_file}" want)
    string(STRIP "${want}" want)
    set(have "v${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
    string(REGEX REPLACE "^v" "" have_number "${have}")

    if(have_number VERSION_LESS ESPOS_IDF_MIN OR have_number VERSION_GREATER_EQUAL ESPOS_IDF_MAX_EXCL)
        set(why "${name}: IDF_PATH is ESP-IDF ${have}; espOS builds with v${ESPOS_IDF_MIN} up to "
                "(not including) v${ESPOS_IDF_MAX_EXCL} and is tested on ${want} (${version_file}). "
                "Install that release — "
                "https://docs.espressif.com/projects/esp-idf/en/${want}/esp32/get-started/ — "
                "and source its export.sh.")
        if(ESPOS_ALLOW_IDF_MISMATCH)
            message(WARNING ${why} " Continuing because ESPOS_ALLOW_IDF_MISMATCH is set.")
        else()
            message(FATAL_ERROR ${why} " To try anyway, pass -DESPOS_ALLOW_IDF_MISMATCH=1.")
        endif()
    elseif(NOT have STREQUAL want)
        message(WARNING "${name}: espOS is tested on ESP-IDF ${want} (${version_file}); you have ${have}, "
                        "which is in the supported range — building on. Report problems with this line.")
    endif()
endfunction()

#
# Assemble SDKCONFIG_DEFAULTS for project(), in this order (later files win):
#
#   <espos>/sdkconfig.d/espos.defaults          the base every firmware shares
#   <espos>/sdkconfig.d/<profile>.defaults      PROFILE / -DESPOS_PROFILE, optional
#   <project>/sdkconfig.defaults                the project's own, if present
#   -DESPOS_EXTRA_SDKCONFIG_DEFAULTS=a;b        extra files, e.g. a board variant
#   <project>/sdkconfig.local                   git-ignored developer overrides
#   <build>/espos_partitions.defaults           generated from PARTITIONS
#
# ESP-IDF appends "<entry>.<IDF_TARGET>" for every entry that has one
# (tools/cmake/kconfig.cmake), so espos.defaults.esp32p4 and the project's
# sdkconfig.defaults.esp32p4 follow their base file without being listed.
#
# This has to happen here, before project(): sdkconfig is generated inside
# idf_build_process() (tools/cmake/build.cmake, __kconfig_generate_config)
# before any component's project_include.cmake or CMakeLists.txt runs, so a
# component cannot contribute defaults — by the time espOS's code runs as a
# component the configuration is final. The only hook a library has is the
# variable project() reads.
#
# A caller that sets SDKCONFIG_DEFAULTS itself (-DSDKCONFIG_DEFAULTS=a;b or the
# environment variable) keeps that list; only the partition fragment is still
# appended, because the table is chosen by PARTITIONS and not by any of the
# defaults files (a list without it would silently fall back to IDF's
# single-app table and every OTA slot would be gone).
#
# ESPOS_EXTRA_SDKCONFIG_DEFAULTS is how to ADD files without that: a board
# variant passed as -DSDKCONFIG_DEFAULTS would replace espos.defaults and with
# it the signed-update settings, and the image would install over USB and be
# refused by every OTA after. The extras win over espOS's and the project's
# defaults; a developer's sdkconfig.local still has the last word.
#
function(_espos_sdkconfig_defaults name profile partitions)
    set(extras)
    foreach(extra IN LISTS ESPOS_EXTRA_SDKCONFIG_DEFAULTS)
        get_filename_component(extra "${extra}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
        # Refused here, naming the variable, rather than left to IDF: a typo
        # in a board name must not get as far as building some other board.
        if(NOT EXISTS "${extra}")
            message(FATAL_ERROR "${name}: ESPOS_EXTRA_SDKCONFIG_DEFAULTS names ${extra}, which does not exist.")
        endif()
        list(APPEND extras "${extra}")
        message(STATUS "${name}: extra sdkconfig defaults ${extra}")
    endforeach()

    if(SDKCONFIG_DEFAULTS)
        set(defaults "${SDKCONFIG_DEFAULTS}")
        message(STATUS "${name}: SDKCONFIG_DEFAULTS given explicitly — not adding espOS's defaults")
    elseif(NOT "$ENV{SDKCONFIG_DEFAULTS}" STREQUAL "")
        set(defaults "$ENV{SDKCONFIG_DEFAULTS}")
        message(STATUS "${name}: SDKCONFIG_DEFAULTS set in the environment — not adding espOS's defaults")
    else()
        set(defaults "${ESPOS_DIR}/sdkconfig.d/espos.defaults")

        if(profile)
            set(overlay "${ESPOS_DIR}/sdkconfig.d/${profile}.defaults")
            if(NOT EXISTS "${overlay}")
                file(GLOB known RELATIVE "${ESPOS_DIR}/sdkconfig.d" "${ESPOS_DIR}/sdkconfig.d/*.defaults")
                list(REMOVE_ITEM known espos.defaults)
                string(REPLACE ".defaults" "" known "${known}")
                message(FATAL_ERROR "${name}: no espOS profile '${profile}' (${overlay}); "
                                    "known profiles: ${known}")
            endif()
            list(APPEND defaults "${overlay}")
            message(STATUS "${name}: sdkconfig profile '${profile}'")
        endif()

        foreach(base sdkconfig.defaults sdkconfig.local)
            if(base STREQUAL "sdkconfig.local")
                list(APPEND defaults ${extras})
                set(extras)
            endif()
            if(EXISTS "${CMAKE_SOURCE_DIR}/${base}")
                list(APPEND defaults "${CMAKE_SOURCE_DIR}/${base}")
            elseif(IDF_TARGET AND EXISTS "${CMAKE_SOURCE_DIR}/${base}.${IDF_TARGET}")
                # IDF only looks for the .<target> variant next to a listed
                # base file; a project with nothing but per-target overrides
                # would otherwise lose them.
                list(APPEND defaults "${CMAKE_SOURCE_DIR}/${base}.${IDF_TARGET}")
            endif()
        endforeach()
    endif()

    # Still set only for an explicit list, which has no sdkconfig.local slot.
    list(APPEND defaults ${extras})

    # Partition table → one generated fragment. The path is absolute because
    # IDF resolves a relative CONFIG_PARTITION_TABLE_CUSTOM_FILENAME against
    # the PROJECT, and the bundled tables live in the submodule.
    if(NOT partitions)
        set(partitions "${ESPOS_PARTITIONS_DIR}/4mb.csv")
    endif()
    get_filename_component(partitions "${partitions}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${partitions}")
        message(FATAL_ERROR "${name}: PARTITIONS '${partitions}' does not exist "
                            "(bundled tables: ${ESPOS_PARTITIONS_DIR}/{4,8,16}mb.csv).")
    endif()
    set(fragment
        "# Generated by espos_project_prologue() on every configure. Change the\n"
        "# PARTITIONS argument (or the CSV it names), not this file.\n"
        "CONFIG_PARTITION_TABLE_CUSTOM=y\n"
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${partitions}\"\n")
    # The bundled tables are sized for one flash size and named after it; a
    # table that does not fit the configured flash fails at build time, so
    # set the size with the table rather than leave the pair to be kept in
    # step by hand. A project's own table says nothing here and keeps
    # whatever its own defaults (or espos.defaults' 4 MB) chose.
    get_filename_component(table_dir "${partitions}" DIRECTORY)
    get_filename_component(table_name "${partitions}" NAME)
    if(table_dir STREQUAL "${ESPOS_PARTITIONS_DIR}" AND table_name MATCHES "^([0-9]+)mb\\.csv$")
        list(APPEND fragment "CONFIG_ESPTOOLPY_FLASHSIZE_${CMAKE_MATCH_1}MB=y\n")
    endif()
    string(CONCAT fragment ${fragment})

    # Rewrite only on change: the file is a configure-time input and a fresh
    # mtime on every run would be noise in anything that watches the build dir.
    set(fragment_file "${CMAKE_BINARY_DIR}/espos_partitions.defaults")
    set(previous "")
    if(EXISTS "${fragment_file}")
        file(READ "${fragment_file}" previous)
    endif()
    if(NOT previous STREQUAL fragment)
        file(WRITE "${fragment_file}" "${fragment}")
    endif()
    list(APPEND defaults "${fragment_file}")
    message(STATUS "${name}: partition table ${partitions}")

    set(SDKCONFIG_DEFAULTS "${defaults}" PARENT_SCOPE)
endfunction()

#
# App-signing key for espOS OTA (docs/ota.md); never committed.
# Missing → generate a DEVELOPMENT key so a fresh checkout builds; devices
# flashed with it will only accept updates signed by that same key.
#
function(_espos_signing_key name key explicit)
    if(NOT EXISTS "${key}" AND explicit)
        # Never invent a key someone named: a release key that is simply not
        # mounted would become a fresh development key, and every device that
        # trusts the real one would refuse the image.
        message(FATAL_ERROR "${name}: SIGNING_KEY ${key} does not exist. A key named explicitly is "
                            "never generated; create it (espos/docs/ota.md) or fix the path.")
    endif()
    if(NOT EXISTS "${key}")
        message(WARNING "${name}: no ${key} — generating a development RSA-3072 signing key. "
                        "For real deployments create and keep your own (espos/docs/ota.md).")
        execute_process(
            COMMAND ${PYTHON} -m espsecure generate_signing_key --version 2 --scheme rsa3072 "${key}"
            RESULT_VARIABLE rc OUTPUT_QUIET)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "${name}: could not generate ${key} (espsecure missing?)")
        endif()
    endif()

    # Re-sign when the KEY changes, not just when the code does.
    #
    # ESP-IDF's signing step depends on the unsigned binary alone
    # (esptool_py/project_include.cmake: DEPENDS "${build_dir}/.bin_timestamp"),
    # so swapping the key and rebuilding silently keeps the signature made with
    # the PREVIOUS key — no source changed, so nothing re-links, and the build
    # log looks completely normal. You get a device that installs fine over USB
    # and then rejects every OTA, with no way to tell why short of running
    # `espsecure verify-signature` by hand.
    #
    # Fingerprint the key (the file's hash — never its contents, which would
    # reach the build directory) and force a re-link whenever it differs.
    # Re-run configure when the key file changes, or this whole check is dead
    # on the path that matters: `idf.py build` after swapping a key changes no
    # CMake input, ninja skips configure, and the image silently keeps the old
    # signature.
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
                 APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${key}")

    file(SHA256 "${key}" hash)
    set(stamp "${CMAKE_BINARY_DIR}/espos_signing_key.stamp")
    set(old "")
    if(EXISTS "${stamp}")
        file(READ "${stamp}" old)
    endif()
    if(NOT old STREQUAL hash)
        if(NOT old STREQUAL "")
            message(STATUS "${name}: signing key changed — forcing a re-link so the "
                           "image is signed with the current key.")
            # Removing the unsigned binary's stamp is what actually re-triggers
            # IDF's sign-data step; deleting only the .bin leaves it satisfied.
            file(REMOVE "${CMAKE_BINARY_DIR}/.bin_timestamp"
                        "${CMAKE_BINARY_DIR}/.signed_bin_timestamp")
        endif()
        file(WRITE "${stamp}" "${hash}")
    endif()

    # IDF signs with CONFIG_SECURE_BOOT_SIGNING_KEY, resolved against the
    # project directory (esptool_py/project_include.cmake), and never looks at
    # this argument. A key named explicitly is handed to it through a
    # generated defaults fragment, appended last so it wins over whatever the
    # project's own defaults say.
    if(explicit)
        set(fragment "# Generated by espos_project_prologue() from SIGNING_KEY. Change the argument, not this file.\n"
                     "CONFIG_SECURE_BOOT_SIGNING_KEY=\"${key}\"\n")
        string(CONCAT fragment ${fragment})
        set(fragment_file "${CMAKE_BINARY_DIR}/espos_signing.defaults")
        set(previous "")
        if(EXISTS "${fragment_file}")
            file(READ "${fragment_file}" previous)
        endif()
        if(NOT previous STREQUAL fragment)
            file(WRITE "${fragment_file}" "${fragment}")
        endif()
        set(SDKCONFIG_DEFAULTS "${SDKCONFIG_DEFAULTS};${fragment_file}" PARENT_SCOPE)
    endif()

    # Defaults only reach a fresh sdkconfig. One that already names a different
    # key keeps signing with it while the fingerprint above follows this one:
    # an image that installs over USB and is refused by every OTA after. Stop.
    if(SDKCONFIG)
        set(cfg "${SDKCONFIG}")
    else()
        set(cfg "${CMAKE_SOURCE_DIR}/sdkconfig")
    endif()
    get_filename_component(cfg "${cfg}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(EXISTS "${cfg}")
        file(STRINGS "${cfg}" configured REGEX "^CONFIG_SECURE_BOOT_SIGNING_KEY=\".*\"$")
        if(configured)
            string(REGEX REPLACE "^CONFIG_SECURE_BOOT_SIGNING_KEY=\"(.*)\"$" "\\1" configured "${configured}")
            get_filename_component(configured "${configured}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
            if(NOT configured STREQUAL key)
                message(FATAL_ERROR "${name}: ${cfg} signs with ${configured}, but the signing key is ${key}. "
                                    "An existing sdkconfig keeps its value (defaults only reach a fresh one), "
                                    "so the image would carry the wrong signature. Delete ${cfg} and configure "
                                    "again, or make SIGNING_KEY name the key it uses.")
            endif()
        endif()
    endif()
endfunction()

#
# espos_project_ui_partition() is NOT defined here. It lives in
# components/espos_httpd/project_include.cmake, which the IDF build system
# includes for every project that has espos_httpd in its component list --
# submodule builds and registry builds alike. espos_httpd owns both ends of
# the web UI (it serves what the function packs, and ships the bundle in
# ui-dist/), and defining it there is what makes it reachable to a firmware
# that consumes espOS from the registry and so never sees this file.
#
