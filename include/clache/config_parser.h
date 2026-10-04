#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include "clache/config.h"

namespace clache {

struct ClacheConfig {
  EngineConfig engine;
  PolicyConfig policy;
};

class ConfigError : public std::runtime_error {
 public:
  ConfigError(std::string source, std::size_t line, const std::string& message);
  const std::string& source() const noexcept;
  std::size_t line() const noexcept;

 private:
  std::string source_;
  std::size_t line_;
};

// Overlay only supplied keys. Parsing does not validate cross-field constraints.
void apply_config_text(ClacheConfig& config, std::string_view text,
                       std::string_view source_name = "<string>");
void apply_config_file(ClacheConfig& config, const std::filesystem::path& path);
void apply_override(ClacheConfig& config, std::string_view assignment);

// Sorted dotted.key=value lines; also accepted by apply_config_text.
std::string canonical_config(const ClacheConfig& config);
std::uint64_t config_fingerprint(const ClacheConfig& config);
std::uint64_t parse_size(std::string_view text);

}  // namespace clache
