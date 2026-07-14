// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <toml++/toml.hpp>

#include <cmath>
#include <cstdint>
#include <optional>

namespace fl {

// SAFE integer reads from TOML. Use these instead of `node.value<int64_t>()` / `value<int>()`.
//
// WHY THIS EXISTS. toml++'s float→int conversion is UNDEFINED BEHAVIOUR for an out-of-range value,
// and the bug is in the guard itself (`value.hpp`, the `// float -> int` branch):
//
//     const double val = *ref_cast<double>();
//     if (impl::fpclassify(val) == fp_class::ok
//         && static_cast<double>(static_cast<int64_t>(val)) == val)   // <-- the UB is HERE
//
// The cast it performs to decide whether the cast is safe is itself the unsafe cast. So handing a
// float node to `value<int64_t>()` is UB whenever the number does not fit in an int64 — and TOML
// makes that trivially reachable, because nothing stops an author (or an attacker) writing a
// floating-point literal where an integer field is expected:
//
//     [server]
//     port = 10888888888888888888888888888888.0
//
// That is the exact input the scheduled deep fuzz run found (#824). fl-server parses server.toml,
// and content packs parse entity/flight-model TOML from untrusted mods, so this is reachable input,
// not a curiosity.
//
// The fix is to never let a float node reach `value<int64_t>()`: read it as a double, validate it
// ourselves, and cast only when the cast is defined.

namespace detail {

inline std::optional<int64_t> tomlIntNode(const toml::node& n) {
    // Integer nodes are safe: toml++ range-checks the int→int narrowing path.
    if (n.is_integer())
        return n.value<int64_t>();

    if (!n.is_floating_point())
        return std::nullopt;

    const std::optional<double> d = n.value<double>();
    if (!d || !std::isfinite(*d))
        return std::nullopt;

    // int64's bounds, as exactly-representable doubles. The upper bound is EXCLUSIVE: 2^63 is
    // representable as a double but is one past INT64_MAX, and casting it is the very UB we are
    // avoiding. Comparing in double avoids any narrowing before the check.
    constexpr double kMin = -9223372036854775808.0;         // -2^63
    constexpr double kMaxExclusive = 9223372036854775808.0; // +2^63
    if (*d < kMin || *d >= kMaxExclusive)
        return std::nullopt;

    if (std::trunc(*d) != *d)
        return std::nullopt; // 1.5 is not an integer setting; reject rather than silently truncate

    return static_cast<int64_t>(*d);
}

} // namespace detail

// Returns the value of an integer-valued node, or nullopt if it is absent, not a number, not finite,
// not a whole number, or outside int64's range. A float literal that IS a whole number in range
// (`port = 4778.0`) is accepted, matching toml++'s intent.
inline std::optional<int64_t> tomlInt(const toml::node& n) {
    return detail::tomlIntNode(n);
}

// node_view overload (`tbl["a"]["b"]`), which may be empty.
template <typename T> [[nodiscard]] std::optional<int64_t> tomlInt(const toml::node_view<T>& n) {
    if (!n)
        return std::nullopt;
    return detail::tomlIntNode(*n.node());
}

// Same, narrowed to `int`. Returns nullopt when the value does not fit — the caller's range checks
// then see an absent field and use their default, which is what they already do for a missing key.
template <typename Node> [[nodiscard]] std::optional<int> tomlIntNarrow(const Node& n) {
    const std::optional<int64_t> v = tomlInt(n);
    if (!v)
        return std::nullopt;
    if (*v < static_cast<int64_t>(INT32_MIN) || *v > static_cast<int64_t>(INT32_MAX))
        return std::nullopt;
    return static_cast<int>(*v);
}

} // namespace fl
