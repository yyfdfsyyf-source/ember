#pragma once

// Single source of truth for the build version: the VERSION file at the repo
// root. Every build path injects it as a raw, unquoted token so no shell or
// generator has to survive nested quoting:
//
//   CMakeLists.txt  -> target_compile_definitions(agent PRIVATE EMBER_VERSION=0.10.0)
//   build.ps1       -> "-DEMBER_VERSION=0.10.0" in $flags
//   build-linux.sh  -> DEFS=(-DEMBER_VERSION=0.10.0)
//
// The injected token is a preprocessing number (0.10.0), so it is stringized
// here instead of being written as a C++ literal. A build that skips the
// injection still compiles and reports "0.0.0dev".
#ifndef EMBER_VERSION
#define EMBER_VERSION 0.0.0dev
#endif

#define EMBER_VERSION_STRINGIZE_INNER(x) #x
#define EMBER_VERSION_STRINGIZE(x) EMBER_VERSION_STRINGIZE_INNER(x)

namespace agent {

/// Version of this build, e.g. "0.10.0"; mirrors the VERSION file at the repo root.
inline constexpr char kVersion[] = EMBER_VERSION_STRINGIZE(EMBER_VERSION);

}  // namespace agent