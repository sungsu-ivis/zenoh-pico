#!/usr/bin/env bash
# cpu_load CPU 부하 측정 실행 (PC 두 대, zenoh-pico)
#
# 보내는 쪽(제어기 A, cpu_load_tester)이 정해진 송신량으로 보내는 동안 두 PC의 CPU 부하를 잰다.
# 같은 양의 일을 할 때의 CPU를 라이브러리, MT1·MT0, 전송 방식끼리 비교하는 값이다.
# 받는 쪽(제어기 B, cpu_load_peer)은 다른 PC에서 띄운다. 같은 PC 안(loopback)에서는 측정하지 않는다.
# 두 PC는 UDP multicast 그룹 하나로 주고받는다. 그룹 주소와 NIC는 main.c / main_peer.c의 CPULOAD_LOCATOR에 있고 바이너리에 그대로 들어간다.
# TCP unicast로 잴 때는 build/examples/cpu_load_tcp/의 이 스크립트를 쓴다 (같은 스크립트, TCP로 빌드한 bin/).
#   받는 쪽이 자기 IP로 TCP 연결을 기다리고, 보내는 쪽이 그 IP로 접속한다. 받는 쪽 IP는 endpoint(tcp/IP:포트)에 있다.
#
# 순서
#   1. 받는 쪽 PC  : ./test_start.sh peer [로그 폴더]
#      cpu_load_peer를 RUNS번 차례로 띄운다. 한 회차는 tester가 끝나 count가 5초 동안 끊기면 끝나고,
#      곧바로 다음 회차의 cpu_load_peer를 띄워 기다린다.
#   2. 보내는 쪽 PC: ./test_start.sh tester [로그 폴더]
#      cpu_load_tester를 RUNS번 실행하고, 유효한 회차들의 두 PC CPU 부하 중앙값을 보고한다.
#      회차 사이에 RUN_GAP_S초 쉬어, 받는 쪽이 이전 회차를 끝내고 다시 뜰 시간을 준다.
#
# 송신량
#   RATE(msg/s)로 고른다. 주지 않으면 500000. 회선이 받아 주는 양(1 GbE, 128 B 메시지 약 870,000)보다 한참 낮게 잡아
#   송신이 막히지 않게 한다. 비교하려는 조합은 모두 같은 RATE로 잰다. 예: RATE=300000 ./test_start.sh tester
#
# CPU 고정 (iperf3 -A와 같은 방식)
#   프로그램을 CPU 하나에 묶어 띄운다. CPU 부하는 그 코어 하나의 값이다. 두 PC 모두 같은 번호(PIN_CPU)의 CPU에 묶는다.
#
# 클럭 설정
#   측정 전에 모든 CPU의 주파수 정책(CPU_GOVERNOR)과 터보(TURBO)를 정해진 값으로 맞춘다 (sudo 필요).
#   두 PC의 클럭 조건이 같아야 결과를 비교할 수 있다. 맞추지 못하면 측정하지 않는다.
#   측정이 끝나도 되돌리지 않는다 (재부팅하거나 직접 되돌린다).
#
# 결과 보고 (중앙값)
#   RUNS는 홀수로 둔다. 두 PC의 RUNS는 같아야 한다. "유효"가 아닌 회차(목표만큼 못 보냄, 미수신 1% 초과, 측정하지 못함)는
#   중앙값에서 빼고, 보내는 쪽과 받는 쪽 값의 중앙값을 따로 낸다.
#
# 측정 조건 기록
#   이 PC의 커널, CPU, 클럭 정책, NIC(이름·속도·MTU·IPv4·드라이버), endpoint를 로그와 요약 맨 위에 남긴다.
#   회차마다 커널 UDP 카운터의 증가분(보낸·받은 데이터그램, 수신 버퍼 부족으로 버린 수)도 요약에 남긴다.
#
# 사용법: ./test_start.sh tester|peer [로그 폴더]
#   로그 폴더를 주지 않으면 지금 폴더에 회차별 tester_log_N.txt(또는 peer_log_N.txt)와 result_summary.txt를 쓴다.
#   한 회차는 약 20초 걸린다 (워밍업 3초 + 측정 10초 + 마무리).
#   Ctrl+C를 누르면 지금 회차를 멈추고(요약을 남긴다), 그때까지 끝난 회차로 결과를 낸다.
#
# 빌드하면 이 파일이 build/examples/cpu_load/test_start.sh(TCP는 cpu_load_tcp/)로 복사되고, 같은 폴더의 bin/을 실행한다.

# source(. 명령)로 실행하면 아래의 exit가 터미널 셸까지 끝내 버리므로, 그 경우에는 실행하지 않는다
if [ "${BASH_SOURCE[0]}" != "$0" ]; then
    echo "test_start.sh는 source 하지 말고 실행하세요: ./test_start.sh tester|peer [로그 폴더]" >&2
    return 1
fi

set -u

ROLE="${1:-}"
LOG_DIR="${2:-$PWD}"
SUMMARY="${LOG_DIR}/result_summary.txt"

RATE="${RATE:-500000}" # 송신량 (msg/s): 1000 ~ 1000000. 보내는 쪽(tester)에만 쓰인다

RUNS=3       # 측정 횟수 (홀수, 두 PC가 같아야 한다). 결과는 중앙값으로 보고한다
RUN_GAP_S=9  # tester: 회차 사이에 쉬는 시간. 받는 쪽이 이전 회차를 끝내고(count가 끊긴 뒤 약 7초) 다시 뜨는 시간보다 길어야 한다

PIN_CPUS=1 # 1: CPU 고정, 0: 고정하지 않음
PIN_CPU=3  # 프로그램을 묶을 CPU 번호 (두 PC가 같은 번호를 쓴다)

CPU_GOVERNOR=performance # 측정 전에 맞출 주파수 정책 (모든 CPU)
TURBO=off                # 측정 전에 맞출 터보(부스트): off 또는 on

# NIC 이름(multicast)과 받는 쪽 IP(TCP)를 바꿀 곳 (빌드 전에 고친다)
IFACE_SOURCE="main.c(보내는 쪽), main_peer.c(받는 쪽)의 CPULOAD_LOCATOR #iface="
TCP_IP_SOURCE="main.c(보내는 쪽), main_peer.c(받는 쪽)의 TCP CPULOAD_LOCATOR tcp/IP"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "$ROLE" in
    tester)
        EXE="${SCRIPT_DIR}/bin/cpu_load_tester"
        EXE_ARGS=("$RATE")
        LOG_PREFIX="tester_log"
        if ! [[ "$RATE" =~ ^[0-9]+$ ]] || [ "$RATE" -lt 1000 ] || [ "$RATE" -gt 1000000 ]; then
            echo "RATE(송신량 msg/s)는 1000 ~ 1000000이어야 합니다 (지금 '${RATE}'). 주지 않으면 500000입니다." >&2
            exit 1
        fi
        ;;
    peer)
        EXE="${SCRIPT_DIR}/bin/cpu_load_peer"
        EXE_ARGS=()
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
    echo "빌드 폴더의 test_start.sh(build/examples/cpu_load/test_start.sh)를 실행하세요." >&2
    exit 1
fi

if [ "$RUNS" -lt 1 ] || [ $((RUNS % 2)) -ne 1 ]; then
    echo "RUNS는 1 이상의 홀수여야 합니다 (지금 ${RUNS})." >&2
    exit 1
fi

# endpoint와 NIC: 바이너리에 들어 있는 endpoint 문자열에서 읽는다 (CPULOAD_LOCATOR가 그대로 컴파일된다)
#   UDP multicast: udp/그룹:포트#iface=NIC → NIC는 #iface= 값
#   TCP unicast  : tcp/받는쪽IP:포트       → 받는 쪽은 그 IP를 가진 NIC, 보내는 쪽은 그 IP로 가는 경로의 NIC
endpoint="$(grep -a -o 'udp/[0-9.]*:[0-9]*#iface=[A-Za-z0-9_.:-]*' "$EXE" | head -n 1)"
if [ -n "$endpoint" ]; then
    nic="${endpoint##*#iface=}"
else
    endpoint="$(grep -a -o 'tcp/[0-9][0-9.]*:[0-9][0-9]*' "$EXE" | head -n 1)"
    if [ -z "$endpoint" ]; then
        echo "바이너리에서 endpoint(udp/주소:포트#iface=NIC 또는 tcp/IP:포트)를 찾지 못했습니다: $EXE" >&2
        exit 1
    fi
    tcp_ip="${endpoint#tcp/}"
    tcp_ip="${tcp_ip%:*}"
    own_nic="$(ip -o -4 addr show 2>/dev/null | awk -v ip="$tcp_ip" '{ split($4, a, "/"); if (a[1] == ip) { print $2; exit } }')"
    if [ "$ROLE" = "peer" ]; then
        # 받는 쪽: 이 PC가 그 IP를 가지고 있어야 그 IP로 연결을 기다릴 수 있다
        if [ -z "$own_nic" ]; then
            echo "이 PC에 TCP 주소 ${tcp_ip}이(가) 없습니다. 이 PC의 IPv4: $(ip -o -4 addr show | awk '{print $4}' | tr '\n' ' ')" >&2
            echo "${TCP_IP_SOURCE}를 받는 쪽 PC의 IP로 바꾸고 두 PC 모두 다시 빌드하세요." >&2
            exit 1
        fi
        nic="$own_nic"
    elif [ -n "$own_nic" ]; then
        nic="lo" # 보내는 쪽이 자기 IP로 접속하면 같은 PC 안(loopback)이다
    else
        nic="$(ip -o route get "$tcp_ip" 2>/dev/null | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }')"
        if [ -z "$nic" ]; then
            echo "TCP 주소 ${tcp_ip}로 가는 경로(NIC)를 찾지 못했습니다. 케이블과 IP 설정을 확인하세요." >&2
            exit 1
        fi
    fi
fi
if [ "$nic" = "lo" ]; then
    echo "NIC가 lo(loopback)입니다. loopback은 NIC 드라이버를 거치지 않으므로 측정에 쓰지 않습니다." >&2
    if [ "${endpoint#tcp/}" != "$endpoint" ]; then
        echo "${TCP_IP_SOURCE}를 다른 PC(받는 쪽)의 IP로 바꾸고 다시 빌드하세요." >&2
    else
        echo "${IFACE_SOURCE}를 이 PC의 NIC 이름으로 바꾸고 다시 빌드하세요." >&2
    fi
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

# 리눅스 프로세스 이름(comm)은 앞 15자만 남으므로 그만큼 잘라서 찾는다
if pgrep -x "$(basename "$EXE" | cut -c1-15)" >/dev/null; then
    echo "$(basename "$EXE")가 이미 실행 중입니다. 먼저 끝내 주세요." >&2
    exit 1
fi

# 클럭 설정 맞추기: 모든 CPU의 주파수 정책과 터보를 위에서 정한 값으로 바꾸고(sudo), 다시 읽어 확인한다.
# cpufreq가 없는 환경(WSL, VM 등)은 바꿀 수 없으므로 경고만 하고 측정한다.
governor_files=(/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor)
turbo_file=""
turbo_want=""
if [ -e /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    turbo_file=/sys/devices/system/cpu/intel_pstate/no_turbo # 1이면 터보 끔
    turbo_want=$([ "$TURBO" = "off" ] && echo 1 || echo 0)
elif [ -e /sys/devices/system/cpu/cpufreq/boost ]; then
    turbo_file=/sys/devices/system/cpu/cpufreq/boost # 1이면 부스트 켬
    turbo_want=$([ "$TURBO" = "off" ] && echo 0 || echo 1)
fi
turbo_label=$([ "$TURBO" = "off" ] && echo 끔 || echo 켬)
read_clock() {
    governor_now="$(sort -u "${governor_files[@]}" | tr '\n' ' ' | sed 's/ $//')"
    turbo_now="$([ -n "$turbo_file" ] && cat "$turbo_file")"
}
if [ ! -e "${governor_files[0]}" ]; then
    echo "경고: 이 환경에서는 주파수 정책을 바꿀 수 없습니다 (cpufreq 없음). 클럭 설정 없이 측정합니다." >&2
else
    read_clock
    if [ "$governor_now" != "$CPU_GOVERNOR" ] || [ "$turbo_now" != "$turbo_want" ]; then
        echo "클럭 설정을 주파수 정책 ${CPU_GOVERNOR}, 터보 ${turbo_label}으로 맞춥니다 (지금: 정책 ${governor_now}, ${turbo_file:-터보 설정 없음} = ${turbo_now:-?})."
        echo "sudo 비밀번호를 물을 수 있습니다. 측정이 끝나도 이 설정은 그대로 둡니다."
        echo "$CPU_GOVERNOR" | sudo tee "${governor_files[@]}" >/dev/null
        [ -n "$turbo_file" ] && echo "$turbo_want" | sudo tee "$turbo_file" >/dev/null
        read_clock
    fi
    if [ "$governor_now" != "$CPU_GOVERNOR" ] || [ "$turbo_now" != "$turbo_want" ]; then
        echo "클럭 설정을 맞추지 못해 측정하지 않습니다 (정책 ${governor_now}, ${turbo_file:-터보 설정 없음} = ${turbo_now:-?})." >&2
        echo "쓸 수 있는 정책: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors 2>/dev/null)" >&2
        exit 1
    fi
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
    ${launch[@]+"${launch[@]}"} "$EXE" ${EXE_ARGS[@]+"${EXE_ARGS[@]}"} >>"$log" 2>&1 &
    child_pid=$!
    if [ "$ROLE" = "tester" ]; then
        echo "측정 중입니다 (약 20초). 진행 상황: tail -f $log"
    else
        echo "보내는 쪽(tester)을 기다립니다. tester가 끝나면 이 회차도 끝납니다. 진행 상황: tail -f $log"
    fi

    wait_for "$child_pid"
    status=$?
    child_pid=""

    echo "종료 코드: $status"
    if [ "$ROLE" = "tester" ]; then
        sed -n '/===== CPU 부하/,$p' "$log"
    else
        sed -n '/===== 제어기 b 요약/,$p' "$log"
    fi
    [ "$status" -eq 0 ]
}

# 회차 로그의 결과에서 값을 읽는다. 출력: "보내는쪽%|보내는쪽µs|받는쪽%|받는쪽µs|판정" (없는 값은 -)
#   결과 줄 예:   보내는 쪽   : 31.2% (메시지당 0.62 µs)
#                 받는 쪽     : 21.5% (메시지당 0.43 µs)
#                 판정        : 유효
result_of() {
    local log=$1 a b verdict
    a="$(sed -n 's/^  보내는 쪽 *: //p' "$log" | head -n 1)"
    b="$(sed -n 's/^  받는 쪽 *: //p' "$log" | head -n 1)"
    verdict="$(sed -n 's/^  판정 *: //p' "$log" | head -n 1)"
    [ -z "$verdict" ] && verdict="결과 없음"
    printf '%s|%s|%s|%s|%s\n' "$(pct_of "$a")" "$(us_of "$a")" "$(pct_of "$b")" "$(us_of "$b")" "$verdict"
}
pct_of() { local v; v="$(printf '%s' "$1" | sed -n 's/^\([0-9.]*\)%.*/\1/p')"; echo "${v:--}"; }
us_of() { local v; v="$(printf '%s' "$1" | sed -n 's/.*메시지당 \([0-9.]*\) µs.*/\1/p')"; echo "${v:--}"; }

# 숫자 목록의 중앙값 (짝수 개면 가운데 두 값 중 작은 쪽). 없으면 -
median_of() {
    local n
    n=$(printf '%s\n' "$@" | grep -c '^[0-9]')
    [ "$n" -eq 0 ] && echo "-" && return
    printf '%s\n' "$@" | grep '^[0-9]' | sort -n | sed -n "$(((n + 1) / 2))p"
}

echo "${conditions#\# }"
if [ "$is_wsl" -eq 1 ]; then
    echo "WSL2: 네트워크 모드에 따라 multicast가 다른 PC까지 나가지 않을 수 있습니다. 성능 측정은 네이티브 리눅스에서 하세요."
fi
if [ "$ROLE" = "tester" ]; then
    echo "송신량 ${RATE} msg/s로 ${RUNS}회 측정해 중앙값으로 보고합니다. 받는 쪽 PC에서 'test_start.sh peer'가 먼저 떠 있어야 합니다."
else
    echo "${RUNS}회 동안 받는 쪽을 띄웁니다. 이제 보내는 쪽 PC에서 'test_start.sh tester'를 실행하세요."
fi

failed_runs=0
valid_runs=0
a_pcts=() a_uss=() b_pcts=() b_uss=() # 유효한 회차의 값 (tester)
lines=()                              # 회차별 결과 줄
for run in $(seq 1 "$RUNS"); do
    [ "$stop_requested" -eq 1 ] && break
    udp_before="$(udp_counters)"
    run_ok=1
    run_once "$run" || run_ok=0
    udp_after="$(udp_counters)"

    if [ "$ROLE" = "tester" ]; then
        IFS='|' read -r a_pct a_us b_pct b_us state <<<"$(result_of "${LOG_DIR}/${LOG_PREFIX}_${run}.txt")"
    else
        state="수신 끝"
    fi
    if [ "$stop_requested" -eq 1 ]; then
        state="중단됨"
    elif [ "$run_ok" -eq 0 ]; then
        # 프로그램이 스스로 측정을 멈춘 회차는 그 판정을 그대로 둔다
        [ "${state#측정하지 못함}" = "$state" ] && state="실행 오류 (로그 확인)"
        failed_runs=$((failed_runs + 1))
    fi
    if [ "$ROLE" = "tester" ]; then
        lines+=("$(printf '  %d회: 보내는 쪽 %s%% (%s µs) · 받는 쪽 %s%% (%s µs)  %s' "$run" "$a_pct" "$a_us" "$b_pct" "$b_us" "$state")")
        # 유효한 회차만 중앙값에 넣는다
        if [ "$stop_requested" -eq 0 ] && [ "$run_ok" -eq 1 ] && [ "$state" = "유효" ]; then
            valid_runs=$((valid_runs + 1))
            a_pcts+=("$a_pct") a_uss+=("$a_us") b_pcts+=("$b_pct") b_uss+=("$b_us")
        fi
    else
        lines+=("$(printf '  %d회: %s' "$run" "$state")")
    fi
    lines+=("$(udp_delta_text "$udp_before" "$udp_after")")
    if [ "$ROLE" = "tester" ] && [ "$run" -lt "$RUNS" ] && [ "$stop_requested" -eq 0 ]; then
        echo "다음 회차까지 ${RUN_GAP_S}초 쉽니다 (받는 쪽이 다시 뜰 시간)."
        sleep "$RUN_GAP_S"
    fi
done

# 요약
#   tester: 회차별 두 PC CPU와 판정, 유효한 회차들의 중앙값 (보내는 쪽·받는 쪽 따로)
#   peer  : 회차별 상태와 UDP 카운터 (결과는 보내는 쪽 PC의 요약에 있다)
{
    echo "$conditions"
    echo
    if [ "$ROLE" = "peer" ]; then
        echo "===== 받는 쪽 요약 (CPU 결과는 보내는 쪽 PC의 result_summary.txt에 있다) ====="
        for line in "${lines[@]}"; do echo "$line"; done
    else
        echo "===== 결과 요약 (송신량 ${RATE} msg/s, 메시지 128 B, 각 PC에서 프로그램을 묶은 코어 하나의 CPU) ====="
        for line in "${lines[@]}"; do echo "$line"; done
        if [ "$valid_runs" -eq 0 ]; then
            echo "  유효한 회차가 없어 중앙값을 낼 수 없습니다."
        else
            echo "  중앙값 (유효 ${valid_runs}회): 보내는 쪽 $(median_of "${a_pcts[@]}")% ($(median_of "${a_uss[@]}") µs) · 받는 쪽 $(median_of "${b_pcts[@]}")% ($(median_of "${b_uss[@]}") µs)"
        fi
    fi
} >"$SUMMARY"

echo
cat "$SUMMARY"
echo
echo "요약을 저장했습니다: $SUMMARY"

[ "$failed_runs" -eq 0 ] && [ "$stop_requested" -eq 0 ]
