# cpu_load: 같은 송신량에서의 CPU 부하 측정 (zenoh-pico)

PC 두 대 사이에서 같은 양의 메시지를 주고받을 때, 두 PC가 CPU를 얼마나 쓰는지 잰다.
라이브러리끼리, MT1·MT0끼리, 전송 방식(UDP multicast, TCP)끼리 비교하는 값이다.
brightonz.foundation의 cpu_load 예제와 같은 측정이다. 측정·판정·출력은 같고, 통신 부분만 zenoh-pico API로 바꿨다.

- 보내는 쪽 A: `cpu_load_tester`. 정해진 송신량(기본 500,000 msg/s, 128 B 메시지)으로 보낸다. 워밍업 3초 뒤 10초 동안 두 PC의 CPU를 잰다.
- 받는 쪽 B: `cpu_load_peer`. 받기만 하면서, 0.25초마다 수신 결과와 자기 누적 CPU 시간을 A에게 돌려준다.
- 전송: UDP multicast `239.192.0.80:19163`, 또는 TCP `tcp/10.0.128.111:19165` (peer 모드).
  zenoh-pico는 peer 모드에서 UDP unicast listen이 없어 UDP unicast는 재지 않는다 (`../max_throughput/README.md` 6장).

최대 처리량은 `../max_throughput/`에서 잰다. 최대 처리량을 재는 동안의 CPU에는 회선이 꽉 차서 생기는 비용이 섞이므로 비교에 쓰지 않는다.
이 예제는 회선보다 한참 낮은 송신량으로 보내므로 그런 비용 없이 잴 수 있다. 자세한 구조는 `cpu_load_measure.h` 맨 위 설명에 있다.

## 1. 준비

1. 두 PC를 `../max_throughput/README.md`의 1장처럼 연결한다. 방화벽을 쓰면 UDP 19163, TCP 19165, multicast를 허용한다.
2. 각 PC의 역할 파일에서 `CPULOAD_LOCATOR`를 고친다. 보내는 쪽(A)은 `main.c`, 받는 쪽(B)은 `main_peer.c`다.
   - multicast: `#iface=` 뒤를 그 PC의 NIC 이름으로 바꾼다. 지금 값은 `enp7s0`이다.
   - TCP: 두 파일 모두 B(받는 쪽) PC의 IP를 넣는다. 지금 값은 `10.0.128.111`이다.

   ```c
   #if CPULOAD_TRANSPORT_TCP
   #define CPULOAD_LOCATOR "tcp/10.0.128.111:19165"
   #else
   #define CPULOAD_LOCATOR "udp/239.192.0.80:19163#iface=enp7s0"
   #endif
   ```

## 2. 빌드 (두 PC 모두 같은 옵션)

빌드 옵션은 `../max_throughput/README.md` 3장과 같다. 저장소 최상위에서 실행한다. 리눅스에서만 빌드된다.

```sh
# MT1
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DZ_FEATURE_MULTI_THREAD=1 -DZ_FEATURE_UNICAST_PEER=1 -DZ_FEATURE_MULTICAST_DECLARATIONS=1 -DZ_FEATURE_MULTICAST_LOOPBACK=0 -DBATCH_MULTICAST_SIZE=1450 -DBATCH_UNICAST_SIZE=1450 -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF
cmake --build build --target cpu_load_tester cpu_load_peer cpu_load_tcp_tester cpu_load_tcp_peer

# MT0
cmake -S . -B build_mt0 -DCMAKE_BUILD_TYPE=Release -DZ_FEATURE_MULTI_THREAD=0 -DZ_CONFIG_SOCKET_TIMEOUT=1 -DZ_FEATURE_UNICAST_PEER=1 -DZ_FEATURE_MULTICAST_DECLARATIONS=1 -DZ_FEATURE_MULTICAST_LOOPBACK=0 -DBATCH_MULTICAST_SIZE=1450 -DBATCH_UNICAST_SIZE=1450 -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF
cmake --build build_mt0 --target cpu_load_tester cpu_load_peer cpu_load_tcp_tester cpu_load_tcp_peer
```

- multicast는 `build/examples/cpu_load/`, TCP는 `build/examples/cpu_load_tcp/`에 생긴다. 실행 파일은 `bin/`, 실행 스크립트는 `test_start.sh`이고, 두 폴더의 실행 파일 이름은 같다.
- `Z_FEATURE_MULTICAST_LOOPBACK=0`: 보내는 쪽이 자기 multicast 데이터그램을 다시 받지 않게 한다.
  1이면 그 데이터그램을 받아 버리는 데 보내는 쪽 CPU를 더 쓴다.
- `test_start.sh`를 고쳤으면 다시 빌드해야 빌드 폴더의 사본에 반영된다.

## 3. 실행 (받는 쪽을 먼저)

```sh
# 받는 쪽 PC (먼저)
./build/examples/cpu_load/test_start.sh peer ~/cpu_logs/zp

# 보내는 쪽 PC
RATE=500000 ./build/examples/cpu_load/test_start.sh tester ~/cpu_logs/zp
```

- TCP는 `build/examples/cpu_load_tcp/`, MT0는 `build_mt0/examples/…`의 `test_start.sh`를 같은 방법으로 쓴다.
- `RATE`는 송신량(msg/s)이다. 1,000 ~ 1,000,000 사이에서 고르고, 주지 않으면 500,000이다. 비교할 조합은 모두 같은 `RATE`로 잰다.
- 3회 잰다. 한 회차는 약 20초다 (워밍업 3초, 측정 10초, 마무리 2초).
- CPU 고정(`PIN_CPU=3`), 클럭 정책 performance, 터보 끔, NIC 확인은 max_throughput의 `test_start.sh`와 같다.
- **받는 쪽(B)을 반드시 먼저 띄운다.** zenoh-pico publisher는 선언하는 순간에 이미 있는 상대 subscriber에게만 보낸다.
  A가 먼저 뜨면 A는 아무것도 보내지 않고, 워밍업 뒤 "받는 쪽 보고가 오지 않음"으로 멈춘다.

## 4. 결과 읽기

A의 회차별 로그 끝에 결과가 나온다.

```text
===== CPU 부하 (송신량 500000 msg/s 고정, 측정 10초, 메시지 128 B, a→b) =====
  실제 송신량 : 499998 msg/s (목표의 100.0%), put 실패로 건너뛴 1 ms 구간 0개
  미수신      : 0개 / 측정 구간에 보낸 5009936개 (0.000%)
  보내는 쪽   : 16.2% (메시지당 0.32 µs)
  받는 쪽     : 18.2% (메시지당 0.36 µs)
  판정        : 유효
```

A의 `result_summary.txt`에는 회차별 값과 유효한 회차들의 중앙값이 나온다. 중앙값은 보내는 쪽과 받는 쪽을 따로 낸다.

- **CPU %:** 측정 구간 동안의 평균 부하다. 각 PC에서 프로그램을 묶은 코어(CPU 3) 하나를 잰다.
  그 코어에서 돈 모든 것이 들어간다: 이 프로그램, 라이브러리 스레드, 그 코어에서 처리된 커널 일.
- **메시지당 µs:** CPU 부하 × 1초 ÷ 실제 송신량. 같은 송신량에서 라이브러리끼리 비교할 때 쓴다.
- **측정 구간:** A가 count를 보낸 두 순간 사이다. 시작은 워밍업 3초 뒤 보낸 count 가운데 B의 보고에 처음 반영된 것이고,
  끝은 측정 10초가 지나 송신을 멈추며 보낸 마지막 count다. 끝을 0.25초 단위로 정하므로 10.0 ~ 10.25초가 된다.
- **받는 쪽 값:** B는 count가 도착한 순간의 누적 CPU 시간을 status에 실어 보낸다.
  A는 시작 count와 마지막 count에 대한 두 값으로 계산하므로, B의 CPU도 A와 같은 구간의 값이다.
  받는 쪽 커널의 수신 인터럽트 처리는 대개 NIC 인터럽트를 받는 다른 코어에서 돌아 이 값에 들어가지 않는다.
- **미수신:** 측정 구간에 보낸 메시지 가운데 B가 받지 못한 수다. 시작 count와 마지막 count에 대한 B의 누적 미수신 차이로 센다
  (워밍업은 넣지 않는다). count보다 늦게 도착한 메시지는 도착하면 미수신에서 빠지고, A는 마무리 동안 받은 가장 늦은 보고를 쓴다.
- **마지막 보고:** A는 송신을 멈춘 뒤 마지막 count를 보내고, 그것을 반영한 B의 보고가 올 때까지 마무리 동안 다시 보낸다.
  마무리 2초 안에 그 보고가 오지 않으면(연결이 끊김 등) 측정 끝까지의 미수신과 B의 CPU를 모르므로 `무효`다.
- **판정:** 실제 송신량이 목표의 99% 이상이고, B의 마지막 보고가 왔고, 미수신이 1% 이하이고,
  B의 CPU를 잰 구간이 측정 구간과 0.25초 안으로 같아야 `유효`다. 아니면 `무효`로 적고 중앙값에서 뺀다.
- **put 실패로 건너뛴 1 ms 구간:** zenoh-pico는 congestion control BLOCK이라 batch가 차도 put 안에서 내보내고 이어서 넣는다.
  그래서 BUSY가 없고, put이 실패할 때만 그 1 ms의 나머지를 건너뛴다.
  brightonz.foundation 결과의 "BUSY로 건너뛴 1 ms 구간"에 해당한다.

## 5. MT0

- MT0에서는 메인 루프가 `zp_spin_once`, 송신 tick, 피드백을 되풀이한다.
- `zp_spin_once`의 수신은 받을 것이 없으면 소켓 timeout(`Z_CONFIG_SOCKET_TIMEOUT=1`, 실제로는 1~10 ms)만큼 기다린다.
  그래서 이 프로그램이 따로 쉬지 않아도, 할 일이 없을 때 CPU를 쓰지 않는다.
  brightonz.foundation의 MT0는 `spin_once`가 기다리지 않아 프로그램이 최대 100 µs씩 쉰다.
- TCP 받는 쪽은 연결되기 전과 연결이 끊긴 뒤에 코어 하나를 다 쓴다(100%). 연결된 동안에는 수신에서 기다린다.
  측정 구간은 연결된 동안이므로 결과에는 들어가지 않는다. 받는 쪽의 1초 출력에서 보인다.
