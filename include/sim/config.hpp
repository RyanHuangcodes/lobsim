#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "calib/calibration.hpp"
#include "lob/types.hpp"
#include "sim/latency.hpp"
#include "sim/messages.hpp"
#include "sim/reference_price.hpp"

namespace sim {

struct FundamentalConfig {
  double sigma_ticks_per_sqrt_s{1.0};
  double dt_s{0.1};
};

struct NoiseTradersConfig {
  ReferencePrice reference_price{ReferencePrice::Floor};  // optional key; absent means Floor (SPEC 5.6)
  std::uint32_t count{0};
  double lambda{5.0};  // wakeups per second, per trader
  double p_limit{0.85};
  double order_lifetime_mean_s{30.0};
  lob::Price D{10};
  lob::Qty max_size{10};
  LatencySpec latency;
};

struct InformedConfig {
  double lambda{2.0};
  double theta_ticks{2.0};
  lob::Qty size{5};
  LatencySpec latency;
};

struct MarketMakerConfig {
  double gamma{0.0001};
  double kappa{1.5};
  double sigma_ticks_per_sqrt_s{1.0};
  lob::Qty quote_size{5};
  lob::Qty q_max{50};
  double requote_interval_ms{100.0};
  double maker_fee_ticks{0.0};
  double taker_fee_ticks{0.0};
  LatencySpec latency;
};

// The optional `background` section: one CalibratedFlowAgent that generates the whole
// background flow from a calibration file (SPEC upgrade 3, 3). Absent: the noise traders.
struct BackgroundConfig {
  std::string file;  // as given (relative to the config file's directory) or absolute, once resolved
  std::uint32_t owner_count{50};
  LatencySpec latency;
  std::optional<lob::Price> start_price;  // default: the calibration's median_mid_ticks
  // Loaded and validated with the config, shared by copies of the config.
  std::shared_ptr<const calib::Calibration> calibration;
};

// Mirrors configs/default.json (SPEC 5.8) field for field. The `_ns` members are derived
// once by finalize(): durations are never recomputed from doubles at run time.
struct SimConfig {
  std::uint64_t seed{42};
  double duration_s{3600.0};
  double warmup_s{60.0};
  lob::Price max_price{1'000'000};
  lob::Price initial_price{10'000};
  double tick_display{0.01};
  FundamentalConfig fundamental;
  NoiseTradersConfig noise_traders;
  std::optional<InformedConfig> informed;  // the agent exists only if the section does
  std::optional<MarketMakerConfig> mm;
  std::optional<BackgroundConfig> background;  // the section exists only if the config has it
  std::vector<double> markout_horizons_s;
  double sample_interval_s{1.0};

  TimeNs duration_ns{0};
  TimeNs warmup_ns{0};
  TimeNs fundamental_dt_ns{0};
  TimeNs sample_interval_ns{0};
  TimeNs requote_interval_ns{0};
  std::vector<TimeNs> markout_horizons_ns;
};

// Parses and validates. Throws std::runtime_error naming the field for a missing key, a
// wrong type, an unknown key (a typo must not be silently ignored) or a bad value.
// `base_dir` is what a relative `background.file` is resolved against (load_config passes the
// config file's directory); empty means the current directory. The calibration file is read
// and validated here.
SimConfig parse_config(const nlohmann::json& j, const std::string& base_dir = {});
SimConfig load_config(const std::string& path);

// Checks every value and fills the derived `_ns` members. Call again after changing a
// field (for example a command-line override).
void finalize(SimConfig& cfg);

// The effective configuration, in the same shape as the input file.
nlohmann::json to_json(const SimConfig& cfg);

}  // namespace sim
