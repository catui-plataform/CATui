// SPDX-License-Identifier: GPL-2.0-or-later
// Optional scalar ABI compatibility for the official LLVM 22.1.8 archive
// built against newer MSVC STL than toolset 14.50. Signatures verified against
// microsoft/STL stl/src/vector_algorithms.cpp. Empty ranges return first;
// ties retain the first element, as required by std::min/max_element.
#include <cstdint>
template<class T, bool Max>
static const void* extremum(const void* begin, const void* end) noexcept {
    auto* first = static_cast<const T*>(begin);
    auto* last = static_cast<const T*>(end);
    if (first == last) return first;
    auto* best = first;
    for (auto* p = first + 1; p != last; ++p)
        if (Max ? *p > *best : *p < *best) best = p;
    return best;
}
extern "C" {
struct minmax_result { const void* min; const void* max; };
minmax_result __stdcall __std_minmax_element_8u(const void* a, const void* b) noexcept {
    auto* first = static_cast<const std::uint64_t*>(a);
    auto* last = static_cast<const std::uint64_t*>(b);
    minmax_result result{a,a};
    if (first == last) return result;
    for (auto* p = first + 1; p != last; ++p) {
        if (*p < *static_cast<const std::uint64_t*>(result.min)) result.min = p;
        if (*p >= *static_cast<const std::uint64_t*>(result.max)) result.max = p;
    }
    return result;
}
const void* __stdcall __std_min_element_4u(const void* a, const void* b) noexcept { return extremum<std::uint32_t, false>(a,b); }
const void* __stdcall __std_min_element_8u(const void* a, const void* b) noexcept { return extremum<std::uint64_t, false>(a,b); }
const void* __stdcall __std_max_element_1u(const void* a, const void* b) noexcept { return extremum<std::uint8_t, true>(a,b); }
const void* __stdcall __std_max_element_4u(const void* a, const void* b) noexcept { return extremum<std::uint32_t, true>(a,b); }
const void* __stdcall __std_max_element_8u(const void* a, const void* b) noexcept { return extremum<std::uint64_t, true>(a,b); }
}
