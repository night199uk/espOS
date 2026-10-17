# espos_sdlog

microSD for espOS: a card over SDMMC or SPI, and sessions of rows on it that survive a power cut, each closed with a row count and a sha256 for the server it is delivered to.

```sh
idf.py add-dependency "signalk-espos/espos_sdlog^0.19.1"  # x-release-please-version
```

Part of [espOS](https://github.com/signalk-espOS/espOS), an ESP-IDF runtime for Signal K devices. The components are released in lockstep, so one version of `espos_sdlog` works with the same version of every other.

**Documentation:** [docs/sdlog.md](https://github.com/signalk-espOS/espOS/blob/main/docs/sdlog.md)

**Examples:**

* [`sd_logger`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sdlog/examples/sd_logger)

Built on the espOS tree as a git submodule, or installed from the registry; either way `REQUIRES espos_sdlog` in a component's `CMakeLists.txt` resolves it.

Apache-2.0. Issues and pull requests: [https://github.com/signalk-espOS/espOS/issues](https://github.com/signalk-espOS/espOS/issues).
