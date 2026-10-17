# espos_usb_storage

A microSD card exported over USB device mode to a chartplotter or PC, which writes to it as a normal storage card, while espOS follows the append-only files the host writes and hands new records on (for example to a Signal K server once WiFi is in range). espOS never writes to the card while it is exported. ESP32-P4 and ESP32-S3.

```sh
idf.py add-dependency "signalk-espos/espos_usb_storage^0.19.1"  # x-release-please-version
```

Part of [espOS](https://github.com/signalk-espOS/espOS), an ESP-IDF runtime for Signal K devices. The components are released in lockstep, so one version of `espos_usb_storage` works with the same version of every other.

**Documentation:** [docs/usb-storage.md](https://github.com/signalk-espOS/espOS/blob/main/docs/usb-storage.md)

**Examples:**

* [`usb_storage_follow`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_usb_storage/examples/usb_storage_follow)

Built on the espOS tree as a git submodule, or installed from the registry; either way `REQUIRES espos_usb_storage` in a component's `CMakeLists.txt` resolves it.

Apache-2.0. Issues and pull requests: [https://github.com/signalk-espOS/espOS/issues](https://github.com/signalk-espOS/espOS/issues).
