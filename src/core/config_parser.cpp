#include "clache/config_parser.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "clache/hash.h"

namespace clache {
namespace {

using Mode = FrequencyConfig::Mode;
using Rule = ClachePolicyConfig::AdmissionRule;
using Source = GdsfPolicyConfig::FrequencySource;

// One registry keeps loading, overrides and fingerprinting in agreement.
template <class Config, class Visit>
void fields(Config& c, Visit visit) {
  visit("clache.capacity_bytes", c.engine.capacity_bytes);
  visit("clache.max_object_bytes", c.engine.max_object_bytes);
  visit("clache.seed", c.engine.seed);
  visit("clache.frequency.mode", c.engine.frequency.mode);
  visit("clache.frequency.window_seconds", c.engine.frequency.window_seconds);
  visit("clache.frequency.bucket_seconds", c.engine.frequency.bucket_seconds);
  visit("clache.frequency.decay_tau_seconds", c.engine.frequency.decay_tau_seconds);
  visit("clache.miss_cost.ewma_alpha", c.engine.miss_cost.ewma_alpha);
  visit("clache.miss_cost.min_ms", c.engine.miss_cost.min_ms);
  visit("clache.miss_cost.max_ms", c.engine.miss_cost.max_ms);
  visit("clache.miss_cost.default_ms", c.engine.miss_cost.default_ms);
  visit("clache.miss_cost.min_samples", c.engine.miss_cost.min_samples);
  visit("clache.value.size_exponent", c.engine.value.size_exponent);
  visit("clache.admission.min_observations", c.engine.admission.min_observations);
  visit("clache.admission.max_victims", c.engine.admission.max_victims);
  visit("clache.observation_table.max_entries", c.engine.observation.max_entries);
  visit("clache.observation_table.ttl_seconds", c.engine.observation.ttl_seconds);
  visit("clache.telemetry.decision_sample_rate", c.engine.telemetry.decision_sample_rate);
  visit("clache.telemetry.sample_hits", c.engine.telemetry.sample_hits);
  visit("policy.clache.use_cost", c.policy.clache.use_cost);
  visit("policy.clache.admission_rule", c.policy.clache.admission_rule);
  visit("policy.gdsf.frequency_source", c.policy.gdsf.frequency_source);
  visit("policy.gdsf.size_exponent", c.policy.gdsf.size_exponent);
  visit("policy.tinylfu.window_fraction", c.policy.tinylfu.window_fraction);
  visit("policy.tinylfu.protected_fraction", c.policy.tinylfu.protected_fraction);
  visit("policy.s3fifo.small_fraction", c.policy.s3fifo.small_fraction);
  visit("policy.s3fifo.move_to_main_threshold", c.policy.s3fifo.move_to_main_threshold);
  visit("policy.redis.samples", c.policy.redis.samples);
  visit("policy.redis.pool_size", c.policy.redis.pool_size);
  visit("policy.redis.lfu_log_factor", c.policy.redis.lfu_log_factor);
  visit("policy.redis.lfu_decay_time_min", c.policy.redis.lfu_decay_time_min);
}

std::string_view trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \r\n");
  if (first == std::string_view::npos) return {};
  return text.substr(first, text.find_last_not_of(" \r\n") - first + 1);
}

template <class T>
T number(std::string_view text) {
  T value{};
  if constexpr (std::is_floating_point_v<T>) {
    // Apple libc++ does not yet supply floating-point from_chars. A classic
    // locale stream avoids strtod's dependence on the process locale.
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    input >> std::noskipws >> value;
    if (!input || input.peek() != std::char_traits<char>::eof() || !std::isfinite(value)) {
      throw std::invalid_argument("expected a finite number");
    }
  } else {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
      throw std::invalid_argument("invalid number '" + std::string(text) + "'");
    }
  }
  return value;
}

template <class T>
void parse_scalar(T& field, std::string_view text, std::string_view key) {
  if constexpr (std::is_same_v<T, bool>) {
    if (text != "true" && text != "false") throw std::invalid_argument("expected true or false");
    field = text == "true";
  } else if constexpr (std::is_same_v<T, Mode>) {
    if (text == "rolling_buckets")
      field = Mode::RollingBuckets;
    else if (text == "forward_decay" || text == "decay")
      field = Mode::ForwardDecay;
    else
      throw std::invalid_argument("expected rolling_buckets or forward_decay");
  } else if constexpr (std::is_same_v<T, Rule>) {
    if (text == "strict")
      field = Rule::Strict;
    else if (text == "always")
      field = Rule::Always;
    else
      throw std::invalid_argument("expected strict or always");
  } else if constexpr (std::is_same_v<T, Source>) {
    if (text == "resident_count")
      field = Source::ResidentCount;
    else if (text == "engine_window")
      field = Source::EngineWindow;
    else
      throw std::invalid_argument("expected resident_count or engine_window");
  } else if constexpr (std::is_same_v<T, std::uint64_t>) {
    field = key.ends_with("_bytes") ? parse_size(text) : number<T>(text);
  } else {
    field = number<T>(text);
  }
}

void assign(ClacheConfig& config, std::string_view key, std::string_view value,
            std::string_view source, std::size_t line) {
  bool found = false;
  try {
    fields(config, [&](std::string_view name, auto& field) {
      if (name == key) {
        found = true;
        parse_scalar(field, value, key);
      }
    });
    if (!found) throw std::invalid_argument("unknown key");
  } catch (const std::invalid_argument& error) {
    throw ConfigError(std::string(source), line, std::string(key) + ": " + error.what());
  }
}

bool is_map(std::string_view key) {
  bool found = false;
  const ClacheConfig defaults;
  const std::string prefix = std::string(key) + ".";
  fields(defaults,
         [&](std::string_view name, const auto&) { found = found || name.starts_with(prefix); });
  return found;
}

template <class T>
std::string scalar_text(T value) {
  if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_same_v<T, Mode>) {
    if (value == Mode::RollingBuckets) return "rolling_buckets";
    if (value == Mode::ForwardDecay) return "forward_decay";
  } else if constexpr (std::is_same_v<T, Rule>) {
    if (value == Rule::Strict) return "strict";
    if (value == Rule::Always) return "always";
  } else if constexpr (std::is_same_v<T, Source>) {
    if (value == Source::ResidentCount) return "resident_count";
    if (value == Source::EngineWindow) return "engine_window";
  } else {
    char buffer[128];
    const auto result = [&] {
      if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(value)) throw std::invalid_argument("config number must be finite");
        return std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general,
                             17);
      } else {
        return std::to_chars(buffer, buffer + sizeof(buffer), value);
      }
    }();
    if (result.ec != std::errc{}) throw std::invalid_argument("cannot format config number");
    return std::string(buffer, result.ptr);
  }
  throw std::invalid_argument("invalid configuration enum");
}

}  // namespace

ConfigError::ConfigError(std::string source, std::size_t line, const std::string& message)
    : std::runtime_error(source + ":" + std::to_string(line) + ": " + message),
      source_(std::move(source)),
      line_(line) {}
const std::string& ConfigError::source() const noexcept { return source_; }
std::size_t ConfigError::line() const noexcept { return line_; }

std::uint64_t parse_size(std::string_view text) {
  text = trim(text);
  const auto suffix_pos = text.find_first_not_of("0123456789");
  const auto digits = text.substr(0, suffix_pos);
  const auto suffix =
      suffix_pos == std::string_view::npos ? std::string_view{} : text.substr(suffix_pos);
  std::uint64_t multiplier = 1;
  if (suffix == "KiB")
    multiplier = 1024;
  else if (suffix == "MiB")
    multiplier = 1024ULL * 1024;
  else if (suffix == "GiB")
    multiplier = 1024ULL * 1024 * 1024;
  else if (suffix == "KB")
    multiplier = 1000;
  else if (suffix == "MB")
    multiplier = 1000000;
  else if (suffix == "GB")
    multiplier = 1000000000;
  else if (!suffix.empty() && suffix != "B")
    throw std::invalid_argument("unknown size suffix");
  const auto value = number<std::uint64_t>(digits);
  if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    throw std::invalid_argument("size overflows uint64");
  }
  return value * multiplier;
}

void apply_config_text(ClacheConfig& config, std::string_view text, std::string_view source_name) {
  // Commit only after the entire input parses, so an error cannot leave a partial overlay.
  ClacheConfig next = config;
  std::vector<std::string> parents;
  std::set<std::string> seen;
  std::size_t line_number = 0;
  const auto fail = [&](const std::string& message) {
    throw ConfigError(std::string(source_name), line_number, message);
  };
  const auto remember = [&](const std::string& key) {
    if (!seen.insert(key).second) fail(key + ": duplicate key");
  };
  while (!text.empty()) {
    ++line_number;
    const auto newline = text.find('\n');
    auto line = text.substr(0, newline);
    text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
    line = line.substr(0, line.find('#'));
    if (line.find('\t') != std::string_view::npos) fail("tabs are not supported");
    if (trim(line).empty()) continue;
    const auto indent = line.find_first_not_of(' ');
    if (indent % 2 != 0) fail("indentation must use two spaces per level");
    const auto depth = indent / 2;
    if (depth > parents.size()) fail("unexpected indentation");
    parents.resize(depth);
    line = trim(line);
    if (line.starts_with('-')) fail("lists are not supported");
    // The canonical dotted assignment format is deliberately also readable.
    const auto equals = line.find('=');
    if (equals != std::string_view::npos) {
      if (depth != 0 || line.find(':') != std::string_view::npos) fail("invalid assignment");
      const std::string key(trim(line.substr(0, equals)));
      remember(key);
      assign(next, key, trim(line.substr(equals + 1)), source_name, line_number);
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) fail("expected key: value");
    const auto name = trim(line.substr(0, colon));
    if (name.empty() ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string_view::npos) {
      fail("invalid map key");
    }
    const std::string key =
        parents.empty() ? std::string(name) : parents.back() + "." + std::string(name);
    remember(key);
    const auto value = trim(line.substr(colon + 1));
    if (value.empty()) {
      if (!is_map(key)) fail(key + ": expected a scalar or unknown key");
      parents.push_back(key);
    } else if (value.starts_with('{')) {
      if (!is_map(key)) fail(key + ": unknown map");
      if (!value.ends_with('}')) fail(key + ": unterminated flow map");
      auto entries = trim(value.substr(1, value.size() - 2));
      while (!entries.empty()) {
        const auto comma = entries.find(',');
        const auto entry = trim(entries.substr(0, comma));
        const auto split = entry.find(':');
        if (split == std::string_view::npos) fail(key + ": expected flow key: value");
        const auto child = trim(entry.substr(0, split));
        if (child.empty() || child.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") !=
                                 std::string_view::npos) {
          fail(key + ": invalid flow key");
        }
        const std::string full = key + "." + std::string(child);
        remember(full);
        assign(next, full, trim(entry.substr(split + 1)), source_name, line_number);
        if (comma == std::string_view::npos) break;
        entries = trim(entries.substr(comma + 1));
        if (entries.empty()) fail(key + ": trailing flow comma");
      }
    } else {
      assign(next, key, value, source_name, line_number);
    }
  }
  config = next;
}

void apply_config_file(ClacheConfig& config, const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw ConfigError(path.string(), 0, "cannot open file");
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (input.bad()) throw ConfigError(path.string(), 0, "cannot read file");
  apply_config_text(config, text, path.string());
}

void apply_override(ClacheConfig& config, std::string_view assignment) {
  const auto equals = assignment.find('=');
  if (equals == std::string_view::npos) throw ConfigError("--set", 0, "expected key=value");
  assign(config, trim(assignment.substr(0, equals)), trim(assignment.substr(equals + 1)), "--set",
         0);
}

std::string canonical_config(const ClacheConfig& config) {
  std::map<std::string, std::string> sorted;
  fields(config, [&](std::string_view name, const auto& field) {
    sorted.emplace(name, scalar_text(field));
  });
  std::string result;
  for (const auto& [key, value] : sorted) result += key + "=" + value + "\n";
  return result;
}
std::uint64_t config_fingerprint(const ClacheConfig& config) {
  return hash64(canonical_config(config));
}

}  // namespace clache
