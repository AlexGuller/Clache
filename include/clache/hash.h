// 64-bit hashing (XXH3_64). Used for Cache key ids, policy sketches,
// deterministic decision sampling, and trace/config fingerprints.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace clache {

std::uint64_t hash64(const void* data, std::size_t len, std::uint64_t seed = 0) noexcept;

inline std::uint64_t hash64(std::string_view s, std::uint64_t seed = 0) noexcept {
  return hash64(s.data(), s.size(), seed);
}

// Hashes the 8-byte little-endian representation of v.
std::uint64_t hash64_u64(std::uint64_t v, std::uint64_t seed = 0) noexcept;

// Streaming XXH3_64 for fingerprinting files and canonicalised configs.
class Hasher64 {
 public:
  explicit Hasher64(std::uint64_t seed = 0);
  ~Hasher64();
  Hasher64(Hasher64&&) noexcept;
  Hasher64& operator=(Hasher64&&) noexcept;
  Hasher64(const Hasher64&) = delete;
  Hasher64& operator=(const Hasher64&) = delete;

  void update(const void* data, std::size_t len);
  void update(std::string_view s) { update(s.data(), s.size()); }
  std::uint64_t digest() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace clache
