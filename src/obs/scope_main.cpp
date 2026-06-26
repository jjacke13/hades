#include "hades/obs/scope.h"
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: hades-scope <log> [KEY_PREFIX]\n";
    return 2;
  }
  std::ifstream f(argv[1]);
  std::vector<std::string> lines;
  std::string l;
  while (std::getline(f, l)) lines.push_back(l);
  for (const auto& o : hades::scope_filter(lines, argc > 2 ? argv[2] : ""))
    std::cout << o << "\n";
  return 0;
}
