/*
 * 네트워크 성능 측정 예제 - CPU 부하 측정 구현 (설명은 perf_cpu_load.h)
 */
#define _GNU_SOURCE /* sched_getaffinity */
#include "perf_cpu_load.h"

#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* /proc/stat에서 mask에 든 CPU들의 일한 jiffies와 전체 jiffies를 더한다 */
static bool perf_cpu_read(uint64_t mask, uint64_t* busy, uint64_t* total) {
    FILE* file = fopen("/proc/stat", "r");
    if (file == NULL) return false;
    char line[512];
    bool is_found = false;
    *busy = 0u;
    *total = 0u;
    while (fgets(line, sizeof(line), file) != NULL) {
        /* "cpuN user nice system idle iowait irq softirq steal ..." 줄만 본다 ("cpu " 합계 줄은 건너뛴다) */
        if ((strncmp(line, "cpu", 3) != 0) || (line[3] < '0') || (line[3] > '9')) continue;
        char* p = &line[3];
        const unsigned long index = strtoul(p, &p, 10);
        if ((index >= 64u) || ((mask & ((uint64_t)1u << index)) == 0u)) continue;
        uint64_t fields[8] = {0u};
        for (int i = 0; i < 8; i++) fields[i] = strtoull(p, &p, 10);
        const uint64_t idle = fields[3] + fields[4]; /* idle + iowait */
        const uint64_t all =
            fields[0] + fields[1] + fields[2] + fields[3] + fields[4] + fields[5] + fields[6] + fields[7];
        *busy += all - idle;
        *total += all;
        is_found = true;
    }
    fclose(file);
    return is_found;
}

void perf_cpu_load_open(perf_cpu_load_t* cpu) {
    memset(cpu, 0, sizeof(*cpu));
    pthread_mutex_init(&cpu->lock, NULL);

    /* 이 스레드가 돌 수 있는 CPU (taskset으로 묶었으면 그 CPU 하나) */
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return;
    unsigned count = 0u;
    int only = -1;
    for (int i = 0; i < 64; i++) {
        if (CPU_ISSET(i, &set)) {
            cpu->cpu_mask |= (uint64_t)1u << i;
            only = i;
            count++;
        }
    }
    if (count == 0u) return;
    if (count == 1u) {
        snprintf(cpu->label, sizeof(cpu->label), "CPU%d", only);
    } else {
        snprintf(cpu->label, sizeof(cpu->label), "CPU %u개 합계", count);
    }
    cpu->is_available = perf_cpu_read(cpu->cpu_mask, &cpu->previous_busy, &cpu->previous_total);
}

void perf_cpu_load_sample_tick(perf_cpu_load_t* cpu) {
    if (!cpu->is_available) return;
    uint64_t busy;
    uint64_t total;
    if (!perf_cpu_read(cpu->cpu_mask, &busy, &total)) return;
    const uint64_t d_busy = busy - cpu->previous_busy;
    const uint64_t d_total = total - cpu->previous_total;
    cpu->previous_busy = busy;
    cpu->previous_total = total;
    const double load_pct = (d_total == 0u) ? 0.0 : 100.0 * (double)d_busy / (double)d_total;

    pthread_mutex_lock(&cpu->lock);
    if (cpu->sample_count < PERF_CPU_SAMPLES_MAX) cpu->samples[cpu->sample_count++] = load_pct;
    cpu->taken++;
    pthread_mutex_unlock(&cpu->lock);
}

void perf_cpu_load_report_tick(perf_cpu_load_t* cpu, const char* my_name, bool is_final) {
    if (!cpu->is_available) return;
    double samples[PERF_CPU_SAMPLES_MAX];
    pthread_mutex_lock(&cpu->lock);
    const uint32_t n = cpu->sample_count;
    const uint32_t last_s = cpu->taken;
    memcpy(samples, cpu->samples, sizeof(samples[0]) * n);
    cpu->sample_count = 0u;
    pthread_mutex_unlock(&cpu->lock);
    if (n == 0u) return;

    double sum = 0.0;
    double min = samples[0];
    double max = samples[0];
    for (uint32_t i = 0u; i < n; i++) {
        sum += samples[i];
        if (samples[i] < min) min = samples[i];
        if (samples[i] > max) max = samples[i];
    }
    const double mean = sum / (double)n;
    double square_sum = 0.0;
    for (uint32_t i = 0u; i < n; i++) square_sum += (samples[i] - mean) * (samples[i] - mean);
    const double stddev = sqrt(square_sum / (double)n);

    char range[32];
    if (n == 1u) {
        snprintf(range, sizeof(range), "%u초", last_s);
    } else {
        snprintf(range, sizeof(range), "%u~%u초", last_s - n + 1u, last_s);
    }
    printf("[%s %3us] %s 부하 %s (1초 샘플 %u개%s): 평균 %5.1f%%  최소 %5.1f%%  최대 %5.1f%%  표준편차 %4.1f%%\n\n",
           my_name, last_s, cpu->label, range, n, is_final ? ", 끝" : "", mean, min, max, stddev);
    fflush(stdout);
}
