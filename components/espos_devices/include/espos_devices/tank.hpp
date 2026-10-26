// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// TankLevel — a tank sender on a Signal K path.
//
// The sender can be resistive (the common 240-33 or 10-180 ohm float arm) or
// voltage-based (a sender with a built-in divider, or an external ADC such as
// the ADS1115). The calibration is either the two-point empty/full
// approximation or a multi-point curve the user can edit on the web UI.
//
//   // Resistive sender on an ADC pin, two-point calibration:
//   espos::devices::TankLevel fresh(g, {
//       .id = "fresh", .type = TankType::kFreshWater, .instance = 0,
//       .location = "Starboard",
//       .gpio = 4, .empty_ohms = 190.0f, .full_ohms = 3.0f,
//   });
//   fresh.start();
//
//   // Voltage sender from an external ADC, multi-point curve:
//   espos::devices::TankLevel holding(g, {
//       .id = "hold", .type = TankType::kBlackWater, .instance = 8,
//       .location = "Port", .position = "Forward",
//       .sensor = espos::devices::TankSensor::kVoltage,
//       .curve = kHoldingCurve, .curve_count = std::size(kHoldingCurve),
//       .capacity_m3 = 0.08f,
//   }, ads1115_channel);
//
// The device owns the ADC only when constructed with a GPIO; pass a
// `flow::Producer<float>&` to use an external reading instead. For a voltage
// sensor that producer is in volts, for a resistance sensor in ohms. A
// `scale` can be supplied for a voltage sensor when the producer's volts are
// still after a divider.
//
// `type` and `instance` derive tanks.<type>.<instance>.{currentLevel,
// capacity,currentVolume}; explicit path fields override an individual path.
// `capacity_m3` enables the capacity/currentVolume outputs. `location` and
// `position` generate a display name such as "Port Forward Blackwater Tank",
// and `level_zones` supplies the currentLevel alarm zones. `display_name` and
// `description` override the generated strings.
//
// What it does NOT hide: every node is still reachable (`analog()`,
// `ohms()`, `curve()`, `level()`, `output()`), because a device class that
// cannot be taken apart is a worse deal than the wiring it replaced. The
// generated path strings live in this object, so keep a TankLevel alive for
// the life of the graph (a local in app_main, or a heap singleton).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "esp_err.h"

#include "espos_devices/detail.hpp"
#include "espos_flow/graph.hpp"
#include "espos_flow/transforms.hpp"
#include "espos_formulas/curve.hpp"
#include "espos_sensors/sensors.hpp"
#include "espos_sk_flow/sk.hpp"

namespace espos::devices
{

// The Signal K tank category. The path segment and the human-readable name
// are derived from this; the N2K sender maps it to tN2kFluidType where the
// NMEA2000 library is already present.
enum class TankType : uint8_t {
    kFreshWater,
    kWasteWater,
    kBlackWater,
    kFuel,
    kLubrication,
    kLiveWell,
    kBaitWell,
    kGas,
    kBallast,
};

namespace detail
{

struct TankTypeInfo {
    const char *path;
    const char *name;
};

inline const TankTypeInfo &tank_type_info(TankType type)
{
    static constexpr TankTypeInfo kInfo[] = {
        { "freshWater", "Freshwater" },
        { "wasteWater", "Waste Water" },
        { "blackWater", "Blackwater" },
        { "fuel", "Fuel" },
        { "lubrication", "Lubrication" },
        { "liveWell", "Live Well" },
        { "baitWell", "Bait Well" },
        { "gas", "Gas" },
        { "ballast", "Ballast" },
    };
    const auto index = static_cast<std::size_t>(type);
    return kInfo[index < sizeof(kInfo) / sizeof(kInfo[0]) ? index : 0];
}

}  // namespace detail

enum class TankSensor {
    // The reading is the sender's resistance in ohms.
    kResistance,
    // The reading is a voltage in volts. Use this for a sender with its own
    // divider and for an external ADC such as the ADS1115.
    kVoltage,
};

struct TankLevelConfig {
    const char *id = "tank";
    // The tank category and its Signal K instance. These are enough to derive
    // the specification paths:
    //   tanks.<type>.<instance>.currentLevel
    //   tanks.<type>.<instance>.capacity
    //   tanks.<type>.<instance>.currentVolume
    TankType type = TankType::kFreshWater;
    uint8_t instance = 0;

    // Human-readable placement for the generated display name. Optional;
    // examples are "Starboard"/"Port" and "Forward"/"Aft"/"Midship".
    const char *location = nullptr;
    const char *position = nullptr;
    // Override the generated name/description when this tank needs something
    // the type and placement cannot express.
    const char *display_name = nullptr;
    const char *description = nullptr;
    // Optional JSON array of Signal K alarm zones for currentLevel. The
    // device does not invent fluid-specific zones; a project that wants them
    // supplies its table here and they are merged into the metadata.
    const char *level_zones = nullptr;

    int gpio = -1;
    // Optional path overrides. Null means derive from type/instance.
    const char *path = nullptr;

    // Two-point calibration for a resistance sender, in ohms. A US sender is
    // often 240-33; a European one 10-180. Either order is fine: the
    // transform divides by (full - empty), so an empty > full sender still
    // runs 0 -> 1.
    float empty_ohms = 190.0f;
    float full_ohms = 3.0f;

    // The fixed leg of the divider the sender sits in, and the voltage across
    // the pair. Only used with the built-in ADC; an external reading is
    // already in the unit named by `sensor`.
    float divider_ohms = 1000.0f;
    float supply_volts = 3.3f;

    uint32_t period_ms = 2000;  // a tank does not move quickly
    uint8_t samples = 16;

    // Which physical quantity the sender (or the external producer) reports.
    // The default preserves the original resistive-sender API.
    TankSensor sensor = TankSensor::kResistance;

    // Voltage sensors only: multiply the reading before calibration. Use 1.0
    // for a producer already in sender volts (the ADS1115 after the HALMET
    // divider) and the divider ratio when the producer still reads the ADC
    // pin, e.g. 33.3f / 3.3f.
    float scale = 1.0f;

    // Two-point calibration for a voltage sender, in volts. Used when no
    // curve is supplied.
    float empty_volts = 0.0f;
    float full_volts = 3.3f;

    // Optional multi-point calibration in sensor units (ohms for a
    // resistance sender, volts for a voltage sender) to a ratio. When
    // present, this wins over the two-point values and is registered as a
    // table on the config page.
    const espos::formulas::CurveSample *curve = nullptr;
    std::size_t curve_count = 0;

    // Optional capacity/currentVolume outputs. Both paths are Signal K
    // specification paths, in m3; the capacity is a constant that is
    // re-published periodically so a late dashboard still sees it. Leave a
    // path null to disable that output; capacity_m3 <= 0 disables both.
    const char *capacity_path = nullptr;
    const char *volume_path = nullptr;
    float capacity_m3 = 0.0f;
    uint32_t capacity_period_ms = 10000;
};

// The capacity/currentVolume pair after the level has been calibrated.
// Publishes the constant capacity on a slow cadence and volume on every
// level; both are optional, so a level-only tank costs no node.
class TankValues : public espos::flow::NodeBase,
                   public espos::flow::Consumer<float>
{
  public:
    TankValues(const char *id, const char *capacity_path,
               const char *volume_path, float capacity_m3,
               uint32_t capacity_period_ms)
        : NodeBase(id),
          capacity_path_(capacity_path),
          volume_path_(volume_path),
          capacity_m3_(capacity_m3),
          capacity_period_ms_(capacity_period_ms)
    {
    }

    using consumes_type = float;

    void set(const float &level) override
    {
        ESPOS_FLOW_ASSERT_TASK(id());
        const uint32_t now = espos_flow_now_ms();
        if (capacity_path_ != nullptr &&
            (!capacity_sent_ ||
             (uint32_t)(now - capacity_last_ms_) >= capacity_period_ms_)) {
            espos_sk_publish_number(capacity_path_, capacity_m3_);
            capacity_last_ms_ = now;
            capacity_sent_ = true;
        }
        if (volume_path_ != nullptr) {
            espos_sk_publish_number(volume_path_, level * capacity_m3_);
        }
    }

  private:
    const char *capacity_path_;
    const char *volume_path_;
    float capacity_m3_;
    uint32_t capacity_period_ms_;
    uint32_t capacity_last_ms_ = 0;
    bool capacity_sent_ = false;
};

class TankLevel
{
  public:
    // A sender on the built-in ADC. `cfg.gpio` must be set; the resulting
    // Analog node's open error is reported by start()/open_error().
    TankLevel(espos::flow::Graph &g, const TankLevelConfig &cfg)
        : TankLevel(g, cfg, nullptr)
    {
    }

    // A sender read by something else -- an ADS1115, another ESP32, a test
    // Value. The producer must emit the unit named by `cfg.sensor` (volts or
    // ohms); `cfg.scale` still applies to a voltage reading. `cfg.gpio` is
    // ignored: the caller owns the source.
    TankLevel(espos::flow::Graph &g, const TankLevelConfig &cfg,
              espos::flow::Producer<float> &reading)
        : TankLevel(g, cfg, &reading)
    {
    }

    // Arm the built-in ADC poll. An external producer is started by its
    // owner, so this is a no-op for that constructor.
    esp_err_t start()
    {
        if (config_error_ != ESP_OK) return config_error_;
        return analog_ != nullptr ? analog_->start() : ESP_OK;
    }

    void stop()
    {
        if (analog_ != nullptr) analog_->stop();
    }

    // Put the calibration on a config page. A curve is a row editor; without
    // one the two-point empty/full values are shown. The divider and voltage
    // scale are settings too, because the soldered resistor and the supply
    // rail are not exactly their nominal values.
    esp_err_t register_config(const char *title = "Tank calibration")
    {
        const char *unit =
            cfg_.sensor == TankSensor::kVoltage ? "V" : "ohm";
        esp_err_t err = ESP_OK;
        if (curve_ != nullptr) {
            err = curve_->register_config(title, unit, "ratio");
        } else {
            err = level_.register_config(title, unit);
        }
        if (err != ESP_OK) return err;
        if (divider_ != nullptr) {
            err = divider_->register_config("Tank divider");
        }
        if (err != ESP_OK) return err;
        if (scale_ != nullptr) {
            err = scale_->register_config("Tank voltage scale");
        }
        return err;
    }

    // The nodes, for a firmware that wants to do more than the default. A
    // device class that cannot be opened is worse than no device class.
    //
    // analog() and ohms() exist only when this instance owns the built-in
    // ADC and the sender is resistive; check has_analog()/has_divider()
    // first. level() is always present, but is only wired when there is no
    // curve; use curve() to find the multi-point node.
    espos::sensors::Analog &analog() { return *analog_; }
    espos::flow::DividerR2 &ohms() { return *divider_; }
    espos::flow::Curve<32> *curve() { return curve_; }
    espos::flow::TankLevel &level() { return level_; }
    // The calibrated level, whichever calibration is in use. This is the
    // producer to wire into an N2K sender or another consumer; level() is
    // the two-point node and is only the active one when curve() is null.
    espos::flow::Producer<float> &level_out()
    {
        return curve_ != nullptr ? static_cast<espos::flow::Producer<float> &>(
                                       *curve_)
                                 : static_cast<espos::flow::Producer<float> &>(
                                       level_);
    }
    espos::flow::Producer<float> &source() { return *source_; }
    espos::sk::Output<float> &output() { return out_; }
    bool has_analog() const { return analog_ != nullptr; }
    bool has_divider() const { return divider_ != nullptr; }
    bool has_values() const { return values_ != nullptr; }

    esp_err_t open_error() const
    {
        if (config_error_ != ESP_OK) return config_error_;
        return analog_ != nullptr ? analog_->open_error() : ESP_OK;
    }

  private:
    TankLevel(espos::flow::Graph &g, const TankLevelConfig &cfg,
              espos::flow::Producer<float> *external)
        : cfg_(cfg),
          paths_(make_paths(cfg)),
          analog_(external != nullptr ? nullptr : make_analog(g, cfg)),
          divider_(external != nullptr ? nullptr : make_divider(g, cfg)),
          scale_(make_scale(g, cfg)),
          curve_(make_curve(g, cfg)),
          values_(make_values(g, cfg, paths_)),
          level_(g.make<espos::flow::TankLevel>(
              detail::SubId(cfg.id, 'l'), empty_reading(cfg),
              full_reading(cfg))),
          out_(g.make<espos::sk::Output<float>>(paths_.level))
    {
        if (external == nullptr && analog_ == nullptr) {
            config_error_ = ESP_ERR_INVALID_ARG;
        }
        source_ = external != nullptr
                      ? external
                      : static_cast<espos::flow::Producer<float> *>(analog_);
        wire();
        declare_meta();
    }

    struct Paths {
        char level[64];
        char capacity[64];
        char volume[64];
    };

    static void copy_path(char *out, std::size_t n, const char *override_path,
                          const char *type, uint8_t instance,
                          const char *suffix)
    {
        if (override_path != nullptr && *override_path != '\0') {
            std::snprintf(out, n, "%s", override_path);
            return;
        }
        std::snprintf(out, n, "tanks.%s.%u.%s", type,
                      static_cast<unsigned>(instance), suffix);
    }

    static Paths make_paths(const TankLevelConfig &cfg)
    {
        const char *type = detail::tank_type_info(cfg.type).path;
        Paths paths {};
        copy_path(paths.level, sizeof(paths.level), cfg.path, type,
                  cfg.instance, "currentLevel");
        copy_path(paths.capacity, sizeof(paths.capacity), cfg.capacity_path,
                  type, cfg.instance, "capacity");
        copy_path(paths.volume, sizeof(paths.volume), cfg.volume_path, type,
                  cfg.instance, "currentVolume");
        return paths;
    }

    static void make_display_name(const TankLevelConfig &cfg, char *out,
                                  std::size_t n)
    {
        if (cfg.display_name != nullptr && *cfg.display_name != '\0') {
            std::snprintf(out, n, "%s", cfg.display_name);
            return;
        }
        const char *type = detail::tank_type_info(cfg.type).name;
        if (cfg.location != nullptr && cfg.position != nullptr) {
            std::snprintf(out, n, "%s %s %s Tank", cfg.location,
                          cfg.position, type);
        } else if (cfg.location != nullptr) {
            std::snprintf(out, n, "%s %s Tank", cfg.location, type);
        } else if (cfg.position != nullptr) {
            std::snprintf(out, n, "%s %s Tank", cfg.position, type);
        } else {
            std::snprintf(out, n, "%s Tank", type);
        }
    }

    void declare_meta()
    {
        char display[96];
        char description[128];
        char json[512];
        make_display_name(cfg_, display, sizeof(display));
        if (cfg_.description != nullptr && *cfg_.description != '\0') {
            std::snprintf(description, sizeof(description), "%s",
                          cfg_.description);
        } else {
            std::snprintf(description, sizeof(description), "%s level",
                          display);
        }
        if (cfg_.level_zones != nullptr && *cfg_.level_zones != '\0') {
            std::snprintf(
                json, sizeof(json),
                "{\"units\":\"ratio\",\"displayUnits\":{\"category\":"
                "\"percentage\"},\"description\":\"%s\","
                "\"displayName\":\"%s\",\"zones\":%s}",
                description, display, cfg_.level_zones);
        } else {
            std::snprintf(
                json, sizeof(json),
                "{\"units\":\"ratio\",\"displayUnits\":{\"category\":"
                "\"percentage\"},\"description\":\"%s\","
                "\"displayName\":\"%s\"}",
                description, display);
        }
        espos_sk_declare_meta(paths_.level, json, cfg_.period_ms);

        if (values_ == nullptr) return;
        std::snprintf(json, sizeof(json),
                      "{\"units\":\"m3\",\"description\":\"%s "
                      "capacity\",\"displayName\":\"%s capacity\"}",
                      display, display);
        espos_sk_declare_meta(paths_.capacity, json, 0);
        std::snprintf(json, sizeof(json),
                      "{\"units\":\"m3\",\"description\":\"Calculated "
                      "%s remaining volume\",\"displayName\":\"%s "
                      "volume\"}",
                      display, display);
        espos_sk_declare_meta(paths_.volume, json, cfg_.period_ms);
    }

    static espos::sensors::Analog *make_analog(espos::flow::Graph &g,
                                               const TankLevelConfig &cfg)
    {
        if (cfg.gpio < 0) return nullptr;
        return &g.make<espos::sensors::Analog>(
            cfg.id, cfg.gpio, cfg.period_ms, ESPOS_ADC_ATTEN_12DB,
            cfg.samples);
    }

    static espos::flow::DividerR2 *make_divider(espos::flow::Graph &g,
                                                const TankLevelConfig &cfg)
    {
        if (cfg.gpio < 0 || cfg.sensor != TankSensor::kResistance) {
            return nullptr;
        }
        return &g.make<espos::flow::DividerR2>(
            detail::SubId(cfg.id, 'r'), cfg.supply_volts, cfg.divider_ohms);
    }

    static espos::flow::Linear<float> *make_scale(espos::flow::Graph &g,
                                                  const TankLevelConfig &cfg)
    {
        if (cfg.sensor != TankSensor::kVoltage || cfg.scale == 1.0f) {
            return nullptr;
        }
        return &g.make<espos::flow::Linear<float>>(
            detail::SubId(cfg.id, 's'), cfg.scale, 0.0f);
    }

    static espos::flow::Curve<32> *make_curve(espos::flow::Graph &g,
                                              const TankLevelConfig &cfg)
    {
        if (cfg.curve == nullptr || cfg.curve_count == 0) return nullptr;
        return &g.make<espos::flow::Curve<32>>(
            detail::SubId(cfg.id, 'c'), cfg.curve, cfg.curve_count);
    }

    static TankValues *make_values(espos::flow::Graph &g,
                                   const TankLevelConfig &cfg,
                                   const Paths &paths)
    {
        if (cfg.capacity_m3 <= 0.0f) return nullptr;
        return &g.make<TankValues>(
            detail::SubId(cfg.id, 'v'), paths.capacity, paths.volume,
            cfg.capacity_m3, cfg.capacity_period_ms);
    }

    static float empty_reading(const TankLevelConfig &cfg)
    {
        return cfg.sensor == TankSensor::kVoltage ? cfg.empty_volts
                                                  : cfg.empty_ohms;
    }

    static float full_reading(const TankLevelConfig &cfg)
    {
        return cfg.sensor == TankSensor::kVoltage ? cfg.full_volts
                                                  : cfg.full_ohms;
    }

    void wire()
    {
        if (source_ == nullptr) return;
        espos::flow::Producer<float> *p = source_;
        if (scale_ != nullptr) {
            *p >> *scale_;
            p = scale_;
        }
        if (divider_ != nullptr) {
            *p >> *divider_;
            p = divider_;
        }
        if (curve_ != nullptr) {
            *p >> *curve_;
            *curve_ >> out_;
            if (values_ != nullptr) *curve_ >> *values_;
        } else {
            *p >> level_;
            level_ >> out_;
            if (values_ != nullptr) level_ >> *values_;
        }
    }

    TankLevelConfig cfg_;
    Paths paths_;
    espos::sensors::Analog *analog_ = nullptr;
    espos::flow::DividerR2 *divider_ = nullptr;
    espos::flow::Linear<float> *scale_ = nullptr;
    espos::flow::Curve<32> *curve_ = nullptr;
    TankValues *values_ = nullptr;
    espos::flow::TankLevel &level_;
    espos::sk::Output<float> &out_;
    espos::flow::Producer<float> *source_ = nullptr;
    esp_err_t config_error_ = ESP_OK;
};

}  // namespace espos::devices
