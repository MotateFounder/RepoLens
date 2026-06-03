#pragma once

#include <string_view>

#ifndef REPOLENS_VERSION
#define REPOLENS_VERSION "0.1.0"
#endif

namespace repolens {

inline constexpr std::string_view version = REPOLENS_VERSION;

} // namespace repolens
