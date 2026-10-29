/*
 * 네트워크 성능 측정 예제 (zenoh-pico) - 두 제어기(A, B)가 같이 쓰는 측정 모듈
 *
 * brightonz.foundation의 z_perf 예제(examples/unix/c99/z_perf)와 같은 측정을 zenoh-pico 공개 API로 한다.
 * 측정 방법·메시지·스레드·출력 형식은 같고, 통신 부분만 zenoh-pico API로 바꿨다.
 *
 * 목적: 손실 0%로 보낼 수 있는 최대 처리량(max throughput)을 찾는다. 메시지 크기는 128 B.
 *   - 보내는 쪽(A): 송신량을 바꿔 가며 num0을 보내고, 상대 피드백으로 손실을 확인해 최대값을 찾는다.
 *                   탐색 순서는 perf_max_throughput.h 참고.
 *   - 받는 쪽(B)  : 받기만 하면서 0.25초마다 수신 결과(처리량·지터·손실률)를 피드백으로 돌려준다.
 *
 * 구성: A와 B는 서로 다른 PC에서 돌고, UDP multicast 그룹 하나로 주고받는다 (peer 모드, listen locator).
 *   같은 PC 안(loopback)에서는 패킷이 NIC 드라이버를 거치지 않으므로 측정하지 않는다.
 *   zenoh-pico는 리눅스에서 UDP unicast listen을 지원하지 않아 unicast로는 두 peer를 UDP로 직접 잇지 못한다.
 *
 * 통신 설정 (brightonz.foundation 예제의 JSON 설정에 맞춘다)
 *   - num0        : reliable(기본값), congestion control BLOCK, priority 기본, express 아님 → batch에 모인다.
 *   - count/status: 같은 QoS에 express → batch를 기다리지 않고 바로 나간다.
 *   - batch       : 보내는 쪽이 zp_batch_start로 켜고, 스케줄러가 1 ms마다 zp_batch_flush로 내보낸다.
 *                   batch 크기(UDP 데이터그램 최대 크기)는 zenoh-pico 빌드 옵션 BATCH_MULTICAST_SIZE로 정한다.
 *
 * 빌드 (저장소 최상위에서. 두 PC 모두 같은 옵션으로)
 *   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBATCH_MULTICAST_SIZE=1450 -DBUILD_SHARED_LIBS=OFF
 *   cmake --build build --target z_perf_tester z_perf_peer
 *   - BATCH_MULTICAST_SIZE=1450: brightonz.foundation 예제의 mtu·batch.length와 같다. 기본값 2048은 이더넷
 *     MTU(1500)보다 커서 IP 단편화가 생긴다. multicast는 batch 크기를 협상하지 않으므로 두 PC가 같아야 한다.
 *   - 실행 파일은 build/examples/z_perf/bin/, 실행 스크립트는 build/examples/z_perf/test_start.sh에 생긴다.
 *
 * 데이터 메시지 num0 (128 B)
 *   [seq 4 B][송신 시각 µs 8 B][사용자 데이터 116 B]
 *   - 앞 12 B는 측정용 헤더다. 처리량에는 사용자 데이터 116 B만 센다.
 *   - 송신 시각은 보내는 PC의 CLOCK_MONOTONIC이고 지터 계산에만 쓴다.
 *     지터는 같은 PC 시계끼리의 차이(수신 간격 − 송신 간격)라서 두 PC의 시계 기준이 달라도 맞다.
 *     단방향 지연(수신 시각 − 송신 시각)은 두 PC의 시계 기준이 달라 잴 수 없다.
 *
 * 피드백 메시지 (0.25초마다, 측정 대상 아님. 처리량·지터는 1초 구간 값)
 *   count  (4 B)  : 보내는 쪽 → 받는 쪽. [지금까지 보낸 마지막 seq]
 *   status (68 B) : 받는 쪽 → 보내는 쪽. [수신 결과 perf_stream_report_t] (처리량·지터·손실률·집계 기준 seq)
 *
 * 스레드 (라이브러리를 Z_FEATURE_MULTI_THREAD=1로 빌드했을 때)
 *   - 스케줄러   : 1 ms마다 깨어나 목표 송신량만큼 메시지를 보내고, 0.25초마다 count도 보낸다.
 *                  (데이터와 count를 한 스레드가 보내므로 순서가 저절로 지켜져 송신 쪽에는 락이 없다)
 *   - TX 피드백  : 0.25초마다 status를 보내고 탐색을 한 단계 진행한다. 1초마다 수신 구간을 마감하고 한 줄 출력한다.
 *   - RX 피드백  : 0.25초마다 받은 count로 미수신율을 계산하고, 받은 status를 저장한다.
 *   (데이터와 피드백 수신 자체는 zenoh-pico 읽기 태스크가 콜백으로 처리한다)
 *
 * 싱글스레드 (Z_FEATURE_MULTI_THREAD=0으로 빌드했을 때. multicast만: 1.6.1은 MT0에서 unicast peer를 끈다)
 *   읽기·lease 태스크와 위 셋을 띄우지 않는다. 메인 스레드 하나가 zp_read(수신 콜백 호출), keep-alive·JOIN 송신과
 *   위 세 tick을 같은 시각표로 되풀이한다. 측정 방법과 판정은 MT1과 같다.
 *
 * 두 프로그램은 각 PC의 test_start.sh로 한 회차씩 짝을 맞춰 띄우고 함께 끝낸다. 한쪽만 다시 띄우는 경우는
 * 다루지 않는다 (seq가 1부터 다시 시작하므로 받는 쪽에서는 순서 바뀜으로 세어진다).
 *
 * 받는 쪽(B)을 반드시 먼저 띄운다.
 *   zenoh-pico publisher는 상대 subscriber가 있을 때만 보낸다(write filter). 라우터 없는 peer 모드에서는
 *   선언하는 순간에 이미 있는 subscriber만 찾고 나중에 생기는 것은 기다리지 않는다.
 *   그래서 A가 먼저 뜨면 B가 뒤에 떠도 A는 아무것도 보내지 않고, 워밍업 뒤 "상대 보고가 따라오지 않아" 중단된다.
 *
 * zenoh-pico는 multicast 되돌림(IP_MULTICAST_LOOP)을 끄지 않는다. 그래서 A는 자기가 보낸 데이터그램도
 * 받아(recvfrom) 보낸 주소를 보고 버린다. 이 처리도 A의 CPU를 쓴다 (brightonz.foundation 예제는 loopback=false).
 */
#ifndef PERF_MEASURE_H
#define PERF_MEASURE_H

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <zenoh-pico.h>

#include "perf_max_throughput.h"

#define PERF_PAYLOAD_BYTES 128 /* 데이터 메시지 전체 크기 */
#define PERF_HEADER_BYTES 12   /* 그중 측정용 헤더 크기: seq 4 + 송신 시각 8 */
#define PERF_COUNT_BYTES 4     /* count 메시지 크기 */
#define PERF_STATUS_BYTES 68   /* status 메시지 크기 (perf_stream_report_t를 순서대로 담은 것) */

/* 스레드 사이에 나누는 값. GCC/Clang의 __atomic 내장 함수로 읽고 쓴다 (perf_measure.c) */
typedef struct {
    uint32_t value;
} perf_atomic_u32_t;

typedef struct {
    bool value;
} perf_atomic_bool_t;

/* 측정 설정 (각 main의 #define 값으로 채운다) */
typedef struct {
    uint32_t drain_s;  /* 송신을 멈춘 뒤 늦게 오는 메시지를 기다리는 시간 (초) */
    bool is_publisher; /* true: 보내는 쪽(최대 처리량 탐색), false: 받는 쪽 */
} perf_options_t;

/* num0 수신 결과. 받는 쪽이 계산해서 status로 보내는 단위다. */
typedef struct {
    uint32_t received;                /* 누적 수신 수 */
    uint32_t interval_received;       /* 직전 1초 수신 수 */
    uint32_t sequence_gaps;           /* 누적: seq가 건너뛴 개수 (누락 후보) */
    uint32_t out_of_order;            /* 누적: 앞 seq보다 작거나 같은 seq가 온 횟수 */
    uint32_t unreceived;              /* 누적 미수신 수 (상대 count 기준) */
    uint32_t unreceived_ppm;          /* 누적 미수신율 (ppm, 1% = 10000) */
    uint32_t interval_unreceived_ppm; /* 직전 count 대비 순간 미수신율 (ppm) */
    uint64_t throughput_bps;          /* 직전 1초 처리량 (사용자 데이터만) */
    uint32_t jitter_rfc_us;           /* RFC 3550 평활 지터 (실행 내내 누적) */
    uint32_t jitter_samples;          /* 직전 1초: D 개수 */
    double jitter_mean_us;            /* 직전 1초: D 평균 */
    double jitter_variance_us2;       /* 직전 1초: D 분산 */
    uint32_t jitter_max_abs_us;       /* 직전 1초: |D| 최댓값 */
    uint32_t counted_seq;             /* 집계 기준 seq: 미수신 계산에 쓴 상대 count의 마지막 seq.
                                         보내는 쪽은 이 값으로 자기 메시지가 어디까지 반영됐는지 안다 */
} perf_stream_report_t;

/* num0 송신 상태.
 * 쓰는 것은 스케줄러 스레드 하나뿐이다. 다른 스레드(TX 피드백, main)가 읽는 카운터만 atomic으로 둔다. */
typedef struct {
    z_owned_publisher_t publisher;
    uint32_t next_seq;              /* 다음 송신에 쓸 seq (1부터). 실패하면 같은 seq로 다시 보낸다 */
    perf_atomic_u32_t attempts;     /* put 시도 수 */
    perf_atomic_u32_t published;    /* put 성공 수 (= 마지막으로 보낸 seq) */
    perf_atomic_u32_t failed;       /* put 실패 수 */
    uint32_t last_second_published; /* TX 피드백 전용: 직전 출력 시점의 published */
} perf_tx_stream_t;

/* num0 수신 상태 (state_lock으로 보호) */
typedef struct {
    z_owned_subscriber_t subscriber;
    bool has_first;     /* 첫 메시지를 받았는가 */
    uint32_t first_seq; /* 처음 받은 seq. 손실은 여기부터 센다 */
    uint32_t last_seq;  /* 가장 최근 seq */
    uint32_t received;
    uint32_t sequence_gaps;
    uint32_t out_of_order;
    /* 지터: 직전 메시지의 송신/수신 시각 (연속된 seq일 때만 D 계산) */
    uint64_t previous_tx_us;
    uint64_t previous_rx_us;
    int64_t jitter_rfc_x16; /* RFC 3550 지터 × 16 (정수 계산용) */
    /* 1초 구간 누적값 */
    uint32_t interval_received;
    uint64_t interval_bytes;
    uint32_t interval_samples;
    double interval_mean_us;
    double interval_m2; /* Welford 제곱합 */
    uint32_t interval_max_abs_us;
    /* 손실 계산 */
    uint32_t received_at_count; /* 상대 count가 도착한 순간의 received */
    bool has_previous_count;
    uint32_t previous_expected; /* 직전 count 처리 때의 기대 수신 수 */
    uint32_t previous_received; /* 직전 count 처리 때의 실제 수신 수 */
    /* 결과 (구간 마감 / count 처리 때 갱신) */
    perf_stream_report_t report;
} perf_rx_stream_t;

/* 제어기 하나의 측정 상태 전체 */
typedef struct {
    const char* my_name;
    const char* peer_name;
    perf_options_t options;
    const z_owned_session_t* session;
    uint64_t start_us;

    perf_tx_stream_t tx; /* 내가 보내는 num0 */
    perf_rx_stream_t rx; /* 내가 받는 num0 */
    z_owned_publisher_t count_publisher;
    z_owned_publisher_t status_publisher;
    z_owned_subscriber_t count_subscriber;
    z_owned_subscriber_t status_subscriber;

    /* 최대 처리량 탐색 (보내는 쪽만 사용) */
    perf_search_t search;              /* 탐색 상태. TX 피드백 스레드만 쓴다 */
    perf_atomic_u32_t target_rate;     /* 스케줄러가 지금 보내야 할 송신량 (msg/s) */
    perf_atomic_bool_t is_search_done; /* 탐색이 끝났는가 (main이 읽는다) */

    /* state_lock: 수신 통계, 우편함, peer view (이 프로그램의 유일한 락).
     * RX 스레드가 계속 더하는 값을 TX 피드백이 1초마다 한꺼번에 읽고 0으로 되돌려야 해서 필요하다. */
    pthread_mutex_t state_lock;
    /* 우편함: RX 스레드가 받은 피드백을 넣고 RX 피드백 스레드가 꺼낸다 (최신 1개만, 덮어씀) */
    bool has_count_mail;
    uint8_t count_mail[PERF_COUNT_BYTES];
    bool has_status_mail;
    uint8_t status_mail[PERF_STATUS_BYTES];
    bool has_peer_view;
    perf_stream_report_t peer_view; /* 상대가 잰 "내 메시지" 수신 결과 */
    uint64_t last_count_us;         /* 상대 count를 마지막으로 받은 시각 (0 = 아직 없음). 상대가 끝났는지 판단용 */

    uint64_t last_tick_us;  /* TX 피드백: 직전 구간 마감 시각 */
    uint32_t print_seconds; /* TX 피드백: 1초 출력 번호 ([a Ns]의 N). 1초 경계를 지난 횟수 */
    uint32_t feedback_ticks; /* TX 피드백: tick 번호. 1초에 한 번(1초 경계) 구간을 마감하고 출력한다 */

    perf_atomic_bool_t is_ready; /* 콜백이 상태를 써도 되는가 */
    perf_atomic_bool_t should_stop_publish;
    perf_atomic_bool_t should_stop_feedback;
    pthread_t scheduler_thread;
    pthread_t tx_feedback_thread;
    pthread_t rx_feedback_thread;
} perf_node_t;

/* 열린 세션으로 측정을 실행한다. publisher/subscriber 선언과 해제도 여기서 한다.
 *   보내는 쪽: 최대 처리량 탐색 → 송신 정지 → drain 대기 → 요약·결과 출력
 *   받는 쪽  : 상대 count가 5초 동안 끊길 때까지(상대가 끝날 때까지) 받기 → 요약 출력
 * is_stop_requested가 1이 되면(Ctrl+C) 바로 정리하고 끝낸다. */
int perf_run(perf_node_t* node, const z_owned_session_t* session, const char* my_name, const char* peer_name,
             const perf_options_t* options, volatile sig_atomic_t* is_stop_requested);

#endif /* PERF_MEASURE_H */
