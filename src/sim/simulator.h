#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

#include "clache/config_parser.h"
#include "clache/engine.h"
#include "clache/trace.h"

namespace clache::sim {
struct Options {
  std::filesystem::path trace_path, out = "results.csv", timeseries, decisions;
  ClacheConfig config;
  std::vector<std::string> policies;
  std::vector<std::uint64_t> capacities;
  std::vector<double> capacity_fractions;
  double warmup_fraction = 0.2;
  std::uint64_t overhead = 0, timeseries_every = 1000000;
  std::size_t jobs = 1;
  bool cost_oracle = false, regional = false, stats_only = false, export_libcachesim = false;
};
struct TraceStats {
  std::uint64_t records = 0, gets = 0, unique_keys = 0, working_set_bytes = 0, potential_cost = 0;
  double mean_object_size = 0, one_hit_fraction = 0;
  std::vector<std::uint16_t> regions;
};
struct Metrics {
  std::uint64_t requests = 0, hits = 0, bytes = 0, hit_bytes = 0, cost = 0, hit_cost = 0;
};
struct Interval {
  std::uint64_t index = 0, first_request = 0;
  Metrics metrics;
};
struct Result {
  std::string policy, domain;
  std::uint64_t capacity = 0, fingerprint = 0, warmup_requests = 0;
  // All timed operations, including warm-up and deletes, in this domain.
  std::uint64_t replayed_records = 0;
  Metrics metrics;
  EngineStats counters;
  std::uint64_t eviction_admits = 0, victims = 0, max_victims = 0, metadata_peak = 0;
  double capacity_fraction = 0, metadata_per_resident = 0, wall_seconds = 0;
  std::vector<Interval> intervals;
  std::string decision_log;
};
Options parse_options(const std::vector<std::string>& args);
TraceStats prepass(const Trace& trace);
std::vector<Result> simulate(const Trace& trace, const TraceStats& stats, const Options& options);
void write_results(std::ostream& out, const std::vector<Result>& results, const TraceStats& stats,
                   const Options& options, std::uint64_t trace_fingerprint);
void export_oracle_general(const Trace& trace, const std::filesystem::path& path);
int run(const Options& options);
std::string help();
}  // namespace clache::sim
