#pragma once
#include <algorithm>
namespace AlertPolicy {
struct Config { bool enabled = true; int warning = 20; int critical = 10; };
inline bool Valid(const Config& c) { return c.critical >= 1 && c.warning <= 100 && c.critical < c.warning; }
struct Tracker {
    bool warned = false, critical = false;
    int previousPercent = -1;
    int Evaluate(const Config& c, bool connected, bool fresh, int percent, bool charging) {
        if (!Valid(c) || !connected || !fresh || percent < 0 || percent > 100) return 0;
        const int reset = std::min(100,c.warning + 5);
        // At warning=100 there is no healthy band above the threshold.
        // Rearm on recovery to full, never on repeated identical full readings.
        if (percent >= reset && (percent > c.warning || previousPercent < reset)) {
            warned = critical = false;
        }
        previousPercent = percent;
        if (!c.enabled || charging) return 0;
        if (percent <= c.critical && !critical) { warned = critical = true; return 2; }
        if (percent <= c.warning && !warned) { warned = true; return 1; }
        return 0;
    }
};
}
