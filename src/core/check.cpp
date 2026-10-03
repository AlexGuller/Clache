#include "clache/check.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace clache::detail {

void contract_violation(const char* expr, const char* msg, const char* file, int line) {
  throw std::logic_error(std::string(file) + ":" + std::to_string(line) +
                         ": contract violated: " + msg + " [" + expr + "]");
}

void assertion_failure(const char* expr, const char* msg, const char* file, int line) {
  std::fprintf(stderr, "%s:%d: assertion failed: %s [%s]\n", file, line, msg, expr);
  std::fflush(stderr);
  std::abort();
}

}  // namespace clache::detail
