include $(APPDIR)/Make.defs

PROGNAME = $(CONFIG_EXAMPLES_UAC2_DAC_PROGNAME)
PRIORITY = $(CONFIG_EXAMPLES_UAC2_DAC_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_UAC2_DAC_STACKSIZE)
MODULE = $(CONFIG_EXAMPLES_UAC2_DAC)

CSRCS = src/uac2_driver.c \
        src/uac2_desc.c \
        src/uac2_audio_dma.c \
        src/uac2_monitor.c \
        src/uac2_sdk_compat.c \
        src/uac2_asmp_sup.c

MAINSRC = src/uac2_main.c

CFLAGS += -I$(CURDIR)/include -I$(CURDIR) -I$(SDKDIR)/modules/include

include $(APPDIR)/Application.mk

# ASMP sub-core worker (formatter) build + ROMFS packing.
# Stamp-guarded like HexaMIDI: worker ELF must never skew from the
# protocol header (ABI gate would refuse it at boot).

ifeq ($(CONFIG_EXAMPLES_UAC2_DAC_ASMP),y)

WORKER_DEPS := $(wildcard worker/Makefile) \
               $(wildcard worker/lib/Makefile) \
               $(wildcard worker/monw/Makefile) \
               $(wildcard worker/monw/*.c) \
               $(wildcard worker/monw/*.h) \
               $(wildcard include/uac2_asmp.h) \
               $(wildcard include/uac2_monitor.h)

build_uac2_worker: worker/.built_stamp

worker/.built_stamp: $(WORKER_DEPS)
	@$(MAKE) -C worker TOPDIR="$(TOPDIR)" SDKDIR="$(SDKDIR)" APPDIR="$(APPDIR)" CROSSDEV=$(CROSSDEV)
	@touch $@

$(OBJS): worker/.built_stamp

ifeq ($(CONFIG_FS_ROMFS),y)
.depend: worker/romfs.h
worker/romfs.h:
	@echo >$@
endif

clean:: clean_uac2_worker

clean_uac2_worker:
	@$(MAKE) -C worker TOPDIR="$(TOPDIR)" SDKDIR="$(SDKDIR)" APPDIR="$(APPDIR)" CROSSDEV=$(CROSSDEV) clean
	@rm -f worker/.built_stamp

endif
