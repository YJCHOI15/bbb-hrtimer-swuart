cmd_scripts/module.lds := gcc -E -Wp,-MMD,scripts/.module.lds.d -nostdinc -isystem /usr/lib/gcc/arm-linux-gnueabihf/12/include -I./arch/arm/include -I./arch/arm/include/generated  -I./include -I./arch/arm/include/uapi -I./arch/arm/include/generated/uapi -I./include/uapi -I./include/generated/uapi -include ./include/linux/kconfig.h -D__KERNEL__ -mlittle-endian -fmacro-prefix-map=./=   -P -Uarm -D__ASSEMBLY__ -DLINKER_SCRIPT -o scripts/module.lds scripts/module.lds.S

source_scripts/module.lds := scripts/module.lds.S

deps_scripts/module.lds := \
    $(wildcard include/config/lto/clang.h) \
  include/linux/kconfig.h \
    $(wildcard include/config/cc/version/text.h) \
    $(wildcard include/config/cpu/big/endian.h) \
    $(wildcard include/config/booger.h) \
    $(wildcard include/config/foo.h) \
  arch/arm/include/asm/module.lds.h \
    $(wildcard include/config/arm/module/plts.h) \

scripts/module.lds: $(deps_scripts/module.lds)

$(deps_scripts/module.lds):
