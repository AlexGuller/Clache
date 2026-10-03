// Assertion and contract macros (Spec §23).
//
// CLACHE_REQUIRE  always on; throws std::logic_error. Use for contract
//                 violations by callers or policies (e.g. an invalid plan).
// CLACHE_ASSERT   on in Debug or with CLACHE_CHECK_INVARIANTS; aborts with a
//                 message. Use for internal invariants on hot paths.
#pragma once

namespace clache::detail {

[[noreturn]] void contract_violation(const char* expr, const char* msg, const char* file, int line);
[[noreturn]] void assertion_failure(const char* expr, const char* msg, const char* file, int line);

}  // namespace clache::detail

#define CLACHE_REQUIRE(cond, msg)                                                      \
  do {                                                                                 \
    if (!(cond)) ::clache::detail::contract_violation(#cond, msg, __FILE__, __LINE__); \
  } while (0)

#if !defined(NDEBUG) || defined(CLACHE_CHECK_INVARIANTS)
#define CLACHE_ASSERTS_ENABLED 1
#define CLACHE_ASSERT(cond, msg)                                                      \
  do {                                                                                \
    if (!(cond)) ::clache::detail::assertion_failure(#cond, msg, __FILE__, __LINE__); \
  } while (0)
#else
#define CLACHE_ASSERTS_ENABLED 0
#define CLACHE_ASSERT(cond, msg) \
  do {                           \
    (void)sizeof(cond);          \
  } while (0)
#endif
