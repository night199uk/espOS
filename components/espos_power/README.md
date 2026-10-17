# espos_power

Deep-sleep duty cycle for battery devices: wake, publish, flush, sleep — without losing the way back in.

```sh
idf.py add-dependency "signalk-espos/espos_power^0.7"
```

Part of [espOS](https://github.com/signalk-espOS/espOS), an ESP-IDF runtime for Signal K devices. The components are released in lockstep, so one version of `espos_power` works with the same version of every other.

**Documentation:** [docs/power.md](https://github.com/signalk-espOS/espOS/blob/main/docs/power.md)

**Examples:**

* [`duty_cycle`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_power/examples/duty_cycle)

Installed from the registry, `REQUIRES espos_power` in a component's `CMakeLists.txt` resolves it. On the espOS tree as a git submodule it is optional, so the project also names it: `espos_project_prologue(... COMPONENTS espos_power)`.

Apache-2.0. Issues and pull requests: [https://github.com/signalk-espOS/espOS/issues](https://github.com/signalk-espOS/espOS/issues).
