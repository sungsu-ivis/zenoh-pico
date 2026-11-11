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
#include <time.h>
#include <unistd.h>

static uint64_t perf_cpu_now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t)now.tv_sec * 1000000000u) + (uint64_t)now.tv_nsec;
}

/* /proc/stat에서 mask에 든 CPU들의 쉰 시간(idle + iowait)을 더한다 */
static bool perf_cpu_read_idle(uint64_t mask, uint64_t* idle) {
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

void perf_cpu_load_open(perf_cpu_load_t* cpu) {
    memset(cpu, 0, sizeof(*cpu));
    pthread_mutex_init(&cpu->lock, NULL);

    /* 이 스레드가 돌 수 있는 CPU (taskset으로 묶었으면 그 CPU 하나) */
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return;
    int only = -1;
    for (int i = 0; i < 64; i++) {
        if (CPU_ISSET(i, &set)) {
            cpu->cpu_mask |= (uint64_t)1u << i;
            only = i;
            cpu->cpu_count++;
        }
    }
    if (cpu->cpu_count == 0u) return;
    if (cpu->cpu_count == 1u) {
        snprintf(cpu->label, sizeof(cpu->label), "CPU%d", only);
    } else {
        snprintf(cpu->label, sizeof(cpu->label), "CPU %u개 합계", cpu->cpu_count);
    }
    cpu->ticks_per_s = sysconf(_SC_CLK_TCK);
    if (cpu->ticks_per_s <= 0) cpu->ticks_per_s = 100;
    cpu->is_available = perf_cpu_read_idle(cpu->cpu_mask, &cpu->previous_idle);
    cpu->previous_at_ns = perf_cpu_now_ns();
}

void perf_cpu_load_sample_tick(perf_cpu_load_t* cpu) {
    if (!cpu->is_available) return;
    uint64_t idle;
    if (!perf_cpu_read_idle(cpu->cpu_mask, &idle)) return;
    const uint64_t at_ns = perf_cpu_now_ns();
    /* 그동안 CPU들이 가진 시간 (/proc/stat 단위) = 지난 실제 시간 × 단위 × CPU 수 */
    const double d_all =
        ((double)(at_ns - cpu->previous_at_ns) / 1e9) * (double)cpu->ticks_per_s * (double)cpu->cpu_count;
    const double d_idle = (double)idle - (double)cpu->previous_idle;
    cpu->previous_idle = idle;
    cpu->previous_at_ns = at_ns;
    double load_pct = (d_all <= 0.0) ? 0.0 : 100.0 * (1.0 - (d_idle / d_all));
    /* 두 값을 읽는 순간이 조금 어긋나 범위를 살짝 넘을 수 있다 */
    if (load_pct < 0.0) load_pct = 0.0;
    if (load_pct > 100.0) load_pct = 100.0;

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
