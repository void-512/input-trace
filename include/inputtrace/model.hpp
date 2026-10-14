#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace inputtrace {
enum Kind : int { key_press=1, key_release=2, combo=3, button_press=4, button_release=5,
                  movement=6, vertical_wheel=7, horizontal_wheel=8, mouse_combo=9 };

struct StatKey {
    std::int64_t minute=0, context=0, code=0;
    int kind=0, x=0, y=0, monitor=0;
    bool operator==(const StatKey&) const = default;
};
struct StatHash {
    std::size_t operator()(const StatKey& k) const noexcept {
        std::uint64_t h=1469598103934665603ull;
        for (std::uint64_t n : {std::uint64_t(k.minute), std::uint64_t(k.context),
             std::uint64_t(k.code), std::uint64_t(k.kind), std::uint64_t(std::uint32_t(k.x)),
             std::uint64_t(std::uint32_t(k.y)), std::uint64_t(k.monitor)}) {
            h=(h^n)*1099511628211ull;
        }
        return std::size_t(h);
    }
};
struct StatValue {
    std::int64_t count=0, duration_us=0, raw_distance=0, pixel_distance=0, wheel_delta=0;
};
struct Context {
    std::int64_t id=0;
    std::uint32_t pid=0;
    std::uintptr_t hwnd=0;
    std::string path, name;
    int confidence=0;
};
struct Monitor {
    int id=0,left=0,top=0,right=0,bottom=0;
    std::string device;
};
struct Batch {
    std::vector<Context> contexts;
    std::vector<Monitor> monitors;
    std::unordered_map<StatKey, StatValue, StatHash> stats;
    std::int64_t stopped_us=0;
};
}
