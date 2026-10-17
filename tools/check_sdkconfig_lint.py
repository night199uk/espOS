#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
"""Keep the consumer sdkconfig lint and sdkconfig.d/espos.defaults in step.

They are two expressions of one truth. The defaults file *provides* the
settings to a firmware that includes the prologue; the lint in
espos_core/project_include.cmake *demands* them from a firmware that does not
-- one installed from the component registry, where the prologue cannot
reach. A setting added to the defaults and not to the lint is invisible to
exactly the consumers who have no other way of learning about it.

Not every default has to be linted. Some are tuning a project may legitimately
choose differently; those are listed in COSMETIC below, with the reason. The
point of this check is that the choice is made deliberately rather than by
forgetting.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULTS = ROOT / "sdkconfig.d" / "espos.defaults"
LINT = ROOT / "components" / "espos_core" / "project_include.cmake"

# Settings espOS sets for itself but does not demand of a consumer, and why.
COSMETIC = {
    "CONFIG_LOG_DEFAULT_LEVEL_INFO": "log verbosity is the project's call",
    "CONFIG_ESPTOOLPY_FLASHSIZE_4MB": "follows the partition table the project picks",
    "CONFIG_SECURE_BOOT_SIGNING_KEY": "path, not a policy; the project names its own key",
    "CONFIG_ESP_TASK_WDT_TIMEOUT_S": "espOS's own watchdog budget, not a build requirement",
    "CONFIG_ESP_TASK_WDT_PANIC": "espOS's own watchdog policy",
    "CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP": "espos_ota refuses plain HTTP itself unless asked",
    "CONFIG_HTTPD_MAX_REQ_HDR_LEN": "raised for long SignalK tokens; a smaller value still builds",
    "CONFIG_HTTPD_MAX_URI_LEN": "raised for long SignalK paths; a smaller value still builds",
    "CONFIG_HTTPD_ERR_RESP_NO_DELAY": "latency tuning",
    "CONFIG_LWIP_DHCP_GET_NTP_SRV": "espos_time falls back to configured servers",
    "CONFIG_MBEDTLS_DYNAMIC_BUFFER": "TLS memory tuning",
    "CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA": "TLS memory tuning",
    "CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT": "TLS memory tuning",
    "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN": "TLS record size; smaller works with smaller servers",
    "CONFIG_MBEDTLS_TLS_CLIENT_ONLY": "size tuning; a server build still works",
    "CONFIG_COMPILER_OPTIMIZATION_SIZE": "size tuning; -Og builds and runs, the project weighs its own slot",
}


def defaults_settings() -> set[str]:
    return {
        m.group(1)
        for line in DEFAULTS.read_text().splitlines()
        if (m := re.match(r"^(CONFIG_[A-Z0-9_]+)=", line.strip()))
    }


def linted_settings() -> set[str]:
    return set(re.findall(r"CONFIG_[A-Z0-9_]+", LINT.read_text()))


def main() -> int:
    in_defaults, linted = defaults_settings(), linted_settings()

    unaccounted = sorted(in_defaults - linted - set(COSMETIC))
    stale = sorted(k for k in COSMETIC if k not in in_defaults)

    rc = 0
    if unaccounted:
        rc = 1
        print("Settings in sdkconfig.d/espos.defaults that the consumer lint neither")
        print("checks nor lists as cosmetic:\n")
        for k in unaccounted:
            print(f"  {k}")
        print(
            "\nA firmware that installs espOS from the registry never sees the defaults\n"
            "file, so an unchecked setting is one it has no way of learning about.\n"
            "Either add a check to components/espos_core/project_include.cmake, or add\n"
            "the setting to COSMETIC in this file with the reason it is optional."
        )

    if stale:
        rc = 1
        print("\nCOSMETIC lists settings that are no longer in espos.defaults:\n")
        for k in stale:
            print(f"  {k}")
        print("\nDrop them from COSMETIC in this file.")

    if rc == 0:
        checked = len(in_defaults) - len(COSMETIC)
        print(
            f"check_sdkconfig_lint: {len(in_defaults)} settings, "
            f"{checked} required of consumers, {len(COSMETIC)} optional by design"
        )
    return rc


if __name__ == "__main__":
    sys.exit(main())
