# Tutorial: a tank level sender

**Newbie.** A resistive tank sender on an analog pin becomes
`tanks.freshWater.0.currentLevel` on the server: voltage-divider and
resistance maths in C, calibration as settings, and the one rule that makes
Signal K instruments understand you — SI units, always. Start where
[first-sensor](first-sensor.md) ends; [add-a-setting](add-a-setting.md) covers the descriptor.

## Wiring

A tank sender is a variable resistor: the common American type reads about
240 Ω empty and 33 Ω full, the European type 0 Ω empty and 190 Ω full. The ADC
measures volts, so the sender is the lower leg (R2) of a divider fed from 3V3 through a fixed R1:

```
3V3 ── R1 (220 Ω) ──┬── ADC pin (GPIO4 on ESP32-C6, GPIO20 on ESP32-P4, as in first-sensor)
                    └── sender (R2) ── GND
```

With R1 = 220 Ω a 240/33 sender puts 1.7 V (empty) to 0.43 V (full) on the
pin at about 13 mA. Two fixed resistors in place of the sender make a bench test.

## The maths, and the SI rule

From the measured voltage `v` and the supply `vin`: `r2 = r1 * v / (vin - v)`,
then `level = (r2 - r_empty) / (r_full - r_empty)`, clamped to `[0, 1]`. For the
American sender `r_full < r_empty` and the ratio still runs 0 → 1; the signs take care of themselves.

Signal K carries every quantity in its SI unit, and the specification names
the unit for every path it defines: `tanks.*.currentLevel` is a **ratio** —
0 to 1, not 0 to 100 — and `capacity` and `currentVolume` are **m³**, not
litres (1 L = 0.001 m³). Publish that and every instrument converts for
display; publish percent and every gauge shows a tank 5000 % full. Spec
paths need no `espos_sk_declare_meta()` — the server already knows the units.

## Settings

The four numbers that change per boat are a descriptor, `main/config/tank.json`, registered with `espos_config_add_descriptor(config/tank.json)`:

```json
{"namespace": "tank", "version": 1, "title": "Fresh water tank",
 "keys": [
  {"name": "r_fixed", "type": "float", "default": 220, "min": 1, "max": 100000, "unit": "Ω", "title": "Fixed resistor R1"},
  {"name": "r_empty", "type": "float", "default": 240, "min": 0, "max": 100000, "unit": "Ω", "title": "Sender at empty"},
  {"name": "r_full", "type": "float", "default": 33, "min": 0, "max": 100000, "unit": "Ω", "title": "Sender at full"},
  {"name": "capacity_m3", "type": "float", "default": 0.1, "min": 0, "max": 100, "unit": "m³", "title": "Capacity"}
 ]}
```

## The code

`main/main.c`, with the includes, `ADC_CH` and the ADC set-up from first-sensor unchanged:

```c
#include "espos_cfg_keys.h"
#include "espos_config.h"

static float s_r1 = 220, s_r_empty = 240, s_r_full = 33, s_cap = 0.1f;

static void load_cfg(const char *ns, const char *key, void *arg)
{
    (void)ns; (void)key; (void)arg;
    espos_config_get_float(ESPOS_CFG_NS_TANK, ESPOS_CFG_TANK_R_FIXED, &s_r1);
    espos_config_get_float(ESPOS_CFG_NS_TANK, ESPOS_CFG_TANK_R_EMPTY, &s_r_empty);
    espos_config_get_float(ESPOS_CFG_NS_TANK, ESPOS_CFG_TANK_R_FULL, &s_r_full);
    espos_config_get_float(ESPOS_CFG_NS_TANK, ESPOS_CFG_TANK_CAPACITY_M3, &s_cap);
}

/* Sender resistance from the divider, level from the two calibration points, clamped. */
static float level_from_mv(int mv)
{
    float v = mv / 1000.0f, vin = 3.3f;
    if (v >= vin - 0.01f) {
        return 0.0f; /* open circuit (sender unplugged): report empty, not garbage */
    }
    float level = (s_r1 * v / (vin - v) - s_r_empty) / (s_r_full - s_r_empty);
    return level < 0 ? 0 : level > 1 ? 1 : level;
}

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));
    load_cfg(NULL, NULL, NULL);
    ESP_ERROR_CHECK(espos_config_subscribe(load_cfg, NULL));
    /* … ADC set-up as in first-sensor … */
    for (;; vTaskDelay(pdMS_TO_TICKS(2000))) {
        long sum = 0;
        int n = 0, raw, mv;
        for (int i = 0; i < 16; i++) { /* the ESP32 ADC is noisy: average 16 conversions per reading */
            if (adc_oneshot_read(adc, ADC_CH, &raw) == ESP_OK && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
                sum += mv, n++;
            }
        }
        if (n == 0) {
            continue;
        }
        float level = level_from_mv((int)(sum / n));
        espos_sk_publish_number("tanks.freshWater.0.currentLevel", level);   /* ratio */
        espos_sk_publish_number("tanks.freshWater.0.capacity", s_cap);       /* m3 */
        espos_sk_publish_number("tanks.freshWater.0.currentVolume", level * s_cap);
    }
}
```

The three publishes fall into one batching window and leave as one delta.
Every two seconds is plenty for a tank; a sloshing sender is better averaged than reported.

## See it

Flash, then in the Data Browser filter `tanks`: `tanks.freshWater.0.currentLevel`
between 0 and 1, `capacity` 0.1, `currentVolume` their product. Swap the bench
resistor from 240 Ω to 33 Ω and the level goes 0 → 1; 120 Ω reads about 0.58.

```sh
curl -s http://<server>:3000/signalk/v1/api/vessels/self/tanks/freshWater/0/currentLevel
```

Set the real capacity on the Config page (`Fresh water tank → Capacity`, in
m³: a 150 L tank is `0.15`) and `currentVolume` follows on the next reading.
Instruments showing litres or percent convert from the SI values themselves.
