/*
 * CPU 부하 측정 예제 (zenoh-pico) - 측정 모듈 구현
 * 전체 구조는 cpu_load_measure.h 맨 위 설명을 먼저 볼 것.
 * brightonz.foundation 예제와 다른 곳은 통신 부분뿐이다: payload 읽기, put/flush, 콜백 등록, 선언/해제, MT0 루프.
 */
#include "cpu_load_measure.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CPULOAD_FEEDBACK_PERIOD_MS 250u /* count·status를 주고받고 단계를 진행하는 주기 */
#define CPULOAD_TICKS_PER_SECOND (1000u / CPULOAD_FEEDBACK_PERIOD_MS)
#define CPULOAD_PEER_IDLE_STOP_US (5u * 1000000u) /* 받는 쪽: count가 이만큼 끊기면 상대가 끝난 것 */
/* 받는 쪽: 시작한 뒤 이만큼 count가 한 번도 오지 않으면 기다리기를 멈춘다 (짝이 어긋난 회차가 끝나지 않게) */
#define CPULOAD_PEER_FIRST_COUNT_TIMEOUT_US (120u * 1000000u)
#define CPULOAD_START_TIMEOUT_S 15u /* 보내는 쪽: 워밍업 뒤 받는 쪽 보고를 기다리는 시간 */
#define CPULOAD_STATUS_STALL_US (5u * 1000000u) /* 보내는 쪽: 측정 중 받는 쪽 보고가 이만큼 끊기면 멈춘다 */
#define CPULOAD_MIN_ACHIEVED_PCT 99u            /* 실제 송신량이 목표의 이 % 이상이어야 유효 */
#define CPULOAD_MAX_LOSS_PPM 10000u             /* 미수신이 이 ppm(1%) 이하여야 유효 */
#define CPULOAD_COUNT_CHECK_EVERY 64u           /* 송신 중에도 이만큼 보낼 때마다 count 시각을 본다 */

/* ===================================================================
 * 시간
 * =================================================================== */

/* 지금 시각 (CLOCK_MONOTONIC, µs) */
static uint64_t cpuload_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

#if Z_FEATURE_MULTI_THREAD == 1 /* 잠드는 함수는 스레드가 있는 MT1에서만 쓴다 */
static void cpuload_timespec_add_ms(struct timespec* t, long ms) {
    t->tv_nsec += ms * 1000000L;
    while (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec += 1;
    }
}

/* 절대 시각 t까지 잔다. 시그널로 깨면 다시 잔다. */
static void cpuload_sleep_until(const struct timespec* t) {
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t, NULL) == EINTR) {
    }
}

static void cpuload_sleep_ms(long ms) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    cpuload_timespec_add_ms(&t, ms);
    cpuload_sleep_until(&t);
}
#endif

/* ===================================================================
 * 바이트 읽기/쓰기 (little-endian). 각 함수는 쓰거나 읽은 다음 위치를 돌려준다.
 * =================================================================== */

static uint8_t* cpuload_put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
    return p + 4;
}

static const uint8_t* cpuload_get_u32(const uint8_t* p, uint32_t* v) {
    *v = 0u;
    for (int i = 0; i < 4; i++) *v |= (uint32_t)p[i] << (8 * i);
    return p + 4;
}

/* 샘플 payload의 앞부분을 buffer에 최대 capacity 바이트 복사하고, payload 전체 길이를 돌려준다 */
static size_t cpuload_read_payload(const z_loaned_sample_t* sample, uint8_t* buffer, size_t capacity) {
    const z_loaned_bytes_t* payload = z_sample_payload(sample);
    z_bytes_reader_t reader = z_bytes_get_reader(payload);
    (void)z_bytes_reader_read(&reader, buffer, capacity);
    return z_bytes_len(payload);
}

/* report → status 메시지. 끝 위치를 돌려준다 (node_open이 크기를 확인한다) */
static uint8_t* cpuload_put_report(uint8_t* p, const cpuload_report_t* r) {
    p = cpuload_put_u32(p, r->received);
    p = cpuload_put_u32(p, r->unreceived);
    p = cpuload_put_u32(p, r->counted_seq);
    p = cpuload_put_u32(p, r->out_of_order);
    p = cpuload_put_u32(p, r->cpu.busy_ms);
    return cpuload_put_u32(p, r->cpu.elapsed_ms);
}

/* status 메시지 → report */
static void cpuload_get_report(const uint8_t* p, cpuload_report_t* r) {
    p = cpuload_get_u32(p, &r->received);
    p = cpuload_get_u32(p, &r->unreceived);
    p = cpuload_get_u32(p, &r->counted_seq);
    p = cpuload_get_u32(p, &r->out_of_order);
    p = cpuload_get_u32(p, &r->cpu.busy_ms);
    (void)cpuload_get_u32(p, &r->cpu.elapsed_ms);
}

/* ===================================================================
 * 작은 보조 함수
 * =================================================================== */

static bool cpuload_flag_get(cpuload_atomic_bool_t* flag) { return __atomic_load_n(&flag->value, __ATOMIC_ACQUIRE); }

static void cpuload_flag_set(cpuload_atomic_bool_t* flag, bool value) {
    __atomic_store_n(&flag->value, value, __ATOMIC_RELEASE);
}

static void cpuload_counter_add(cpuload_atomic_u32_t* counter) {
    (void)__atomic_fetch_add(&counter->value, 1u, __ATOMIC_RELEASE);
}

static uint32_t cpuload_counter_get(const cpuload_atomic_u32_t* counter) {
    return __atomic_load_n(&counter->value, __ATOMIC_ACQUIRE);
}

/* ===================================================================
 * 송신: 스케줄러 (1 ms 주기, 보내는 쪽만)
 * =================================================================== */

/* batch에 모인 메시지를 링크로 내보낸다 (num0은 express가 아니라 batch에 모였다가 flush 때 데이터그램 하나로 나간다).
 * batch가 가득 차면 zenoh-pico가 put 안에서 스스로 내보내므로, 여기서는 덜 찬 batch만 내보내면 된다 */
static void cpuload_flush_batch(cpuload_node_t* node) { (void)zp_batch_flush(z_session_loan(node->session)); }

/* put 한 번. 보낼 데이터를 복사하지 않고 감싸기만 해서(z_bytes_from_static_buf) 넘긴다.
 * z_publisher_put은 반환하기 전에 데이터를 batch로 인코딩하므로 호출하는 쪽 버퍼로 충분하다.
 * congestion control이 BLOCK이라 batch가 가득 차면 내보낸 뒤 이어서 넣는다 (brightonz.foundation의 BUSY 재시도에 해당).
 * express 메시지(count) 앞에 아직 나가지 않은 데이터가 있으면, zenoh-pico가 그 데이터와 함께 바로 내보낸다. */
static z_result_t cpuload_put(const z_owned_publisher_t* publisher, const uint8_t* data, size_t len) {
    z_owned_bytes_t payload;
    const z_result_t result = z_bytes_from_static_buf(&payload, data, len);
    if (result != Z_OK) return result;
    return z_publisher_put(z_publisher_loan(publisher), z_bytes_move(&payload), NULL);
}

/* num0을 한 번 보낸다. [seq][채움]. 받아들여졌으면 true */
static bool cpuload_send_message(cpuload_node_t* node) {
    uint8_t payload[CPULOAD_PAYLOAD_BYTES];
    uint8_t* p = cpuload_put_u32(payload, node->next_seq);
    memset(p, 'A', sizeof(payload) - 4u); /* 내용은 의미 없음 */
    if (cpuload_put(&node->msg_publisher, payload, sizeof(payload)) != Z_OK) return false;
    cpuload_counter_add(&node->published);
    node->next_seq++;
    return true;
}

/* count를 보낸다: [지금까지 보낸 마지막 seq].
 * 데이터를 보내는 스케줄러가 직접 보내므로 그 seq까지의 데이터가 먼저 나간다 */
static void cpuload_send_count(cpuload_node_t* node) {
    uint8_t message[CPULOAD_COUNT_BYTES];
    (void)cpuload_put_u32(message, cpuload_counter_get(&node->published));
    (void)cpuload_put(&node->count_publisher, message, sizeof(message));
}

/* count를 보낼 때가 됐으면 보낸다 (실제 시간 0.25초마다) */
static void cpuload_send_count_if_due(cpuload_node_t* node) {
    const uint64_t period_us = (uint64_t)CPULOAD_FEEDBACK_PERIOD_MS * 1000u;
    const uint64_t now = cpuload_now_us();
    if (node->next_count_us == 0u) node->next_count_us = now + period_us;
    if (now < node->next_count_us) return;
    cpuload_send_count(node);
    node->next_count_us += period_us;
    if (node->next_count_us <= now) node->next_count_us = now + period_us; /* 많이 밀렸으면 몰아서 보내지 않는다 */
}

/* 스케줄러 tick 한 번 (1 ms). rate_credit은 1 ms마다 송신량만큼 쌓고, 1000이 모일 때마다 1개 보낸다.
 * put이 실패하면 이 구간의 나머지는 건너뛴다.
 * 남은 양은 2 ms 분량까지만 다음 구간으로 넘겨, 막혔다 풀릴 때 한꺼번에 몰리지 않게 한다. */
static void cpuload_scheduler_tick(cpuload_node_t* node, uint64_t* rate_credit) {
    const uint64_t rate = node->options.rate;
    *rate_credit += rate;
    if (*rate_credit > 2u * rate) *rate_credit = 2u * rate;
    uint32_t sent_in_tick = 0u;
    while (*rate_credit >= 1000u) {
        if (!cpuload_send_message(node)) {
            cpuload_counter_add(&node->skipped_ticks);
            break;
        }
        *rate_credit -= 1000u;
        if ((++sent_in_tick % CPULOAD_COUNT_CHECK_EVERY) == 0u) cpuload_send_count_if_due(node);
    }
    cpuload_flush_batch(node); /* 메시지가 batch에서 기다리는 시간은 1 ms를 넘지 않는다 */
    cpuload_send_count_if_due(node);
}

/* ===================================================================
 * 수신 콜백 (subscriber마다 하나, context는 cpuload_node_t*)
 * (MT1: zenoh-pico executor 스레드, MT0: 메인 스레드의 zp_spin_once 안에서 불린다)
 * =================================================================== */

static void cpuload_on_message(z_loaned_sample_t* sample, void* context) {
    cpuload_node_t* node = (cpuload_node_t*)context;
    if ((node == NULL) || (sample == NULL) || !cpuload_flag_get(&node->is_ready)) return;
    uint8_t header[4];
    if (cpuload_read_payload(sample, header, sizeof(header)) < sizeof(header)) return;
    uint32_t seq;
    (void)cpuload_get_u32(header, &seq);

    pthread_mutex_lock(&node->state_lock);
    if (!node->has_first) {
        node->has_first = true;
        node->first_seq = seq;
        node->last_seq = seq;
        node->received++;
    } else if (seq > node->last_seq) {
        node->last_seq = seq;
        node->received++;
    } else {
        /* 앞 seq 이하: 순서 바뀜 또는 중복. 받은 수에 더하면 빠진 메시지를 가리므로 받지 않은 것으로 센다 */
        node->out_of_order++;
    }
    pthread_mutex_unlock(&node->state_lock);
}

static void cpuload_on_count(z_loaned_sample_t* sample, void* context) {
    cpuload_node_t* node = (cpuload_node_t*)context;
    if ((node == NULL) || (sample == NULL) || !cpuload_flag_get(&node->is_ready)) return;
    uint8_t data[CPULOAD_COUNT_BYTES];
    if (cpuload_read_payload(sample, data, sizeof(data)) != sizeof(data)) return;
    uint32_t seq;
    (void)cpuload_get_u32(data, &seq);

    pthread_mutex_lock(&node->state_lock);
    node->count_mail_seq = seq;
    node->has_count_mail = true;
    node->last_count_us = cpuload_now_us();
    node->received_at_count = node->received;
    pthread_mutex_unlock(&node->state_lock);
}

static void cpuload_on_status(z_loaned_sample_t* sample, void* context) {
    cpuload_node_t* node = (cpuload_node_t*)context;
    if ((node == NULL) || (sample == NULL) || !cpuload_flag_get(&node->is_ready)) return;
    uint8_t data[CPULOAD_STATUS_BYTES];
    const size_t length = cpuload_read_payload(sample, data, sizeof(data));

    pthread_mutex_lock(&node->state_lock);
    if (length == sizeof(data)) {
        memcpy(node->status_mail, data, sizeof(data));
        node->has_status_mail = true;
    } else {
        node->wrong_status_bytes = length; /* 크기가 다르면 버린다. 보내는 쪽 피드백이 한 번 알린다 */
    }
    pthread_mutex_unlock(&node->state_lock);
}

/* ===================================================================
 * 피드백 (0.25초): count·status 처리, status 송신(받는 쪽), 측정 구간(보내는 쪽), 1초마다 한 줄 출력
 * =================================================================== */

/* 상대 count로 미수신 수를 계산한다 (state_lock 안에서).
 *   기대 수신 수 = 상대가 보낸 마지막 seq − 내가 처음 받은 seq + 1
 *   미수신 수   = 기대 수신 수 − count가 도착한 순간의 내 수신 수 */
static void cpuload_update_loss(cpuload_node_t* node, uint32_t peer_published_seq) {
    if (!node->has_first || (peer_published_seq < node->first_seq)) return; /* 아직 받은 것이 없음 */
    node->report.counted_seq = peer_published_seq;
    const uint32_t expected = peer_published_seq - node->first_seq + 1u;
    node->report.unreceived = (expected > node->received_at_count) ? expected - node->received_at_count : 0u;
}

/* 보내는 쪽: 측정을 멈춘다 */
static void cpuload_abort(cpuload_node_t* node, const char* reason) {
    node->is_aborted = true;
    node->abort_reason = reason;
    node->phase = CPULOAD_PHASE_DONE;
    cpuload_flag_set(&node->should_stop_publish, true);
    cpuload_flag_set(&node->is_done, true);
    printf("[%s 측정] %s. 측정을 멈춥니다\n\n", node->my_name, reason);
}

/* 보내는 쪽: 워밍업 → 측정 → 마무리 */
static void cpuload_advance_phase(cpuload_node_t* node, uint64_t now, uint32_t published, bool has_peer_view,
                                  const cpuload_report_t* peer, uint64_t last_status_us) {
    const cpuload_options_t* o = &node->options;
    const uint64_t elapsed_us = now - node->phase_start_us;
    switch (node->phase) {
        case CPULOAD_PHASE_WARMUP:
            if ((elapsed_us >= (uint64_t)o->warmup_s * 1000000u) && has_peer_view && (peer->received != 0u)) {
                node->window_start_us = now;
                node->window_start_published = published;
                node->window_start_skipped = cpuload_counter_get(&node->skipped_ticks);
                node->window_start_mine = cpuload_meter_read(&node->meter);
                node->window_start_peer = peer->cpu;
                node->phase = CPULOAD_PHASE_MEASURE;
                node->phase_start_us = now;
                printf("[%s 측정] 워밍업 끝. 지금부터 %u초 동안 두 PC의 CPU를 잽니다\n\n", node->my_name, o->measure_s);
            } else if (elapsed_us >= (uint64_t)(o->warmup_s + CPULOAD_START_TIMEOUT_S) * 1000000u) {
                cpuload_abort(node, "받는 쪽 보고가 오지 않음 (받는 쪽을 먼저 띄웠는지 확인)");
            }
            break;
        case CPULOAD_PHASE_MEASURE:
            if (now - last_status_us > CPULOAD_STATUS_STALL_US) {
                cpuload_abort(node, "측정 중에 받는 쪽 보고가 끊김");
            } else if (elapsed_us >= (uint64_t)o->measure_s * 1000000u) {
                node->window_end_us = now;
                node->window_end_published = published;
                node->window_end_skipped = cpuload_counter_get(&node->skipped_ticks);
                node->window_end_mine = cpuload_meter_read(&node->meter);
                node->window_end_peer = peer->cpu;
                node->phase = CPULOAD_PHASE_DRAIN;
                node->phase_start_us = now;
                cpuload_flag_set(&node->should_stop_publish, true); /* 스케줄러가 마지막 count를 보내고 멈춘다 */
            }
            break;
        case CPULOAD_PHASE_DRAIN:
            if (elapsed_us >= (uint64_t)o->drain_s * 1000000u) {
                node->phase = CPULOAD_PHASE_DONE;
                cpuload_flag_set(&node->is_done, true);
            }
            break;
        case CPULOAD_PHASE_DONE:
        default:
            break;
    }
}

static const char* cpuload_phase_label(cpuload_phase_t phase) {
    switch (phase) {
        case CPULOAD_PHASE_WARMUP:
            return "워밍업";
        case CPULOAD_PHASE_MEASURE:
            return "측정";
        case CPULOAD_PHASE_DRAIN:
        case CPULOAD_PHASE_DONE:
        default:
            return "마무리";
    }
}

static void cpuload_feedback_tick(cpuload_node_t* node) {
    const uint64_t now = cpuload_now_us();
    const bool is_second_tick = (node->feedback_ticks++ % CPULOAD_TICKS_PER_SECOND) == 0u;
    const uint32_t published = cpuload_counter_get(&node->published);

    /* (1) 받은 count·status를 꺼낸다 */
    pthread_mutex_lock(&node->state_lock);
    if (node->has_count_mail) {
        /* count의 seq는 한 실행 안에서 줄지 않는다. 줄었으면 보내는 쪽이 다시 시작된 것이다 */
        if (node->has_count_seq && (node->count_mail_seq < node->last_count_seq)) {
            node->is_sender_restarted = true;
        } else if (!node->is_sender_restarted) {
            node->has_count_seq = true;
            node->last_count_seq = node->count_mail_seq;
            cpuload_update_loss(node, node->count_mail_seq);
        }
        node->has_count_mail = false;
    }
    if (node->has_status_mail) {
        cpuload_report_t report;
        cpuload_get_report(node->status_mail, &report);
        /* 내가 보낸 것보다 큰 seq를 반영했다는 보고는 다른 실행(이전 회차)의 보내는 쪽에 대한 것이다 */
        if (report.counted_seq <= published) {
            node->peer_view = report;
            node->has_peer_view = true;
            node->last_status_us = now;
        } else {
            node->foreign_status++;
        }
        node->has_status_mail = false;
    }
    cpuload_report_t mine = node->report;
    mine.received = node->received;
    mine.out_of_order = node->out_of_order;
    const cpuload_report_t peer = node->peer_view;
    const bool has_peer_view = node->has_peer_view;
    const uint64_t last_status_us = node->last_status_us;
    const bool is_sender_restarted = node->is_sender_restarted;
    const uint32_t foreign_status = node->foreign_status;
    const size_t wrong_status_bytes = node->wrong_status_bytes;
    pthread_mutex_unlock(&node->state_lock);

    /* (2) 받는 쪽: status = [수신 결과, 누적 CPU 시간]. 보내는 쪽이 다시 시작된 뒤에는 보내지 않는다 */
    if (!node->options.is_publisher && !is_sender_restarted) {
        uint8_t message[CPULOAD_STATUS_BYTES];
        mine.cpu = cpuload_meter_read(&node->meter);
        (void)cpuload_put_report(message, &mine);
        (void)cpuload_put(&node->status_publisher, message, sizeof(message));
    }

    /* (3) 보내는 쪽: 받는 쪽 보고를 버리고 있으면 원인을 한 번 알리고, 측정 단계를 진행한다 */
    if (node->options.is_publisher) {
        if (!node->is_status_warned && ((wrong_status_bytes != 0u) || (foreign_status != 0u))) {
            node->is_status_warned = true;
            if (wrong_status_bytes != 0u) {
                printf(
                    "[%s] 받는 쪽 status가 %zu B라 버립니다 (이 프로그램은 %u B). 두 PC를 같은 코드로 빌드하세요\n\n",
                    node->my_name, wrong_status_bytes, (unsigned)CPULOAD_STATUS_BYTES);
            } else {
                printf(
                    "[%s] 이 실행이 보낸 것보다 큰 seq를 반영한 보고를 버립니다. 다른 실행의 받는 쪽이 남아 "
                    "있습니다\n\n",
                    node->my_name);
            }
        }
        cpuload_advance_phase(node, now, published, has_peer_view, &peer, last_status_us);
    }

    /* (4) 1초마다 한 줄: 초당 송신·수신 수와 이 코어의 1초 부하 */
    if (!is_second_tick) return;
    const cpuload_usage_t cpu_now = cpuload_meter_read(&node->meter);
    const double load_pct = cpuload_usage_load_pct(node->last_second_cpu, cpu_now);
    node->last_second_cpu = cpu_now;
    const unsigned elapsed_s = ++node->print_seconds;
    if (node->options.is_publisher) {
        printf("[%s %3us] 송신 %s→%s %7u msg/s | %s %5.1f%% | %s\n", node->my_name, elapsed_s, node->my_name,
               node->peer_name, published - node->last_second_published, node->meter.label, load_pct,
               cpuload_phase_label(node->phase));
        node->last_second_published = published;
    } else {
        printf("[%s %3us] 수신 %s→%s %7u msg/s | %s %5.1f%%\n", node->my_name, elapsed_s, node->peer_name,
               node->my_name, mine.received - node->last_second_received, node->meter.label, load_pct);
        node->last_second_received = mine.received;
    }
    fflush(stdout);
}

/* 끝낼 때인가
 *   - 보내는 쪽: 측정을 마쳤거나 멈췄으면
 *   - 받는 쪽  : 상대 count가 5초 동안 끊기면(상대가 끝남), 보내는 쪽이 다시 시작됐으면,
 *                처음 count가 120초 동안 오지 않으면 */
static bool cpuload_is_finished(cpuload_node_t* node) {
    if (node->options.is_publisher) return cpuload_flag_get(&node->is_done);
    pthread_mutex_lock(&node->state_lock);
    const uint64_t last_count_us = node->last_count_us;
    const bool is_sender_restarted = node->is_sender_restarted;
    pthread_mutex_unlock(&node->state_lock);
    if (is_sender_restarted) return true;
    const uint64_t now = cpuload_now_us();
    if (last_count_us == 0u) {
        if (now - node->start_us < CPULOAD_PEER_FIRST_COUNT_TIMEOUT_US) return false;
        node->is_peer_never_seen = true;
        return true;
    }
    return now - last_count_us >= CPULOAD_PEER_IDLE_STOP_US;
}

#if Z_FEATURE_MULTI_THREAD == 1
/* ===================================================================
 * MT1: 스케줄러·피드백 스레드
 * =================================================================== */

static void* cpuload_scheduler_main(void* arg) {
    cpuload_node_t* node = (cpuload_node_t*)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t rate_credit = 0u;
    while (!cpuload_flag_get(&node->should_stop_publish)) {
        cpuload_scheduler_tick(node, &rate_credit);
        /* 다음 깨어날 시각 = 이전 목표 + 1 ms ("지금 + 1 ms"로 하면 처리 시간만큼 밀린다) */
        cpuload_timespec_add_ms(&next, 1);
        cpuload_sleep_until(&next);
    }
    cpuload_send_count(node); /* 마지막 count: 받는 쪽이 최종 미수신 수를 정할 수 있게 */
    return NULL;
}

/* 피드백 스레드: 다음 1초 경계부터 0.25초마다 */
static void* cpuload_feedback_main(void* arg) {
    cpuload_node_t* node = (cpuload_node_t*)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    next.tv_sec += 1;
    next.tv_nsec = 0;
    while (!cpuload_flag_get(&node->should_stop_feedback)) {
        cpuload_sleep_until(&next);
        if (!cpuload_flag_get(&node->should_stop_feedback)) cpuload_feedback_tick(node);
        cpuload_timespec_add_ms(&next, (long)CPULOAD_FEEDBACK_PERIOD_MS);
    }
    return NULL;
}

/* 스레드를 모두 만들었으면 true. 하나라도 못 만들면 만든 것만 멈춰 정리하고 false */
static bool cpuload_run_threads(cpuload_node_t* node, volatile sig_atomic_t* is_stop_requested) {
    const bool has_feedback = pthread_create(&node->feedback_thread, NULL, cpuload_feedback_main, node) == 0;
    const bool has_scheduler = node->options.is_publisher &&
                               (pthread_create(&node->scheduler_thread, NULL, cpuload_scheduler_main, node) == 0);
    const bool is_started = has_feedback && (has_scheduler || !node->options.is_publisher);
    if (!is_started) fprintf(stderr, "측정 스레드를 만들지 못했습니다\n");

    while (is_started && (*is_stop_requested == 0) && !cpuload_is_finished(node)) cpuload_sleep_ms(100);

    cpuload_flag_set(&node->should_stop_publish, true);
    if (has_scheduler) pthread_join(node->scheduler_thread, NULL);
    cpuload_flag_set(&node->should_stop_feedback, true);
    if (has_feedback) pthread_join(node->feedback_thread, NULL);
    return is_started;
}
#else
/* ===================================================================
 * MT0: 메인 스레드 하나가 zp_spin_once·스케줄러·피드백을 같은 시각표로 되풀이한다.
 * zp_spin_once는 라이브러리 태스크 하나(수신·콜백 호출, keep-alive·JOIN 송신, TCP accept)를 돌린다.
 * 수신은 받을 것이 없으면 소켓 timeout(Z_CONFIG_SOCKET_TIMEOUT, MT0 빌드는 1 ms)만큼 소켓에서 기다리므로
 * 따로 쉬지 않는다. 커널이 tick 단위로 올려 실제로는 1~10 ms 기다릴 수 있고, 그동안 밀린 송신 tick은
 * 이어서 돌아 평균 송신량을 목표에 맞춘다.
 * =================================================================== */
static void cpuload_run_single_thread(cpuload_node_t* node, volatile sig_atomic_t* is_stop_requested) {
    const z_loaned_session_t* session = z_session_loan(node->session);
    const uint64_t feedback_period_us = (uint64_t)CPULOAD_FEEDBACK_PERIOD_MS * 1000u;
    uint64_t now = cpuload_now_us();
    uint64_t next_schedule_us = now;
    uint64_t next_feedback_us = (now / 1000000u + 1u) * 1000000u; /* 다음 1초 경계 */
    uint64_t next_finish_check_us = now + 100000u;                /* 0.1초마다 끝낼 때인지 본다 */
    uint64_t rate_credit = 0u;
    bool is_publishing = node->options.is_publisher;

    for (;;) {
        (void)zp_spin_once(session);
        now = cpuload_now_us();
        if (is_publishing && cpuload_flag_get(&node->should_stop_publish)) {
            cpuload_send_count(node); /* 마지막 count */
            is_publishing = false;
        }
        while (is_publishing && (now >= next_schedule_us)) {
            cpuload_scheduler_tick(node, &rate_credit);
            next_schedule_us += 1000u; /* 이전 목표 + 1 ms */
        }
        if (now >= next_feedback_us) {
            cpuload_feedback_tick(node);
            next_feedback_us += feedback_period_us;
        }
        if (now >= next_finish_check_us) {
            next_finish_check_us += 100000u;
            if ((*is_stop_requested != 0) || cpuload_is_finished(node)) break;
        }
    }
}
#endif

/* ===================================================================
 * 시작 / 종료 / 결과
 * =================================================================== */

/* publisher 하나를 선언한다. QoS는 brightonz.foundation 예제의 JSON과 같다 (reliable 기본값, BLOCK, priority 기본). */
static bool cpuload_declare_publisher(cpuload_node_t* node, z_owned_publisher_t* publisher, const char* key,
                                      bool is_express) {
    z_view_keyexpr_t keyexpr;
    if (z_view_keyexpr_from_str(&keyexpr, key) != Z_OK) return false;
    z_publisher_options_t options;
    z_publisher_options_default(&options);
    options.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
    options.is_express = is_express;
    return z_declare_publisher(z_session_loan(node->session), publisher, z_view_keyexpr_loan(&keyexpr), &options) ==
           Z_OK;
}

/* subscriber 하나를 선언한다. 콜백의 context는 node다. */
static bool cpuload_declare_subscriber(cpuload_node_t* node, z_owned_subscriber_t* subscriber, const char* key,
                                       z_closure_sample_callback_t callback) {
    z_view_keyexpr_t keyexpr;
    if (z_view_keyexpr_from_str(&keyexpr, key) != Z_OK) return false;
    z_owned_closure_sample_t closure;
    if (z_closure_sample(&closure, callback, NULL, node) != Z_OK) return false;
    return z_declare_subscriber(z_session_loan(node->session), subscriber, z_view_keyexpr_loan(&keyexpr),
                                z_closure_sample_move(&closure), NULL) == Z_OK;
}

/* 선언한 것을 모두 해제한다 (선언하지 않은 것은 null이라 drop해도 된다). 보내는 쪽은 batch도 끈다. */
static void cpuload_node_close(cpuload_node_t* node) {
    if (node->options.is_publisher) (void)zp_batch_stop(z_session_loan(node->session));
    z_subscriber_drop(z_subscriber_move(&node->msg_subscriber));
    z_subscriber_drop(z_subscriber_move(&node->count_subscriber));
    z_subscriber_drop(z_subscriber_move(&node->status_subscriber));
    z_publisher_drop(z_publisher_move(&node->msg_publisher));
    z_publisher_drop(z_publisher_move(&node->count_publisher));
    z_publisher_drop(z_publisher_move(&node->status_publisher));
}

/* publisher/subscriber를 선언하고 상태를 초기화한다.
 * key는 brightonz.foundation 예제의 JSON과 같다. 선언할 때 key를 복사하므로 key 버퍼는 다시 써도 된다. */
static bool cpuload_node_open(cpuload_node_t* node, const z_owned_session_t* session, const char* my_name,
                              const char* peer_name, const cpuload_options_t* options) {
    char key[64];
    /* status 크기(CPULOAD_STATUS_BYTES)가 put_report가 쓰는 크기와 같은가 */
    {
        uint8_t probe[CPULOAD_STATUS_BYTES + 16u];
        cpuload_report_t zero;
        memset(&zero, 0, sizeof(zero));
        const size_t written = (size_t)(cpuload_put_report(probe, &zero) - probe);
        if (written != CPULOAD_STATUS_BYTES) {
            fprintf(stderr, "status 크기가 맞지 않습니다: %zu B를 쓰는데 CPULOAD_STATUS_BYTES는 %u B\n", written,
                    (unsigned)CPULOAD_STATUS_BYTES);
            return false;
        }
    }
    node->my_name = my_name;
    node->peer_name = peer_name;
    node->options = *options;
    node->session = session;
    z_internal_publisher_null(&node->msg_publisher);
    z_internal_publisher_null(&node->count_publisher);
    z_internal_publisher_null(&node->status_publisher);
    z_internal_subscriber_null(&node->msg_subscriber);
    z_internal_subscriber_null(&node->count_subscriber);
    z_internal_subscriber_null(&node->status_subscriber);
    pthread_mutex_init(&node->state_lock, NULL);

    snprintf(key, sizeof(key), "cpu/msg/%s/%s/num0", my_name, peer_name); /* 내가 보내는 것 */
    if (!cpuload_declare_publisher(node, &node->msg_publisher, key, false)) goto failed;
    snprintf(key, sizeof(key), "cpu/msg/%s/%s/num0", peer_name, my_name); /* 내가 받는 것 */
    if (!cpuload_declare_subscriber(node, &node->msg_subscriber, key, cpuload_on_message)) goto failed;
    snprintf(key, sizeof(key), "cpu/report/%s/%s/count", my_name, peer_name);
    if (!cpuload_declare_publisher(node, &node->count_publisher, key, true)) goto failed;
    snprintf(key, sizeof(key), "cpu/report/%s/%s/status", my_name, peer_name);
    if (!cpuload_declare_publisher(node, &node->status_publisher, key, true)) goto failed;
    snprintf(key, sizeof(key), "cpu/report/%s/%s/count", peer_name, my_name);
    if (!cpuload_declare_subscriber(node, &node->count_subscriber, key, cpuload_on_count)) goto failed;
    snprintf(key, sizeof(key), "cpu/report/%s/%s/status", peer_name, my_name);
    if (!cpuload_declare_subscriber(node, &node->status_subscriber, key, cpuload_on_status)) goto failed;

    /* 보내는 쪽만 batch를 켠다. num0이 batch에 모이고 스케줄러가 1 ms마다 내보낸다 */
    if (options->is_publisher && (zp_batch_start(z_session_loan(session)) != Z_OK)) {
        snprintf(key, sizeof(key), "batch");
        goto failed;
    }

    node->next_seq = 1u;
    return true;

failed:
    fprintf(stderr, "선언하지 못했습니다: %s\n", key);
    cpuload_node_close(node);
    return false;
}

/* CPU 한 칸: "31.2% (메시지당 0.62 µs)" */
static void cpuload_format(char* out, size_t size, double pct, double rate) {
    if (pct < 0.0) {
        snprintf(out, size, "재지 못함");
    } else if (rate <= 0.0) {
        snprintf(out, size, "%.1f%%", pct);
    } else {
        snprintf(out, size, "%.1f%% (메시지당 %.2f µs)", pct, pct / 100.0 * 1e6 / rate);
    }
}

/* 보내는 쪽 결과 */
static void cpuload_print_result(const cpuload_node_t* node) {
    const cpuload_options_t* o = &node->options;
    printf("\n===== CPU 부하 (송신량 %u msg/s 고정, 측정 %u초, 메시지 %u B, %s→%s) =====\n", o->rate, o->measure_s,
           CPULOAD_PAYLOAD_BYTES, node->my_name, node->peer_name);
    if (node->is_aborted) {
        printf("  판정        : 측정하지 못함 (%s)\n", node->abort_reason);
        return;
    }
    if (node->window_end_us == 0u) {
        printf("  판정        : 측정하지 못함 (측정 구간을 마치기 전에 멈춤)\n");
        return;
    }
    const double window_s = (double)(node->window_end_us - node->window_start_us) / 1e6;
    const uint32_t window_sent = node->window_end_published - node->window_start_published;
    const double actual_rate = (window_s > 0.0) ? (double)window_sent / window_s : 0.0;
    const double achieved_pct = 100.0 * actual_rate / (double)o->rate;
    const uint32_t skipped = node->window_end_skipped - node->window_start_skipped;
    /* 미수신은 받는 쪽이 처음 받은 메시지부터 센다 (연결되기 전에 보낸 것은 넣지 않는다).
     * 그래서 분모도 그 뒤로 받아야 했던 수(받은 수 + 미수신 수)다 */
    const uint32_t lost = node->has_peer_view ? node->peer_view.unreceived : 0u;
    const uint32_t expected = node->has_peer_view ? node->peer_view.received + lost : 0u;
    const double loss_pct = (expected == 0u) ? 0.0 : 100.0 * (double)lost / (double)expected;
    const double mine_pct = cpuload_usage_load_pct(node->window_start_mine, node->window_end_mine);
    const double peer_pct = cpuload_usage_load_pct(node->window_start_peer, node->window_end_peer);
    char mine[64];
    char peer[64];
    cpuload_format(mine, sizeof(mine), mine_pct, actual_rate);
    cpuload_format(peer, sizeof(peer), peer_pct, actual_rate);

    const char* verdict = "유효";
    if (achieved_pct < (double)CPULOAD_MIN_ACHIEVED_PCT) {
        verdict = "무효 (목표만큼 보내지 못함. 송신량을 낮춰 다시 재세요)";
    } else if ((expected == 0u) || ((uint64_t)lost * 1000000u > (uint64_t)expected * CPULOAD_MAX_LOSS_PPM)) {
        verdict = "무효 (미수신이 1%를 넘었거나 받는 쪽이 받지 못함)";
    } else if ((mine_pct < 0.0) || (peer_pct < 0.0)) {
        verdict = "무효 (CPU를 읽지 못함)";
    }

    printf("  실제 송신량 : %.0f msg/s (목표의 %.1f%%), put 실패로 건너뛴 1 ms 구간 %u개\n", actual_rate, achieved_pct,
           skipped);
    printf("  미수신      : %u개 / 받는 쪽이 받기 시작한 뒤 보낸 %u개 (%.3f%%)\n", lost, expected, loss_pct);
    if (node->has_peer_view && (node->peer_view.out_of_order != 0u)) {
        printf("  순서 바뀜   : %u개 (중복·역순, 미수신으로 셈)\n", node->peer_view.out_of_order);
    }
    printf("  보내는 쪽   : %s\n", mine);
    printf("  받는 쪽     : %s\n", peer);
    printf("  판정        : %s\n", verdict);
    printf("                (측정 구간 %.1f초 평균. 각 PC에서 프로그램을 묶은 코어(%s) 하나 기준.\n", window_s,
           node->meter.label);
    printf("                받는 쪽 수신 인터럽트 처리는 대개 다른 코어에서 돌아 빠진다)\n");
#if Z_FEATURE_MULTI_THREAD == 0
    printf("                MT0: 받을 것이 없으면 소켓 timeout(%d ms)만큼 수신에서 기다리며 돈다\n",
           (int)Z_CONFIG_SOCKET_TIMEOUT);
#endif
}

/* 받는 쪽 요약 */
static void cpuload_print_peer_summary(cpuload_node_t* node) {
    pthread_mutex_lock(&node->state_lock);
    printf("\n===== 제어기 %s 요약 =====\n", node->my_name);
    printf("  받음 %u개, 미수신 %u개, 순서 바뀜·중복 %u개 (CPU 결과는 보내는 쪽 PC에 나온다)\n", node->received,
           node->report.unreceived, node->out_of_order);
    pthread_mutex_unlock(&node->state_lock);
}

bool cpuload_parse_rate(int argc, char** argv, uint32_t* rate) {
    *rate = CPULOAD_DEFAULT_RATE;
    if ((argc < 2) || (argv[1][0] == '\0')) return true; /* 주지 않으면 기본값 */
    if (argc == 2) {
        char* end = NULL;
        const unsigned long value = strtoul(argv[1], &end, 10);
        if ((end != NULL) && (*end == '\0') && (value >= CPULOAD_MIN_RATE) && (value <= CPULOAD_MAX_RATE)) {
            *rate = (uint32_t)value;
            return true;
        }
    }
    fprintf(stderr, "사용법: %s [송신량 msg/s, %u ~ %u. 주지 않으면 %u]\n", argv[0], CPULOAD_MIN_RATE, CPULOAD_MAX_RATE,
            CPULOAD_DEFAULT_RATE);
    return false;
}

int cpuload_run(cpuload_node_t* node, const z_owned_session_t* session, const char* my_name, const char* peer_name,
                const cpuload_options_t* options, volatile sig_atomic_t* is_stop_requested) {
    if (!cpuload_node_open(node, session, my_name, peer_name, options)) return 1;
    cpuload_meter_open(&node->meter);
    node->start_us = cpuload_now_us();
    node->phase = CPULOAD_PHASE_WARMUP;
    node->phase_start_us = node->start_us;
    node->last_second_cpu = cpuload_meter_read(&node->meter);
    cpuload_flag_set(&node->is_ready, true); /* 이제부터 콜백이 상태를 쓴다 */

    if (options->is_publisher) {
        printf("제어기 %s 시작 (보내는 쪽: %u msg/s로 보내며 CPU 부하 측정, 워밍업 %u초 + 측정 %u초, 메시지 %u B)\n",
               my_name, options->rate, options->warmup_s, options->measure_s, CPULOAD_PAYLOAD_BYTES);
    } else {
        printf("제어기 %s 시작 (받는 쪽: 상대가 끝나면 함께 끝남)\n", my_name);
    }
    if (node->meter.is_available) {
        printf("CPU: %s의 누적 CPU 시간을 잰다\n", node->meter.label);
    } else {
        printf("CPU: /proc/stat이나 CPU affinity를 읽지 못해 재지 않습니다\n");
    }
    fflush(stdout);

    int exit_code = 0;
#if Z_FEATURE_MULTI_THREAD == 1
    if (!cpuload_run_threads(node, is_stop_requested)) exit_code = 1;
#else
    cpuload_run_single_thread(node, is_stop_requested);
#endif
    cpuload_flag_set(&node->is_ready, false);

    if (options->is_publisher) {
        cpuload_print_result(node);
        if (node->is_aborted) exit_code = 1;
    } else {
        cpuload_print_peer_summary(node);
        if (node->is_sender_restarted) {
            printf("\n받는 쪽: 보내는 쪽이 다시 시작된 것을 봐서(count의 seq가 줄어듦) 이 실행을 끝냈습니다.\n");
            exit_code = 1;
        } else if (node->is_peer_never_seen) {
            printf("\n받는 쪽: 시작한 뒤 %u초 동안 보내는 쪽 count가 오지 않아 끝냈습니다.\n",
                   CPULOAD_PEER_FIRST_COUNT_TIMEOUT_US / 1000000u);
            exit_code = 1;
        }
    }
    fflush(stdout);
    cpuload_node_close(node);
    return exit_code;
}
