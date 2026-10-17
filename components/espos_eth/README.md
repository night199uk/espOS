# espos_eth

Wired Ethernet (internal EMAC + RMII PHY) as an espos_net transport; preferred over WiFi when both are up.

```sh
idf.py add-dependency "signalk-espos/espos_eth^0.7"
```

Part of [espOS](https://github.com/signalk-espOS/espOS), an ESP-IDF runtime for Signal K devices. The components are released in lockstep, so one version of `espos_eth` works with the same version of every other.

**Documentation:** [docs/net.md](https://github.com/signalk-espOS/espOS/blob/main/docs/net.md)

**Examples:**

* [`ethernet`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_eth/examples/ethernet)

Installed from the registry, `REQUIRES espos_eth` in a component's `CMakeLists.txt` resolves it. On the espOS tree as a git submodule it is optional, so the project also names it: `espos_project_prologue(... COMPONENTS espos_eth)`.

Apache-2.0. Issues and pull requests: [https://github.com/signalk-espOS/espOS/issues](https://github.com/signalk-espOS/espOS/issues).
