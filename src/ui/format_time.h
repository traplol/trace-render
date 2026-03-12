#pragma once
#include <cmath>
#include <cstddef>
#include <cstdio>

inline void format_time(double us, char* buf, size_t buf_size) {
    double abs_us = std::abs(us);
    if (abs_us < 1.0)
        snprintf(buf, buf_size, "%.1f ns", us * 1000.0);
    else if (abs_us < 1000.0)
        snprintf(buf, buf_size, "%.3f us", us);
    else if (abs_us < 1000000.0)
        snprintf(buf, buf_size, "%.3f ms", us / 1000.0);
    else
        snprintf(buf, buf_size, "%.3f s", us / 1000000.0);
}

// Format a timestamp for the ruler - uses tick_interval to determine precision
inline void format_ruler_time(double us, double tick_interval, char* buf, size_t buf_size) {
    double abs_us = std::abs(us);

    if (tick_interval < 1.0) {
        // Nanosecond ticks - show in appropriate unit with extra decimals
        int decimals = 3;
        if (tick_interval < 0.01)
            decimals = 4;
        else if (tick_interval < 0.1)
            decimals = 3;
        if (abs_us >= 1000000.0)
            snprintf(buf, buf_size, "%.*f s", decimals + 6, us / 1000000.0);
        else if (abs_us >= 1000.0)
            snprintf(buf, buf_size, "%.*f ms", decimals + 3, us / 1000.0);
        else
            snprintf(buf, buf_size, "%.*f us", decimals, us);
    } else if (tick_interval < 1000.0) {
        // Microsecond ticks
        if (abs_us >= 1000000.0) {
            int decimals = 6;
            if (tick_interval >= 100.0)
                decimals = 4;
            else if (tick_interval >= 10.0)
                decimals = 5;
            snprintf(buf, buf_size, "%.*f s", decimals, us / 1000000.0);
        } else if (abs_us >= 1000.0) {
            int decimals = 3;
            if (tick_interval >= 100.0)
                decimals = 1;
            else if (tick_interval >= 10.0)
                decimals = 2;
            snprintf(buf, buf_size, "%.*f ms", decimals, us / 1000.0);
        } else {
            snprintf(buf, buf_size, "%.1f us", us);
        }
    } else if (tick_interval < 1000000.0) {
        // Millisecond ticks
        if (abs_us >= 1000000.0) {
            int decimals = 3;
            if (tick_interval >= 100000.0)
                decimals = 1;
            else if (tick_interval >= 10000.0)
                decimals = 2;
            snprintf(buf, buf_size, "%.*f s", decimals, us / 1000000.0);
        } else {
            snprintf(buf, buf_size, "%.1f ms", us / 1000.0);
        }
    } else {
        // Second ticks
        int decimals = 0;
        if (tick_interval < 10000000.0) decimals = 1;
        snprintf(buf, buf_size, "%.*f s", decimals, us / 1000000.0);
    }
}
