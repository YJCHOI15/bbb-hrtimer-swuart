# SW UART for BeagleBone Black

HW UART 자원이 부족한 임베디드 리눅스 환경의 제약을 극복하고자, BeagleBone Black 보드에서 GPIO와 커널 고해상도 타이머(hrtimer)를 이용한 9600bps SW UART 디바이스 드라이버를 LKM(Loadable Kernel Module) 형태로 개발했습니다.

정밀한 타이밍 제어로 안정적인 송신(TX) 기능을 구현했으며, 수신(RX) 기능 구현 과정에서 발생하는 실시간성(Real-time) 이슈를 분석하고 이에 대한 기술적 해결 방안을 제시했습니다.


---

## 전체 목표

* 비글본 블랙에서 GPIO Bit Banging 기반 SW UART 디바이스 드라이버를 LKM 형태로 구현

* udev 연동으로 /dev/sw_uart_hr 자동 생성되도록 구성

* 하드 실시간이 아닌 일반 리눅스 커널에서 hrtimer + GPIO IRQ + 링버퍼 조합으로 동작하도록 설계

* 로직 애널라이저로 비트 지속 시간, 지터를 계측해 결과와 한계 문서화

더 자세한 배경/분석은 [docs](https://github.com/YJCHOI15/sw-uart/tree/main/docs) 참고해주세요.

---

## 시스템 아키텍처

TX: hrtimer 만료마다 GPIO를 SET/CLEAR하며 Start → D0~D7(LSB-first) → Stop 순으로 출력

RX: GPIO Falling Edge IRQ로 Start 검출 → hrtimer로 비트 중앙 시점에 샘플링 → Stop=1 확인 시 링버퍼에 적재

디바이스 I/F: /dev/sw_uart_hr에 대해 write()는 동기 블로킹 전송, read()는 가용 데이터만 즉시 반환

<img width="1291" height="650" alt="image" src="https://github.com/user-attachments/assets/27fdaf44-e3bd-4056-9d65-c92f87f2d758" /> <BR><BR>


### 하드웨어 매핑(AM335x GPIO1)

* TX: GPIO1_12 (P8_12)

* RX: GPIO1_13 (P8_11)

* 베이스: GPIO1 base = 0x4804_C000

* 레지스터: OE(0x134), DATAIN(0x138), CLR(0x190), SET(0x194)

* 접근: ioremap() 후 writel()/readl()로 토글/샘플링

---

## 개발 스택

**보드**: BeagleBone Black (TI AM3358 ARM Cortex-A8)

**언어**: C

**OS / 커널**: Debian on ARM, Linux Kernel (v5.10)

**Tool**: gcc-arm-linux-gnueabihf, Makefile, Git, udev, Saleae Logic 2 (신호 분석), putty (ssh)

**핵심 기술**: Device Driver (LKM), Kernel Timer (hrtimer), Interrupt Handling (IRQ), GPIO,  Cross-Compilation, Bit-Banging

---

## Out-of-Tree 방식 모듈 구현
1. 호스트 PC(WSL2)에서 타겟용 커널 모듈을 크로스 컴파일할 수 있는 빌드 시스템을 구축했습니다. 
<img width="928" height="439" alt="image" src="https://github.com/user-attachments/assets/f006f6f9-fa5a-428c-b65e-5627beb7bc80" /> <BR><BR>

2. udev와 연동하여 모듈 로드 시 /sys/class 를 감시하다가 아래와 같은 dev 파일을 읽어서 주번호와 부번호를 알아내고, /dev 아래에 해당 디바이스 노드를 생성합니다.
<img width="928" height="65" alt="image" src="https://github.com/user-attachments/assets/9d32b15a-2730-430d-b41e-763cdae3a5de" />

---

## 빌드 및 배포

아래 절차로 드라이버 및 유저 프로그램 빌드 → BBB 배포/로드 → TX/RX 유저 테스트 진행 가능합니다.
```
# 호스트에서
cd drivers
make
# make clean

cd ..
./deploy.sh

# BBB에서
sudo insmod /home/debian/swuart_hr/swuart_hrtimer.ko

# 송신(TX) 테스트
sudo ./swuart_tx_test

# 수신(RX) 테스트: 시리얼로 문자열 송신해줘야 합니다. 
sudo ./swuart_rx_test
```

## 구현 결과 (TX)

- 9600bps에서 **문자열 송신 정상 동작** 확인했습니다.
    
<img width="1278" height="124" alt="image" src="https://github.com/user-attachments/assets/21ac09e6-0bd8-4597-9339-76060e0f2719" /> <BR><BR>

    
- 위상 고정(매 비트마다 `hrtimer_forward(prev_expire, bit_period)`)으로 누적 오차를 억제했습니다.
    
<img width="911" height="135" alt="image" src="https://github.com/user-attachments/assets/2b76f065-5d0e-4e8b-92ea-ec0e774087bf" /> <BR><BR>


---

## 구현 결과 및 한계 (RX)

- 로직 애널라이저 분석 결과, 9600 baud에서 **비트 폭은 평균 약 104 µs입니다**.
- 또한 스톱 비트 종료 → 다음 프레임 시작(하강 엣지)까지 간격이 **약 49 µs**로 관측되었습니다(프레임마다 소폭 변동 존재함)
    
<img width="1450" height="312" alt="image" src="https://github.com/user-attachments/assets/0e106e42-4bca-4ac5-9f36-a86a855f313e" /> <BR><BR>

    

- 이상적인 샘플링은 **스타트 비트 하강 엣지 + 1.5비트(≈156 µs)** 지점에서 D1를 읽고, 이후 매 비트마다 중앙 시점에서 샘플링하는 것입니다.
- 그러나 **IRQ latency**로 인해 최초 샘플(D1) 정렬이 불안정하여, 중앙 샘플링을 꾸준히 유지하기 어려웠습니다.

| 단계 | 시간 기록 | 이전 단계와의 시간 차이 (µs) | 분석 |
| --- | --- | --- | --- |
| FALLING EDGE | 10197.858877 | - | Start Bit 감지(T=0) |
| data bit1 | 10197.859094 | 217 | **1.5비트(≈156 µs)보다 늦음** |
| data bit2 | 10197.859182 | 88 | **1비트(≈104 µs)보다  약간 빠름** |
| data bit3 | 10197.859285 | 103 | 정상 범위 |
| data bit4 | 10197.859389 | 104 | 정상 범위 |
| data bit5 | 10197.859493 | 104 | 정상 범위 |
| data bit6 | 10197.859597 | 104 | 정상 범위 |
| data bit7 | 10197.859702 | 105 | 정상 범위 |
| data bit8 | 10197.859806 | 104 | 정상 범위 |
| framing error | 10197.859913 | 107 | 누적 오프셋으로 **프레이밍 에러** 발생 |
- 위 측정에서 **초기 샘플 지연이 1.5비트 기준보다 크게 발생**했음을 확인했다. 이로 인해 뒤따르는 샘플들이 한 비트씩 밀리는 패턴이 나타났습니다.

- 이에 따라, **초기 지연 보상**으로 하강 엣지 이후 대기 시간을 **1.5비트 → 1.0비트**로 단축하여 실험했습니다.
- 보상 적용 후에 **D1 처리 시점에서 약 157 µs 경과** 사례가 관측되었지만, 실제 수신에서는 **비트 밀림**이 여전히 발생했습니다.
- 아래 그림과 같이 157us 시점에서 D1 비트를 읽음으로써 1이 기록되어야 합니다.
    
<img width="589" height="188" alt="image" src="https://github.com/user-attachments/assets/bbd8fa25-b0ba-4eb0-a036-8363021fb229" /> <BR><BR>

    
- 로그 상 **하강 엣지 직후 RX 핸들러 진입 시점에서 이미 GPIO가 D1(=1) 상태**로 관측되는 경우가 반복되었습니다(스타트 비트 0 구간을 일부 놓침).
    
<img width="428" height="163" alt="image" src="https://github.com/user-attachments/assets/6ed2823c-e8f5-4333-b848-11d5acfd8bd3" /> <BR><BR>

    
- 따라서 원인은 **gpio irq latency**라고 추정했습니다.

- 결론적으로 **일반 리눅스 커널 + hrtimer만으로는 RX를 완전 안정화하기 어려움**을 확인했습니다.
- 안정화를 위해서는 다음과 같은 대안이 필요합니다.
    - **PREEMPT_RT 커널** 적용해 IRQ를 스레드화하고 latency를 축소
    - 비글본 블랙의 PRU(Programmable Real-time Unit)를 사용해 데이터 비트 샘플링을 하드 RT로 수행하여 us 지터 제거
    - RTOS: 리눅스 대신 FreeRTOS와 같은 실시간 운영체제를 사용하여 커널 단순화와 결정적 스케줄링을 통해 인터럽트 및 태스크 지연 축소

--- 

## 프로젝트 성과 및 배운점

1. 안정적으로 동작하는 커널 기반 SW UART 송신 드라이버 개발을 완료했습니다. 

2. 실시간 시스템에서 발생하는 IRQ Latency 문제를 데이터(로직 분석) 기반으로 체계적으로 분석하고 원인을 규명했습니다.

3. 저수준 시스템 제어 능력: 리눅스 커널 API를 활용하여 하드웨어를 직접 제어하는 능력과 디바이스 드라이버의 구조에 대한 깊은 이해를 얻었습니다.

4. 일반 리눅스 커널 + hrtimer만으로는 RX 실시간 안정화가 어렵다는 점을 확인했으며, 실시간 처리를 위해 PRU/RTOS 등 하드 RT 자원의 필요성을 절실히 체감했습니다. 

