# espos_n2k

NMEA 2000 over TWAI: a candump TCP server, and an optional NMEA 2000 node (`CONFIG_ESPOS_N2K_NODE`) with address claim, instances and product information. The node's protocol layer is the vendored [NMEA2000](https://github.com/ttlappalainen/NMEA2000) library (MIT, `third_party/NMEA2000/`).

```sh
idf.py add-dependency "signalk-espos/espos_n2k^0.17.1"  # x-release-please-version
```

Part of [espOS](https://github.com/signalk-espOS/espOS), an ESP-IDF runtime for Signal K devices. The components are released in lockstep, so one version of `espos_n2k` works with the same version of every other.

**Documentation:** [docs/n2k.md](https://github.com/signalk-espOS/espOS/blob/main/docs/n2k.md)

**Examples:**

* [`n2k_candump`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_n2k/examples/n2k_candump)
* [`n2k_switch_bank`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_n2k/examples/n2k_switch_bank)

Built on the espOS tree as a git submodule, or installed from the registry; either way `REQUIRES espos_n2k` in a component's `CMakeLists.txt` resolves it.

Apache-2.0; the vendored library is MIT. Issues and pull requests: [https://github.com/signalk-espOS/espOS/issues](https://github.com/signalk-espOS/espOS/issues).
