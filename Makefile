include $(APPDIR)/Make.defs

PROGNAME = $(CONFIG_EXAMPLES_UAC2_DAC_PROGNAME)
PRIORITY = $(CONFIG_EXAMPLES_UAC2_DAC_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_UAC2_DAC_STACKSIZE)
MODULE = $(CONFIG_EXAMPLES_UAC2_DAC)

CSRCS = src/uac2_driver.c \
        src/uac2_desc.c \
        src/uac2_audio_dma.c

MAINSRC = src/uac2_main.c

CFLAGS += -I$(CURDIR)/include -I$(SDKDIR)/modules/include

include $(APPDIR)/Application.mk
