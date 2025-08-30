obj-m += swuart_hrtimer.o

KERNEL_SRC ?= $(abspath $(PWD)/../../linux)
ARCH       ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-

# 상위 디렉토리 경로
TOP_DIR := $(PWD)/..
BUILD_DIR := $(TOP_DIR)/build

all:
	$(MAKE) -C $(KERNEL_SRC) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(PWD) modules
	$(CROSS_COMPILE)gcc -O2 -o $(TOP_DIR)/swuart_tx_test $(TOP_DIR)/swuart_tx_test.c
	$(CROSS_COMPILE)gcc -O2 -o $(TOP_DIR)/swuart_rx_test $(TOP_DIR)/swuart_rx_test.c

clean:
	$(MAKE) -C $(KERNEL_SRC) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(PWD) clean
	rm -f $(TOP_DIR)/swuart_tx_test
	rm -f $(TOP_DIR)/swuart_rx_test
