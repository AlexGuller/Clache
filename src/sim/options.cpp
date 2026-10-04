#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "sim/simulator.h"

namespace clache::sim {
namespace {
std::vector<std::string> split(std::string_view text) {
  std::vector<std::string> values;
  for (;;) {
    const auto comma = text.find(',');
    if (text.substr(0, comma).empty()) throw std::invalid_argument("empty list element");
    values.emplace_back(text.substr(0, comma));
    if (comma == std::string_view::npos) return values;
    text.remove_prefix(comma + 1);
  }
}
std::uint64_t integer(const std::string& text) {
  std::uint64_t n = 0;
  const auto r = std::from_chars(text.data(), text.data() + text.size(), n);
  if (text.empty() || r.ec != std::errc{} || r.ptr != text.data() + text.size())
    throw std::invalid_argument("invalid unsigned integer: " + text);
  return n;
}
double fraction(const std::string& text, bool allow_one = true) {
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  double n = 0;
  in >> std::noskipws >> n;
  if (!in || in.peek() != std::char_traits<char>::eof() || !std::isfinite(n) || n < 0 || n > 1 ||
      (!allow_one && n == 1))
    throw std::invalid_argument("invalid fraction: " + text);
  return n;
}
}  // namespace
Options parse_options(const std::vector<std::string>& args) {
  Options o;
  std::vector<std::string> files, overrides;
  std::set<std::string> seen;
  std::string sample;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto& flag = args[i];
    if (flag != "--set" && flag != "--config" && !seen.insert(flag).second)
      throw std::invalid_argument("duplicate option: " + flag);
    const auto value = [&]() -> const std::string& {
      if (++i >= args.size() || args[i].starts_with("--"))
        throw std::invalid_argument("missing value for " + flag);
      return args[i];
    };
    if (flag == "--trace")
      o.trace_path = value();
    else if (flag == "--policies")
      o.policies = split(value());
    else if (flag == "--capacity") {
      for (const auto& text : split(value())) {
        const auto n = parse_size(text);
        if (!n) throw std::invalid_argument("capacity must be positive");
        o.capacities.push_back(n);
      }
    } else if (flag == "--capacity-fractions") {
      for (const auto& text : split(value())) {
        const auto f = fraction(text);
        if (f == 0) throw std::invalid_argument("capacity fraction must be positive");
        o.capacity_fractions.push_back(f);
      }
    } else if (flag == "--config")
      files.push_back(value());
    else if (flag == "--set")
      overrides.push_back(value());
    else if (flag == "--warmup-fraction")
      o.warmup_fraction = fraction(value(), false);
    else if (flag == "--per-entry-overhead")
      o.overhead = parse_size(value());
    else if (flag == "--jobs") {
      const auto n = integer(value());
      if (!n || n > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("jobs out of range");
      o.jobs = static_cast<std::size_t>(n);
    } else if (flag == "--domain-by") {
      const auto& v = value();
      if (v != "none" && v != "region")
        throw std::invalid_argument("domain-by must be none or region");
      o.regional = v == "region";
    } else if (flag == "--out")
      o.out = value();
    else if (flag == "--timeseries")
      o.timeseries = value();
    else if (flag == "--timeseries-every") {
      o.timeseries_every = integer(value());
      if (!o.timeseries_every) throw std::invalid_argument("timeseries-every must be positive");
    } else if (flag == "--decisions")
      o.decisions = value();
    else if (flag == "--decision-sample-rate") {
      sample = value();
      (void)fraction(sample);
    } else if (flag == "--cost-oracle")
      o.cost_oracle = true;
    else if (flag == "--stats-only")
      o.stats_only = true;
    else if (flag == "--export") {
      if (value() != "libcachesim")
        throw std::invalid_argument("only libcachesim export is supported");
      o.export_libcachesim = true;
    } else
      throw std::invalid_argument("unknown option: " + flag);
  }
  if (o.trace_path.empty()) throw std::invalid_argument("--trace is required");
  if (!o.capacities.empty() && !o.capacity_fractions.empty())
    throw std::invalid_argument("choose capacities or fractions");
  if (o.stats_only && o.export_libcachesim)
    throw std::invalid_argument("stats-only and export are mutually exclusive");
  for (const auto& file : files) apply_config_file(o.config, file);
  for (const auto& override : overrides) apply_override(o.config, override);
  if (!sample.empty()) apply_override(o.config, "clache.telemetry.decision_sample_rate=" + sample);
  if (o.policies.empty())
    o.policies = {"lru",      "lfu",    "lfu_da",    "gds",       "gdsf",  "gdsf_window",
                  "wtinylfu", "s3fifo", "redis_lru", "redis_lfu", "clache"};
  if (o.capacities.empty() && o.capacity_fractions.empty()) {
    if (o.config.engine.capacity_bytes)
      o.capacities.push_back(o.config.engine.capacity_bytes);
    else
      o.capacity_fractions = {0.001, 0.01, 0.05, 0.1, 0.2};
  }
  if (o.export_libcachesim && !seen.contains("--out"))
    o.out = o.trace_path.string() + ".oracleGeneral";
  return o;
}
std::string help() {
  return "Usage: clache_sim --trace PATH [--policies NAMES] [--capacity BYTES,... | "
         "--capacity-fractions F,...]\n"
         "  [--config FILE] [--set key=value] [--warmup-fraction 0.2] [--cost-oracle]\n"
         "  [--per-entry-overhead BYTES] [--domain-by none|region] [--jobs N]\n"
         "  [--out results.csv] [--timeseries PATH --timeseries-every 1000000]\n"
         "  [--decisions PATH --decision-sample-rate 0.001] [--stats-only | --export "
         "libcachesim]\n";
}
}  // namespace clache::sim
