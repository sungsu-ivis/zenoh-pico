/*
 * CPU 부하 측정 예제 - 코어 CPU 시간 읽기 구현 (설명은 cpu_usage.h)
 */
#define _GNU_SOURCE /* sched_getaffinity */
#include "cpu_usage.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t cpuload_now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t)now.tv_sec * 1000000000u) + (uint64_t)now.tv_nsec;
}

/* /proc/stat에서 mask에 든 CPU들의 쉰 시간(idle + iowait)을 더한다 */
static bool cpuload_read_idle(uint64_t mask, uint64_t* idle) {
    FILE* file = fopen("/proc/stat", "r");
    if (file == NULL) return false;
    char line[512];
    bool is_found = false;
    *idle = 0u;
    while (fgets(line, sizeof(line), file) != NULL) {
        /* "cpuN user nice system idle iowait ..." 줄만 본다 ("cpu " 합계 줄은 건너뛴다) */
        if ((strncmp(line, "cpu", 3) != 0) || (line[3] < '0') || (line[3] > '9')) continue;
        char* p = &line[3];
        const unsigned long index = strtoul(p, &p, 10);
        if ((index >= 64u) || ((mask & ((uint64_t)1u << index)) == 0u)) continue;
        uint64_t fields[5] = {0u};
        for (int i = 0; i < 5; i++) fields[i] = strtoull(p, &p, 10);
        *idle += fields[3] + fields[4];
        is_found = true;
    }
    fclose(file);
    return is_found;
}

void cpuload_meter_open(cpuload_meter_t* meter) {
    memset(meter, 0, sizeof(*meter));

    /* 이 스레드가 돌 수 있는 CPU (taskset으로 묶었으면 그 CPU 하나) */
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return;
    int only = -1;
    for (int i = 0; i < 64; i++) {
        if (CPU_ISSET(i, &set)) {
            meter->cpu_mask |= (uint64_t)1u << i;
            only = i;
            meter->cpu_count++;
        }
    }
    if (meter->cpu_count == 0u) return;
    if (meter->cpu_count == 1u) {
        snprintf(meter->label, sizeof(meter->label), "CPU%d", only);
    } else {
        snprintf(meter->label, sizeof(meter->label), "CPU %u개 평균", meter->cpu_count);
    }
    meter->ticks_per_s = sysconf(_SC_CLK_TCK);
    if (meter->ticks_per_s <= 0) meter->ticks_per_s = 100;
    meter->is_available = cpuload_read_idle(meter->cpu_mask, &meter->base_idle);
    meter->base_at_ns = cpuload_now_ns();
}

cpuload_usage_t cpuload_meter_read(const cpuload_meter_t* meter) {
    cpuload_usage_t usage = {0u, 0u};
    uint64_t idle;
    if (!meter->is_available || !cpuload_read_idle(meter->cpu_mask, &idle)) return usage;
    const double elapsed_ms = (double)(cpuload_now_ns() - meter->base_at_ns) / 1e6;
    /* 쉰 시간 (ms, CPU 하나 기준) */
    const double idle_ms = (idle <= meter->base_idle) ? 0.0
                                                      : (double)(idle - meter->base_idle) * 1000.0 /
                                                            ((double)meter->ticks_per_s * (double)meter->cpu_count);
    double busy_ms = elapsed_ms - idle_ms;
    if (busy_ms < 0.0) busy_ms = 0.0;
    usage.elapsed_ms = (elapsed_ms < 1.0) ? 1u : (uint32_t)elapsed_ms;
    usage.busy_ms = (uint32_t)(busy_ms + 0.5);
    if (usage.busy_ms > usage.elapsed_ms) usage.busy_ms = usage.elapsed_ms;
    return usage;
}

double cpuload_usage_load_pct(cpuload_usage_t from, cpuload_usage_t to) {
    if ((from.elapsed_ms == 0u) || (to.elapsed_ms == 0u)) return -1.0;
    const uint32_t d_elapsed = to.elapsed_ms - from.elapsed_ms;
    if (d_elapsed == 0u) return -1.0;
    /* /proc/stat의 쉰 시간은 10 ms 단위라 짧은 구간에서는 일한 시간이 거꾸로 갈 수 있다 */
    const uint32_t d_busy = (to.busy_ms > from.busy_ms) ? to.busy_ms - from.busy_ms : 0u;
    const double load_pct = 100.0 * (double)d_busy / (double)d_elapsed;
    return (load_pct > 100.0) ? 100.0 : load_pct;
}
