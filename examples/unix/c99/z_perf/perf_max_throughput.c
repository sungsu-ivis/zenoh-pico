/*
 * 네트워크 성능 측정 예제 - 최대 처리량 탐색 구현
 * 동작 순서는 perf_max_throughput.h 맨 위 설명을 먼저 볼 것.
 */
#include "perf_max_throughput.h"

#include <stdio.h>
#include <string.h>

#include "perf_measure.h"

#define PERF_SEARCH_WARMUP_RATE 1000u                  /* 워밍업 송신량 (msg/s) */
#define PERF_SEARCH_START_RATE 1000u                   /* 첫 탐색 단계 송신량 (msg/s) */
#define PERF_SEARCH_MAX_RATE 1000000u                  /* 이 이상은 올리지 않는다 (msg/s) */
#define PERF_SEARCH_WARMUP_US (3u * 1000000u)          /* 워밍업 단계에 보내는 시간 */
#define PERF_SEARCH_SEND_US (15u * 1000000u)           /* 탐색 단계마다 보내는 시간 */
#define PERF_SEARCH_CONFIRM_US (30u * 1000000u)        /* 확인 단계에 보내는 시간 */
#define PERF_SEARCH_CONFIRM_MAX_TRIES 5u               /* 확인 단계를 이 횟수까지 해 본다 */
#define PERF_SEARCH_SETTLE_TIMEOUT_US (15u * 1000000u) /* 이 시간 안에 상대 보고가 따라오지 않으면 중단 */
#define PERF_SEARCH_PRECISION_PCT 2u                   /* 통과값과 실패값 차이가 통과값의 이 % 이하면 탐색 끝 */
#define PERF_SEARCH_MIN_GAP_RATE 100u                  /* 위 정밀도의 최솟값 (msg/s) */
#define PERF_SEARCH_ACHIEVED_PCT 95u                   /* 실제 송신량이 목표의 이 % 이상이어야 통과 */

/* 송신량(msg/s) → Mbps. user_bytes는 메시지 한 개에서 셀 바이트 수 */
static double perf_rate_to_mbps(uint32_t rate, uint32_t user_bytes) { return (double)rate * user_bytes * 8.0 / 1e6; }

/* 탐색 정밀도: rate의 2% (최소 100 msg/s).
 * 이분 탐색을 끝낼 기준과, 확인 단계가 실패했을 때 목표를 내리는 폭에 같이 쓴다. */
static uint32_t perf_precision_of(uint32_t rate) {
    const uint32_t precision = rate * PERF_SEARCH_PRECISION_PCT / 100u;
    return (precision < PERF_SEARCH_MIN_GAP_RATE) ? PERF_SEARCH_MIN_GAP_RATE : precision;
}

/* 단계 종류별 송신 시간 */
static uint64_t perf_send_time_of(perf_step_kind_t kind) {
    switch (kind) {
        case PERF_STEP_WARMUP:
            return PERF_SEARCH_WARMUP_US;
        case PERF_STEP_CONFIRM:
            return PERF_SEARCH_CONFIRM_US;
        case PERF_STEP_SEARCH:
        default:
            return PERF_SEARCH_SEND_US;
    }
}

/* 출력용 단계 이름 */
static const char* perf_kind_label(perf_step_kind_t kind) {
    switch (kind) {
        case PERF_STEP_WARMUP:
            return "워밍업";
        case PERF_STEP_CONFIRM:
            return "확인";
        case PERF_STEP_SEARCH:
        default:
            return "탐색";
    }
}

/* 새 단계를 시작한다 (보내기 phase) */
static void perf_start_sending(perf_search_t* s, uint64_t now_us, uint32_t published, uint32_t rate,
                          perf_step_kind_t kind) {
    s->phase = PERF_SEARCH_SENDING;
    s->kind = kind;
    s->send_us = perf_send_time_of(kind);
    s->target_rate = rate;
    s->phase_start_us = now_us;
    s->step_start_published = published;
    s->has_step_end = false;
}

/* 확인 단계를 시작한다 */
static void perf_start_confirm(perf_search_t* s, uint64_t now_us, uint32_t published, uint32_t rate) {
    s->confirm_tries++;
    perf_start_sending(s, now_us, published, rate, PERF_STEP_CONFIRM);
}

/* 판정 결과로 다음 탐색 목표를 정한다. 0이면 탐색 끝. */
static uint32_t perf_next_target_rate(const perf_search_t* s) {
    if (s->fail_rate == 0u) {
        /* 아직 실패가 없으면 2배씩 올린다 */
        if (s->pass_rate >= PERF_SEARCH_MAX_RATE) return 0u;
        const uint32_t doubled = s->pass_rate * 2u;
        return (doubled > PERF_SEARCH_MAX_RATE) ? PERF_SEARCH_MAX_RATE : doubled;
    }
    /* 실패가 나온 뒤에는 통과값과 실패값의 가운데로 좁힌다 */
    if (s->fail_rate <= s->pass_rate) return 0u; /* 측정이 흔들려 순서가 뒤집혔으면 여기서 끝 */
    const uint32_t gap = s->fail_rate - s->pass_rate;
    if (gap <= perf_precision_of(s->pass_rate)) return 0u;
    return s->pass_rate + gap / 2u;
}

/* 이번 단계에 목표의 95% 이상을 실제로 보냈는가 */
static bool perf_is_rate_achieved(const perf_search_t* s) {
    return (uint64_t)s->step_actual_rate * 100u >= (uint64_t)s->target_rate * PERF_SEARCH_ACHIEVED_PCT;
}

/* 판정 줄의 공통 뒷부분: 목표, 실제 송신량, 보낸 수, 미수신, 결과 */
static void perf_print_step_result(const perf_search_t* s, uint32_t lost, bool is_pass) {
    const uint32_t sent = s->step_end_published - s->step_start_published;
    const char* verdict = is_pass ? "통과" : ((lost != 0u) ? "실패 (손실)" : "실패 (목표만큼 못 보냄)");
    printf("목표 %7u msg/s | 실제 %7u msg/s (%7.2f Mbps) | 보냄 %8u | 미수신 %6u → %s\n\n", s->target_rate,
           s->step_actual_rate, perf_rate_to_mbps(s->step_actual_rate, PERF_PAYLOAD_BYTES - PERF_HEADER_BYTES), sent, lost,
           verdict);
}

/* 탐색 단계 하나를 판정하고 한 줄 출력한다 */
static void perf_judge_step(perf_search_t* s, uint32_t lost, const char* my_name) {
    const bool is_achieved = perf_is_rate_achieved(s);
    const bool is_pass = (lost == 0u) && is_achieved;

    s->step_no++;
    printf("[%s 탐색] 단계 %2u: ", my_name, s->step_no);
    perf_print_step_result(s, lost, is_pass);

    if (is_pass) {
        if (s->target_rate > s->pass_rate) s->pass_rate = s->target_rate;
        if (s->step_actual_rate > s->best_actual_rate) s->best_actual_rate = s->step_actual_rate;
    } else {
        if ((s->fail_rate == 0u) || (s->target_rate < s->fail_rate)) s->fail_rate = s->target_rate;
        if (!is_achieved) s->is_sender_limited = true;
    }
}

/* 확인 단계를 판정하고 한 줄 출력한다. 통과하면 true */
static bool perf_judge_confirm(perf_search_t* s, uint32_t lost, const char* my_name) {
    const bool is_achieved = perf_is_rate_achieved(s);
    const bool is_pass = (lost == 0u) && is_achieved;

    printf("[%s 확인] %u회째 (%u초): ", my_name, s->confirm_tries, PERF_SEARCH_CONFIRM_US / 1000000u);
    perf_print_step_result(s, lost, is_pass);

    if (is_pass) {
        s->is_confirmed = true;
        s->confirmed_target_rate = s->target_rate;
        s->confirmed_rate = s->step_actual_rate;
    } else if (!is_achieved) {
        s->is_sender_limited = true;
    }
    return is_pass;
}

/* 탐색 단계를 판정한 뒤: 다음 탐색 단계로 가거나, 탐색이 끝났으면 확인 단계를 시작한다 */
static uint32_t perf_finish_search_step(perf_search_t* s, uint64_t now_us, uint32_t published, uint32_t lost,
                                   const char* my_name) {
    perf_judge_step(s, lost, my_name);

    const uint32_t next = perf_next_target_rate(s);
    if (next != 0u) {
        perf_start_sending(s, now_us, published, next, PERF_STEP_SEARCH);
        return s->target_rate;
    }
    if (s->pass_rate == 0u) {
        /* 통과한 단계가 없으면 확인할 값도 없다 */
        s->phase = PERF_SEARCH_DONE;
        return 0u;
    }
    printf("[%s 확인] 탐색 끝 (통과한 최대 목표 %u msg/s). 이 값으로 %u초 동안 다시 보내 확인합니다\n\n", my_name,
           s->pass_rate, PERF_SEARCH_CONFIRM_US / 1000000u);
    perf_start_confirm(s, now_us, published, s->pass_rate);
    return s->target_rate;
}

/* 확인 단계를 판정한 뒤: 통과면 끝, 실패면 목표를 정밀도만큼 낮춰 다시 확인한다 */
static uint32_t perf_finish_confirm_step(perf_search_t* s, uint64_t now_us, uint32_t published, uint32_t lost,
                                    const char* my_name) {
    if (perf_judge_confirm(s, lost, my_name)) {
        s->phase = PERF_SEARCH_DONE;
        return 0u;
    }
    const uint32_t step_down = perf_precision_of(s->target_rate);
    if ((s->confirm_tries >= PERF_SEARCH_CONFIRM_MAX_TRIES) || (s->target_rate <= step_down)) {
        printf("[%s 확인] %u번 모두 통과하지 못해 확인을 멈춥니다\n\n", my_name, s->confirm_tries);
        s->phase = PERF_SEARCH_DONE;
        return 0u;
    }
    perf_start_confirm(s, now_us, published, s->target_rate - step_down);
    return s->target_rate;
}

void perf_search_init(perf_search_t* s, uint64_t now_us) {
    memset(s, 0, sizeof(*s));
    perf_start_sending(s, now_us, 0u, PERF_SEARCH_WARMUP_RATE, PERF_STEP_WARMUP);
}

uint32_t perf_search_step(perf_search_t* s, uint64_t now_us, uint32_t published, bool has_peer_view,
                          uint32_t peer_counted_seq, uint32_t peer_unreceived, const char* my_name) {
    switch (s->phase) {
        case PERF_SEARCH_SENDING: {
            const uint64_t elapsed_us = now_us - s->phase_start_us;
            if (elapsed_us < s->send_us) return s->target_rate;
            /* 보내기 끝 → 송신을 멈추고 기다리기로. 실제 송신량은 지금까지 보낸 수로 계산한다 */
            s->step_actual_rate = (uint32_t)((uint64_t)(published - s->step_start_published) * 1000000u / elapsed_us);
            s->phase = PERF_SEARCH_SETTLING;
            s->phase_start_us = now_us;
            return 0u;
        }

        case PERF_SEARCH_SETTLING: {
            /* 송신 멈춤 지시 후 1초가 지나 스케줄러가 확실히 멈췄을 때의 값을 이번 단계의 끝으로 잡는다 */
            if (!s->has_step_end) {
                s->has_step_end = true;
                s->step_end_published = published;
                return 0u;
            }
            /* 상대 보고가 이번 단계의 마지막 seq까지 반영될 때까지 기다린다 */
            if (!has_peer_view || (peer_counted_seq < s->step_end_published)) {
                if (now_us - s->phase_start_us > PERF_SEARCH_SETTLE_TIMEOUT_US) {
                    printf("[%s %s] 상대 보고가 %u초 동안 따라오지 않아 탐색을 중단합니다\n\n", my_name,
                           perf_kind_label(s->kind), PERF_SEARCH_SETTLE_TIMEOUT_US / 1000000u);
                    s->is_aborted = true;
                    s->phase = PERF_SEARCH_DONE;
                }
                return 0u;
            }

            /* 이번 단계 손실 = 상대 누적 미수신 수의 증가분 */
            const uint32_t lost = (peer_unreceived > s->base_unreceived) ? peer_unreceived - s->base_unreceived : 0u;
            s->base_unreceived = peer_unreceived;

            switch (s->kind) {
                case PERF_STEP_WARMUP:
                    printf("[%s 탐색] 워밍업 끝 (연결 안정화). 최대 처리량 탐색을 시작합니다 (단계마다 %u초)\n\n",
                           my_name, PERF_SEARCH_SEND_US / 1000000u);
                    perf_start_sending(s, now_us, published, PERF_SEARCH_START_RATE, PERF_STEP_SEARCH);
                    return s->target_rate;
                case PERF_STEP_CONFIRM:
                    return perf_finish_confirm_step(s, now_us, published, lost, my_name);
                case PERF_STEP_SEARCH:
                default:
                    return perf_finish_search_step(s, now_us, published, lost, my_name);
            }
        }

        case PERF_SEARCH_DONE:
        default:
            return 0u;
    }
}

bool perf_search_is_done(const perf_search_t* s) { return s->phase == PERF_SEARCH_DONE; }

void perf_search_print_result(const perf_search_t* s, const char* my_name, const char* peer_name) {
    printf("\n===== 최대 처리량 (손실 0%%, 메시지 %u B, %s→%s) =====\n", PERF_PAYLOAD_BYTES, my_name, peer_name);
    if (s->is_aborted) printf("  (상대 보고가 끊겨 중간에 중단됨. 아래는 그때까지의 결과)\n");
    if (s->best_actual_rate == 0u) {
        printf("  손실 없이 통과한 단계가 없습니다.\n");
        return;
    }

    /* 결과: 확인 단계를 통과했으면 그 실제 송신량, 아니면 탐색 단계에서 통과한 최대값 */
    const uint32_t result = s->is_confirmed ? s->confirmed_rate : s->best_actual_rate;
    const char* note = "";
    if (!s->is_confirmed) {
        note = (s->confirm_tries > 0u) ? "  (확인 단계를 통과하지 못해 탐색 결과를 표시)"
                                       : "  (탐색이 끝나기 전에 멈춰 그때까지의 탐색 결과를 표시)";
    }
    printf("  결과        : %u msg/s%s\n", result, note);
    printf("                %.2f Mbps (사용자 데이터 %u B 기준)\n",
           perf_rate_to_mbps(result, PERF_PAYLOAD_BYTES - PERF_HEADER_BYTES), PERF_PAYLOAD_BYTES - PERF_HEADER_BYTES);
    printf("                %.2f Mbps (메시지 %u B 기준)\n", perf_rate_to_mbps(result, PERF_PAYLOAD_BYTES), PERF_PAYLOAD_BYTES);
    if (s->fail_rate == 0u) {
        printf("  탐색        : 통과한 최대 목표 %u msg/s, 실패한 단계 없음 (%u단계, 단계마다 %u초)\n", s->pass_rate,
               s->step_no, PERF_SEARCH_SEND_US / 1000000u);
    } else {
        printf("  탐색        : 통과한 최대 목표 %u msg/s, 실패한 최소 목표 %u msg/s (%u단계, 단계마다 %u초)\n",
               s->pass_rate, s->fail_rate, s->step_no, PERF_SEARCH_SEND_US / 1000000u);
    }
    if (s->is_confirmed) {
        printf("  확인        : 목표 %u msg/s로 %u초 동안 손실 없음 (%u회째 통과)\n", s->confirmed_target_rate,
               PERF_SEARCH_CONFIRM_US / 1000000u, s->confirm_tries);
    } else if (s->confirm_tries > 0u) {
        printf("  확인        : %u번 시도했지만 %u초 동안 손실 없이 보내지 못함\n", s->confirm_tries,
               PERF_SEARCH_CONFIRM_US / 1000000u);
    }
    if (s->is_sender_limited) {
        printf("  참고        : 목표만큼 보내지 못한 단계가 있었습니다. 이 값은 링크·수신 측이 아니라\n"
               "                송신 측(CPU, zenoh 송신 처리)의 한계일 수 있습니다.\n");
    }
}
