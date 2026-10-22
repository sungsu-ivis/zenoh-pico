#!/usr/bin/env bash
# z_perf 최대 처리량 측정 실행 (PC 두 대)
#
# 보내는 쪽(제어기 A, z_perf_tester)과 받는 쪽(제어기 B, z_perf_peer)을 서로 다른 PC에서 띄운다.
# 같은 PC 안(loopback)에서는 패킷이 NIC 드라이버를 거치지 않으므로 측정하지 않는다.
# 두 PC는 UDP multicast 그룹 하나로 주고받는다. 그룹 주소와 NIC는 main.c / main_peer.c의 PERF_LOCATOR에 있고 바이너리에 그대로 들어간다.
#
# 순서
#   1. 받는 쪽 PC  : ./test_start.sh peer [로그 폴더]
#      z_perf_peer를 RUNS번 차례로 띄운다. 한 회차는 tester가 끝나 count가 5초 동안 끊기면 끝나고,
#      곧바로 다음 회차의 z_perf_peer를 띄워 기다린다.
#   2. 보내는 쪽 PC: ./test_start.sh tester [로그 폴더]
#      z_perf_tester를 RUNS번 실행하고 결과의 중앙값을 보고한다.
#      회차 사이에 RUN_GAP_S초 쉬어, 받는 쪽이 이전 회차를 끝내고 다시 뜰 시간을 준다.
#
# CPU 고정 (iperf3 -A와 같은 방식)
#   프로그램을 CPU 하나에 묶어 띄운다. 송신 스레드가 CPU를 옮겨 다니면 결과가 흔들린다.
#
# 결과 보고 (중앙값)
#   실행마다 결과가 흔들리는 환경에서는 한 번의 값보다 여러 번의 중앙값이 대표값으로 낫다.
#   RUNS는 홀수로 둔다. 그래야 중앙값이 실제 회차 하나의 결과가 된다. 두 PC의 RUNS는 같아야 한다.
#   30초 확인을 통과하지 못한 회차는 "확인 실패"로 표시하고, 그 회차의 탐색 결과를 그대로 쓴다.
#
# 측정 조건 기록
#   이 PC의 커널, CPU, 클럭 정책, NIC(이름·속도·MTU·IPv4·드라이버), multicast endpoint를 로그와 요약 맨 위에 남긴다.
#   회차마다 커널 UDP 카운터의 증가분(보낸·받은 데이터그램, 수신 버퍼 부족으로 버린 수)도 요약에 남긴다.
#
# 사용법: ./test_start.sh tester|peer [로그 폴더]
#   로그 폴더를 주지 않으면 지금 폴더에 회차별 tester_log_N.txt(또는 peer_log_N.txt)와 result_summary.txt를 쓴다.
#   한 회차는 보통 6~10분 걸린다 (RUNS=3이면 약 30분).
#   Ctrl+C를 누르면 지금 회차를 멈추고(요약을 남긴다), 그때까지 끝난 회차로 결과를 낸다.
#
# 빌드하면 이 파일이 build/examples/z_perf/test_start.sh로 복사되고, 같은 폴더의 bin/을 실행한다.

# source(. 명령)로 실행하면 아래의 exit가 터미널 셸까지 끝내 버리므로, 그 경우에는 실행하지 않는다
if [ "${BASH_SOURCE[0]}" != "$0" ]; then
    echo "test_start.sh는 source 하지 말고 실행하세요: ./test_start.sh tester|peer [로그 폴더]" >&2
    return 1
fi

set -u

ROLE="${1:-}"
LOG_DIR="${2:-$PWD}"
SUMMARY="${LOG_DIR}/result_summary.txt"

RUNS=3       # 측정 횟수 (홀수, 두 PC가 같아야 한다). 결과는 중앙값으로 보고한다
RUN_GAP_S=15 # tester: 회차 사이에 쉬는 시간. 받는 쪽이 이전 회차를 끝내고(count가 끊긴 뒤 약 7초) 다시 뜨는 시간보다 길어야 한다

PIN_CPUS=1   # 1: CPU 고정, 0: 고정하지 않음
TESTER_CPU=3 # 보내는 쪽 PC에서 z_perf_tester를 묶을 CPU 번호
PEER_CPU=2   # 받는 쪽 PC에서 z_perf_peer를 묶을 CPU 번호

# NIC 이름을 바꿀 곳 (빌드 전에 고친다)
IFACE_SOURCE="main.c(보내는 쪽), main_peer.c(받는 쪽)의 PERF_LOCATOR #iface="

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "$ROLE" in
    tester)
        EXE="${SCRIPT_DIR}/bin/z_perf_tester"
        PIN_CPU=$TESTER_CPU
        LOG_PREFIX="tester_log"
        ;;
    peer)
        EXE="${SCRIPT_DIR}/bin/z_perf_peer"
        PIN_CPU=$PEER_CPU
        LOG_PREFIX="peer_log"
        ;;
    *)
        echo "사용법: ./test_start.sh tester|peer [로그 폴더]" >&2
        echo "  받는 쪽 PC에서 peer를 먼저 띄우고, 보내는 쪽 PC에서 tester를 띄운다." >&2
        exit 1
        ;;
esac

if [ ! -x "$EXE" ]; then
    echo "실행 파일이 없습니다: $EXE" >&2
    echo "빌드 폴더의 test_start.sh(build/examples/z_perf/test_start.sh)를 실행하세요." >&2
    exit 1
fi

if [ "$RUNS" -lt 1 ] || [ $((RUNS % 2)) -ne 1 ]; then
    echo "RUNS는 1 이상의 홀수여야 합니다 (지금 ${RUNS})." >&2
    exit 1
fi

# multicast endpoint와 NIC: 바이너리에 들어 있는 endpoint 문자열에서 읽는다 (PERF_LOCATOR가 그대로 컴파일된다)
endpoint="$(grep -a -o 'udp/[0-9.]*:[0-9]*#iface=[A-Za-z0-9_.:-]*' "$EXE" | head -n 1)"
if [ -z "$endpoint" ]; then
    echo "바이너리에서 multicast endpoint(udp/주소:포트#iface=NIC)를 찾지 못했습니다: $EXE" >&2
    exit 1
fi
nic="${endpoint##*#iface=}"
if [ "$nic" = "lo" ]; then
    echo "NIC가 lo(loopback)입니다. loopback은 NIC 드라이버를 거치지 않으므로 측정에 쓰지 않습니다." >&2
    echo "${IFACE_SOURCE}를 이 PC의 NIC 이름으로 바꾸고 다시 빌드하세요." >&2
    exit 1
fi
if [ "$nic" = "auto" ]; then
    nic_text="NIC auto (라이브러리가 고름. 측정에는 이름을 직접 쓴다)"
else
    if [ ! -d "/sys/class/net/${nic}" ]; then
        echo "바이너리에 설정된 NIC '${nic}'이(가) 이 PC에 없습니다. 이 PC의 NIC: $(ls /sys/class/net | tr '\n' ' ')" >&2
        echo "${IFACE_SOURCE}를 이 PC의 NIC 이름으로 바꾸고 다시 빌드하세요." >&2
        exit 1
    fi
    nic_state="$(cat "/sys/class/net/${nic}/operstate" 2>/dev/null)"
    nic_ipv4="$(ip -4 -o addr show dev "$nic" 2>/dev/null | awk '{print $4}' | head -n 1)"
    if [ "$nic_state" = "down" ] || [ -z "$nic_ipv4" ]; then
        echo "NIC '${nic}'이(가) 꺼져 있거나 IPv4 주소가 없습니다 (상태 ${nic_state:-?}). 케이블과 IP 설정을 확인하세요." >&2
        exit 1
    fi
    nic_speed="$(cat "/sys/class/net/${nic}/speed" 2>/dev/null)"
    case "$nic_speed" in '' | -*) nic_speed="?" ;; esac
    nic_mtu="$(cat "/sys/class/net/${nic}/mtu" 2>/dev/null)"
    nic_driver="$(basename "$(readlink -f "/sys/class/net/${nic}/device/driver" 2>/dev/null)" 2>/dev/null)"
    nic_text="NIC ${nic} ${nic_speed} Mb/s MTU ${nic_mtu} ${nic_ipv4} ${nic_driver:-?}"
fi

# CPU 고정 준비: taskset이 있고, 고른 CPU가 이 PC에 있는지 확인한다
launch=()
if [ "$PIN_CPUS" -eq 1 ]; then
    if ! command -v taskset >/dev/null; then
        echo "taskset이 없습니다 (util-linux). 설치하거나 PIN_CPUS=0으로 바꾸세요." >&2
        exit 1
    fi
    if [ ! -d "/sys/devices/system/cpu/cpu${PIN_CPU}" ] || [ "$(cat "/sys/devices/system/cpu/cpu${PIN_CPU}/online" 2>/dev/null || echo 1)" != "1" ]; then
        echo "CPU ${PIN_CPU}이(가) 없거나 꺼져 있습니다. 쓸 수 있는 CPU: $(cat /sys/devices/system/cpu/online)" >&2
        exit 1
    fi
    launch=(taskset -c "$PIN_CPU")
fi

# 측정 조건 한 줄 (로그와 요약 맨 위에 남긴다)
is_wsl=0
grep -qi microsoft /proc/version 2>/dev/null && is_wsl=1
cpu_model="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ *//')"
if [ "$PIN_CPUS" -eq 1 ]; then
    pin_text="CPU 고정 ${ROLE}=${PIN_CPU}"
    governor="$(cat "/sys/devices/system/cpu/cpu${PIN_CPU}/cpufreq/scaling_governor" 2>/dev/null || echo '?')"
else
    pin_text="CPU 고정 안 함"
    governor="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo '?')"
fi
if [ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    turbo_text="터보 $([ "$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)" = "1" ] && echo 끔 || echo 켬)"
elif [ -r /sys/devices/system/cpu/cpufreq/boost ]; then
    turbo_text="부스트 $([ "$(cat /sys/devices/system/cpu/cpufreq/boost)" = "1" ] && echo 켬 || echo 끔)"
else
    turbo_text="터보 ?"
fi
conditions="# 측정 조건 (${ROLE}): $(date '+%Y-%m-%d %H:%M:%S') | $(hostname) | 커널 $(uname -r) | ${cpu_model} | CPU $(nproc)개 | ${pin_text} | 클럭 정책 ${governor} | ${turbo_text} | ${nic_text} | ${endpoint}"
[ "$is_wsl" -eq 1 ] && conditions="${conditions} | WSL2"

if pgrep -x "$(basename "$EXE")" >/dev/null; then
    echo "$(basename "$EXE")가 이미 실행 중입니다. 먼저 끝내 주세요." >&2
    exit 1
fi
mkdir -p "$LOG_DIR" || exit 1

child_pid=""
stop_requested=0

# Ctrl+C / kill: 프로그램에 SIGINT를 보낸다 (송신을 멈추고 요약을 남긴 뒤 끝난다). 다음 회차는 시작하지 않는다.
stop_current() {
    stop_requested=1
    echo "중단 요청: 지금 회차를 멈춥니다 (요약을 남기고 끝남). 다음 회차는 시작하지 않습니다."
    [ -n "$child_pid" ] && kill -INT "$child_pid" 2>/dev/null
}
trap stop_current INT TERM

# 프로세스가 끝날 때까지 기다리고 종료 코드를 돌려준다.
# wait는 시그널을 받으면 바로 돌아오므로, 아직 살아 있으면 다시 기다린다.
wait_for() {
    local pid=$1 status
    while :; do
        wait "$pid"
        status=$?
        kill -0 "$pid" 2>/dev/null || return "$status"
    done
}

# 커널 UDP 카운터 (/proc/net/snmp의 Udp: 값 줄). 출력: "InDatagrams OutDatagrams RcvbufErrors InErrors"
udp_counters() {
    awk '/^Udp:/ { if (seen) { print $2, $5, $6, $4 }; seen = 1 }' /proc/net/snmp
}

# 두 카운터의 차이를 한 줄로
udp_delta_text() {
    local before after
    read -r -a before <<<"$1"
    read -r -a after <<<"$2"
    printf '        UDP 카운터 증가: 보냄 %d, 받음 %d, 수신 버퍼 부족으로 버림 %d, 수신 오류 %d' \
        $((after[1] - before[1])) $((after[0] - before[0])) $((after[2] - before[2])) $((after[3] - before[3]))
}

# 한 회차를 실행한다. 성공하면 0을 돌려준다.
run_once() {
    local run=$1
    local log="${LOG_DIR}/${LOG_PREFIX}_${run}.txt"
    local status

    echo
    echo "===== ${run}/${RUNS}회 시작 (${ROLE}) → $log ====="
    echo "${conditions} | ${run}/${RUNS}회" >"$log"
    ${launch[@]+"${launch[@]}"} "$EXE" >>"$log" 2>&1 &
    child_pid=$!
    if [ "$ROLE" = "tester" ]; then
        echo "측정 중입니다 (6~10분). 진행 상황: tail -f $log"
    else
        echo "보내는 쪽(tester)을 기다립니다. tester가 끝나면 이 회차도 끝납니다. 진행 상황: tail -f $log"
    fi

    wait_for "$child_pid"
    status=$?
    child_pid=""

    echo "종료 코드: $status"
    if [ "$ROLE" = "tester" ]; then
        sed -n '/===== 최대 처리량/,$p' "$log"
    else
        sed -n '/===== 제어기 b 요약/,$p' "$log"
    fi
    [ "$status" -eq 0 ]
}

# 회차 로그에서 결과 송신량(msg/s)과 확인 상태를 읽는다. 출력: "송신량|상태"
result_of() {
    local log=$1 value state
    value=$(sed -n 's/^  결과 *: \([0-9][0-9]*\) msg\/s.*/\1/p' "$log" | head -n 1)
    [ -z "$value" ] && value=0 # 손실 없이 통과한 단계가 없음
    state="확인 실패"
    grep -q '^  확인 *: 목표 .*손실 없음' "$log" && state="확인 통과"
    grep -q '탐색이 끝나기 전에 멈춰' "$log" && state="중단됨"
    echo "${value}|${state}"
}

echo "${conditions#\# }"
if [ "$is_wsl" -eq 1 ]; then
    echo "WSL2: 네트워크 모드에 따라 multicast가 다른 PC까지 나가지 않을 수 있습니다. 성능 측정은 네이티브 리눅스에서 하세요."
fi
if [ "$ROLE" = "tester" ]; then
    echo "${RUNS}회 측정해 중앙값으로 보고합니다. 받는 쪽 PC에서 'test_start.sh peer'가 먼저 떠 있어야 합니다."
else
    echo "${RUNS}회 동안 받는 쪽을 띄웁니다. 이제 보내는 쪽 PC에서 'test_start.sh tester'를 실행하세요."
fi

failed_runs=0
done_runs=() # 중앙값에 넣을 회차 번호 (tester)
lines=()     # 회차별 결과 줄
for run in $(seq 1 "$RUNS"); do
    [ "$stop_requested" -eq 1 ] && break
    udp_before="$(udp_counters)"
    run_ok=1
    run_once "$run" || run_ok=0
    udp_after="$(udp_counters)"

    if [ "$ROLE" = "tester" ]; then
        IFS='|' read -r value state <<<"$(result_of "${LOG_DIR}/${LOG_PREFIX}_${run}.txt")"
    else
        value="-"
        state="수신 끝"
    fi
    if [ "$stop_requested" -eq 1 ]; then
        state="중단됨"
    elif [ "$run_ok" -eq 0 ]; then
        state="실행 오류 (로그 확인)"
        failed_runs=$((failed_runs + 1))
    fi
    if [ "$ROLE" = "tester" ]; then
        lines+=("$(printf '  %d회: %7s msg/s  %s' "$run" "$value" "$state")")
    else
        lines+=("$(printf '  %d회: %s' "$run" "$state")")
    fi
    lines+=("$(udp_delta_text "$udp_before" "$udp_after")")
    # 중단·실행 오류 회차는 중앙값에서 뺀다
    [ "$stop_requested" -eq 0 ] && [ "$run_ok" -eq 1 ] && done_runs+=("${value} ${run}")
    if [ "$ROLE" = "tester" ] && [ "$run" -lt "$RUNS" ] && [ "$stop_requested" -eq 0 ]; then
        echo "다음 회차까지 ${RUN_GAP_S}초 쉽니다 (받는 쪽이 다시 뜰 시간)."
        sleep "$RUN_GAP_S"
    fi
done

# 요약
#   tester: 회차별 결과와 중앙값 (중단된 회차는 빼고, 짝수 개가 남으면 가운데 두 값 중 작은 쪽)
#   peer  : 회차별 상태와 UDP 카운터 (결과는 보내는 쪽 PC의 요약에 있다)
{
    echo "$conditions"
    echo
    if [ "$ROLE" = "peer" ]; then
        echo "===== 받는 쪽 요약 (결과 송신량은 보내는 쪽 PC의 result_summary.txt에 있다) ====="
        for line in "${lines[@]}"; do echo "$line"; done
    else
        echo "===== 결과 요약 (${#done_runs[@]}회 중앙값, 손실 0%, 메시지 128 B) ====="
        for line in "${lines[@]}"; do echo "$line"; done
        if [ "${#done_runs[@]}" -eq 0 ]; then
            echo "  끝까지 마친 회차가 없어 중앙값을 낼 수 없습니다."
        else
            median_index=$(((${#done_runs[@]} + 1) / 2))
            median_line=$(printf '%s\n' "${done_runs[@]}" | sort -n -k1,1 | sed -n "${median_index}p")
            median_value=${median_line%% *}
            median_run=${median_line##* }
            confirmed=0
            for run_info in "${done_runs[@]}"; do
                r=${run_info##* }
                [ "$(result_of "${LOG_DIR}/${LOG_PREFIX}_${r}.txt" | cut -d'|' -f2)" = "확인 통과" ] && confirmed=$((confirmed + 1))
            done
            echo "  중앙값: ${median_value} msg/s (${median_run}회 결과, ${#done_runs[@]}회 중 30초 확인 통과 ${confirmed}회)"
            echo
            echo "중앙값 회차(${median_run}회)의 결과:"
            sed -n '/===== 최대 처리량/,$p' "${LOG_DIR}/${LOG_PREFIX}_${median_run}.txt"
        fi
    fi
} >"$SUMMARY"

echo
cat "$SUMMARY"
echo
echo "요약을 저장했습니다: $SUMMARY"

[ "$failed_runs" -eq 0 ] && [ "$stop_requested" -eq 0 ]
