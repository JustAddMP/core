#pragma once
// Version of the JustAddMP core framework. Game repos pin core to a release tag (vMAJOR.MINOR.PATCH) through their
// external/core submodule; see CHANGELOG.md and README.md "Versions". Bump this together with the tag.
namespace mc {

constexpr int kCoreVersionMajor = 0;
constexpr int kCoreVersionMinor = 3;
constexpr int kCoreVersionPatch = 0;
constexpr const char* kCoreVersion = "0.3.0";

} // namespace mc
