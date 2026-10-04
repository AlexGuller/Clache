#include "sim/simulator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include "clache/hash.h"
#include "clache/policy.h"

namespace clache::sim {
namespace {
void add(std::uint64_t& dest, std::uint64_t value) {
  if (value > std::numeric_limits<std::uint64_t>::max() - dest)
    throw std::overflow_error("trace metric overflow");
  dest += value;
}
void merge_metrics(Metrics& a, const Metrics& b) {
  add(a.requests, b.requests);
  add(a.hits, b.hits);
  add(a.bytes, b.bytes);
  add(a.hit_bytes, b.hit_bytes);
  add(a.cost, b.cost);
  add(a.hit_cost, b.hit_cost);
}
void record(Metrics& m, const TraceRecord& r, bool hit) {
  add(m.requests, 1);
  add(m.bytes, r.size_bytes);
  add(m.cost, r.miss_cost_us);
  if (hit) {
    add(m.hits, 1);
    add(m.hit_bytes, r.size_bytes);
    add(m.hit_cost, r.miss_cost_us);
  }
}
std::string real(double value) {
  if (!std::isfinite(value)) return "null";
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << value;
  return out.str();
}
class LogSink final : public DecisionSink {
 public:
  std::string domain;
  std::uint64_t capacity = 0;
  std::string text;
  void record(const DecisionRecord& r) override {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"policy\":\"" << r.policy << "\",\"domain\":\"" << domain
        << "\",\"capacity_bytes\":" << capacity << ",\"seq\":" << r.seq
        << ",\"t_ns\":" << r.now.ns() << ",\"key\":" << r.key << ",\"decision\":\""
        << decision_class(r.decision) << "\",\"reason\":\"" << to_string(r.decision)
        << "\",\"value\":" << real(r.candidate_value)
        << ",\"threshold_value\":" << real(r.threshold_value)
        << ",\"frequency_rps\":" << real(r.frequency_rps)
        << ",\"miss_cost_ms\":" << real(r.miss_cost_ms) << ",\"size_bytes\":" << r.size_bytes
        << ",\"bytes_needed\":" << r.bytes_needed << ",\"victim_count\":" << r.victim_count
        << ",\"victims\":[";
    const auto n = std::min<std::size_t>(r.victim_count, r.victims_head.size());
    for (std::size_t i = 0; i < n; ++i) {
      if (i) out << ',';
      out << r.victims_head[i];
    }
    out << "]}\n";
    text += out.str();
  }
};
ClacheConfig effective(const Options& o, const std::string& policy, std::uint64_t capacity) {
  auto c = o.config;
  c.engine.capacity_bytes = capacity;
  if (policy == "clache_decay") c.engine.frequency.mode = FrequencyConfig::Mode::ForwardDecay;
  if (policy == "clache_noghost") c.engine.observation.max_entries = 0;
  if (policy == "clache_nocost") c.policy.clache.use_cost = false;
  if (policy == "clache_nosize") c.engine.value.size_exponent = 0;
  if (policy == "clache_noadmit")
    c.policy.clache.admission_rule = ClachePolicyConfig::AdmissionRule::Always;
  if (policy == "gdsf_window")
    c.policy.gdsf.frequency_source = GdsfPolicyConfig::FrequencySource::EngineWindow;
  auto errors = validate(c.engine);
  const auto policy_errors = validate(c.policy);
  errors.insert(errors.end(), policy_errors.begin(), policy_errors.end());
  if (!errors.empty()) {
    std::string message = "invalid effective configuration";
    for (const auto& error : errors) message += "\n" + error;
    throw std::invalid_argument(message);
  }
  return c;
}
std::uint64_t fingerprint(const ClacheConfig& c, const Options& o, const std::string& policy,
                          std::uint64_t expected_entries) {
  // All sim.* entries sort after the clache.* and policy.* registry entries.
  return hash64(canonical_config(c) + "sim.cost_oracle=" + (o.cost_oracle ? "true\n" : "false\n") +
                "sim.domain_by=" + (o.regional ? "region\n" : "none\n") +
                "sim.expected_entries=" + std::to_string(expected_entries) + "\n" +
                "sim.per_entry_overhead=" + std::to_string(o.overhead) + "\nsim.policy=" + policy +
                "\nsim.warmup_fraction=" + real(o.warmup_fraction) + "\n");
}
Result replay(const Trace& t, const Options& o, const std::string& policy, std::uint64_t capacity,
              const std::string& domain, const std::vector<std::size_t>& indices,
              double capacity_fraction, std::uint64_t expected_entries) {
  Result r;
  r.policy = policy;
  r.domain = domain;
  r.capacity = capacity;
  r.capacity_fraction = capacity_fraction;
  const auto config = effective(o, policy, capacity);
  r.fingerprint = fingerprint(config, o, policy, expected_entries);
  LogSink sink;
  sink.domain = domain;
  sink.capacity = capacity;
  CacheEngine engine(config.engine, policy, config.policy, o.decisions.empty() ? nullptr : &sink,
                     expected_entries);
  const auto cutoff = static_cast<std::size_t>(static_cast<double>(t.size()) * o.warmup_fraction);
  EngineStats before;
  std::uint64_t measured_records = 0;
  const auto sample = [&] {
    const auto bytes = engine.metadata_bytes();
    r.metadata_peak = std::max(r.metadata_peak, bytes);
    if (engine.resident_count())
      r.metadata_per_resident =
          std::max(r.metadata_per_resident,
                   static_cast<double>(bytes) / static_cast<double>(engine.resident_count()));
  };
  const auto started = std::chrono::steady_clock::now();
  for (const auto i : indices) {
    const auto request = t.at(i);
    const bool measured = i >= cutoff;
    const auto now = TimePoint::from_ns(static_cast<std::int64_t>(request.timestamp_ns));
    if (request.op == TraceOp::Del) {
      engine.invalidate(request.key, now);
    } else {
      std::uint64_t charged = request.size_bytes;
      add(charged, o.overhead);
      const auto lookup = engine.lookup(request.key, now, charged);
      if (!lookup.hit) {
        const std::optional<Nanos> ttl =
            request.ttl_s ? std::optional<Nanos>(
                                Nanos{static_cast<std::int64_t>(request.ttl_s) * 1000000000LL})
                          : std::nullopt;
        const auto admission = engine.complete_miss(
            request.key, charged, us_to_ms(request.miss_cost_us), ttl, lookup.generation, now);
        if (measured && admission.decision == Decision::AdmitWithEviction) {
          add(r.eviction_admits, 1);
          add(r.victims, admission.evicted.size());
          r.max_victims =
              std::max(r.max_victims, static_cast<std::uint64_t>(admission.evicted.size()));
        }
      } else if (o.cost_oracle)
        engine.observe_cost_hint(request.key, us_to_ms(request.miss_cost_us), now);
      if (measured) {
        record(r.metrics, request, lookup.hit);
        if (!o.timeseries.empty()) {
          const auto interval = (i - cutoff) / o.timeseries_every;
          if (r.intervals.empty() || r.intervals.back().index != interval)
            r.intervals.push_back({interval, cutoff + interval * o.timeseries_every, {}});
          record(r.intervals.back().metrics, request, lookup.hit);
        }
        if (r.metrics.requests % 65536 == 0) sample();
      } else
        ++r.warmup_requests;
    }
    if (!measured)
      before = engine.stats();
    else
      ++measured_records;
  }
  r.wall_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  if (measured_records) sample();
  const auto& after = engine.stats();
  r.counters.admissions = after.admissions - before.admissions;
  r.counters.admission_rejections = after.admission_rejections - before.admission_rejections;
  r.counters.evictions = after.evictions - before.evictions;
  r.counters.expirations = after.expirations - before.expirations;
  r.counters.invalidations = after.invalidations - before.invalidations;
  r.counters.size_changes = after.size_changes - before.size_changes;
  r.decision_log = std::move(sink.text);
  return r;
}
}  // namespace
TraceStats prepass(const Trace& t) {
  if (t.timestamp_ns.size() != t.key.size() || t.size_bytes.size() != t.key.size() ||
      t.miss_cost_us.size() != t.key.size() || t.ttl_s.size() != t.key.size() ||
      t.region.size() != t.key.size() || t.op.size() != t.key.size())
    throw std::invalid_argument("trace arrays have unequal lengths");
  TraceStats stats;
  stats.records = t.size();
  struct KeyStats {
    std::uint32_t maximum = 0;
    std::uint64_t count = 0;
  };
  std::unordered_map<KeyId, KeyStats> keys;
  std::set<std::uint16_t> regions;
  for (std::size_t i = 0; i < t.size(); ++i) {
    const auto r = t.at(i);
    if (r.timestamp_ns > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      throw std::invalid_argument("timestamp exceeds engine signed nanosecond range");
    if (i && r.timestamp_ns < t.timestamp_ns[i - 1])
      throw std::invalid_argument("timestamps decrease");
    regions.insert(r.region);
    if (r.op == TraceOp::Get) {
      if (!r.size_bytes) throw std::invalid_argument("GET size must be positive");
      ++stats.gets;
      add(stats.potential_cost, r.miss_cost_us);
      auto& key = keys[r.key];
      key.maximum = std::max(key.maximum, r.size_bytes);
      ++key.count;
    } else if (r.op != TraceOp::Del)
      throw std::invalid_argument("unknown trace operation");
  }
  std::uint64_t one_hits = 0;
  for (const auto& [key, info] : keys) {
    (void)key;
    add(stats.working_set_bytes, info.maximum);
    if (info.count == 1) ++one_hits;
  }
  stats.unique_keys = keys.size();
  if (!keys.empty()) {
    stats.mean_object_size =
        static_cast<double>(stats.working_set_bytes) / static_cast<double>(stats.unique_keys);
    stats.one_hit_fraction = static_cast<double>(one_hits) / static_cast<double>(stats.unique_keys);
  }
  stats.regions.assign(regions.begin(), regions.end());
  return stats;
}
std::vector<Result> simulate(const Trace& trace, const TraceStats& stats, const Options& o) {
  if (!o.jobs || !o.timeseries_every || !std::isfinite(o.warmup_fraction) ||
      o.warmup_fraction < 0 || o.warmup_fraction >= 1)
    throw std::invalid_argument("invalid replay options");
  std::vector<std::pair<std::uint64_t, double>> capacities;
  for (auto capacity : o.capacities) {
    capacities.emplace_back(
        capacity, stats.working_set_bytes
                      ? static_cast<double>(capacity) / static_cast<double>(stats.working_set_bytes)
                      : 0);
  }
  for (const auto f : o.capacity_fractions) {
    if (!std::isfinite(f) || f <= 0 || f > 1)
      throw std::invalid_argument("invalid capacity fraction");
    const long double bytes =
        static_cast<long double>(static_cast<double>(stats.working_set_bytes) * f);
    if (bytes < 1 || bytes > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
      throw std::invalid_argument(
          "capacity fraction rounds to zero or overflows; use explicit --capacity");
    capacities.emplace_back(static_cast<std::uint64_t>(bytes), f);
  }
  if (capacities.empty() || o.policies.empty()) throw std::invalid_argument("no runs selected");
  const auto registered = registered_policies();
  for (const auto& policy : o.policies) {
    if (std::find(registered.begin(), registered.end(), policy) == registered.end())
      throw std::invalid_argument("unknown policy: " + policy);
    for (const auto& capacity : capacities) (void)effective(o, policy, capacity.first);
  }
  const auto expected_entries = [&](std::uint64_t capacity) -> std::uint64_t {
    if (stats.unique_keys == 0) return 0;
    const long double mean_charged =
        static_cast<long double>(stats.working_set_bytes) / stats.unique_keys + o.overhead;
    const long double estimate = static_cast<long double>(capacity) / mean_charged;
    // Clamp before converting: platforms with double-width long double can
    // round UINT64_MAX to 2^64. A trace GET always charges at least one byte.
    if (estimate >= static_cast<long double>(capacity)) return capacity;
    return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(estimate));
  };
  std::map<std::string, std::vector<std::size_t>> domains;
  if (!o.regional)
    domains["ALL"] = {};
  else
    for (auto region : stats.regions) domains[std::to_string(region)] = {};
  for (std::size_t i = 0; i < trace.size(); ++i)
    domains[o.regional ? std::to_string(trace.region[i]) : "ALL"].push_back(i);
  struct Task {
    std::string policy, domain;
    std::uint64_t capacity;
    double capacity_fraction;
    const std::vector<std::size_t>* indices;
  };
  std::vector<Task> tasks;
  for (const auto& policy : o.policies)
    for (const auto& capacity : capacities)
      for (const auto& [domain, indices] : domains)
        tasks.push_back({policy, domain, capacity.first, capacity.second, &indices});
  std::vector<Result> results(tasks.size());
  std::vector<std::exception_ptr> errors(tasks.size());
  std::atomic<std::size_t> cursor{0};
  {
    std::vector<std::thread> workers;
    struct JoinThreads {
      std::vector<std::thread>& threads;
      ~JoinThreads() {
        for (auto& thread : threads)
          if (thread.joinable()) thread.join();
      }
    } join{workers};
    for (std::size_t j = 0; j < std::min(o.jobs, tasks.size()); ++j)
      workers.emplace_back([&] {
        for (;;) {
          const auto i = cursor.fetch_add(1);
          if (i >= tasks.size()) return;
          const auto& task = tasks[i];
          try {
            results[i] = replay(trace, o, task.policy, task.capacity, task.domain, *task.indices,
                                task.capacity_fraction, expected_entries(task.capacity));
          } catch (...) {
            errors[i] = std::current_exception();
          }
        }
      });
  }
  for (const auto& error : errors)
    if (error) std::rethrow_exception(error);
  if (!o.regional || domains.empty()) return results;
  std::vector<Result> combined;
  for (std::size_t start = 0; start < results.size(); start += domains.size()) {
    Result all;
    all.policy = results[start].policy;
    all.domain = "ALL";
    all.capacity = results[start].capacity;
    all.capacity_fraction = results[start].capacity_fraction;
    all.fingerprint = results[start].fingerprint;
    std::map<std::uint64_t, Interval> intervals;
    for (std::size_t j = start; j < start + domains.size(); ++j) {
      const auto& r = results[j];
      merge_metrics(all.metrics, r.metrics);
      add(all.warmup_requests, r.warmup_requests);
      add(all.eviction_admits, r.eviction_admits);
      add(all.victims, r.victims);
      all.max_victims = std::max(all.max_victims, r.max_victims);
      add(all.metadata_peak, r.metadata_peak);
      all.metadata_per_resident = std::max(all.metadata_per_resident, r.metadata_per_resident);
      all.wall_seconds += r.wall_seconds;
      add(all.counters.admissions, r.counters.admissions);
      add(all.counters.admission_rejections, r.counters.admission_rejections);
      add(all.counters.evictions, r.counters.evictions);
      add(all.counters.expirations, r.counters.expirations);
      add(all.counters.invalidations, r.counters.invalidations);
      add(all.counters.size_changes, r.counters.size_changes);
      for (const auto& interval : r.intervals) {
        auto& dest = intervals[interval.index];
        dest.index = interval.index;
        dest.first_request = interval.first_request;
        merge_metrics(dest.metrics, interval.metrics);
      }
      combined.push_back(std::move(results[j]));
    }
    for (const auto& [index, interval] : intervals) {
      (void)index;
      all.intervals.push_back(interval);
    }
    combined.push_back(std::move(all));
  }
  return combined;
}
}  // namespace clache::sim
