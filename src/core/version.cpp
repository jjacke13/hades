// src/core/version.cpp — hades semver string
//
// Single definition of hades::version(); returns the current semver string.
// Consumed by hades_main.cpp for --version output.

#include "hades/version.h"
namespace hades { std::string version() { return "0.1.0"; } }
