// src/obs/scope_main.cpp — hades-scope CLI entry point
//
// Reads an Eventlog .alog file from argv[1], optionally filters lines by KEY_PREFIX
// (argv[2]) via scope_filter(), and prints matching lines to stdout. The uXMS analog
// for hades: inspect or replay the Eventlog by key prefix from the command line.

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
  if (!f) { std::cerr << "hades-scope: cannot open: " << argv[1] << "\n"; return 1; }
  std::vector<std::string> lines;
  std::string l;
  while (std::getline(f, l)) lines.push_back(l);
  for (const auto& o : hades::scope_filter(lines, argc > 2 ? argv[2] : ""))
    std::cout << o << "\n";
  return 0;
}
