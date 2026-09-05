//
// Profile.h — per-effect frame timing. Header-only, always on (a handful of
// clock reads per frame is free next to the effects themselves).
//
// EffectHost calls beginFrame(); every effect render — including children
// of nested Effect Lists — is wrapped in timed(), which records the effect's
// SELF time (its total minus whatever nested effects recorded meanwhile), so
// a list's entry is just its own blend overhead and nothing is double
// counted. summary() merges same-name entries and sorts by cost.
//
#pragma once
#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace viz { namespace prof {

struct Entry { const char* name; double ms; };

inline std::vector<Entry>& entries() { static std::vector<Entry> v; return v; }

inline double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

inline void beginFrame() { entries().clear(); }

// Record `name` with self time around fn().
template <class Fn>
inline void timed(const char* name, Fn&& fn) {
    auto& es = entries();
    const size_t before = es.size();
    const double t0 = nowMs();
    fn();
    const double total = nowMs() - t0;
    double nested = 0.0;
    for (size_t i = before; i < es.size(); ++i) nested += es[i].ms;
    es.push_back({name, std::max(0.0, total - nested)});
}

// Merged (by name), sorted by descending ms.
inline std::vector<std::pair<std::string, double>> summary() {
    std::vector<std::pair<std::string, double>> out;
    for (const Entry& e : entries()) {
        bool found = false;
        for (auto& o : out) if (o.first == e.name) { o.second += e.ms; found = true; break; }
        if (!found) out.emplace_back(e.name, e.ms);
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

}} // namespace viz::prof
