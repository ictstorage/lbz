CC=gcc
ifeq ($(KERNVERSION),)
KERNVERSION = $(shell uname -r)
endif
ifeq ($KERNHDIR,)             
KERNHDIR = /lib/modules/$(KERNVERSION)/build/include
endif
KERNELDIR ?= /lib/modules/$(KERNVERSION)/build
EXTRA_CFLAGS += -Wall -Wimplicit-fallthrough=0 -Wunused-function
EXTRA_CFLAGS += -I$(PWD)/include
PWD := $(shell pwd)
EXTRA_CFLAGS += -DCONFIG_LBZ_NAT_SIT_SUPPORT #only support nat and sit statistics.
#EXTRA_CFLAGS += -DCONFIG_LBZ_NAT_SIT_FREE_SUPPORT #free duplicate block.
EXTRA_CFLAGS += -DCONFIG_LBZ_NAT_SIT_STREAM_SUPPORT #write block to diff zone by SSA.
#EXTRA_CFLAGS += -DCONFIG_*
#EXTRA_CFLAGS += -I$(KERNHDIR)

DRIVER_NAME = lbz
${DRIVER_NAME}-objs += lbz-zone-metadata.o
${DRIVER_NAME}-objs += lbz-mapping.o
${DRIVER_NAME}-objs += lbz-io-scheduler.o
${DRIVER_NAME}-objs += lbz-gc.o
${DRIVER_NAME}-objs += lbz-dev.o
${DRIVER_NAME}-objs += lbz-request.o
${DRIVER_NAME}-objs += lbz-proc.o
${DRIVER_NAME}-objs += lbz-nat-sit.o

obj-m += ${DRIVER_NAME}.o

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
	@echo '==================================================================='
	@echo 'lbz module make done'
	@echo '==================================================================='
install:
	mkdir -p ${DESTDIR}/lib/modules/$(KERNVERSION)/kernel/drivers/block
	install lbz.ko ${DESTDIR}/lib/modules/$(KERNVERSION)/kernel/drivers/block

	@echo '==================================================================='
	@echo 'lbz module make-install done!'
	@echo '==================================================================='
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f Module.symvers
	@echo '================================================================='
	@echo 'lbz module make-clean done!'
	@echo '================================================================='
