# SW UART (hrtimer) for BeagleBone Black — README

AM335x BBB에서 GPIO 비트뱅잉과 커널 hrtimer로 구현한 SW UART LKM에 관한 프로젝트이다.  
TX는 안정화되었고, RX는 일반 커널 환경의 IRQ 지연 문제로 한계를 확인했다.

## 전체 목표

* 비글본 블랙에서 GPIO Bit Banging 기반 SW UART 디바이스 드라이버를 LKM 형태로 구현

* udev 연동으로 /dev/sw_uart_hr 자동 생성되도록 구성

* 하드 실시간이 아닌 일반 리눅스 커널에서 hrtimer + GPIO IRQ + 링버퍼 조합으로 동작하도록 설계

* 로직 애널라이저로 비트 지속 시간, 지터를 계측해 결과와 한계 문서화

더 자세한 배경/분석은 docs 문서 참고

## 아키텍처 개요

TX: hrtimer 만료마다 GPIO를 SET/CLEAR하며 Start → D0~D7(LSB-first) → Stop 순으로 출력함

RX: GPIO Falling Edge IRQ로 Start 검출 → hrtimer로 비트 중앙 시점에 샘플링 → Stop=1 확인 시 링버퍼에 적재함

디바이스 I/F: /dev/sw_uart_hr에 대해 write()는 동기 블로킹 전송, read()는 가용 데이터만 즉시 반환함


### 하드웨어 매핑(AM335x GPIO1)

* TX: GPIO1_12 (P8_12)

* RX: GPIO1_13 (P8_11)

* 베이스: GPIO1 base = 0x4804_C000

* 레지스터: OE(0x134), DATAIN(0x138), CLR(0x190), SET(0x194)

* 접근: ioremap() 후 writel()/readl()로 토글/샘플링함

## 개발 환경

보드: BeagleBone Black (TI AM335x, ARMv7)

커널: 5.10.168-ti-r71 (일반 커널, PREEMPT_RT 미적용에서 검증함)

OS: Debian (BBB 기본 배포판 기준)

호스트: Ubuntu 22.04 (WSL2) + VSCode

툴: arm-linux-gnueabihf- 크로스 컴파일러, Saleae Logic 2

## 빌드 및 배포

makefile과 deploy.sh 내용
rx, tx 각 테스트 코드 BBB에서 실행

## 구현 결과 및 한계 (요약)

TX: 9600 bps에서 문자열 송신 정상 동작 확인함. hrtimer_forward(+1bit)로 누적 지터를 최소화함

RX: 이상적으로는 Start + 1.5bit 지점부터 중앙 샘플링해야 하나, IRQ latency 분산으로 첫 샘플(D0) 정렬이 흔들림 → 프레이밍 에러 발생함

초기 지연을 1.0bit로 보정해도 일반 커널(HZ=1000, RT 미적용) 환경에서는 일관된 안정화에 실패함

결론적으로 일반 리눅스 커널 + hrtimer만으로 RX 완전 안정화는 어려움을 확인함

왜 실시간 처리를 위해 RTOS를 사용하거나 PRU 같은 보조 코어가 중요한지 필요성을 느꼈음

다음과 같은 대안으로 지연 상한 축소/결정성 확보를 계획함

* PREEMPT_RT 커널: IRQ 쓰레드화 + SCHED_FIFO/affinity로 지연 분포 축소함

* PRU(Programmable Real-time Unit): PRU가 핀 샘플링을 하드 RT로 수행해 µs 지터를 근본적으로 제거함

* RTOS: FreeRTOS 등으로 결정적 스케줄링을 확보해 인터럽트/태스크 지연을 크게 줄임