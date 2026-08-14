// Single source of truth for the version string.
//
// The version is bumped in lockstep across this file, the installer .iss, the
// CMake project() line and the deprecated Python app's APP_VERSION -- see the
// workspace CLAUDE.md.  test_version.cpp asserts this file and CMake agree, so
// a half-applied bump fails the build rather than shipping.

#ifndef PDFSHERPA_APP_VERSION_H
#define PDFSHERPA_APP_VERSION_H

namespace pdfsherpa {

inline constexpr const char* kAppVersion = "2.0.0";
inline constexpr const char* kAppName = "PDF Sherpa";

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_VERSION_H
