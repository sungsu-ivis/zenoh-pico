/*
 * CPU 부하 측정 예제 - 코어 CPU 시간 읽기
 *
 * 이 프로그램이 도는 코어의 누적 CPU 시간을 읽는다. test_start.sh는 프로그램을 CPU 하나(PIN_CPU, 기본 3)에 묶으므로
 * 그 코어 하나의 값이다. 묶지 않고 실행하면(쓸 수 있는 CPU가 여럿이면) 그 CPU들의 평균이다.
 *   - 일한 시간 = 지난 실제 시간 - 그 코어가 쉰 시간.
 *     쉰 시간(idle + iowait)은 /proc/stat에서 읽는다. NO_HZ 커널(일반 배포판)은 쉬기 시작한 시각과 깬 시각으로
 *     정확히 잰다. 지난 실제 시간은 CLOCK_MONOTONIC으로 잰다.
 *     이 프로그램뿐 아니라 그 코어에서 돈 모든 것(다른 프로세스, 그 코어에서 처리된 커널 일)이 들어간다.
 *   - /proc/stat의 일한 시간(user, system 등)은 쓰지 않는다. 일반 커널은 그 값을 타이머 틱마다 표본으로 세므로,
 *     1 ms마다 깨어 일하는 이 프로그램처럼 주기적인 일은 틱과 박자가 맞물리면 실행마다 0%에서 실제의 몇 배까지 틀린다.
 *   - 두 시점의 누적 값 차이로 그 구간의 평균 부하를 낸다.
 */
#ifndef CPULOAD_USAGE_H
#define CPULOAD_USAGE_H

#include <stdbool.h>
#include <stdint.h>

/* 잴 수 있는 CPU 번호의 범위: glibc cpu_set_t 하나(CPU_SETSIZE)와 같다.
 * CPU가 이보다 많은 커널에서는 affinity를 읽지 못해 재지 않는다 (잘못된 값을 내지 않는다). */
#define CPULOAD_MAX_CPUS 1024u

/* 잴 코어 정보 (open 때 정하고 그 뒤로는 읽기만 하므로 어느 스레드에서 불러도 된다) */
typedef struct {
    bool is_available; /* /proc/stat과 CPU affinity를 읽을 수 있는가. false면 값이 모두 0이다 */
    char label[32];    /* 출력 이름: "CPU3", "CPU 8개 평균" */
    uint64_t cpu_mask[CPULOAD_MAX_CPUS / 64u]; /* 잴 CPU 번호 (비트 n = CPU n) */
    uint32_t cpu_count;                        /* cpu_mask에 든 CPU 수 */
    long ticks_per_s;                          /* /proc/stat 값의 단위 (USER_HZ, 보통 100) */
    uint64_t base_idle;                        /* 시작할 때까지 쉰 시간 (/proc/stat 단위) */
    uint64_t base_at_ns;                       /* 시작한 시각 (CLOCK_MONOTONIC) */
} cpuload_meter_t;

/* 시작한 뒤의 누적 CPU 시간 */
typedef struct {
    uint32_t busy_ms;    /* 일한 시간 (ms). CPU가 여럿이면 CPU 하나 기준으로 나눈 값 */
    uint32_t elapsed_ms; /* 지난 실제 시간 (ms). 0이면 재지 못한 값 */
} cpuload_usage_t;

/* 잴 CPU를 정하고(이 스레드의 affinity) 기준값을 읽는다. 읽을 수 없으면 is_available = false. */
void cpuload_meter_open(cpuload_meter_t* meter);

/* 지금의 누적 CPU 시간. 재지 못하면 두 값 모두 0. */
cpuload_usage_t cpuload_meter_read(const cpuload_meter_t* meter);

/* 두 시점 사이의 평균 부하 (%, 0~100). 어느 쪽이든 재지 못했거나 구간이 없으면 음수. */
double cpuload_usage_load_pct(cpuload_usage_t from, cpuload_usage_t to);

#endif /* CPULOAD_USAGE_H */
