#pragma once
#include <cstddef>

#if defined(__linux__)
#include <cstdio>
#include <unistd.h>
inline size_t get_rss_bytes() {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages = 0;
    long dummy = 0;
    if (fscanf(f, "%ld %ld", &dummy, &pages) != 2) pages = 0;
    fclose(f);
    return (size_t)pages * (size_t)sysconf(_SC_PAGESIZE);
}
#elif defined(__APPLE__)
#include <mach/mach.h>
inline size_t get_rss_bytes() {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return info.resident_size;
}
#else
inline size_t get_rss_bytes() {
    return 0;
}
#endif
