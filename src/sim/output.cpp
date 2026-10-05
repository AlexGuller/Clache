#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include "sim/simulator.h"

#ifndef CLACHE_BUILD_GIT_SHA
#define CLACHE_BUILD_GIT_SHA "unknown"
#endif
#ifndef CLACHE_BUILD_VERSION
#define CLACHE_BUILD_VERSION "0.1.0"
#endif

namespace clache::sim {
namespace {
double ratio(std::uint64_t numerator, std::uint64_t denominator) {
  return denominator ? static_cast<double>(numerator) / static_cast<double>(denominator) : 0;
}
std::string csv(std::string_view value) {
  std::string result = "\"";
  for (char c : value) {
    if (c == '"') result += '"';
    result += c;
  }
  return result + '"';
}
std::ofstream output(const std::filesystem::path& path, bool binary = false) {
  std::ofstream out(
      path, std::ios::out | std::ios::trunc | (binary ? std::ios::binary : std::ios::openmode{}));
  if (!out) throw std::runtime_error("cannot open output: " + path.string());
  out.exceptions(std::ios::badbit | std::ios::failbit);
  out.imbue(std::locale::classic());
  out << std::setprecision(17);
  return out;
}
void timeseries(std::ostream& out, const std::vector<Result>& results, const std::string& domain) {
  out << "policy,capacity_bytes,interval_index,first_request,requests,misses,miss_cost_us,hit_rate,"
         "cost_hit_rate\n";
  for (const auto& r : results)
    if (r.domain == domain)
      for (const auto& interval : r.intervals) {
        const auto& m = interval.metrics;
        out << r.policy << ',' << r.capacity << ',' << interval.index << ','
            << interval.first_request << ',' << m.requests << ',' << m.requests - m.hits << ','
            << m.cost - m.hit_cost << ',' << ratio(m.hits, m.requests) << ','
            << ratio(m.hit_cost, m.cost) << '\n';
      }
}
void write_stats(std::ostream& out, const TraceStats& s) {
  out << std::setprecision(17) << "{\"records\":" << s.records << ",\"get_requests\":" << s.gets
      << ",\"unique_keys\":" << s.unique_keys << ",\"working_set_bytes\":" << s.working_set_bytes
      << ",\"mean_object_size\":" << s.mean_object_size
      << ",\"total_potential_cost_us\":" << s.potential_cost
      << ",\"one_hit_wonder_fraction\":" << s.one_hit_fraction << ",\"regions\":[";
  for (std::size_t i = 0; i < s.regions.size(); ++i) {
    if (i) out << ',';
    out << s.regions[i];
  }
  out << "]}\n";
}
void check_paths(const Options& o, const TraceStats& stats) {
  const auto canonical = [](const std::filesystem::path& p) {
    return std::filesystem::weakly_canonical(p);
  };
  std::vector<std::filesystem::path> paths{o.out, o.out.string() + ".stats.json"};
  if (!o.timeseries.empty()) {
    paths.push_back(o.timeseries);
    if (o.regional)
      for (auto region : stats.regions)
        paths.emplace_back(o.timeseries.string() + ".region-" + std::to_string(region) + ".csv");
  }
  if (!o.decisions.empty()) paths.push_back(o.decisions);
  std::set<std::filesystem::path> used{canonical(o.trace_path)};
  for (const auto& path : paths)
    if (!used.insert(canonical(path)).second)
      throw std::invalid_argument("input/output paths must be distinct: " + path.string());
}
}  // namespace
void write_results(std::ostream& out, const std::vector<Result>& results, const TraceStats& stats,
                   const Options& o, std::uint64_t trace_fingerprint) {
  out.imbue(std::locale::classic());
  out << std::setprecision(17);
  out << "clache_version,git_sha,trace_path,trace_fingerprint,config_fingerprint,policy,domain,"
         "capacity_bytes,capacity_fraction,working_set_bytes,seed,warmup_requests,requests,hits,"
         "misses,hit_rate,byte_hit_rate,total_miss_cost_us,cost_hit_rate,admissions,rejections,"
         "evictions,expirations,invalidations,size_changes,mean_victims,max_victims,metadata_bytes_"
         "peak,metadata_bytes_per_resident,ns_per_request,wall_seconds\n";
  for (const auto& r : results) {
    const auto& m = r.metrics;
    const auto& c = r.counters;
    out << CLACHE_BUILD_VERSION << ',' << CLACHE_BUILD_GIT_SHA << ',' << csv(o.trace_path.string())
        << ',' << trace_fingerprint << ',' << r.fingerprint << ',' << r.policy << ',' << r.domain
        << ',' << r.capacity << ',' << r.capacity_fraction << ',' << stats.working_set_bytes << ','
        << o.config.engine.seed << ',' << r.warmup_requests << ',' << m.requests << ',' << m.hits
        << ',' << m.requests - m.hits << ',' << ratio(m.hits, m.requests) << ','
        << ratio(m.hit_bytes, m.bytes) << ',' << m.cost - m.hit_cost << ','
        << ratio(m.hit_cost, m.cost) << ',' << c.admissions << ',' << c.admission_rejections << ','
        << c.evictions << ',' << c.expirations << ',' << c.invalidations << ',' << c.size_changes
        << ',' << ratio(r.victims, r.eviction_admits) << ',' << r.max_victims << ','
        << r.metadata_peak << ',' << r.metadata_per_resident << ','
        << (r.replayed_records ? r.wall_seconds * 1e9 / static_cast<double>(r.replayed_records) : 0)
        << ',' << r.wall_seconds << '\n';
  }
}
void export_oracle_general(const Trace& trace, const std::filesystem::path& path) {
  // oracleGeneralBin.h: packed 24 bytes, offsets 0/4/12/16; no header.
  // Future accesses are one-based absolute request positions, -1 for none.
  std::vector<std::uint64_t> next(trace.size(), std::numeric_limits<std::uint64_t>::max());
  std::unordered_map<KeyId, std::uint64_t> future;
  for (std::size_t i = trace.size(); i > 0; --i) {
    const auto r = trace.at(i - 1);
    if (r.op != TraceOp::Get || r.ttl_s)
      throw std::invalid_argument(
          "oracleGeneral cannot represent deletes or TTL; export a GET-only trace with TTL=0");
    if (r.timestamp_ns / 1000000000ULL > std::numeric_limits<std::uint32_t>::max())
      throw std::invalid_argument("oracleGeneral timestamp overflows uint32 seconds");
    const auto found = future.find(r.key);
    if (found != future.end()) next[i - 1] = found->second;
    future[r.key] = i;
  }
  auto out = output(path, true);
  for (std::size_t i = 0; i < trace.size(); ++i) {
    std::array<char, 24> bytes{};
    const auto put = [&](std::size_t offset, std::uint64_t value, std::size_t width) {
      for (std::size_t j = 0; j < width; ++j)
        bytes[offset + j] = static_cast<char>((value >> (8 * j)) & 255);
    };
    put(0, trace.timestamp_ns[i] / 1000000000ULL, 4);
    put(4, trace.key[i], 8);
    put(12, trace.size_bytes[i], 4);
    put(16, next[i], 8);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  out.close();
}
int run(const Options& o) {
  const auto trace = load_trace(o.trace_path);
  const auto stats = prepass(trace);
  check_paths(o, stats);
  write_stats(std::cout, stats);
  if (o.stats_only) {
    auto out = output(o.out);
    write_stats(out, stats);
    out.close();
    return 0;
  }
  if (o.export_libcachesim) {
    export_oracle_general(trace, o.out);
    return 0;
  }
  const auto results = simulate(trace, stats, o);
  auto out = output(o.out);
  write_results(out, results, stats, o, fingerprint_file(o.trace_path));
  out.close();
  auto stats_out = output(o.out.string() + ".stats.json");
  write_stats(stats_out, stats);
  stats_out.close();
  if (!o.timeseries.empty()) {
    auto ts = output(o.timeseries);
    timeseries(ts, results, "ALL");
    ts.close();
    if (o.regional)
      for (auto region : stats.regions) {
        const auto domain = std::to_string(region);
        auto regional = output(o.timeseries.string() + ".region-" + domain + ".csv");
        timeseries(regional, results, domain);
        regional.close();
      }
  }
  if (!o.decisions.empty()) {
    auto decisions = output(o.decisions);
    for (const auto& result : results) decisions << result.decision_log;
    decisions.close();
  }
  return 0;
}
}  // namespace clache::sim
