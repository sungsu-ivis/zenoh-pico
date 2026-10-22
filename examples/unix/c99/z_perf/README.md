# z_perf: 최대 처리량 측정 (zenoh-pico)

손실 0%로 보낼 수 있는 128 B 메시지의 최대 송신량(msg/s)을 찾는다.
brightonz.foundation의 z_perf 예제와 같은 측정이다. 탐색·판정 코드(`perf_max_throughput.*`)는 그 예제와 같고, 통신 부분만 zenoh-pico API로 바꿨다.

- 보내는 쪽 A: `z_perf_tester`. 송신량을 2배씩 올리고 이분 탐색으로 좁힌 뒤, 30초 확인으로 결과를 정한다.
- 받는 쪽 B: `z_perf_peer`. 받은 결과를 1초마다 A에게 돌려준다.
- 전송: UDP multicast `239.192.0.78:19153` (peer 모드). PC 두 대에서 잰다. loopback은 NIC 드라이버를 거치지 않으므로 쓰지 않는다.

자세한 구조는 `perf_measure.h`, 탐색 순서는 `perf_max_throughput.h` 맨 위 설명에 있다.

## 1. 준비 (두 PC)

1. 두 PC의 NIC를 케이블이나 스위치로 연결하고, 같은 서브넷의 IPv4를 준다 (예: `192.168.77.1/24`, `192.168.77.2/24`).
2. 방화벽을 쓰면 UDP 19153과 multicast를 허용한다.
3. NIC 이름을 확인한다: `ip -br link`

## 2. NIC 이름 바꾸기

각 PC에서 자기 역할 파일의 `PERF_LOCATOR`에서 `#iface=` 뒤를 그 PC의 NIC 이름으로 바꾼다.

| PC | 파일 |
|---|---|
| 보내는 쪽 (A) | `main.c` |
| 받는 쪽 (B) | `main_peer.c` |

```c
#define PERF_LOCATOR "udp/239.192.0.78:19153#iface=eth0"
```

## 3. 빌드 (두 PC 모두 같은 옵션)

저장소 최상위에서:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBATCH_MULTICAST_SIZE=1450 -DBUILD_SHARED_LIBS=OFF
cmake --build build --target z_perf_tester z_perf_peer
```

- `BATCH_MULTICAST_SIZE=1450`: UDP 데이터그램 최대 크기. brightonz.foundation 예제의 mtu·batch.length와 같다.
  기본값 2048은 이더넷 MTU(1500)보다 커서 IP 단편화가 생긴다. multicast는 batch 크기를 협상하지 않으므로 두 PC가 같아야 한다.
- 실행 파일은 `build/examples/z_perf/bin/`, 실행 스크립트는 `build/examples/z_perf/test_start.sh`에 생긴다.
- configure가 `include/zenoh-pico.h`, `include/zenoh-pico/config.h`, `library.json`, `zenohpico.pc`를 다시 쓴다.
  저장소에 들어 있는 값이 오래된 것이라 수정으로 보이는데, 빌드 산출물이므로 커밋하지 않는다.

## 4. 실행 (받는 쪽을 먼저)

```sh
# 받는 쪽 PC (먼저)
./build/examples/z_perf/test_start.sh peer ~/zperf_logs

# 보내는 쪽 PC
./build/examples/z_perf/test_start.sh tester ~/zperf_logs
```

- 3회 측정해 중앙값을 보고한다. 한 회차는 6~10분, 전체는 약 30분 걸린다. 측정 중에는 두 PC를 쓰지 않는다.
- 결과는 보내는 쪽 PC의 `result_summary.txt`에 있다. 회차별 로그는 `tester_log_N.txt`, `peer_log_N.txt`.
- 스크립트는 바이너리에 든 `#iface=`로 NIC를 확인한다. NIC가 없거나 꺼져 있거나 `lo`이면 시작하지 않는다.
- 측정 조건(커널, CPU, 클럭 정책, 터보, NIC 속도·MTU·IP·드라이버)과 회차별 UDP 카운터 증가분을 로그와 요약에 남긴다.
- CPU 고정 번호는 `test_start.sh` 위쪽의 `TESTER_CPU`, `PEER_CPU`에서 바꾼다.

## 5. 주의

- **받는 쪽(B)을 반드시 먼저 띄운다.** zenoh-pico publisher는 상대 subscriber가 있을 때만 보낸다(write filter).
  라우터 없는 peer 모드에서는 선언하는 순간에 이미 있는 subscriber만 찾으므로, A가 먼저 뜨면 B가 나중에 떠도 A는 아무것도 보내지 않는다.
  이 경우 A는 워밍업 뒤 "상대 보고가 15초 동안 따라오지 않아 탐색을 중단합니다"로 끝난다.
- **A는 자기가 보낸 데이터그램도 받는다.** zenoh-pico는 multicast 되돌림(IP_MULTICAST_LOOP)을 끄지 않아,
  A의 읽기 태스크가 자기 데이터그램을 받아 버린다. 이 처리도 A의 CPU를 쓴다 (brightonz.foundation 예제는 `loopback: false`).
- **단방향 지연은 재지 않는다.** 두 PC의 시계 기준이 달라서다. 지터는 같은 PC 시계끼리의 차이라 그대로 맞다.
- 이 브랜치는 zenoh-pico 1.6.1 기준이다. 1.6.1의 `z_open`은 태스크를 띄우지 않으므로 `main`에서 읽기·lease 태스크를 직접 시작한다.
