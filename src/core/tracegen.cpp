#include "clache/tracegen.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <locale>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "clache/rng.h"
namespace clache {
namespace {
using Params = std::map<std::string, std::string>;
constexpr std::uint64_t kMaxKeys = 10'000'000;
constexpr double kU32Max = 4294967295.0;
[[noreturn]] void invalid(const std::string& key, const std::string& message) {
  throw std::invalid_argument(key + ": " + message);
}
std::uint64_t integer(std::string_view s, const std::string& key) {
  std::uint64_t value = 0, multiplier = 1;
  const std::pair<std::string_view, std::uint64_t> suffixes[] = {
      {"KiB", 1024}, {"MiB", 1048576}, {"GiB", 1073741824},
      {"KB", 1000},  {"MB", 1000000},  {"GB", 1000000000}};
  for (const auto& [suffix, m] : suffixes)
    if (s.ends_with(suffix)) {
      s.remove_suffix(suffix.size());
      multiplier = m;
      break;
    }
  const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
  if (result.ec != std::errc{} || result.ptr != s.data() + s.size() ||
      value > std::numeric_limits<std::uint64_t>::max() / multiplier)
    invalid(key, "expected an unsigned integer without overflow");
  return value * multiplier;
}
double real(std::string_view s, const std::string& key) {
  double value = 0;
  std::istringstream input{std::string(s)};
  input.imbue(std::locale::classic());
  input >> std::noskipws >> value;
  if (!input || input.peek() != std::char_traits<char>::eof() || !std::isfinite(value))
    invalid(key, "expected a finite number");
  return value;
}
std::vector<std::string_view> split(std::string_view s) {
  std::vector<std::string_view> values;
  while (true) {
    const auto comma = s.find(',');
    values.push_back(s.substr(0, comma));
    if (comma == std::string_view::npos) break;
    s.remove_prefix(comma + 1);
  }
  return values;
}
struct Options {
  Params params;
  std::string workload;
  std::uint64_t requests = 0, keys = 0, seed = 0, scan_every = 1, scan_len = 0, hot_keys = 0,
                burst_keys = 1, regions = 1;
  double rate = 0, alpha = 0, size_min = 0, size_max = 0, size_median = 0, size_sigma = 0,
         cost_min = 0, cost_max = 0, cost_median = 0, cost_sigma = 0, jitter = 0, ttl_fraction = 0;
  double hot_frac = 0, expensive_p = 0, cheap = 0, medium = 0, expensive = 0, base = 0, per_kib = 0,
         scan_cost = 0, shift_seconds = 1, bursts_per_hour = 0, burst_share = 0, burst_duration = 0,
         overlap = 0;
  std::string size_dist, cost_dist;
  std::vector<std::uint32_t> ttls;
  std::vector<double> multipliers;
  bool scans = false, shifts = false, inversion = false, variable = false;
  explicit Options(const TracegenConfig& config)
      : params(tracegen_defaults(config.workload)), workload(config.workload) {
    for (const auto& [k, v] : config.params) {
      if (!params.contains(k)) invalid(k, "unknown parameter for " + workload);
      params[k] = v;
    }
    auto u = [&](const std::string& k) { return integer(params.at(k), k); };
    auto d = [&](const std::string& k) { return real(params.at(k), k); };
    auto range = [&](const std::string& k, double low, double high) {
      const double v = d(k);
      if (v < low || v > high) invalid(k, "out of range");
      return v;
    };
    auto positive = [&](const std::string& k) {
      const double v = d(k);
      if (v <= 0) invalid(k, "must be positive");
      return v;
    };
    requests = u("requests");
    keys = u("keys");
    seed = u("seed");
    if (keys == 0 || keys > kMaxKeys) invalid("keys", "must be in [1,10000000]");
    rate = positive("rate_rps");
    alpha = range("alpha", 0, 100);
    size_min = static_cast<double>(u("size.min"));
    size_max = static_cast<double>(u("size.max"));
    size_median = static_cast<double>(u("size.median"));
    if (size_min < 1 || size_max < size_min || size_max > kU32Max || size_median < 1 ||
        size_median > kU32Max)
      invalid("size", "invalid bounds or median");
    size_sigma = range("size.sigma", 0, 100);
    size_dist = params.at("size.dist");
    cost_dist = params.at("cost.dist");
    for (const auto& name : {"size.dist", "cost.dist"})
      if (params.at(name) != "fixed" && params.at(name) != "lognormal" &&
          params.at(name) != "loguniform")
        invalid(name, "expected fixed, lognormal, or loguniform");
    cost_min = positive("cost.min_us");
    cost_max = positive("cost.max_us");
    cost_median = positive("cost.median_us");
    if (cost_min > cost_max || cost_max > kU32Max || cost_median > kU32Max)
      invalid("cost", "invalid bounds or median");
    cost_sigma = range("cost.sigma", 0, 100);
    jitter = range("cost.jitter_sigma", 0, 100);
    ttl_fraction = range("ttl.fraction", 0, 1);
    for (auto value : split(params.at("ttl.choices_s"))) {
      const auto ttl = integer(value, "ttl.choices_s");
      if (ttl == 0 || ttl > std::numeric_limits<std::uint32_t>::max())
        invalid("ttl.choices_s", "TTLs must fit positive u32");
      ttls.push_back(static_cast<std::uint32_t>(ttl));
    }
    scans = workload == "scan" || workload == "mixed";
    shifts = workload == "hotset_shift" || workload == "mixed";
    inversion = workload == "cost_inversion" || workload == "mixed";
    variable = workload == "variable_size" || workload == "mixed";
    if (inversion) {
      hot_frac = range("hot_frac", 0, 1);
      expensive_p = range("expensive_p", 0, 1);
      cheap = positive("cheap_us");
      medium = positive("medium_us");
      expensive = positive("expensive_us");
    }
    if (variable) {
      base = range("base_us", 0, kU32Max);
      per_kib = range("per_kib_us", 0, kU32Max);
    }
    if (scans) {
      scan_every = u("scan_every");
      scan_len = u("scan_len");
      scan_cost = positive("scan_cost_us");
      if (scan_every == 0 || scan_len > scan_every)
        invalid("scan_len", "must be <= positive scan_every");
    }
    if (shifts) {
      shift_seconds = positive("shift_every_s");
      hot_keys = std::min(keys, u("hot_keys"));
      if (hot_keys == 0) invalid("hot_keys", "must be positive");
    }
    if (workload == "burst") {
      bursts_per_hour = range("bursts_per_hour", 0, 1e9);
      burst_keys = u("burst_keys");
      if (burst_keys == 0) invalid("burst_keys", "must be positive");
      burst_share = range("burst_share", 0, 1);
      burst_duration = positive("burst_duration_s");
    }
    if (workload == "regional_split") {
      regions = u("regions");
      if (regions == 0 || regions > 65536 || regions > 100'000'000 / keys)
        invalid("regions", "must fit u16 IDs and at most 100M permutation entries");
      overlap = range("overlap", 0, 1);
      for (auto value : split(params.at("cost_multipliers"))) {
        const auto v = real(value, "cost_multipliers");
        if (v <= 0) invalid("cost_multipliers", "must be positive");
        multipliers.push_back(v);
      }
      if (multipliers.size() != regions)
        invalid("cost_multipliers", "supply exactly one multiplier per region");
    }
  }
};
double sample(Xoshiro256ss& rng, const std::string& dist, double median, double sigma, double low,
              double high) {
  double value = median;
  if (dist == "lognormal")
    value = rng.lognormal(median, sigma);
  else if (dist == "loguniform")
    value = std::exp(std::log(low) + rng.uniform01() * (std::log(high) - std::log(low)));
  return std::clamp(value, low, high);
}
struct Attributes {
  std::uint32_t size = 0, ttl = 0;
  double cost = 0;
};
struct Burst {
  double end;
  std::uint64_t first;
};
class Generator {
 public:
  explicit Generator(const Options& o)
      : o_(o),
        arrivals_(Xoshiro256ss::for_stream(o.seed, 1)),
        popularity_(Xoshiro256ss::for_stream(o.seed, 2)),
        jitter_(Xoshiro256ss::for_stream(o.seed, 3)),
        shifts_(Xoshiro256ss::for_stream(o.seed, 4)),
        regions_(Xoshiro256ss::for_stream(o.seed, 5)),
        burst_events_(Xoshiro256ss::for_stream(o.seed, 6)),
        burst_traffic_(Xoshiro256ss::for_stream(o.seed, 7)),
        zipf_(o.keys, o.alpha),
        fresh_(o.keys) {
    attributes_.resize(static_cast<std::size_t>(o.keys));
    if (o.shifts || o.workload == "regional_split") {
      permutations_.resize(
          static_cast<std::size_t>(o.workload == "regional_split" ? o.regions : 1));
      for (auto& p : permutations_) {
        p.resize(static_cast<std::size_t>(o.keys));
        std::iota(p.begin(), p.end(), std::uint64_t{0});
      }
      if (o.workload == "regional_split") {
        auto rng = Xoshiro256ss::for_stream(o.seed, 8);
        const auto shared = static_cast<std::size_t>(o.overlap * static_cast<double>(o.keys));
        for (auto& p : permutations_)
          rng.shuffle(p.begin() + static_cast<std::ptrdiff_t>(shared), p.end());
      }
    }
    next_burst_ = o.bursts_per_hour > 0 ? burst_events_.exponential(o.bursts_per_hour / 3600)
                                        : std::numeric_limits<double>::infinity();
    next_shift_ = o.shift_seconds;
  }
  TraceMeta run(const std::function<void(const TraceRecord&)>& emit) {
    TraceMeta meta;
    meta.generator_version = "clache-tracegen-1";
#ifdef CLACHE_GIT_SHA
    meta.git_sha = CLACHE_GIT_SHA;
#else
    meta.git_sha = "unknown";
#endif
    meta.workload = o_.workload;
    meta.seed = o_.seed;
    meta.request_count = o_.requests;
    for (const auto& item : o_.params) meta.params.push_back(item);
    std::unordered_map<KeyId, std::uint32_t> sizes;
    std::uint64_t timestamp = 0;
    for (std::uint64_t i = 0; i < o_.requests; ++i) {
      if (i != 0) {
        const double interval = arrivals_.exponential(o_.rate) * 1e9;
        if (!std::isfinite(interval) || interval >= 18446744073709551616.0)
          throw std::overflow_error("arrival interval exceeds u64 nanoseconds");
        const auto step = static_cast<std::uint64_t>(interval);
        if (step > std::numeric_limits<std::uint64_t>::max() - timestamp)
          throw std::overflow_error("trace timestamp exceeds u64");
        timestamp += step;
      }
      const double seconds = static_cast<double>(timestamp) * 1e-9;
      if (o_.shifts)
        while (seconds >= next_shift_) {
          auto& p = permutations_[0];
          // Partial Fisher-Yates selects a uniform new hot set without duplicate IDs.
          for (std::uint64_t j = 0; j < o_.hot_keys; ++j) {
            const auto k = j + shifts_.uniform_below(o_.keys - j);
            std::swap(p[static_cast<std::size_t>(j)], p[static_cast<std::size_t>(k)]);
          }
          advance(next_shift_, o_.shift_seconds, "shift_every_s");
        }
      std::uint64_t rank = zipf_.sample(popularity_);
      std::uint16_t region = 0;
      if (o_.workload == "regional_split") {
        region = static_cast<std::uint16_t>(regions_.uniform_below(o_.regions));
        rank = permutations_[region][static_cast<std::size_t>(rank)];
      } else if (o_.shifts)
        rank = permutations_[0][static_cast<std::size_t>(rank)];
      const bool scan = o_.scans && i >= o_.scan_every && i % o_.scan_every < o_.scan_len;
      if (scan) rank = allocate(1);
      if (o_.workload == "burst") {
        while (seconds >= next_burst_) {
          bursts_.push_back({next_burst_ + o_.burst_duration, allocate(o_.burst_keys)});
          advance(next_burst_, burst_events_.exponential(o_.bursts_per_hour / 3600),
                  "bursts_per_hour");
        }
        bursts_.erase(std::remove_if(bursts_.begin(), bursts_.end(),
                                     [&](const Burst& b) { return b.end <= seconds; }),
                      bursts_.end());
        if (!bursts_.empty() && burst_traffic_.bernoulli(o_.burst_share)) {
          const auto& b =
              bursts_[static_cast<std::size_t>(burst_traffic_.uniform_below(bursts_.size()))];
          rank = b.first + burst_traffic_.uniform_below(o_.burst_keys);
        }
      }
      const KeyId key = splitmix64_mix(rank ^ (o_.seed * 0x9E3779B97F4A7C15ULL));
      Attributes attr;
      if (rank < o_.keys) {
        auto& cached = attributes_[static_cast<std::size_t>(rank)];
        if (cached.size == 0) cached = attributes(key, rank, false);
        attr = cached;
      } else
        attr = attributes(key, rank, scan);
      double cost = attr.cost * jitter_.lognormal(1, o_.jitter);
      if (o_.workload == "regional_split") cost *= o_.multipliers[region];
      cost = std::clamp(cost, o_.cost_min, o_.cost_max);
      const TraceRecord record{timestamp, key,    attr.size,   static_cast<std::uint32_t>(cost),
                               attr.ttl,  region, TraceOp::Get};
      emit(record);
      const auto [it, inserted] = sizes.emplace(key, attr.size);
      if (inserted) {
        if (attr.size > std::numeric_limits<std::uint64_t>::max() - meta.working_set_bytes)
          throw std::overflow_error("working set bytes exceed u64");
        meta.working_set_bytes += attr.size;
      } else if (attr.size > it->second) {
        meta.working_set_bytes += attr.size - it->second;
        it->second = attr.size;
      }
    }
    meta.unique_keys = sizes.size();
    return meta;
  }

 private:
  static void advance(double& time, double delta, const char* param) {
    const double next = time + delta;
    if (!std::isfinite(next) || next <= time) invalid(param, "event interval cannot advance time");
    time = next;
  }
  std::uint64_t allocate(std::uint64_t count) {
    if (count > std::numeric_limits<std::uint64_t>::max() - fresh_)
      throw std::overflow_error("fresh key ranks exceed u64");
    const auto start = fresh_;
    fresh_ += count;
    return start;
  }
  Attributes attributes(KeyId key, std::uint64_t rank, bool scan) const {
    auto size_rng = Xoshiro256ss::for_stream(key, 101);
    auto cost_rng = Xoshiro256ss::for_stream(key, 102);
    auto ttl_rng = Xoshiro256ss::for_stream(key, 103);
    const double size = o_.variable ? sample(size_rng, "loguniform", 0, 0, o_.size_min, o_.size_max)
                                    : sample(size_rng, o_.size_dist, o_.size_median, o_.size_sigma,
                                             o_.size_min, o_.size_max);
    double cost =
        sample(cost_rng, o_.cost_dist, o_.cost_median, o_.cost_sigma, o_.cost_min, o_.cost_max);
    if (o_.inversion) {
      const bool hot =
          rank < static_cast<std::uint64_t>(o_.hot_frac * static_cast<double>(o_.keys));
      const double median =
          hot ? o_.cheap : (cost_rng.bernoulli(o_.expensive_p) ? o_.expensive : o_.medium);
      cost = sample(cost_rng, "lognormal", median, o_.cost_sigma, o_.cost_min, o_.cost_max);
    } else if (o_.variable)
      cost = o_.base + o_.per_kib * static_cast<double>(static_cast<std::uint32_t>(size)) / 1024;
    if (scan)
      cost = sample(cost_rng, "lognormal", o_.scan_cost, o_.cost_sigma, o_.cost_min, o_.cost_max);
    const auto ttl = ttl_rng.bernoulli(o_.ttl_fraction)
                         ? o_.ttls[static_cast<std::size_t>(ttl_rng.uniform_below(o_.ttls.size()))]
                         : 0;
    return {static_cast<std::uint32_t>(size), ttl, std::clamp(cost, o_.cost_min, o_.cost_max)};
  }
  const Options& o_;
  Xoshiro256ss arrivals_, popularity_, jitter_, shifts_, regions_, burst_events_, burst_traffic_;
  ZipfTable zipf_;
  std::vector<Attributes> attributes_;
  std::vector<std::vector<std::uint64_t>> permutations_;
  std::vector<Burst> bursts_;
  std::uint64_t fresh_;
  double next_burst_ = 0, next_shift_ = 0;
};
}  // namespace
std::vector<std::string> tracegen_workloads() {
  return {"zipf",         "cost_inversion", "variable_size",  "scan",
          "hotset_shift", "burst",          "regional_split", "mixed"};
}
Params tracegen_defaults(std::string_view workload) {
  const auto names = tracegen_workloads();
  if (std::find(names.begin(), names.end(), workload) == names.end())
    invalid("workload", "unknown workload " + std::string(workload));
  Params p = {
      {"requests", "10000000"},      {"keys", "1000000"},        {"seed", "1"},
      {"rate_rps", "10000"},         {"alpha", "0.9"},           {"size.dist", "lognormal"},
      {"size.median", "4096"},       {"size.sigma", "1.0"},      {"size.min", "64"},
      {"size.max", "4194304"},       {"cost.dist", "lognormal"}, {"cost.median_us", "2000"},
      {"cost.sigma", "1.0"},         {"cost.min_us", "10"},      {"cost.max_us", "30000000"},
      {"cost.jitter_sigma", "0.25"}, {"ttl.fraction", "0"},      {"ttl.choices_s", "60,300,3600"}};
  if (workload == "cost_inversion" || workload == "mixed")
    p.insert({{"hot_frac", "0.05"},
              {"cheap_us", "500"},
              {"medium_us", "2000"},
              {"expensive_us", "200000"},
              {"expensive_p", "0.2"}});
  if (workload == "variable_size" || workload == "mixed") {
    p.insert({{"base_us", "1000"}, {"per_kib_us", "5"}});
    p["size.dist"] = "loguniform";
    p["size.min"] = "1024";
    p["size.max"] = "8388608";
  }
  if (workload == "scan" || workload == "mixed")
    p.insert({{"scan_every", "100000"}, {"scan_len", "20000"}, {"scan_cost_us", "100000"}});
  if (workload == "hotset_shift" || workload == "mixed")
    p.insert({{"shift_every_s", "600"}, {"hot_keys", "10000"}});
  if (workload == "burst")
    p.insert({{"bursts_per_hour", "6"},
              {"burst_keys", "1000"},
              {"burst_share", "0.3"},
              {"burst_duration_s", "120"}});
  if (workload == "regional_split")
    p.insert({{"regions", "3"}, {"overlap", "0.5"}, {"cost_multipliers", "1,3,10"}});
  return p;
}
TraceMeta generate_trace_records(const TracegenConfig& config,
                                 const std::function<void(const TraceRecord&)>& emit) {
  const Options options(config);
  return Generator(options).run(emit);
}
TraceMeta generate_trace(const TracegenConfig& config, const std::filesystem::path& path,
                         TraceFormat format) {
  const Options options(config);
  auto writer = TraceWriter::create(path, format, {config.workload == "regional_split", false});
  auto meta = Generator(options).run([&](const TraceRecord& record) { writer->write(record); });
  writer->close();
  meta.fingerprint = fingerprint_file(path);
  write_trace_meta(meta_path_for(path), meta);
  return meta;
}
}  // namespace clache
