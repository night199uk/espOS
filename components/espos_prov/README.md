# espos_prov

BLE provisioning: WiFi credentials from a phone over GATT, without an access point.

```sh
idf.py add-dependency "signalk-espos/espos_prov^0.7"
```

Part of [espOS](https://github.com/signalk-espOS/espOS), an ESP-IDF runtime for Signal K devices. The components are released in lockstep, so one version of `espos_prov` works with the same version of every other.

**Documentation:** [docs/provisioning.md](https://github.com/signalk-espOS/espOS/blob/main/docs/provisioning.md)

Installed from the registry, `REQUIRES espos_prov` in a component's `CMakeLists.txt` resolves it. On the espOS tree as a git submodule it is optional, so the project also names it: `espos_project_prologue(... COMPONENTS espos_prov)`.

Apache-2.0. Issues and pull requests: [https://github.com/signalk-espOS/espOS/issues](https://github.com/signalk-espOS/espOS/issues).
