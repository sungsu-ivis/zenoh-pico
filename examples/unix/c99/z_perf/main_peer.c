/*
 * 네트워크 성능 측정 예제 (zenoh-pico) - 제어기 B (받는 쪽)
 *
 * brightonz.foundation의 z_perf 예제와 같은 측정을 zenoh-pico로 한다. 구조는 perf_measure.h 참고.
 * 세션      : peer 모드. UDP multicast (PERF_LOCATOR), 또는 TCP unicast (PERF_TRANSPORT_TCP=1로 빌드한 z_perf_tcp_*)
 *             TCP는 B(이쪽)가 PERF_LOCATOR 주소로 A의 접속을 기다린다.
 *             #iface=는 이 프로그램을 돌릴 PC(받는 쪽)의 NIC 이름으로 바꾸고 빌드한다.
 * 받는 것   : perf/msg/a/b/num0, perf/report/a/b/count
 * 보내는 것 : perf/report/b/a/status (1초마다 수신 결과를 A에게)
 *
 * 실행: 받는 쪽 PC에서 build/examples/z_perf/test_start.sh peer
 *       (TCP는 build/examples/z_perf_tcp/test_start.sh peer)
 *       (이쪽을 먼저 띄운 뒤 보내는 쪽 PC에서 test_start.sh tester를 띄운다)
 *       A가 끝나면(count가 5초 동안 끊기면) 요약을 출력하고 스스로 끝난다.
 */
#include <signal.h>
#include <stdio.h>
#include <zenoh-pico.h>

#include "perf_measure.h"

#define PERF_MY_NAME "b"
#define PERF_PEER_NAME "a"

/* 측정 설정 */
#ifndef PERF_TRANSPORT_TCP
#define PERF_TRANSPORT_TCP 0 /* 1: TCP unicast. CMake가 z_perf_tcp_* 대상(build/examples/z_perf_tcp/)에 1을 준다 */
#endif
#if PERF_TRANSPORT_TCP
#define PERF_LOCATOR "tcp/169.254.236.32:19155" /* B(받는 쪽) PC의 IP와 포트. 두 PC 모두 같은 값 */
#else
#define PERF_LOCATOR "udp/239.192.0.78:19153#iface=enp7s0" /* multicast 그룹과 이 PC의 NIC */
#endif
#define PERF_DRAIN_S 2u                                   /* 송신을 멈춘 뒤 늦게 오는 메시지를 기다리는 시간 (초) */
#define PERF_IS_PUBLISHER false                           /* true: 보내는 쪽(최대 처리량 탐색), false: 받는 쪽 */

#if Z_FEATURE_MULTI_THREAD == 1 && Z_FEATURE_PUBLICATION == 1 && Z_FEATURE_SUBSCRIPTION == 1 && \
    Z_FEATURE_BATCHING == 1 &&                                                                   \
    ((PERF_TRANSPORT_TCP && Z_FEATURE_LINK_TCP == 1 && Z_FEATURE_UNICAST_PEER == 1) ||           \
     (!PERF_TRANSPORT_TCP && Z_FEATURE_LINK_UDP_MULTICAST == 1))
static volatile sig_atomic_t perf_is_stop_requested = 0;

static void perf_on_signal(int signal_number) {
    (void)signal_number;
    perf_is_stop_requested = 1;
}

int main(void) {
    static perf_node_t node; /* 콜백이 쓰는 측정 상태. 선언 전에 0으로 준비돼 있어야 한다 */
    const perf_options_t options = {
        .drain_s = PERF_DRAIN_S,
        .is_publisher = PERF_IS_PUBLISHER,
    };

    signal(SIGINT, perf_on_signal);
    signal(SIGTERM, perf_on_signal);

    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_config_loan_mut(&config), Z_CONFIG_MODE_KEY, "peer");
    zp_config_insert(z_config_loan_mut(&config), Z_CONFIG_LISTEN_KEY, PERF_LOCATOR);

    z_owned_session_t session;
    if (z_open(&session, z_config_move(&config), NULL) != Z_OK) {
        fprintf(stderr, "세션을 열지 못했습니다: %s\n", PERF_LOCATOR);
        return 1;
    }
    /* z_open은 태스크를 띄우지 않는다. 읽기 태스크는 수신 콜백을, lease 태스크는 keep-alive(multicast는 JOIN도)를 맡는다 */
    if ((zp_start_read_task(z_session_loan_mut(&session), NULL) != Z_OK) ||
        (zp_start_lease_task(z_session_loan_mut(&session), NULL) != Z_OK)) {
        fprintf(stderr, "읽기·lease 태스크를 시작하지 못했습니다\n");
        z_session_drop(z_session_move(&session));
        return 1;
    }
#if PERF_TRANSPORT_TCP
    printf("zenoh-pico %s | %s | unicast batch %u B\n", ZENOH_PICO, PERF_LOCATOR, (unsigned)Z_BATCH_UNICAST_SIZE);
#else
    printf("zenoh-pico %s | %s | multicast batch %u B\n", ZENOH_PICO, PERF_LOCATOR, (unsigned)Z_BATCH_MULTICAST_SIZE);
#endif

    const int result = perf_run(&node, &session, PERF_MY_NAME, PERF_PEER_NAME, &options, &perf_is_stop_requested);

    z_session_drop(z_session_move(&session));
    return result;
}
#else
int main(void) {
    printf("ERROR: z_perf needs Z_FEATURE_MULTI_THREAD, PUBLICATION, SUBSCRIPTION, BATCHING and\n"
           "       LINK_UDP_MULTICAST (or LINK_TCP and UNICAST_PEER for TCP).\n");
    return -2;
}
#endif
