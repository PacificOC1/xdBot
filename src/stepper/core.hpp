#pragma once
// The sub-tick stepper's timeline, without the game (host-tested in tests/stepper_test.cpp). Ported from the Frame
// Window Counter's src/Stepper.cpp (PacificOC1/frame-windows, v1.150.0), made to work on xdBot's own macro inputs.
//
// A click sits at a macro frame and a part of it: `frame` is xdBot's frame (the level clock × TPS + 1, as xdBot counts
// it when it presses its inputs, at the end of a step's processCommands), and `sub` (0 .. kUnits - 1) how far into the
// physics that comes right after that press point it lands. sub 0 = a whole-frame click, exactly what xdBot records and
// plays; sub > 0 = pressed that far into the players' update, split the way Click Between Frames splits it.
//
// The helpers are templates over any type with: frame (unsigned / int), long long sub, int button, bool player2 (xdBot's
// "2p" flag: GD's isPlayer1 argument, xdBot's own quirk), bool down, long long origin.

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace xdstep {

// One frame in the stepper's units (so 2,400,000,000 Hz at 240 TPS is one unit).
constexpr long long kUnits = 10'000'000;
// InputEvent::origin when a click was never moved
constexpr long long kNoOrigin = LLONG_MIN;

template <class T> long long keyOf(T const& e) { return static_cast<long long>(e.frame) * kUnits + e.sub; }
template <class T> bool inside(T const& e) { return e.sub > 0; }
template <class T> double fractionOf(T const& e) { return static_cast<double>(e.sub) / static_cast<double>(kUnits); }
// where a click acts, in frames (for the graph)
template <class T> double timeOf(T const& e) { return static_cast<double>(e.frame) + fractionOf(e); }

// Parts a frame is split into for a rate: 2400 Hz = 10 parts at 240 TPS. At least 1 (whole frames), at most kUnits.
inline int partsFor(long long hz, double tps) {
    long long parts = std::llround(static_cast<double>(hz) / std::max(1.0, tps));
    return static_cast<int>(std::clamp<long long>(parts, 1, kUnits));
}
inline long long hzOf(int parts, double tps) { return std::llround(static_cast<double>(parts) * tps); }
// where part `part` of a frame split into `parts` sits, in units
inline long long subOf(int part, int parts) {
    if (parts <= 0) return 0;
    return (static_cast<long long>(part) * kUnits + parts / 2) / parts;
}
// decimals worth showing: 0 for whole frames, 1 up to 10 parts ... 7
inline int decimalsFor(int parts) {
    int d = 0;
    for (long long p = 1; p < parts && d < 7; p *= 10) ++d;
    return d;
}
inline std::string hzText(long long hz) {
    std::string digits = std::to_string(hz), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}
// "1832.30" for a time in units
inline std::string unitsText(long long units, int parts) {
    long long const t = units >= 0 ? units / kUnits : -((-units + kUnits - 1) / kUnits);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", std::max(decimalsFor(parts), 1),
        static_cast<double>(t) + static_cast<double>(units - t * kUnits) / static_cast<double>(kUnits));
    return buf;
}
// The next rate up (dir > 0: finer) or down: the TPS × 1, 10, 100 ... 10,000,000.
inline long long nextHz(long long hz, int dir, double tps) {
    long long rates[8];
    long long r0 = std::max<long long>(1, std::llround(tps));
    for (int k = 0; k < 8; ++k, r0 *= 10) rates[k] = r0;
    if (dir > 0) {
        for (long long r : rates) if (r > hz) return r;
        return rates[7];
    }
    for (int k = 7; k >= 0; --k) if (rates[k] < hz) return rates[k];
    return rates[0];
}

// A place between parts: `part` parts into frame `frame` (the coming one).
struct Pos {
    int frame = 0;
    int part = 0;
    bool operator==(Pos const&) const = default;
};
inline long long keyOf(Pos p, int parts) { return static_cast<long long>(p.frame) * kUnits + subOf(p.part, parts); }
inline std::string posText(Pos p, int parts) { return unitsText(keyOf(p, parts), parts); }
// p moved by `by` parts (negative = back), across frames; never before frame `minFrame`.
inline Pos moved(Pos p, long long by, int parts, int minFrame = 0) {
    parts = std::max(1, parts);
    long long at = static_cast<long long>(p.frame) * parts + p.part + by;
    at = std::max(at, static_cast<long long>(minFrame) * parts);
    return {static_cast<int>(at / parts), static_cast<int>(at % parts)};
}

template <class T> void sortTimeline(std::vector<T>& tl) {
    std::stable_sort(tl.begin(), tl.end(), [](T const& a, T const& b) { return keyOf(a) < keyOf(b); });
}
template <class T> bool isSorted(std::vector<T> const& tl) {
    return std::is_sorted(tl.begin(), tl.end(), [](T const& a, T const& b) { return keyOf(a) < keyOf(b); });
}
// the first click at or after `key` (size() if none)
template <class T> std::size_t firstAtOrAfter(std::vector<T> const& tl, long long key) {
    auto it = std::lower_bound(tl.begin(), tl.end(), key, [](T const& e, long long k) { return keyOf(e) < k; });
    return static_cast<std::size_t>(it - tl.begin());
}
// puts a click in after any at the same moment; where it went
template <class T> std::size_t insertClickAt(std::vector<T>& tl, T const& e) {
    auto it = std::upper_bound(tl.begin(), tl.end(), keyOf(e), [](long long k, T const& x) { return k < keyOf(x); });
    // (the insert first: it can move the vector, and a begin() taken before it would be stale)
    auto const placed = tl.insert(it, e);
    return static_cast<std::size_t>(placed - tl.begin());
}

// Each click's input number: presses 1, 2, 3 ... in order; a release gets the number of the press it lets go of.
template <class T> std::vector<int> inputNumbers(std::vector<T> const& tl) {
    std::vector<int> out(tl.size(), 0);
    int presses = 0;
    int last[4][2] = {};
    for (std::size_t i = 0; i < tl.size(); ++i) {
        auto const& e = tl[i];
        int const b = std::clamp(e.button, 0, 3), p = e.player2 ? 1 : 0;
        if (e.down) {
            out[i] = ++presses;
            last[b][p] = presses;
        }
        else out[i] = last[b][p];
    }
    return out;
}
template <class T> int movedCount(std::vector<T> const& tl) {
    int n = 0;
    for (auto const& e : tl) n += e.origin != kNoOrigin && e.origin != keyOf(e);
    return n;
}
template <class T> double movedFrames(T const& e) {
    if (e.origin == kNoOrigin) return 0.0;
    return static_cast<double>(keyOf(e) - e.origin) / static_cast<double>(kUnits);
}
// A click moved by `units`, never before frame `minFrame`; its first place kept in `origin`.
template <class T> T movedBy(T e, long long units, int minFrame = 0) {
    if (e.origin == kNoOrigin) e.origin = keyOf(e);
    long long t = std::max(keyOf(e) + units, static_cast<long long>(minFrame) * kUnits);
    e.frame = static_cast<decltype(e.frame)>(t / kUnits);
    e.sub = t % kUnits;
    return e;
}
// the earliest key where two timelines differ (LLONG_MAX if the same)
template <class T> long long firstDifference(std::vector<T> const& a, std::vector<T> const& b) {
    std::size_t const n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        auto const& x = a[i];
        auto const& y = b[i];
        if (keyOf(x) != keyOf(y) || x.down != y.down || x.button != y.button || x.player2 != y.player2)
            return std::min(keyOf(x), keyOf(y));
    }
    if (a.size() > n) return keyOf(a[n]);
    if (b.size() > n) return keyOf(b[n]);
    return std::numeric_limits<long long>::max();
}
// The next click at or after `key` moved there. Its index, or -1.
template <class T> int moveNextHere(std::vector<T>& tl, long long key) {
    auto i = firstAtOrAfter(tl, key);
    if (i >= tl.size()) return -1;
    auto& e = tl[i];
    if (e.origin == kNoOrigin) e.origin = keyOf(e);
    e.frame = static_cast<decltype(e.frame)>(key / kUnits);
    e.sub = key % kUnits;
    return static_cast<int>(i);
}
template <class T> std::optional<T> eraseNext(std::vector<T>& tl, long long key) {
    auto i = firstAtOrAfter(tl, key);
    if (i >= tl.size()) return std::nullopt;
    T e = tl[i];
    tl.erase(tl.begin() + static_cast<std::ptrdiff_t>(i));
    return e;
}
// whether `button` of that flag is held at `key` (the last click on it at or before `key` is a press); nullopt: none
template <class T> std::optional<bool> heldAt(std::vector<T> const& tl, long long key, int button, bool player2) {
    for (auto i = firstAtOrAfter(tl, key == LLONG_MAX ? key : key + 1); i-- > 0;) {
        auto const& e = tl[i];
        if (e.button == button && e.player2 == player2) return e.down;
    }
    return std::nullopt;
}
// clicks with frame in (after, upTo], in order: the indices
template <class T> std::vector<std::size_t> dueIn(std::vector<T> const& tl, long long after, long long upTo) {
    std::vector<std::size_t> out;
    for (auto i = firstAtOrAfter(tl, (after + 1) * kUnits); i < tl.size() && static_cast<long long>(tl[i].frame) <= upTo; ++i)
        out.push_back(i);
    return out;
}

// Two marks (the earliest and latest timing that live): the window between them, counted the way a CBF count counts
// it (both ends in: hi − lo + one step).
struct Marks {
    std::optional<long long> lo, hi;
    void add(long long units) {
        if (lo && hi) clear();
        if (!lo) {
            lo = units;
            return;
        }
        hi = units;
        if (*hi < *lo) std::swap(*lo, *hi);
    }
    void clear() { lo.reset(); hi.reset(); }
    bool done() const { return lo && hi; }
    double windowFrames(int parts) const {
        if (!done()) return 0.0;
        return static_cast<double>(*hi - *lo) / kUnits + 1.0 / std::max(1, parts);
    }
    std::string text(int parts) const {
        if (!lo) return {};
        if (!hi) return "earliest " + unitsText(*lo, parts) + " (mark the latest next)";
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.*f", decimalsFor(parts), windowFrames(parts));
        return unitsText(*lo, parts) + " to " + unitsText(*hi, parts) + " = " + buf + " frames";
    }
};

} // namespace xdstep
