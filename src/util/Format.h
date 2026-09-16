#pragma once
//
// std::format shim.
//
// <format> landed in MSVC 19.30 (VS2022 17.0), GCC 13 and Clang 17. This project
// is otherwise buildable with VS2019 16.11 (MSVC 19.29), which has C++20 language
// support but no <format>. Rather than hand-roll formatting, fall back to {fmt} --
// which is what <format> was standardised from, so the call syntax is identical.
//
// CMake decides which path applies by actually compiling a probe, not by version
// sniffing, and only fetches {fmt} when the probe fails.
//
#include <version>

#if defined(SOI_USE_FMTLIB)
  #include <fmt/core.h>
  #include <fmt/format.h>
namespace soi {
template <class... A> using FormatString = fmt::format_string<A...>;
using fmt::format;
} // namespace soi

#else
  #include <format>
namespace soi {
template <class... A> using FormatString = std::format_string<A...>;
using std::format;
} // namespace soi
#endif
