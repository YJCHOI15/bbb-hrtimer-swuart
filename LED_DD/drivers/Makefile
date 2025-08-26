obj-m += LED_DD.o

KERNEL_SRC ?= /home/yj/bbb_ws/linux
ARCH       ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-

# 상위 디렉토리 경로
TOP_DIR := $(PWD)/..
BUILD_DIR := $(TOP_DIR)/build

all:
	$(MAKE) -C $(KERNEL_SRC) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(PWD) modules
	$(CROSS_COMPILE)gcc -O2 -o $(TOP_DIR)/main $(TOP_DIR)/main.c

clean:
	$(MAKE) -C $(KERNEL_SRC) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(PWD) clean
	rm -f $(TOP_DIR)/main
