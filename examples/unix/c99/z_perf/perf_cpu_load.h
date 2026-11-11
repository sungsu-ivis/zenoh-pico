/*
 * 네트워크 성능 측정 예제 - CPU 부하 측정 (1초 샘플, 10초 통계)
 *
 * 이 프로그램이 도는 코어의 CPU 부하율을 잰다. test_start.sh는 프로그램을 CPU 하나(PIN_CPU, 기본 3)에 묶으므로
 * 그 코어 하나의 부하다. 묶지 않고 실행하면(쓸 수 있는 CPU가 여럿이면) 그 CPU들을 합친 부하다.
 *   - 부하율 = 1 - 그 코어가 쉰 시간 / 지난 실제 시간.
 *     쉰 시간(idle + iowait)은 /proc/stat에서 읽는다. 커널(NO_HZ)이 쉬기 시작한 시각과 깬 시각으로 잰 정확한 값이다.
 *     지난 실제 시간은 CLOCK_MONOTONIC으로 잰다.
 *     이 프로그램뿐 아니라 그 코어에서 돈 모든 것(다른 프로세스, 커널의 네트워크 처리)이 들어간다.
 *   - /proc/stat의 일한 시간(user, system 등)은 쓰지 않는다. 일반 커널은 그 값을 타이머 틱마다 표본으로 세므로,
 *     1 ms마다 깨어 일하는 이 프로그램처럼 주기적인 일은 틱과 박자가 맞물리면 실행마다 0%에서 실제의 몇 배까지 틀린다
 *     (WSL2 HZ=250에서 실제 41%인 일이 0~63%로 나왔다).
 *   - 1초 태스크 : 샘플 하나를 잰다 (직전 샘플 이후의 부하율).
 *   - 10초 태스크: 그동안 모인 샘플(보통 10개)의 평균·최소·최대·표준편차를 한 줄 출력하고 비운다.
 *   - 끝날 때 남은 샘플이 있으면 마지막으로 한 번 더 출력한다.
 * 시각표: 시작할 때 기준값을 읽고, 첫 샘플은 다음 1초 경계(1초 출력 [a 1s]와 같은 순간)에 잰다.
 * 그래서 샘플 k는 1초 출력 [a ks]와 같은 구간이고, 통계 줄은 10초 경계보다 0.5초 늦게 나온다.
 */
#ifndef PERF_CPU_LOAD_H
#define PERF_CPU_LOAD_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define PERF_CPU_SAMPLE_PERIOD_MS 1000u  /* 1초 태스크 주기 */
#define PERF_CPU_REPORT_PERIOD_MS 10000u /* 10초 태스크 주기 */
/* 10초 태스크를 1초 경계보다 늦추는 시간 (그 경계의 샘플 뒤에 돈다) */
#define PERF_CPU_REPORT_OFFSET_MS 500u
#define PERF_CPU_SAMPLES_MAX 32u /* 한 번 출력할 때까지 담아 두는 샘플 수 (보통 10개) */

typedef struct {
    bool is_available;       /* /proc/stat을 읽을 수 있는가. false면 아무것도 하지 않는다 */
    char label[32];          /* 출력 이름: "CPU3", "CPU 8개 합계" */
    uint64_t cpu_mask;       /* 잴 CPU 번호 (비트 n = CPU n). 64개까지 */
    uint32_t cpu_count;      /* cpu_mask에 든 CPU 수 */
    long ticks_per_s;        /* /proc/stat 값의 단위 (USER_HZ, 보통 100) */
    uint64_t previous_idle;  /* 1초 태스크 전용: 직전 샘플 때까지 쉰 시간 (/proc/stat 단위) */
    uint64_t previous_at_ns; /* 1초 태스크 전용: 직전 샘플 시각 (CLOCK_MONOTONIC) */

    /* lock: 1초 태스크가 넣고 10초 태스크가 꺼내는 샘플 */
    pthread_mutex_t lock;
    double samples[PERF_CPU_SAMPLES_MAX]; /* 부하율 (%) */
    uint32_t sample_count;                /* samples에 든 개수 */
    uint32_t taken;                       /* 지금까지 잰 샘플 수 (= 샘플을 잰 초) */
} perf_cpu_load_t;

/* 잴 CPU를 정하고(이 스레드의 affinity) 기준값을 읽는다. 읽을 수 없으면 is_available = false. */
void perf_cpu_load_open(perf_cpu_load_t* cpu);

/* 1초 태스크: 샘플 하나를 잰다 */
void perf_cpu_load_sample_tick(perf_cpu_load_t* cpu);

/* 10초 태스크: 모인 샘플의 통계를 한 줄 출력하고 비운다. is_final이면 끝날 때 남은 샘플을 출력한다. */
void perf_cpu_load_report_tick(perf_cpu_load_t* cpu, const char* my_name, bool is_final);

#endif /* PERF_CPU_LOAD_H */
