#include "clache/tracegen.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "clache/rng.h"

#include "gtest/gtest.h"
namespace clache {
namespace {
TracegenConfig config(std::string workload = "zipf") {
  return {std::move(workload),
          {{"requests", "500"}, {"keys", "32"}, {"seed", "17"}, {"cost.jitter_sigma", "0"}}};
}
std::vector<TraceRecord> records(const TracegenConfig& c) {
  std::vector<TraceRecord> out;
  generate_trace_records(c, [&](const TraceRecord& r) { out.push_back(r); });
  return out;
}
KeyId key(std::uint64_t rank, std::uint64_t seed = 17) {
  return splitmix64_mix(rank ^ (seed * 0x9E3779B97F4A7C15ULL));
}
TEST(Tracegen, EveryWorkloadDeterministicAndMetadataActual) {
  for (const auto& workload : tracegen_workloads()) {
    auto c = config(workload);
    std::vector<TraceRecord> a;
    const auto meta = generate_trace_records(c, [&](const TraceRecord& r) { a.push_back(r); });
    const auto b = records(c);
    ASSERT_EQ(a.size(), 500U);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(a.front().timestamp_ns, 0U);
    std::map<KeyId, std::uint32_t> sizes;
    for (std::size_t i = 0; i < a.size(); ++i) {
      EXPECT_EQ(a[i].timestamp_ns, b[i].timestamp_ns);
      EXPECT_EQ(a[i].key, b[i].key);
      EXPECT_EQ(a[i].size_bytes, b[i].size_bytes);
      EXPECT_EQ(a[i].miss_cost_us, b[i].miss_cost_us);
      EXPECT_EQ(a[i].ttl_s, b[i].ttl_s);
      EXPECT_EQ(a[i].region, b[i].region);
      EXPECT_GT(a[i].size_bytes, 0U);
      if (i > 0) {
        EXPECT_GE(a[i].timestamp_ns, a[i - 1].timestamp_ns);
      }
      sizes[a[i].key] = std::max(sizes[a[i].key], a[i].size_bytes);
    }
    std::uint64_t total = 0;
    for (const auto& [id, size] : sizes) {
      static_cast<void>(id);
      total += size;
    }
    EXPECT_EQ(meta.request_count, a.size());
    EXPECT_EQ(meta.unique_keys, sizes.size());
    EXPECT_EQ(meta.working_set_bytes, total);
    EXPECT_FALSE(meta.git_sha.empty());
    EXPECT_EQ(meta.params.size(), tracegen_defaults(workload).size());
    c.params["seed"] = "18";
    EXPECT_NE(a[0].key, records(c)[0].key);
  }
}
TEST(Tracegen, StableAttributesAndIndependentStreams) {
  auto c = config();
  c.params["ttl.fraction"] = "1";
  c.params["ttl.choices_s"] = "60,300";
  const auto a = records(c);
  std::map<KeyId, TraceRecord> first;
  for (const auto& r : a) {
    const auto [it, inserted] = first.emplace(r.key, r);
    if (!inserted) {
      EXPECT_EQ(r.size_bytes, it->second.size_bytes);
      EXPECT_EQ(r.miss_cost_us, it->second.miss_cost_us);
      EXPECT_EQ(r.ttl_s, it->second.ttl_s);
    }
    EXPECT_TRUE(r.ttl_s == 60 || r.ttl_s == 300);
  }
  c.params["cost.jitter_sigma"] = "1";
  c.params["size.sigma"] = "2";
  c.params["ttl.fraction"] = "0";
  const auto b = records(c);
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].timestamp_ns, b[i].timestamp_ns);
    EXPECT_EQ(a[i].key, b[i].key);
    EXPECT_EQ(b[i].ttl_s, 0U);
  }
}
TEST(Tracegen, CostInversionAndVariableSizeFormula) {
  auto c = config("cost_inversion");
  c.params["alpha"] = "0";
  c.params["hot_frac"] = "0.5";
  c.params["expensive_p"] = "1";
  c.params["cost.sigma"] = "0";
  std::set<KeyId> hot;
  for (std::uint64_t i = 0; i < 16; ++i) hot.insert(key(i));
  for (const auto& r : records(c)) EXPECT_EQ(r.miss_cost_us, hot.contains(r.key) ? 500U : 200000U);
  c = config("variable_size");
  c.params["base_us"] = "1000";
  c.params["per_kib_us"] = "5";
  for (const auto& r : records(c)) {
    EXPECT_GE(r.size_bytes, 1024U);
    EXPECT_LE(r.size_bytes, 8388608U);
    EXPECT_EQ(r.miss_cost_us,
              static_cast<std::uint32_t>(1000 + 5 * static_cast<double>(r.size_bytes) / 1024));
  }
}
TEST(Tracegen, FractionalHotCountAndZeroFormulaCost) {
  auto c = config("cost_inversion");
  c.params["keys"] = "1";
  c.params["hot_frac"] = "0.5";
  c.params["expensive_p"] = "1";
  c.params["cost.sigma"] = "0";
  EXPECT_EQ(records(c)[0].miss_cost_us, 200000U);
  c = config("variable_size");
  c.params["base_us"] = "0";
  c.params["per_kib_us"] = "0";
  for (const auto& r : records(c)) EXPECT_EQ(r.miss_cost_us, 10U);
}
TEST(Tracegen, ScansAreFreshAndExpensive) {
  for (const auto& name : {"scan", "mixed"}) {
    auto c = config(name);
    c.params["scan_every"] = "20";
    c.params["scan_len"] = "5";
    c.params["cost.sigma"] = "0";
    const auto a = records(c);
    std::map<KeyId, std::size_t> counts;
    for (const auto& r : a) ++counts[r.key];
    std::set<KeyId> catalogue;
    for (std::uint64_t i = 0; i < 32; ++i) catalogue.insert(key(i));
    for (std::size_t i = 0; i < a.size(); ++i)
      if (i >= 20 && i % 20 < 5) {
        EXPECT_FALSE(catalogue.contains(a[i].key));
        EXPECT_EQ(counts[a[i].key], 1U);
        EXPECT_EQ(a[i].miss_cost_us, 100000U);
      } else {
        EXPECT_TRUE(catalogue.contains(a[i].key));
      }
  }
}
TEST(Tracegen, HotSetActuallyShiftsAndTinyCataloguesWork) {
  auto c = config("hotset_shift");
  c.params["alpha"] = "100";
  c.params["rate_rps"] = "10";
  c.params["shift_every_s"] = "0.1";
  c.params["hot_keys"] = "1";
  std::set<KeyId> seen;
  for (const auto& r : records(c)) seen.insert(r.key);
  EXPECT_GT(seen.size(), 10U);
  for (const auto& workload : tracegen_workloads()) {
    c = config(workload);
    c.params["keys"] = "1";
    c.params["requests"] = "10";
    EXPECT_EQ(records(c).size(), 10U);
  }
}
TEST(Tracegen, BurstsIntroduceFreshTrafficAndZeroRateDisablesThem) {
  auto c = config("burst");
  c.params["bursts_per_hour"] = "3600";
  c.params["rate_rps"] = "10";
  c.params["burst_duration_s"] = "10";
  c.params["burst_keys"] = "3";
  c.params["burst_share"] = "1";
  std::set<KeyId> catalogue;
  for (std::uint64_t i = 0; i < 32; ++i) catalogue.insert(key(i));
  std::size_t outside = 0;
  for (const auto& r : records(c))
    if (!catalogue.contains(r.key)) ++outside;
  EXPECT_GT(outside, 300U);
  c.params["bursts_per_hour"] = "0";
  for (const auto& r : records(c)) EXPECT_TRUE(catalogue.contains(r.key));
}
TEST(Tracegen, RegionMultipliersAndPermutations) {
  auto c = config("regional_split");
  c.params["cost.dist"] = "fixed";
  c.params["cost.median_us"] = "100";
  c.params["alpha"] = "100";
  c.params["overlap"] = "1";
  std::set<std::uint16_t> regions;
  for (const auto& r : records(c)) {
    regions.insert(r.region);
    EXPECT_EQ(r.key, key(0));
    EXPECT_EQ(r.miss_cost_us, r.region == 0 ? 100U : (r.region == 1 ? 300U : 1000U));
  }
  EXPECT_EQ(regions.size(), 3U);
  c.params["overlap"] = "0";
  std::set<KeyId> top;
  for (const auto& r : records(c)) top.insert(r.key);
  EXPECT_GT(top.size(), 1U);
}
TEST(Tracegen, RejectsInvalidParametersBeforeEmission) {
  const std::vector<std::pair<std::string, std::string>> bad = {{"keys", "0"},
                                                                {"keys", "10000001"},
                                                                {"requests", "-1"},
                                                                {"seed", "18446744073709551616"},
                                                                {"rate_rps", "0"},
                                                                {"alpha", "nan"},
                                                                {"size.min", "0"},
                                                                {"size.max", "4294967296"},
                                                                {"size.sigma", "-1"},
                                                                {"cost.dist", "other"},
                                                                {"cost.min_us", "0"},
                                                                {"cost.max_us", "1"},
                                                                {"ttl.fraction", "1.1"},
                                                                {"ttl.choices_s", "0"},
                                                                {"ttl.choices_s", "60,"},
                                                                {"unknown", "1"}};
  for (const auto& [k, v] : bad) {
    auto c = config();
    c.params[k] = v;
    std::size_t emitted = 0;
    EXPECT_THROW(generate_trace_records(c, [&](const TraceRecord&) { ++emitted; }),
                 std::invalid_argument)
        << k;
    EXPECT_EQ(emitted, 0U);
  }
  auto c = config("scan");
  c.params["scan_every"] = "0";
  EXPECT_THROW(records(c), std::invalid_argument);
  c = config("regional_split");
  c.params["regions"] = "2";
  EXPECT_THROW(records(c), std::invalid_argument);
  c = config();
  c.params["rate_rps"] = "1e-100";
  EXPECT_THROW(records(c), std::overflow_error);
  c = config();
  c.params["requests"] = "0";
  EXPECT_TRUE(records(c).empty());
  c = config();
  c.params["size.dist"] = "fixed";
  c.params["size.median"] = "1KiB";
  EXPECT_EQ(records(c)[0].size_bytes, 1024U);
}
}  // namespace
}  // namespace clache
