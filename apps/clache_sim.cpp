#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "sim/simulator.h"
int main(int argc, char** argv) {
  try {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
      std::cout << clache::sim::help();
      return 0;
    }
    return clache::sim::run(clache::sim::parse_options(args));
  } catch (const std::exception& error) {
    std::cerr << "clache_sim: " << error.what() << '\n';
    return 1;
  }
}
