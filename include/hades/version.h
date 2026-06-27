// include/hades/version.h — single version() accessor for the hades harness
//
// Returns the build-time version string (populated from CMake via version.cpp).
// Used by hades_main for --version output; no Blackboard or Module dependency.

#pragma once
#include <string>
namespace hades { std::string version(); }
