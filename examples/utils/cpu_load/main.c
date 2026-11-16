/*
 * CPU 부하 측정 예제 (zenoh-pico) - 제어기 A (보내는 쪽: 정해진 송신량으로 보내며 두 PC의 CPU를 잰다)
 *
 * brightonz.foundation의 cpu_load 예제와 같은 측정을 zenoh-pico로 한다. 구조는 cpu_load_measure.h 참고.
 * 세션      : peer 모드. UDP multicast (CPULOAD_LOCATOR)
 *             또는 TCP unicast (CPULOAD_TRANSPORT_TCP=1로 빌드한 cpu_load_tcp_*)
 *             TCP는 B가 CPULOAD_LOCATOR 주소로 연결을 기다리고 A가 그 주소로 접속한다.
 *             #iface=는 이 프로그램을 돌릴 PC(보내는 쪽)의 NIC 이름으로 바꾸고 빌드한다.
 * 보내는 것 : cpu/msg/a/b/num0, cpu/report/a/b/count
 * 받는 것   : cpu/report/b/a/status (B의 수신 결과와 누적 CPU 시간)
 *
 * 실행: 보내는 쪽 PC에서 build/examples/cpu_load/test_start.sh tester
 *       (TCP는 build/examples/cpu_load_tcp/test_start.sh tester)
 *       (받는 쪽 PC에서 test_start.sh peer를 먼저 띄운다)
 *       측정이 끝나면 결과를 출력하고 스스로 끝난다.
 *       직접 실행할 때: cpu_load_tester [송신량 msg/s]. 주지 않으면 500000.
 *       test_start.sh는 RATE 값을 이 인자로 넘긴다.
 */
#include <signal.h>
#include <stdio.h>
#include <zenoh-pico.h>

#include "cpu_load_measure.h"

/* 빌드 설정값을 문자열로 남긴다 (실행 첫 줄 출력) */
#define CPULOAD_STR_(x) #x
#define CPULOAD_STR(x) CPULOAD_STR_(x)

#define CPULOAD_MY_NAME "a"
#define CPULOAD_PEER_NAME "b"

/* 측정 설정 */
#ifndef CPULOAD_TRANSPORT_TCP
#define CPULOAD_TRANSPORT_TCP 0 /* 1: TCP unicast. CMake가 cpu_load_tcp_* 대상에 1을 준다 */
#endif
#if CPULOAD_TRANSPORT_TCP
#define CPULOAD_LOCATOR "tcp/10.0.128.111:19165" /* B(받는 쪽) PC의 IP와 포트. 두 PC 모두 같은 값 */
#else
#define CPULOAD_LOCATOR "udp/239.192.0.80:19163#iface=enp7s0" /* multicast 그룹과 이 PC의 NIC */
#endif
#define CPULOAD_WARMUP_S 3u   /* 측정 구간 앞에 보내는 시간 (초) */
#define CPULOAD_MEASURE_S 10u /* 측정 구간 (초) */
#define CPULOAD_DRAIN_S 2u    /* 송신을 멈춘 뒤 마지막 보고를 기다리는 시간 (초) */

#if Z_FEATURE_PUBLICATION == 1 && Z_FEATURE_SUBSCRIPTION == 1 && Z_FEATURE_BATCHING == 1 && \
    ((CPULOAD_TRANSPORT_TCP && Z_FEATURE_LINK_TCP == 1 && Z_FEATURE_UNICAST_PEER == 1) ||   \
     (!CPULOAD_TRANSPORT_TCP && Z_FEATURE_LINK_UDP_MULTICAST == 1))
static volatile sig_atomic_t cpuload_is_stop_requested = 0;

static void cpuload_on_signal(int signal_number) {
    (void)signal_number;
    cpuload_is_stop_requested = 1;
}

int main(int argc, char** argv) {
    static cpuload_node_t node; /* 콜백이 쓰는 측정 상태. 선언 전에 0으로 준비돼 있어야 한다 */
    uint32_t rate;
    if (!cpuload_parse_rate(argc, argv, &rate)) return 1;
    const cpuload_options_t options = {
        .is_publisher = true,
        .rate = rate,
        .warmup_s = CPULOAD_WARMUP_S,
        .measure_s = CPULOAD_MEASURE_S,
        .drain_s = CPULOAD_DRAIN_S,
    };

    signal(SIGINT, cpuload_on_signal);
    signal(SIGTERM, cpuload_on_signal);

    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_config_loan_mut(&config), Z_CONFIG_MODE_KEY, "peer");
    /* multicast는 그룹에 들어가고(listen), TCP는 B에 접속한다(connect) */
    zp_config_insert(z_config_loan_mut(&config), CPULOAD_TRANSPORT_TCP ? Z_CONFIG_CONNECT_KEY : Z_CONFIG_LISTEN_KEY,
                     CPULOAD_LOCATOR);

    z_owned_session_t session;
    const z_result_t opened = z_open(&session, z_config_move(&config), NULL);
    if (opened != Z_OK) {
        fprintf(stderr, "세션을 열지 못했습니다: %s (오류 %d)\n", CPULOAD_LOCATOR, (int)opened);
        return 1;
    }
    /* MT1: z_open이 executor 스레드를 띄운다 (수신 콜백, keep-alive·JOIN, TCP accept).
     * MT0: 스레드가 없다. cpuload_run의 메인 루프가 zp_spin_once로 같은 일을 한다 */
#if CPULOAD_TRANSPORT_TCP
    printf("zenoh-pico %s | %s | unicast batch %u B | MULTI_THREAD=%d\n", ZENOH_PICO, CPULOAD_LOCATOR,
           (unsigned)Z_BATCH_UNICAST_SIZE, Z_FEATURE_MULTI_THREAD);
#else
    printf("zenoh-pico %s | %s | multicast batch %u B | MULTI_THREAD=%d | MULTICAST_DECLARATIONS=" CPULOAD_STR(
               Z_FEATURE_MULTICAST_DECLARATIONS) " MULTICAST_LOOPBACK=" CPULOAD_STR(Z_FEATURE_MULTICAST_LOOPBACK) "\n",
           ZENOH_PICO, CPULOAD_LOCATOR, (unsigned)Z_BATCH_MULTICAST_SIZE, Z_FEATURE_MULTI_THREAD);
#endif

    const int result =
        cpuload_run(&node, &session, CPULOAD_MY_NAME, CPULOAD_PEER_NAME, &options, &cpuload_is_stop_requested);

    z_session_drop(z_session_move(&session));
    return result;
}
#else
int main(void) {
    printf(
        "ERROR: cpu_load needs Z_FEATURE_PUBLICATION, SUBSCRIPTION, BATCHING and LINK_UDP_MULTICAST\n"
        "       (TCP: LINK_TCP and UNICAST_PEER).\n");
    return -2;
}
#endif
