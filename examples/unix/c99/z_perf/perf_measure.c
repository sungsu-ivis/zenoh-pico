/*
 * 네트워크 성능 측정 예제 (zenoh-pico) - 측정 모듈 구현
 * 전체 구조는 perf_measure.h 맨 위 설명을 먼저 볼 것.
 * brightonz.foundation 예제와 다른 곳은 통신 부분뿐이다: payload 읽기, put/flush, 콜백 등록, 선언/해제.
 */
#include "perf_measure.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PERF_FEEDBACK_PERIOD_MS 250u /* count·status를 주고받고 탐색을 판정하는 주기 */
#define PERF_TICKS_PER_SECOND (1000u / PERF_FEEDBACK_PERIOD_MS) /* 1초의 피드백 tick 수 */
#define PERF_PEER_IDLE_STOP_US (5u * 1000000u) /* 받는 쪽: count가 이만큼 끊기면 상대가 끝난 것 */
#define PERF_USER_BYTES (PERF_PAYLOAD_BYTES - PERF_HEADER_BYTES)

/* ===================================================================
 * 시간
 * =================================================================== */

/* 지금 시각 (CLOCK_MONOTONIC, µs) */
static uint64_t perf_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void perf_timespec_add_ms(struct timespec* t, long ms) {
    t->tv_nsec += ms * 1000000L;
    while (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec += 1;
    }
}

/* 절대 시각 t까지 잔다. 시그널로 깨면 다시 잔다. */
static void perf_sleep_until(const struct timespec* t) {
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t, NULL) == EINTR) {
    }
}

static void perf_sleep_ms(long ms) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    perf_timespec_add_ms(&t, ms);
    perf_sleep_until(&t);
}

/* ===================================================================
 * 바이트 읽기/쓰기 (little-endian)
 * 각 함수는 쓰거나 읽은 다음 위치를 돌려준다.
 * =================================================================== */

static uint8_t* perf_put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
    return p + 4;
}

static uint8_t* perf_put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
    return p + 8;
}

static uint8_t* perf_put_f64(uint8_t* p, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return perf_put_u64(p, bits);
}

static const uint8_t* perf_get_u32(const uint8_t* p, uint32_t* v) {
    *v = 0u;
    for (int i = 0; i < 4; i++) *v |= (uint32_t)p[i] << (8 * i);
    return p + 4;
}

static const uint8_t* perf_get_u64(const uint8_t* p, uint64_t* v) {
    *v = 0u;
    for (int i = 0; i < 8; i++) *v |= (uint64_t)p[i] << (8 * i);
    return p + 8;
}

static const uint8_t* perf_get_f64(const uint8_t* p, double* v) {
    uint64_t bits;
    p = perf_get_u64(p, &bits);
    memcpy(v, &bits, sizeof(*v));
    return p;
}

/* 샘플 payload의 앞부분을 buffer에 최대 capacity 바이트 복사하고, payload 전체 길이를 돌려준다. */
static size_t perf_read_payload(const z_loaned_sample_t* sample, uint8_t* buffer, size_t capacity) {
    const z_loaned_bytes_t* payload = z_sample_payload(sample);
    z_bytes_reader_t reader = z_bytes_get_reader(payload);
    (void)z_bytes_reader_read(&reader, buffer, capacity);
    return z_bytes_len(payload);
}

/* ===================================================================
 * 작은 보조 함수
 * =================================================================== */

static bool perf_flag_get(perf_atomic_bool_t* flag) { return __atomic_load_n(&flag->value, __ATOMIC_ACQUIRE); }

static void perf_flag_set(perf_atomic_bool_t* flag, bool value) { __atomic_store_n(&flag->value, value, __ATOMIC_RELEASE); }

/* 송신 카운터 (스케줄러가 올리고 다른 스레드가 읽는다) */
static void perf_counter_add(perf_atomic_u32_t* counter) {
    (void)__atomic_fetch_add(&counter->value, 1u, __ATOMIC_RELEASE);
}

static uint32_t perf_counter_get(const perf_atomic_u32_t* counter) {
    return __atomic_load_n(&counter->value, __ATOMIC_ACQUIRE);
}

/* 스케줄러가 보낼 송신량 (TX 피드백이 쓰고 스케줄러가 읽는다) */
static uint32_t perf_target_rate_get(perf_node_t* node) {
    return __atomic_load_n(&node->target_rate.value, __ATOMIC_ACQUIRE);
}

static void perf_target_rate_set(perf_node_t* node, uint32_t rate) {
    __atomic_store_n(&node->target_rate.value, rate, __ATOMIC_RELEASE);
}

/* 비율 → ppm (분모가 0이면 0) */
static uint32_t perf_ratio_ppm(uint64_t part, uint64_t whole) {
    return (whole == 0u) ? 0u : (uint32_t)((part * 1000000u) / whole);
}

/* ===================================================================
 * 송신: 스케줄러 스레드 (1 ms 주기)
 * =================================================================== */

/* batch에 모인 메시지를 링크로 내보낸다.
 * num0 publisher는 express가 아니라서, put한 메시지는 batch(BATCH_MULTICAST_SIZE)에 모였다가 flush 때 데이터그램 하나로 나간다.
 * batch가 가득 차면 zenoh-pico가 put 안에서 스스로 내보내므로, 여기서는 덜 찬 batch만 내보내면 된다. */
static void perf_flush_batch(perf_node_t* node) { (void)zp_batch_flush(z_session_loan(node->session)); }

/* put 한 번. 보낼 데이터를 복사하지 않고 감싸기만 해서(z_bytes_from_static_buf) 넘긴다.
 * z_publisher_put은 반환하기 전에 데이터를 batch로 인코딩하고 payload를 놓으므로, 호출하는 쪽 버퍼로 충분하다.
 * congestion control이 BLOCK이라 batch가 가득 차면 내보낸 뒤 이어서 넣는다 (brightonz.foundation의 BUSY 재시도에 해당).
 * express 메시지(count) 앞에 아직 나가지 않은 데이터가 있으면, zenoh-pico가 그 데이터와 함께 바로 내보낸다. */
static z_result_t perf_put(const z_owned_publisher_t* publisher, const uint8_t* data, size_t len) {
    z_owned_bytes_t payload;
    const z_result_t result = z_bytes_from_static_buf(&payload, data, len);
    if (result != Z_OK) return result;
    return z_publisher_put(z_publisher_loan(publisher), z_bytes_move(&payload), NULL);
}

/* num0을 한 번 보낸다. [seq][송신 시각][사용자 데이터] */
static void perf_send_message(perf_node_t* node) {
    perf_tx_stream_t* tx = &node->tx;
    uint8_t payload[PERF_PAYLOAD_BYTES];

    uint8_t* p = perf_put_u32(payload, tx->next_seq);
    p = perf_put_u64(p, perf_now_us());          /* 송신 시각은 put 바로 앞에서 찍는다 (batch에서 기다린 시간의 흔들림도 지터에 들어간다) */
    memset(p, 'A', PERF_USER_BYTES); /* 사용자 데이터 자리 (내용은 의미 없음) */

    const z_result_t result = perf_put(&tx->publisher, payload, sizeof(payload));
    perf_counter_add(&tx->attempts);
    if (result == Z_OK) {
        perf_counter_add(&tx->published);
        tx->next_seq++;
    } else {
        perf_counter_add(&tx->failed); /* 같은 seq로 다음에 다시 보낸다 */
    }
}

/* count를 보낸다: [지금까지 보낸 마지막 seq].
 * 데이터를 보내는 스케줄러 스레드가 직접 보내므로, 여기 적힌 seq까지의 데이터는 반드시 이 count보다 먼저 나간다.
 * (count는 express라서 batch에 데이터가 남아 있으면 zenoh-pico가 그 batch 끝에 붙여 함께 바로 내보낸다.) */
static void perf_send_count(perf_node_t* node) {
    uint8_t message[PERF_COUNT_BYTES];
    (void)perf_put_u32(message, perf_counter_get(&node->tx.published)); /* 실패하면 seq를 다시 쓰므로 published = 마지막 seq */
    (void)perf_put(&node->count_publisher, message, sizeof(message));
}

static void* perf_scheduler_main(void* arg) {
    perf_node_t* node = (perf_node_t*)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t tick_ms = 0u;
    uint64_t rate_credit = 0u; /* 1 ms마다 목표 송신량만큼 쌓고, 1000이 모일 때마다 1개 보낸다 */

    while (!perf_flag_get(&node->should_stop_publish)) {
        /* 목표 송신량(msg/s)을 1 ms마다 rate/1000개씩 나눠 보낸다 (예: 30000 msg/s → 1 ms마다 30개).
         * 보내는 데 1 ms보다 오래 걸리면 다음 tick이 늦어져, 송신 측이 낼 수 있는 최대 속도로 보내게 된다. */
        const uint32_t rate = perf_target_rate_get(node);
        rate_credit = (rate == 0u) ? 0u : rate_credit + rate;
        while (rate_credit >= 1000u) {
            perf_send_message(node);
            rate_credit -= 1000u;
        }
        /* 이번 tick에 batch에 남은 메시지를 내보낸다. 메시지가 batch에서 기다리는 시간은 1 ms를 넘지 않는다. */
        if (rate != 0u) perf_flush_batch(node);

        /* 0.25초마다: 방금까지 보낸 데이터의 count를 보낸다 (탐색 중 쉬는 동안에도 보낸다) */
        if ((tick_ms > 0u) && (tick_ms % PERF_FEEDBACK_PERIOD_MS == 0u)) perf_send_count(node);

        /* 다음 깨어날 시각 = 이전 목표 + 1 ms ("지금 + 1 ms"로 하면 처리 시간만큼 밀린다) */
        perf_timespec_add_ms(&next, 1);
        perf_sleep_until(&next);
        tick_ms++;
    }

    /* 멈추기 직전 마지막 count: 받는 쪽이 drain 동안 최종 미수신율을 정할 수 있게 */
    perf_send_count(node);
    return NULL;
}

/* ===================================================================
 * 수신: zenoh-pico 읽기 태스크에서 불리는 콜백 (subscriber마다 하나, context는 perf_node_t*)
 * =================================================================== */

/* D 하나를 지터 통계에 넣는다 */
static void perf_jitter_add(perf_rx_stream_t* rx, int64_t d_us) {
    const int64_t abs_d = (d_us < 0) ? -d_us : d_us;
    /* RFC 3550: J = J + (|D| − J) / 16. J를 16배 한 정수로 들고 계산한다 (RFC 3550 부록 A.8). */
    rx->jitter_rfc_x16 += abs_d - ((rx->jitter_rfc_x16 + 8) >> 4);
    /* Welford: 값을 하나씩 받으면서 평균과 분산을 갱신한다 (부호 유지) */
    rx->interval_samples++;
    const double delta = (double)d_us - rx->interval_mean_us;
    rx->interval_mean_us += delta / (double)rx->interval_samples;
    rx->interval_m2 += delta * ((double)d_us - rx->interval_mean_us);
    if ((uint64_t)abs_d > rx->interval_max_abs_us) rx->interval_max_abs_us = (uint32_t)abs_d;
}

static void perf_on_message(z_loaned_sample_t* sample, void* context) {
    perf_node_t* node = (perf_node_t*)context;
    const uint64_t rx_us = perf_now_us(); /* 수신 시각은 가장 먼저 찍는다 */
    if ((node == NULL) || (sample == NULL) || !perf_flag_get(&node->is_ready)) return;

    uint8_t header[PERF_HEADER_BYTES];
    const size_t length = perf_read_payload(sample, header, sizeof(header));
    if (length < PERF_HEADER_BYTES) return;

    uint32_t seq;
    uint64_t tx_us;
    const uint8_t* p = perf_get_u32(header, &seq);
    (void)perf_get_u64(p, &tx_us);

    pthread_mutex_lock(&node->state_lock);
    perf_rx_stream_t* rx = &node->rx;
    if (!rx->has_first) {
        /* 처음 받은 메시지: 여기부터 센다. 지터는 기준값만 저장한다 */
        rx->has_first = true;
        rx->first_seq = seq;
        rx->last_seq = seq;
        rx->previous_tx_us = tx_us;
        rx->previous_rx_us = rx_us;
    } else if (seq > rx->last_seq) {
        if (seq == rx->last_seq + 1u) {
            /* 연속된 seq일 때만 D를 계산한다 (문서 3.2) */
            const int64_t d_us = ((int64_t)rx_us - (int64_t)rx->previous_rx_us) -
                                 ((int64_t)tx_us - (int64_t)rx->previous_tx_us);
            perf_jitter_add(rx, d_us);
        } else {
            rx->sequence_gaps += seq - rx->last_seq - 1u; /* 건너뛴 seq = 누락 후보 */
        }
        rx->last_seq = seq;
        rx->previous_tx_us = tx_us;
        rx->previous_rx_us = rx_us;
    } else {
        rx->out_of_order++; /* 앞 seq보다 작거나 같다: 순서 바뀜 또는 중복 */
    }

    rx->received++;
    rx->interval_received++;
    rx->interval_bytes += (length > PERF_HEADER_BYTES) ? (length - PERF_HEADER_BYTES) : 0u;
    pthread_mutex_unlock(&node->state_lock);
}

/* count 도착: 우편함에 넣고, 이 순간의 수신 수를 기록한다.
 * 상대는 데이터와 count를 같은 스레드(스케줄러)에서 차례로 보내므로, count에 적힌 seq까지의 데이터 중
 * 손실되지 않은 것은 지금 모두 도착해 있다. 그래서 이 순간의 수신 수와 비교하면 정확하다. */
static void perf_on_count(z_loaned_sample_t* sample, void* context) {
    perf_node_t* node = (perf_node_t*)context;
    if ((node == NULL) || (sample == NULL) || !perf_flag_get(&node->is_ready)) return;
    uint8_t data[PERF_COUNT_BYTES];
    if (perf_read_payload(sample, data, sizeof(data)) != sizeof(data)) return; /* 크기가 다르면 버린다 */

    pthread_mutex_lock(&node->state_lock);
    memcpy(node->count_mail, data, sizeof(data));
    node->has_count_mail = true;
    node->last_count_us = perf_now_us();
    node->rx.received_at_count = node->rx.received;
    pthread_mutex_unlock(&node->state_lock);
}

/* status 도착: 우편함에 넣기만 한다 */
static void perf_on_status(z_loaned_sample_t* sample, void* context) {
    perf_node_t* node = (perf_node_t*)context;
    if ((node == NULL) || (sample == NULL) || !perf_flag_get(&node->is_ready)) return;
    uint8_t data[PERF_STATUS_BYTES];
    if (perf_read_payload(sample, data, sizeof(data)) != sizeof(data)) return; /* 크기가 다르면 버린다 */

    pthread_mutex_lock(&node->state_lock);
    memcpy(node->status_mail, data, sizeof(data));
    node->has_status_mail = true;
    pthread_mutex_unlock(&node->state_lock);
}

/* ===================================================================
 * TX 피드백 스레드 (0.25초): status 송신(받는 쪽) / 탐색 진행(보내는 쪽). 1초마다 구간 마감과 한 줄 출력
 * =================================================================== */

/* 수신 1초 구간을 마감해 report에 옮기고 구간 값을 0으로 한다 (state_lock 안에서) */
static void perf_rx_close_interval(perf_rx_stream_t* rx, uint64_t interval_us) {
    perf_stream_report_t* r = &rx->report;
    r->received = rx->received;
    r->interval_received = rx->interval_received;
    r->sequence_gaps = rx->sequence_gaps;
    r->out_of_order = rx->out_of_order;
    r->throughput_bps = (interval_us == 0u) ? 0u : rx->interval_bytes * 8u * 1000000u / interval_us;
    r->jitter_rfc_us = (uint32_t)(rx->jitter_rfc_x16 >> 4);
    r->jitter_samples = rx->interval_samples;
    r->jitter_mean_us = rx->interval_mean_us;
    r->jitter_variance_us2 = (rx->interval_samples == 0u) ? 0.0 : rx->interval_m2 / rx->interval_samples;
    r->jitter_max_abs_us = rx->interval_max_abs_us;
    /* unreceived 관련 값은 RX 피드백 스레드가 count를 처리할 때 채운다 */

    /* 구간 값만 초기화. 누적 수신 수, RFC 지터, 직전 메시지 기준값은 유지 (문서 4.2 ③-3) */
    rx->interval_received = 0u;
    rx->interval_bytes = 0u;
    rx->interval_samples = 0u;
    rx->interval_mean_us = 0.0;
    rx->interval_m2 = 0.0;
    rx->interval_max_abs_us = 0u;
}

/* report → status 메시지 (PERF_STATUS_BYTES = 68 B) */
static void perf_put_report(uint8_t* p, const perf_stream_report_t* r) {
    p = perf_put_u32(p, r->received);
    p = perf_put_u32(p, r->interval_received);
    p = perf_put_u32(p, r->sequence_gaps);
    p = perf_put_u32(p, r->out_of_order);
    p = perf_put_u32(p, r->unreceived);
    p = perf_put_u32(p, r->unreceived_ppm);
    p = perf_put_u32(p, r->interval_unreceived_ppm);
    p = perf_put_u64(p, r->throughput_bps);
    p = perf_put_u32(p, r->jitter_rfc_us);
    p = perf_put_u32(p, r->jitter_samples);
    p = perf_put_f64(p, r->jitter_mean_us);
    p = perf_put_f64(p, r->jitter_variance_us2);
    p = perf_put_u32(p, r->jitter_max_abs_us);
    (void)perf_put_u32(p, r->counted_seq);
}

/* status 메시지 → report */
static void perf_get_report(const uint8_t* p, perf_stream_report_t* r) {
    p = perf_get_u32(p, &r->received);
    p = perf_get_u32(p, &r->interval_received);
    p = perf_get_u32(p, &r->sequence_gaps);
    p = perf_get_u32(p, &r->out_of_order);
    p = perf_get_u32(p, &r->unreceived);
    p = perf_get_u32(p, &r->unreceived_ppm);
    p = perf_get_u32(p, &r->interval_unreceived_ppm);
    p = perf_get_u64(p, &r->throughput_bps);
    p = perf_get_u32(p, &r->jitter_rfc_us);
    p = perf_get_u32(p, &r->jitter_samples);
    p = perf_get_f64(p, &r->jitter_mean_us);
    p = perf_get_f64(p, &r->jitter_variance_us2);
    p = perf_get_u32(p, &r->jitter_max_abs_us);
    (void)perf_get_u32(p, &r->counted_seq);
}

/* 첫 tick 시각 = 다음 1초 경계(CLOCK_MONOTONIC) + offset */
static void perf_first_tick_at(struct timespec* t, long offset_ms) {
    clock_gettime(CLOCK_MONOTONIC, t);
    t->tv_sec += 1;
    t->tv_nsec = 0;
    perf_timespec_add_ms(t, offset_ms);
}

static void perf_tx_feedback_tick(perf_node_t* node) {
    const uint64_t now = perf_now_us();
    /* 1초 통계(수신 구간 마감, 초당 송신량)와 출력은 1초 경계의 tick에서만 한다 */
    const bool is_second_tick = (node->feedback_ticks++ % PERF_TICKS_PER_SECOND) == 0u;

    /* (1) 송신 카운터 읽기 (count는 스케줄러가 직접 보낸다. 여기서는 atomic으로 읽기만 한다) */
    perf_tx_stream_t* tx = &node->tx;
    const uint32_t published = perf_counter_get(&tx->published);
    const uint32_t sent_per_s = published - tx->last_second_published;
    if (is_second_tick) tx->last_second_published = published;
    const uint32_t failed_total = perf_counter_get(&tx->failed);

    /* (2) 1초 경계면 수신 구간을 마감한다. 내 수신 결과와 상대 보고를 꺼낸다 */
    pthread_mutex_lock(&node->state_lock);
    if (is_second_tick) {
        perf_rx_close_interval(&node->rx, now - node->last_tick_us);
        node->last_tick_us = now;
    }
    const perf_stream_report_t mine = node->rx.report;
    const perf_stream_report_t peer = node->peer_view;
    const bool has_peer_view = node->has_peer_view;
    pthread_mutex_unlock(&node->state_lock);

    /* (3) 받는 쪽: status = [내 수신 결과] */
    if (!node->options.is_publisher) {
        uint8_t message[PERF_STATUS_BYTES];
        perf_put_report(message, &mine);
        (void)perf_put(&node->status_publisher, message, sizeof(message));
    }

    /* (4) 보내는 쪽: 상대 보고를 보고 탐색을 한 단계 진행하고, 다음 송신량을 스케줄러에 알려 준다 */
    if (node->options.is_publisher && !perf_flag_get(&node->is_search_done)) {
        const uint32_t rate = perf_search_step(&node->search, now, published, has_peer_view, peer.counted_seq,
                                               peer.unreceived, node->my_name);
        perf_target_rate_set(node, rate);
        if (perf_search_is_done(&node->search)) perf_flag_set(&node->is_search_done, true);
    }

    /* (5) 1초마다 한 줄 출력: 받는 쪽은 수신 결과, 보내는 쪽은 송신량과 상대 보고 */
    if (!is_second_tick) return;
    const unsigned elapsed_s = ++node->print_seconds;
    if (!node->options.is_publisher) {
        printf("[%s %3us] 수신 %s→%s %6u msg/s %9.1f kbps  미수신 %6.3f%% (순간 %6.3f%%)  지터 RFC %4uus |D|최대 %5uus\n",
               node->my_name, elapsed_s, node->peer_name, node->my_name, mine.interval_received,
               (double)mine.throughput_bps / 1000.0, mine.unreceived_ppm / 10000.0, mine.interval_unreceived_ppm / 10000.0,
               mine.jitter_rfc_us, mine.jitter_max_abs_us);
    } else {
        const uint32_t target = perf_target_rate_get(node);
        printf("[%s %3us] 송신 %s→%s %6u msg/s (다음 목표 %6u, 실패 누적 %u)", node->my_name, elapsed_s, node->my_name,
               node->peer_name, sent_per_s, target, failed_total);
        if (has_peer_view) {
            printf("  상대 보고: %8.1f kbps  미수신 %6.3f%%  지터 RFC %4uus", (double)peer.throughput_bps / 1000.0,
                   peer.unreceived_ppm / 10000.0, peer.jitter_rfc_us);
        }
        printf("\n");
    }
    printf("\n"); /* 1초 기록마다 빈 줄로 구분한다 */
    fflush(stdout);
}

/* ===================================================================
 * RX 피드백 스레드 (0.25초): count로 미수신율 계산, status를 peer view에 저장
 * =================================================================== */

/* 상대 count로 미수신 수를 계산한다 (state_lock 안에서).
 *   기대 수신 수 = 상대가 보낸 마지막 seq − 내가 처음 받은 seq + 1
 *   미수신 수   = 기대 수신 수 − count가 도착한 순간의 내 수신 수 */
static void perf_rx_update_loss(perf_rx_stream_t* rx, uint32_t peer_published_seq) {
    if (!rx->has_first || (peer_published_seq < rx->first_seq)) return; /* 아직 받은 것이 없음 */
    rx->report.counted_seq = peer_published_seq; /* 이 seq까지 반영했다는 것을 보내는 쪽에 알려 준다 */
    const uint32_t expected = peer_published_seq - rx->first_seq + 1u;
    const uint32_t received = rx->received_at_count;
    perf_stream_report_t* r = &rx->report;
    r->unreceived = (expected > received) ? expected - received : 0u;
    r->unreceived_ppm = perf_ratio_ppm(r->unreceived, expected);

    /* 순간 미수신율: 직전 count 이후 늘어난 만큼만 비교 (문서 3.1) */
    if (rx->has_previous_count && (expected >= rx->previous_expected) && (received >= rx->previous_received)) {
        const uint32_t d_expected = expected - rx->previous_expected;
        const uint32_t d_received = received - rx->previous_received;
        r->interval_unreceived_ppm = perf_ratio_ppm((d_expected > d_received) ? d_expected - d_received : 0u, d_expected);
    }
    rx->has_previous_count = true;
    rx->previous_expected = expected;
    rx->previous_received = received;
}

static void perf_rx_feedback_tick(perf_node_t* node) {
    pthread_mutex_lock(&node->state_lock);

    /* count = [상대가 보낸 마지막 seq] */
    if (node->has_count_mail) {
        uint32_t published_seq;
        (void)perf_get_u32(node->count_mail, &published_seq);
        perf_rx_update_loss(&node->rx, published_seq);
        node->has_count_mail = false;
    }

    /* status = [상대가 잰 "내 메시지" 수신 결과] */
    if (node->has_status_mail) {
        perf_get_report(node->status_mail, &node->peer_view);
        node->has_peer_view = true;
        node->has_status_mail = false;
    }

    pthread_mutex_unlock(&node->state_lock);
}

/* 피드백 스레드 공통 틀: 0.25초(FEEDBACK_PERIOD_MS)마다 tick을 부른다 */
static void* perf_tx_feedback_main(void* arg) {
    perf_node_t* node = (perf_node_t*)arg;
    struct timespec next;
    perf_first_tick_at(&next, 0L);
    while (!perf_flag_get(&node->should_stop_feedback)) {
        perf_sleep_until(&next);
        if (!perf_flag_get(&node->should_stop_feedback)) perf_tx_feedback_tick(node);
        perf_timespec_add_ms(&next, (long)PERF_FEEDBACK_PERIOD_MS);
    }
    return NULL;
}

static void* perf_rx_feedback_main(void* arg) {
    perf_node_t* node = (perf_node_t*)arg;
    struct timespec next;
    /* TX 피드백과 반 박자 엇갈려 돈다 */
    perf_first_tick_at(&next, (long)PERF_FEEDBACK_PERIOD_MS / 2L);
    while (!perf_flag_get(&node->should_stop_feedback)) {
        perf_sleep_until(&next);
        perf_rx_feedback_tick(node);
        perf_timespec_add_ms(&next, (long)PERF_FEEDBACK_PERIOD_MS);
    }
    return NULL;
}

/* ===================================================================
 * 시작 / 종료 / 요약
 * =================================================================== */

/* publisher 하나를 선언한다. QoS는 brightonz.foundation 예제의 JSON과 같다 (reliable 기본값, BLOCK, priority 기본). */
static bool perf_declare_publisher(perf_node_t* node, z_owned_publisher_t* publisher, const char* key, bool is_express) {
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
static bool perf_declare_subscriber(perf_node_t* node, z_owned_subscriber_t* subscriber, const char* key,
                                    z_closure_sample_callback_t callback) {
    z_view_keyexpr_t keyexpr;
    if (z_view_keyexpr_from_str(&keyexpr, key) != Z_OK) return false;
    z_owned_closure_sample_t closure;
    if (z_closure_sample(&closure, callback, NULL, node) != Z_OK) return false;
    return z_declare_subscriber(z_session_loan(node->session), subscriber, z_view_keyexpr_loan(&keyexpr),
                                z_closure_sample_move(&closure), NULL) == Z_OK;
}

/* 선언한 것을 모두 해제한다 (선언하지 않은 것은 null이라 drop해도 된다). 보내는 쪽은 batch도 끈다. */
static void perf_node_close(perf_node_t* node) {
    if (node->options.is_publisher) (void)zp_batch_stop(z_session_loan(node->session));
    z_subscriber_drop(z_subscriber_move(&node->rx.subscriber));
    z_subscriber_drop(z_subscriber_move(&node->count_subscriber));
    z_subscriber_drop(z_subscriber_move(&node->status_subscriber));
    z_publisher_drop(z_publisher_move(&node->tx.publisher));
    z_publisher_drop(z_publisher_move(&node->count_publisher));
    z_publisher_drop(z_publisher_move(&node->status_publisher));
}

/* publisher/subscriber를 선언하고 상태를 초기화한다.
 * key는 brightonz.foundation 예제의 JSON과 같다. 선언할 때 key를 복사하므로 key 버퍼는 다시 써도 된다. */
static bool perf_node_open(perf_node_t* node, const z_owned_session_t* session, const char* my_name,
                           const char* peer_name, const perf_options_t* options) {
    char key[64];
    node->my_name = my_name;
    node->peer_name = peer_name;
    node->options = *options;
    node->session = session;
    z_internal_publisher_null(&node->tx.publisher);
    z_internal_publisher_null(&node->count_publisher);
    z_internal_publisher_null(&node->status_publisher);
    z_internal_subscriber_null(&node->rx.subscriber);
    z_internal_subscriber_null(&node->count_subscriber);
    z_internal_subscriber_null(&node->status_subscriber);
    pthread_mutex_init(&node->state_lock, NULL);

    snprintf(key, sizeof(key), "perf/msg/%s/%s/num0", my_name, peer_name); /* 내가 보내는 것 */
    if (!perf_declare_publisher(node, &node->tx.publisher, key, false)) goto failed;
    snprintf(key, sizeof(key), "perf/msg/%s/%s/num0", peer_name, my_name); /* 내가 받는 것 */
    if (!perf_declare_subscriber(node, &node->rx.subscriber, key, perf_on_message)) goto failed;
    snprintf(key, sizeof(key), "perf/report/%s/%s/count", my_name, peer_name);
    if (!perf_declare_publisher(node, &node->count_publisher, key, true)) goto failed;
    snprintf(key, sizeof(key), "perf/report/%s/%s/status", my_name, peer_name);
    if (!perf_declare_publisher(node, &node->status_publisher, key, true)) goto failed;
    snprintf(key, sizeof(key), "perf/report/%s/%s/count", peer_name, my_name);
    if (!perf_declare_subscriber(node, &node->count_subscriber, key, perf_on_count)) goto failed;
    snprintf(key, sizeof(key), "perf/report/%s/%s/status", peer_name, my_name);
    if (!perf_declare_subscriber(node, &node->status_subscriber, key, perf_on_status)) goto failed;

    /* 보내는 쪽만 batch를 켠다. num0이 batch에 모이고 스케줄러가 1 ms마다 내보낸다 */
    if (options->is_publisher && (zp_batch_start(z_session_loan(session)) != Z_OK)) {
        snprintf(key, sizeof(key), "batch");
        goto failed;
    }

    node->tx.next_seq = 1u;
    return true;

failed:
    fprintf(stderr, "선언하지 못했습니다: %s\n", key);
    perf_node_close(node);
    return false;
}

static void perf_print_summary(perf_node_t* node) {
    pthread_mutex_lock(&node->state_lock);
    printf("\n===== 제어기 %s 요약 =====\n", node->my_name);
    if (node->options.is_publisher) {
        /* 보내는 쪽: 탐색 전체에서 보낸 수와, 상대가 보고한 전체 수신 결과 */
        const perf_tx_stream_t* tx = &node->tx;
        const perf_stream_report_t* r = &node->peer_view;
        printf("[송신 %s→%s]       시도       성공   실패 | 상대 보고:   수신   미수신율  지터RFC\n",
               node->my_name, node->peer_name);
        printf("  num0     %10u %10u %6u |", perf_counter_get(&tx->attempts), perf_counter_get(&tx->published),
               perf_counter_get(&tx->failed));
        if (node->has_peer_view) {
            printf(" %12u  %8.3f%%  %5uus\n", r->received, r->unreceived_ppm / 10000.0, r->jitter_rfc_us);
        } else {
            printf("  (상대 보고 없음)\n");
        }
    } else {
        /* 받는 쪽: 전체 수신 결과 (탐색 중 일부러 손실이 나는 단계가 섞여 있으므로 미수신율은 0이 아닐 수 있다) */
        const perf_rx_stream_t* rx = &node->rx;
        const perf_stream_report_t* r = &rx->report;
        printf("[수신 %s→%s]       수신  누락후보 순서바뀜   미수신   미수신율  지터RFC\n",
               node->peer_name, node->my_name);
        printf("  num0     %10u  %8u %8u %8u  %8.3f%%  %5uus\n", rx->received, rx->sequence_gaps,
               rx->out_of_order, r->unreceived, r->unreceived_ppm / 10000.0, r->jitter_rfc_us);
    }
    pthread_mutex_unlock(&node->state_lock);
}

int perf_run(perf_node_t* node, const z_owned_session_t* session, const char* my_name, const char* peer_name,
             const perf_options_t* options, volatile sig_atomic_t* is_stop_requested) {
    if (!perf_node_open(node, session, my_name, peer_name, options)) return 1;
    node->start_us = perf_now_us();
    node->last_tick_us = node->start_us;
    perf_flag_set(&node->is_ready, true); /* 이제부터 콜백이 통계를 쓴다 */

    /* 보내는 쪽은 최대 처리량을 탐색하고, 받는 쪽은 받으면서 status를 보내다가 상대가 끝나면 따라 끝난다 */
    if (options->is_publisher) {
        perf_search_init(&node->search, node->start_us);
        perf_target_rate_set(node, node->search.target_rate);
        printf("제어기 %s 시작 (보내는 쪽: 최대 처리량 탐색, 손실 0%%, 메시지 %u B)\n", my_name, PERF_PAYLOAD_BYTES);
    } else {
        printf("제어기 %s 시작 (받는 쪽: 상대가 끝나면 함께 끝남)\n", my_name);
    }
    pthread_create(&node->tx_feedback_thread, NULL, perf_tx_feedback_main, node);
    pthread_create(&node->rx_feedback_thread, NULL, perf_rx_feedback_main, node);
    if (options->is_publisher) pthread_create(&node->scheduler_thread, NULL, perf_scheduler_main, node);

    /* 끝날 때까지 기다린다
     *   - 보내는 쪽: 탐색이 끝나면
     *   - 받는 쪽  : 상대 count가 5초 동안 끊기면 (상대 프로그램이 끝남) */
    while (*is_stop_requested == 0) {
        if (options->is_publisher) {
            if (perf_flag_get(&node->is_search_done)) break;
        } else {
            pthread_mutex_lock(&node->state_lock);
            const uint64_t last_count_us = node->last_count_us;
            pthread_mutex_unlock(&node->state_lock);
            if ((last_count_us != 0u) && (perf_now_us() - last_count_us >= PERF_PEER_IDLE_STOP_US)) break;
        }
        perf_sleep_ms(100);
    }

    /* 송신을 멈추고, 피드백은 drain 시간 동안 계속 돌려서 최종 count/status를 주고받는다 */
    perf_flag_set(&node->should_stop_publish, true);
    if (options->is_publisher) pthread_join(node->scheduler_thread, NULL);
    perf_sleep_ms((long)options->drain_s * 1000L);
    perf_flag_set(&node->should_stop_feedback, true);
    pthread_join(node->tx_feedback_thread, NULL);
    pthread_join(node->rx_feedback_thread, NULL);
    perf_flag_set(&node->is_ready, false);

    perf_print_summary(node);
    if (options->is_publisher) perf_search_print_result(&node->search, my_name, peer_name);
    perf_node_close(node);
    return 0;
}
