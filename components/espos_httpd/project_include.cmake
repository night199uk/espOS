# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_project_ui_partition() lives here rather than in the prologue because
# espos_httpd owns both ends of the web UI: src/static.c mounts the "storage"
# partition and serves what this function packs into it, and embeds the
# placeholder page shown when that partition has no index.html.
#
# It also makes the function reachable WITHOUT the prologue. A firmware that
# consumes espOS from the component registry never includes
# cmake/espos_project.cmake -- that file is not in the published archives and
# cannot be, since the prologue must run before project() and a component's
# project_include.cmake runs after. The bundle itself ships in this component
# (ui-dist/), which is the same rule the rest of the repo already follows:
# anything a component needs at build time lives inside the component.
#
# Included by the IDF build system after sdkconfig is generated and before any
# component CMakeLists.txt runs. Defining a function here is safe at that
# point; CALLING littlefs_create_partition_image is not, which is why the
# function is called from the consumer's CMakeLists.txt after project().

#
# espos_project_ui_partition([PARTITION <name>] [DIR <dir>] [NAME <label>])
#
# Pack the espOS web UI into a LittleFS image flashed with `idf.py flash`.
# The default bundle is also built into the app image and served from there
# (CONFIG_ESPOS_HTTPD_UI_EMBED), because OTA never rewrites this partition;
# a custom DIR is served from the partition instead.
# Call AFTER project() -- littlefs_create_partition_image comes from the
# managed component. Default DIR is the bundle committed in this component, so
# a firmware build never needs Node; `npm run build` in <espos>/ui regenerates
# it when the UI changes, and CI fails if the commit forgot to.
#
# Captured at include time. Inside the function CMAKE_CURRENT_LIST_DIR is the
# CALLER's directory (the consumer's project root), not this file's, because
# the function body is evaluated where it is called.
set(ESPOS_HTTPD_UI_DIST "${CMAKE_CURRENT_LIST_DIR}/ui-dist" CACHE INTERNAL
    "espOS web UI bundle shipped inside espos_httpd")

function(espos_project_ui_partition)
    cmake_parse_arguments(_UI "" "PARTITION;DIR;NAME" "" ${ARGN})
    if(NOT _UI_PARTITION)
        set(_UI_PARTITION storage)
    endif()
    # Resolves the same whether espos_httpd came from a submodule checkout or
    # from managed_components/, because it was captured where this file lives.
    set(_UI_DEFAULT_DIR "${ESPOS_HTTPD_UI_DIST}")
    if(NOT _UI_DIR)
        set(_UI_DIR "${_UI_DEFAULT_DIR}")
    endif()
    if(NOT _UI_NAME)
        get_filename_component(_UI_NAME "${CMAKE_SOURCE_DIR}" NAME)
    endif()

    if(EXISTS "${_UI_DIR}/index.html.gz")
        littlefs_create_partition_image(${_UI_PARTITION} "${_UI_DIR}" FLASH_IN_PROJECT)
        message(STATUS "${_UI_NAME}: UI bundle from ${_UI_DIR} will be flashed to '${_UI_PARTITION}'")
        if(NOT _UI_DIR STREQUAL _UI_DEFAULT_DIR)
            # A custom UI lives only in the partition: serve it ahead of the
            # espOS bundle built into the app.
            idf_component_get_property(_ui_lib espos_httpd COMPONENT_LIB)
            target_compile_definitions(${_ui_lib} PRIVATE ESPOS_HTTPD_UI_FROM_STORAGE=1)
        endif()
    elseif(_UI_DIR STREQUAL _UI_DEFAULT_DIR)
        # The default bundle ships with the component; its absence is a damaged
        # checkout or a truncated package, not a skipped build step, and a
        # firmware that ships the placeholder page instead of the config UI
        # must not come out of it.
        message(FATAL_ERROR "${_UI_NAME}: ${_UI_DIR}/index.html.gz — the espos_httpd UI bundle is "
                            "missing. Restore components/espos_httpd/ui-dist from git, rebuild it "
                            "with `npm ci && npm run build` in <espos>/ui, or reinstall the "
                            "signalk-espos/espos_httpd component.")
    else()
        # WARNING, not STATUS: a STATUS line disappears into cmake's output and
        # the device then silently serves the placeholder page instead of the
        # real config UI, which looks like a firmware bug rather than a missing
        # build step.
        message(WARNING "${_UI_NAME}: ${_UI_DIR} missing — the device will serve the placeholder page, "
                        "not the espOS web UI. Build it, or drop DIR to use the bundle that ships "
                        "with espos_httpd.")
    endif()
endfunction()
