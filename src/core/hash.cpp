#include "clache/hash.h"

#include <array>
#include <stdexcept>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace clache {

std::uint64_t hash64(const void* data, std::size_t len, std::uint64_t seed) noexcept {
  return XXH3_64bits_withSeed(data, len, seed);
}

std::uint64_t hash64_u64(std::uint64_t v, std::uint64_t seed) noexcept {
  std::array<unsigned char, 8> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<unsigned char>(v >> (8 * i));
  }
  return XXH3_64bits_withSeed(bytes.data(), bytes.size(), seed);
}

struct Hasher64::State {
  XXH3_state_t xxh;
};

Hasher64::Hasher64(std::uint64_t seed) : state_(std::make_unique<State>()) {
  if (XXH3_64bits_reset_withSeed(&state_->xxh, seed) != XXH_OK) {
    throw std::runtime_error("XXH3 reset failed");
  }
}

Hasher64::~Hasher64() = default;
Hasher64::Hasher64(Hasher64&&) noexcept = default;
Hasher64& Hasher64::operator=(Hasher64&&) noexcept = default;

void Hasher64::update(const void* data, std::size_t len) {
  if (XXH3_64bits_update(&state_->xxh, data, len) != XXH_OK) {
    throw std::runtime_error("XXH3 update failed");
  }
}

std::uint64_t Hasher64::digest() const { return XXH3_64bits_digest(&state_->xxh); }

}  // namespace clache
