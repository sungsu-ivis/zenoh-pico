# z_perf: 최대 처리량 측정 (zenoh-pico)

손실 0%로 보낼 수 있는 128 B 메시지의 최대 송신량(msg/s)을 찾는다.
brightonz.foundation의 z_perf 예제와 같은 측정이다. 탐색·판정 코드(`perf_max_throughput.*`)는 그 예제와 같고, 통신 부분만 zenoh-pico API로 바꿨다.

- 보내는 쪽 A: `z_perf_tester`. 500,000 msg/s부터 송신량을 2배씩 올리거나(처음부터 실패하면 절반씩 내리고) 이분 탐색으로 좁힌 뒤, 15초 확인으로 결과를 정한다.
- 받는 쪽 B: `z_perf_peer`. 받은 결과를 0.25초마다 A에게 돌려준다.
- 전송: UDP multicast `239.192.0.78:19153` (peer 모드). PC 두 대에서 잰다. loopback은 NIC 드라이버를 거치지 않으므로 쓰지 않는다.

자세한 구조는 `perf_measure.h`, 탐색 순서는 `perf_max_throughput.h` 맨 위 설명에 있다.

## 1. 준비 (두 PC)

1. 두 PC의 NIC를 케이블이나 스위치로 연결하고, 같은 서브넷의 IPv4를 준다 (예: `192.168.77.1/24`, `192.168.77.2/24`).
   직결 케이블에서 자동으로 잡히는 링크 로컬 주소(`169.254.x.x/16`)도 된다.
2. 방화벽을 쓰면 UDP 19153과 multicast를 허용한다.
3. NIC 이름을 확인한다: `ip -br link`

## 2. NIC 이름 바꾸기

각 PC에서 자기 역할 파일의 `PERF_LOCATOR`에서 `#iface=` 뒤를 그 PC의 NIC 이름으로 바꾼다.
지금 값은 `enp7s0`이다 (두 PC의 NIC 이름이 같으면 그대로 쓴다).

| PC | 파일 |
|---|---|
| 보내는 쪽 (A) | `main.c` |
| 받는 쪽 (B) | `main_peer.c` |

```c
#define PERF_LOCATOR "udp/239.192.0.78:19153#iface=enp7s0"
```

## 3. 빌드 (두 PC 모두 같은 옵션)

저장소 최상위에서:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBATCH_MULTICAST_SIZE=1450 -DBATCH_UNICAST_SIZE=1450 -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF
cmake --build build --target z_perf_tester z_perf_peer z_perf_tcp_tester z_perf_tcp_peer
```

- `BATCH_MULTICAST_SIZE=1450`: UDP 데이터그램 최대 크기. brightonz.foundation 예제의 mtu·batch.length와 같다.
  기본값 2048은 이더넷 MTU(1500)보다 커서 IP 단편화가 생긴다. multicast는 batch 크기를 협상하지 않으므로 두 PC가 같아야 한다.
- `BATCH_UNICAST_SIZE=1450`: TCP에서 batch 하나의 최대 크기. multicast와 같은 크기로 맞춰, 전송 방식만 바꾼 비교가 되게 한다.
- 실행 파일은 `build/examples/z_perf/bin/`, 실행 스크립트는 `build/examples/z_perf/test_start.sh`에 생긴다.
  TCP로 빌드한 것은 `build/examples/z_perf_tcp/`에 같은 이름으로 따로 생긴다 (6장).
- configure가 `include/zenoh-pico.h`, `include/zenoh-pico/config.h`, `library.json`, `zenohpico.pc`를 다시 쓴다.
  저장소에 들어 있는 값이 오래된 것이라 수정으로 보이는데, 빌드 산출물이므로 커밋하지 않는다.

## 4. 실행 (받는 쪽을 먼저)

저장소 최상위에서 (`cd`가 실패하면 실행하지 않도록 `&&`로 잇는다):

```sh
# 받는 쪽 PC (먼저)
cd ~/workspace/zenoh-pico && ./build/examples/z_perf/test_start.sh peer ~/zperf_logs/zp

# 보내는 쪽 PC
cd ~/workspace/zenoh-pico && ./build/examples/z_perf/test_start.sh tester ~/zperf_logs/zp
```

- 3회 측정해 중앙값을 보고한다. 한 회차는 1분쯤, 전체는 약 3분 걸린다. 측정 중에는 두 PC를 쓰지 않는다.
- 결과는 보내는 쪽 PC의 `result_summary.txt`에 있다. 회차별 로그는 `tester_log_N.txt`, `peer_log_N.txt`.
- 스크립트는 바이너리에 든 `#iface=`로 NIC를 확인한다. NIC가 없거나 꺼져 있거나 `lo`이면 시작하지 않는다.
- 두 PC 모두 같은 CPU(`PIN_CPU=3`)에 프로그램을 묶는다.
- 측정 전에 모든 CPU의 주파수 정책을 `performance`, 터보를 끔으로 맞춘다 (`CPU_GOVERNOR`, `TURBO`).
  sudo 비밀번호를 물을 수 있고, 맞추지 못하면 측정하지 않는다. 측정이 끝나도 되돌리지 않는다 (재부팅하면 원래대로).
- 측정 조건(커널, CPU, CPU 고정, 클럭 정책, 터보, NIC 속도·MTU·IP·드라이버)과 회차별 UDP 카운터 증가분을 로그와 요약에 남긴다.
  두 PC의 로그 첫 줄이 `CPU 고정 …=3 | 클럭 정책 performance | 터보 끔`인지 확인한다.
- `test_start.sh`를 고쳤거나 받은 뒤에는 `cmake --build build --target z_perf_tester z_perf_peer`를 다시 해야
  `build/examples/z_perf/test_start.sh`(빌드 때 복사한 사본)에 반영된다.

## 5. 주의

- **받는 쪽(B)을 반드시 먼저 띄운다.** zenoh-pico publisher는 상대 subscriber가 있을 때만 보낸다(write filter).
  라우터 없는 peer 모드에서는 선언하는 순간에 이미 있는 subscriber만 찾으므로, A가 먼저 뜨면 B가 나중에 떠도 A는 아무것도 보내지 않는다.
  이 경우 A는 워밍업 뒤 "상대 보고가 15초 동안 따라오지 않아 탐색을 중단합니다"로 끝난다.
- **A는 자기가 보낸 데이터그램도 받는다.** zenoh-pico는 multicast 되돌림(IP_MULTICAST_LOOP)을 끄지 않아,
  A의 읽기 태스크가 자기 데이터그램을 받아 버린다. 이 처리도 A의 CPU를 쓴다 (brightonz.foundation 예제는 `loopback: false`).
- **단방향 지연은 재지 않는다.** 두 PC의 시계 기준이 달라서다. 지터는 같은 PC 시계끼리의 차이라 그대로 맞다.
- 이 브랜치는 zenoh-pico 1.6.1 기준이다. 1.6.1의 `z_open`은 태스크를 띄우지 않으므로 `main`에서 읽기·lease 태스크를 직접 시작한다.

## 6. TCP unicast로 재기

같은 측정을 TCP 연결 하나로 한다. B가 자기 IP로 연결을 기다리고(listen), A가 그 IP로 접속한다(connect).
zenoh-pico 1.6.1은 UDP unicast를 받는 쪽(listen)이 없어, unicast는 TCP로 잰다.

1. B(받는 쪽) PC의 IP를 두 파일의 TCP `PERF_LOCATOR`에 똑같이 넣는다. 지금 값은 `tcp/169.254.236.32:19155`다.

   ```c
   #if PERF_TRANSPORT_TCP
   #define PERF_LOCATOR "tcp/169.254.236.32:19155" /* B(받는 쪽) PC의 IP와 포트 */
   ```

2. 3장처럼 빌드한다. `z_perf_tcp_tester`, `z_perf_tcp_peer`가 `PERF_TRANSPORT_TCP=1`로 빌드되어
   `build/examples/z_perf_tcp/bin/`에 생긴다.
3. 실행은 4장과 같고, 폴더만 `z_perf_tcp`다 (받는 쪽을 먼저):

   ```sh
   # 받는 쪽 PC (먼저)
   cd ~/workspace/zenoh-pico && ./build/examples/z_perf_tcp/test_start.sh peer ~/zperf_logs/zp_tcp

   # 보내는 쪽 PC
   cd ~/workspace/zenoh-pico && ./build/examples/z_perf_tcp/test_start.sh tester ~/zperf_logs/zp_tcp
   ```

- 스크립트는 바이너리에 든 `tcp/IP`로 NIC를 찾는다. 받는 쪽은 그 IP를 가진 NIC, 보내는 쪽은 그 IP로 가는 경로의 NIC다.
  받는 쪽 PC에 그 IP가 없거나, 보내는 쪽 PC가 그 IP를 가지고 있으면(같은 PC) 시작하지 않는다.
- TCP는 흐름 제어가 있어 받는 쪽이 늦으면 보내는 쪽 put이 기다린다(BLOCK). 그래서 손실 없이 "목표만큼 못 보냄"으로 한계가 나온다.
- 이 브랜치(perfTest)는 1.6.1 그대로가 아니다. peer TCP 버그 2개를 고친 커밋이 들어 있다. 고치지 않은 1.6.1은
  128k msg/s쯤에서 받는 쪽 스트림이 어긋나 연결이 끊기므로 TCP 측정이 안 된다. 결과에는 "1.6.1 + TCP peer 버그 2개 수정"으로 적는다.
  1. 받는 쪽 (`src/transport/unicast/read.c`): 길이 2바이트가 두 번에 나뉘어 오면 상태를 되돌리지 않아,
     다음 메시지 길이를 잘못 읽는다 ("Unknown message type received").
  2. 보내는 쪽 (`src/link/link.c`): peer 모드 소켓이 non-blocking이라, 송신 버퍼가 차면 batch를 버리거나
     앞부분만 보낸다. 쓸 수 있을 때까지 기다렸다가 이어서 보내게 고쳤다 (이제 BLOCK이 TCP에서도 실제로 기다린다).
