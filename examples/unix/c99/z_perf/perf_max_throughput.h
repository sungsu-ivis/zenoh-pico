/*
 * 네트워크 성능 측정 예제 - 최대 처리량 탐색 (보내는 쪽)
 *
 * "손실 0%로 보낼 수 있는 최대 송신량"을 찾는다. 메시지는 num0 하나(128 B)만 쓴다.
 *
 * 단계 하나 = [보내기] → [쉬면서 상대 보고 기다리기] → [판정]
 *   - 보내기  : 목표 송신량(msg/s)으로 num0을 보낸다. 워밍업 3초, 탐색 단계 5초, 확인 단계 15초.
 *   - 기다리기: 송신을 멈추고, 상대 status의 "집계 기준 seq(counted_seq)"가 이번 단계의 마지막 seq에
 *               도달할 때까지 기다린다. 도달하면 이번 단계에 보낸 메시지가 모두 상대 손실 계산에 들어간 것이다.
 *   - 판정    : 이번 단계의 미수신 증가분이 0이고, 실제로 목표의 95% 이상 보냈으면 통과.
 *
 * 진행 순서
 *   1. 워밍업: 1,000 msg/s로 연결을 안정화한다. 판정하지 않는다.
 *   2. 탐색  : 500,000 msg/s부터 시작해 통과할 때마다 2배로 올린다. 첫 단계부터 실패하면
 *              절반(250,000, 125,000, …)으로 내려가며 통과하는 값을 찾는다.
 *              실패가 나온 뒤에는 "통과한 최대값"과 "실패한 최소값"의 가운데를 다음 목표로 한다(이분 탐색).
 *              두 값의 차이가 통과값의 4%(최소 100 msg/s) 이하가 되면 탐색 끝.
 *   3. 확인  : 통과한 최대 목표로 15초 동안 다시 보낸다. 손실 없이 통과하면 그 실제 송신량이 결과다.
 *              실패하면 목표를 4%(최소 100 msg/s) 낮춰 다시 확인한다. 최대 5번까지 한다.
 *
 * 이 모듈의 함수는 TX 피드백 스레드만 부른다 (결과 출력은 그 스레드가 끝난 뒤 main이 부른다).
 */
#ifndef PERF_MAX_THROUGHPUT_H
#define PERF_MAX_THROUGHPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PERF_SEARCH_SENDING = 0, /* 목표 송신량으로 보내는 중 */
    PERF_SEARCH_SETTLING,    /* 송신을 멈추고 상대 보고를 기다리는 중 */
    PERF_SEARCH_DONE,        /* 탐색 끝 */
} perf_search_phase_t;

/* 지금 단계가 무엇을 하려는 단계인가 */
typedef enum {
    PERF_STEP_WARMUP = 0, /* 연결 안정화 (판정 안 함) */
    PERF_STEP_SEARCH,     /* 2배 / 이분 탐색 */
    PERF_STEP_CONFIRM,    /* 탐색이 찾은 값을 길게 다시 보내 확인 */
} perf_step_kind_t;

typedef struct {
    perf_search_phase_t phase;
    perf_step_kind_t kind;          /* 지금 단계의 종류 */
    uint64_t send_us;               /* 이번 단계에 보내는 시간 */
    uint32_t step_no;               /* 판정한 탐색 단계 수 (1부터) */
    uint32_t target_rate;           /* 이번 단계 목표 송신량 (msg/s) */
    uint32_t pass_rate;             /* 통과한 탐색 목표 중 최대 (0 = 아직 없음) */
    uint32_t fail_rate;             /* 실패한 탐색 목표 중 최소 (0 = 아직 없음) */
    uint32_t best_actual_rate;      /* 통과한 탐색 단계의 실제 송신량 중 최대 */
    uint32_t confirm_tries;         /* 확인 단계를 몇 번 시작했는가 */
    bool is_confirmed;              /* 확인 단계를 통과했는가 */
    uint32_t confirmed_target_rate; /* 통과한 확인 단계의 목표 송신량 */
    uint32_t confirmed_rate;        /* 통과한 확인 단계의 실제 송신량 = 결과 */
    uint64_t phase_start_us;        /* 지금 phase가 시작된 시각 */
    uint32_t step_start_published;  /* 단계 시작 때 내 송신 성공 수 */
    bool has_step_end;              /* 단계 끝 값을 잡았는가 */
    uint32_t step_end_published;    /* 단계 끝(송신이 완전히 멈춘 뒤) 내 송신 성공 수 */
    uint32_t step_actual_rate;      /* 이번 단계에 실제로 보낸 송신량 (msg/s) */
    uint32_t base_unreceived;       /* 직전 단계 판정 때 상대가 보고한 누적 미수신 수 */
    bool is_sender_limited;         /* 목표만큼 보내지 못해 실패한 단계가 있었는가 */
    bool is_aborted;                /* 상대 보고가 오지 않아 중단했는가 */
} perf_search_t;

/* 탐색을 처음 상태(워밍업 단계 송신 중)로 만든다 */
void perf_search_init(perf_search_t* s, uint64_t now_us);

/* TX 피드백 tick(0.25초)마다 부른다. 내 송신 성공 수와 상대 보고(num0)를 보고 단계를 진행하고,
 * 스케줄러가 지금 보내야 할 송신량(msg/s)을 돌려준다. 0이면 쉰다. */
uint32_t perf_search_step(perf_search_t* s, uint64_t now_us, uint32_t published, bool has_peer_view,
                          uint32_t peer_counted_seq, uint32_t peer_unreceived, const char* my_name);

bool perf_search_is_done(const perf_search_t* s);

/* 탐색 결과 출력 */
void perf_search_print_result(const perf_search_t* s, const char* my_name, const char* peer_name);

#endif /* PERF_MAX_THROUGHPUT_H */
