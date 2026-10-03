#include <map>
#include <stdexcept>
#include <utility>

#include "clache/policy.h"

namespace clache {
namespace {

// Function-local static: safe regardless of static-initialisation order.
std::map<std::string, PolicyFactory, std::less<>>& registry() {
  static std::map<std::string, PolicyFactory, std::less<>> instance;
  return instance;
}

}  // namespace

bool register_policy(std::string_view name, PolicyFactory factory) {
  if (name.empty()) throw std::logic_error("policy name must not be empty");
  if (!factory) throw std::logic_error("policy factory must not be empty: " + std::string(name));
  auto [it, inserted] = registry().emplace(std::string(name), std::move(factory));
  if (!inserted) throw std::logic_error("duplicate policy registration: " + std::string(name));
  return true;
}

std::unique_ptr<Policy> make_policy(std::string_view name, const PolicyConfig& config,
                                    const PolicyContext& context) {
  const auto it = registry().find(name);
  if (it == registry().end()) {
    std::string known;
    for (const auto& [registered, factory] : registry()) {
      if (!known.empty()) known += ", ";
      known += registered;
    }
    throw std::invalid_argument("unknown policy '" + std::string(name) +
                                "' (registered: " + (known.empty() ? "none" : known) + ")");
  }
  auto policy = it->second(config, context);
  if (!policy) throw std::logic_error("policy factory returned null: " + std::string(name));
  return policy;
}

bool is_registered_policy(std::string_view name) {
  return registry().find(name) != registry().end();
}

std::vector<std::string> registered_policies() {
  std::vector<std::string> names;
  names.reserve(registry().size());
  for (const auto& [name, factory] : registry()) names.push_back(name);
  return names;
}

}  // namespace clache
