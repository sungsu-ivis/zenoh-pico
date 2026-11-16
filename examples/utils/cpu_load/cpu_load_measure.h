/*
 * CPU 부하 측정 예제 (zenoh-pico) - 두 제어기(A, B)가 같이 쓰는 측정 모듈
 *
 * brightonz.foundation의 cpu_load 예제(examples/utils/cpu_load)와 같은 측정을 zenoh-pico 공개 API로 한다.
 * 측정 방법·메시지·스레드·출력 형식은 같고, 통신 부분만 zenoh-pico API로 바꿨다.
 *
 * 목적: 같은 양의 일을 할 때 두 PC가 CPU를 얼마나 쓰는지 잰다. 라이브러리, MT1·MT0, 전송 방식끼리 비교하는 값이다.
 *   - 보내는 쪽(A): 정해진 송신량(기본 500,000 msg/s, 128 B 메시지)으로 보낸다. 회선이 받아 주는 양보다 한참 낮게 잡아
 *                   송신이 막히지 않게 한다. 워밍업 3초 뒤 10초 동안을 측정 구간으로 삼는다.
 *   - 받는 쪽(B)  : 받기만 하면서 0.25초마다 수신 결과와 자기 누적 CPU 시간을 status로 돌려준다.
 *   - 결과        : A가 측정 구간의 시작과 끝에 자기 누적 CPU 시간과, 그때 갖고 있던
 *                   가장 최근 status의 B 누적 CPU 시간을 잡아 두 PC의 평균 부하(%)와
 *                   메시지 하나에 쓴 CPU 시간(µs)을 출력한다.
 *                   실제 송신량이 목표의 99% 이상이고 미수신이 1% 이하일 때만 유효한 값으로 본다.
 *   CPU 값은 각 PC에서 프로그램을 묶은 코어 하나의 부하다 (cpu_usage.h).
 *   받는 쪽 커널의 수신 인터럽트 처리는 대개 NIC 인터럽트를 받는 다른 코어에서 돌아 이 값에 들어가지 않는다.
 *
 * 구성: A와 B는 서로 다른 PC에서 돌고, 빌드한 전송 방식 하나(UDP multicast 그룹 또는 TCP)로 주고받는다 (peer 모드).
 *   zenoh-pico는 리눅스에서 UDP unicast listen을 지원하지 않아 unicast는 TCP로만 잰다.
 *
 * 통신 설정 (brightonz.foundation 예제의 JSON 설정에 맞춘다. max_throughput과 같다)
 *   - num0        : reliable(기본값), congestion control BLOCK, priority 기본, express 아님 → batch에 모인다.
 *   - count/status: 같은 QoS에 express → batch를 기다리지 않고 바로 나간다.
 *   - batch       : 보내는 쪽이 zp_batch_start로 켜고, 스케줄러가 1 ms마다 zp_batch_flush로 내보낸다.
 *
 * 메시지
 *   num0   (128 B): A → B. [seq 4 B][채움 124 B]
 *   count  (4 B)  : A → B. [지금까지 보낸 마지막 seq] (실제 시간 0.25초마다)
 *   status (24 B) : B → A. [받은 수, 미수신 수, 집계 기준 seq, 순서 바뀜·중복 수, 누적 CPU 일한 시간 ms, 지난 시간 ms]
 *
 * 송신 (A): 1 ms마다 목표 송신량만큼 put한다. congestion control이 BLOCK이라 batch가 차면 zenoh-pico가 put 안에서
 *   내보내고 이어서 넣는다. put이 실패하면 그 1 ms 구간의 나머지는 건너뛴다 (막힌 채로 계속 다시 시도하는 시간이
 *   CPU 값에 섞이지 않게). 밀린 양은 2 ms 분량까지만 다음 구간에 몰아 보낸다.
 *
 * 스레드 (라이브러리를 Z_FEATURE_MULTI_THREAD=1로 빌드했을 때)
 *   - 스케줄러 : 1 ms마다 깨어 목표 송신량만큼 보내고, count도 보낸다 (A만).
 *   - 피드백   : 0.25초마다 받은 count·status를 처리하고, status를 보내고(B), 측정 구간을 진행하고(A),
 *                1초마다 한 줄 출력한다.
 *   (데이터와 피드백 수신 자체는 zenoh-pico executor 스레드가 콜백으로 처리한다)
 *
 * 싱글스레드 (Z_FEATURE_MULTI_THREAD=0으로 빌드했을 때)
 *   메인 스레드 하나가 zp_spin_once(수신 콜백 호출, keep-alive·JOIN 송신, TCP accept)와 위 두 일을 같은 시각표로
 *   되풀이한다. zp_spin_once의 수신은 받을 것이 없으면 소켓 timeout(Z_CONFIG_SOCKET_TIMEOUT, MT0 빌드는 1 ms)만큼
 *   소켓에서 기다리므로, 이 프로그램이 따로 쉬지 않아도 할 일이 없을 때 CPU를 쓰지 않는다.
 *
 * 두 프로그램은 각 PC의 test_start.sh로 한 회차씩 짝을 맞춰 띄우고 함께 끝낸다. 다른 실행의 보고가 섞이지 않게 한다.
 *   - 받는 쪽: 상대 count의 seq가 줄면(보내는 쪽이 다시 시작됨) 보고를 멈추고 그 실행을 끝낸다.
 *   - 보내는 쪽: 자기가 보낸 것보다 큰 seq를 반영했다는 status는 다른 실행의 것이므로 버린다.
 *
 * 받는 쪽(B)을 반드시 먼저 띄운다. zenoh-pico publisher는 선언하는 순간에 이미 있는 상대 subscriber에게만 보낸다
 * (라우터 없는 peer 모드). A가 먼저 뜨면 A는 아무것도 보내지 않고 워밍업 뒤 "받는 쪽 보고가 오지 않음"으로 멈춘다.
 */
#ifndef CPULOAD_MEASURE_H
#define CPULOAD_MEASURE_H

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <zenoh-pico.h>

#include "cpu_usage.h"

#define CPULOAD_PAYLOAD_BYTES 128 /* 데이터 메시지 크기 */
#define CPULOAD_COUNT_BYTES 4     /* count 메시지 크기 */
#define CPULOAD_STATUS_BYTES 24   /* status 메시지 크기 (cpuload_report_t를 순서대로 담은 것) */

#define CPULOAD_DEFAULT_RATE 500000u /* 송신량을 주지 않았을 때의 값 (msg/s) */
#define CPULOAD_MIN_RATE 1000u       /* 고를 수 있는 송신량의 범위 (msg/s) */
#define CPULOAD_MAX_RATE 1000000u

/* 스레드 사이에 나누는 값. GCC/Clang의 __atomic 내장 함수로 읽고 쓴다 (cpu_load_measure.c) */
typedef struct {
    uint32_t value;
} cpuload_atomic_u32_t;

typedef struct {
    bool value;
} cpuload_atomic_bool_t;

/* 측정 설정 (각 main의 #define 값과 실행 인자로 채운다) */
typedef struct {
    bool is_publisher;  /* true: 보내는 쪽, false: 받는 쪽 */
    uint32_t rate;      /* 보내는 쪽: 송신량 (msg/s) */
    uint32_t warmup_s;  /* 보내는 쪽: 측정 구간 앞에 보내는 시간 (초) */
    uint32_t measure_s; /* 보내는 쪽: 측정 구간 (초) */
    uint32_t drain_s; /* 송신을 멈춘 뒤 늦게 오는 메시지와 마지막 status를 기다리는 시간 (초) */
} cpuload_options_t;

/* 받는 쪽이 status로 보내는 것 */
typedef struct {
    uint32_t received;     /* 누적 수신 수 (순서 바뀜·중복 제외) */
    uint32_t unreceived;   /* 누적 미수신 수 (상대 count 기준) */
    uint32_t counted_seq;  /* 미수신 계산에 쓴 상대 count의 seq (0 = 아직 없음) */
    uint32_t out_of_order; /* 누적: 앞 seq 이하가 온 수 (수신 수에서 뺌) */
    cpuload_usage_t cpu;   /* 보낸 순간 받는 쪽의 누적 CPU 시간 */
} cpuload_report_t;

/* 보내는 쪽 단계 */
typedef enum {
    CPULOAD_PHASE_WARMUP = 0, /* 보내면서 받는 쪽이 받기 시작하기를 기다린다 */
    CPULOAD_PHASE_MEASURE,    /* 측정 구간 */
    CPULOAD_PHASE_DRAIN,      /* 송신을 멈추고 마지막 status를 기다린다 */
    CPULOAD_PHASE_DONE,
} cpuload_phase_t;

/* 제어기 하나의 측정 상태 전체 */
typedef struct {
    const char* my_name;
    const char* peer_name;
    cpuload_options_t options;
    const z_owned_session_t* session;
    uint64_t start_us;
    cpuload_meter_t meter;

    z_owned_publisher_t msg_publisher;
    z_owned_publisher_t count_publisher;
    z_owned_publisher_t status_publisher;
    z_owned_subscriber_t msg_subscriber;
    z_owned_subscriber_t count_subscriber;
    z_owned_subscriber_t status_subscriber;

    /* 송신 (스케줄러만 쓴다. 다른 스레드가 읽는 카운터만 atomic) */
    uint32_t next_seq;      /* 다음 송신에 쓸 seq (1부터). 실패하면 같은 seq로 다시 보낸다 */
    uint64_t next_count_us; /* 다음 count를 보낼 시각 (0 = 아직 정하지 않음) */
    cpuload_atomic_u32_t published;     /* 보낸 수 (= 마지막으로 보낸 seq) */
    cpuload_atomic_u32_t skipped_ticks; /* put 실패로 나머지를 건너뛴 1 ms 구간 수 */

    /* 수신 (state_lock) */
    pthread_mutex_t state_lock;
    bool has_first;             /* 첫 메시지를 받았는가 */
    uint32_t first_seq;         /* 처음 받은 seq. 미수신은 여기부터 센다 */
    uint32_t last_seq;          /* 가장 최근 seq */
    uint32_t received;          /* 누적 수신 수 (순서 바뀜·중복 제외) */
    uint32_t out_of_order;      /* 앞 seq 이하가 온 수 */
    uint32_t received_at_count; /* 상대 count가 도착한 순간의 received */
    bool has_count_mail;        /* 처리하지 않은 count가 있는가 */
    uint32_t count_mail_seq;    /* 그 count의 seq */
    uint64_t last_count_us;     /* 상대 count를 마지막으로 받은 시각 (0 = 아직 없음) */
    bool has_count_seq;         /* 상대 count를 하나라도 처리했는가 */
    uint32_t last_count_seq;    /* 직전에 처리한 상대 count의 seq. 한 실행 안에서는 줄지 않는다 */
    bool is_sender_restarted;   /* 받는 쪽: count의 seq가 줄어 보내는 쪽이 다시 시작된 것을 봤다 */
    cpuload_report_t report;    /* 받는 쪽: 보낼 수신 결과 */
    bool has_status_mail;       /* 처리하지 않은 status가 있는가 */
    uint8_t status_mail[CPULOAD_STATUS_BYTES];
    size_t wrong_status_bytes;  /* 크기가 달라 버린 status의 크기 (0 = 없음) */
    uint32_t foreign_status;    /* 다른 실행의 것으로 보고 버린 status 수 */
    bool has_peer_view;         /* 보내는 쪽: 받는 쪽 보고를 받았는가 */
    cpuload_report_t peer_view; /* 보내는 쪽: 가장 최근 받는 쪽 보고 */
    uint64_t last_status_us;    /* 보내는 쪽: 받는 쪽 보고를 마지막으로 받은 시각 */

    /* 보내는 쪽 측정 구간 (피드백 전용) */
    cpuload_phase_t phase;
    uint64_t phase_start_us;
    uint64_t window_start_us;
    uint64_t window_end_us;
    uint32_t window_start_published;
    uint32_t window_end_published;
    uint32_t window_start_skipped;
    uint32_t window_end_skipped;
    cpuload_usage_t window_start_mine;
    cpuload_usage_t window_end_mine;
    cpuload_usage_t window_start_peer;
    cpuload_usage_t window_end_peer;
    bool is_aborted;          /* 받는 쪽 보고가 없거나 끊겨 측정을 멈췄다 */
    const char* abort_reason; /* 멈춘 까닭 (출력용) */

    /* 1초 출력 (피드백 전용) */
    uint32_t feedback_ticks;
    uint32_t print_seconds;
    uint32_t last_second_published;
    uint32_t last_second_received;
    cpuload_usage_t last_second_cpu;
    bool is_status_warned; /* 버린 status를 이미 알렸는가 */
    bool is_peer_never_seen; /* 받는 쪽: 시작한 뒤 상대 count가 한 번도 오지 않아 기다리기를 멈췄다 (main 전용) */

    cpuload_atomic_bool_t is_ready; /* 콜백이 상태를 써도 되는가 */
    cpuload_atomic_bool_t is_done;  /* 보내는 쪽: 측정을 마쳤다 (main이 읽는다) */
    cpuload_atomic_bool_t should_stop_publish;
    cpuload_atomic_bool_t should_stop_feedback;
    pthread_t scheduler_thread;
    pthread_t feedback_thread;
} cpuload_node_t;

/* 보내는 쪽 실행 인자에서 송신량 (msg/s)을 읽는다 (1,000 ~ 1,000,000). 인자가 없거나 빈 문자열이면 500,000.
 * 그 밖의 값이면 오류를 출력하고 false를 돌려준다. */
bool cpuload_parse_rate(int argc, char** argv, uint32_t* rate);

/* 열린 세션으로 측정을 실행한다. publisher/subscriber 선언과 해제도 여기서 한다.
 * 정상이면 0, 측정을 멈췄거나(받는 쪽 보고 없음 등) 실패하면 1. */
int cpuload_run(cpuload_node_t* node, const z_owned_session_t* session, const char* my_name, const char* peer_name,
                const cpuload_options_t* options, volatile sig_atomic_t* is_stop_requested);

#endif /* CPULOAD_MEASURE_H */
