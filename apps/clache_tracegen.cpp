#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "clache/tracegen.h"
int main(int argc, char** argv) {
  try {
    clache::TracegenConfig config;
    std::string out;
    auto format = clache::TraceFormat::Csv;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--help" || arg == "-h") {
        std::cout << "Usage: clache_tracegen --workload NAME --out PATH [--format csv|bin]\n"
                     "       [--param k=v ...] [--seed N] [--keys N] [--requests N]\n"
                     "       clache_tracegen --list\n";
        return 0;
      }
      if (arg == "--list") {
        list = true;
        continue;
      }
      if (arg != "--workload" && arg != "--out" && arg != "--format" && arg != "--param" &&
          arg != "--seed" && arg != "--keys" && arg != "--requests")
        throw std::invalid_argument("unknown option: " + arg);
      if (i + 1 == argc) throw std::invalid_argument("missing value for " + arg);
      const std::string value = argv[++i];
      if (arg == "--workload")
        config.workload = value;
      else if (arg == "--out")
        out = value;
      else if (arg == "--format") {
        if (value == "csv")
          format = clache::TraceFormat::Csv;
        else if (value == "bin")
          format = clache::TraceFormat::Binary;
        else
          throw std::invalid_argument("--format must be csv or bin");
      } else if (arg == "--param") {
        const auto eq = value.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == value.size())
          throw std::invalid_argument("--param expects k=v");
        config.params[value.substr(0, eq)] = value.substr(eq + 1);
      } else
        config.params[arg.substr(2)] = value;
    }
    if (list) {
      for (const auto& workload : clache::tracegen_workloads()) {
        std::cout << workload << '\n';
        for (const auto& [k, v] : clache::tracegen_defaults(workload))
          std::cout << "  " << k << '=' << v << '\n';
      }
      return 0;
    }
    if (out.empty())
      throw std::invalid_argument("--out is required (use --list to see workloads and defaults)");
    const auto meta = clache::generate_trace(config, out, format);
    std::cout << "Wrote " << meta.request_count << " requests, " << meta.unique_keys
              << " unique keys, " << meta.working_set_bytes << " working-set bytes\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "clache_tracegen: " << e.what() << '\n';
    return 1;
  }
}
