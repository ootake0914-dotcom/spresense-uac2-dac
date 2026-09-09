############################################################################
# worker/monw/Makefile
# SubCore 1: telemetry formatter worker ELF (UAC2 DAC ASMP phase 1)
############################################################################

include $(APPDIR)/Make.defs
-include $(SDKDIR)/Make.defs

ifeq ($(WINTOOL),y)
LIB_DIR = "${shell cygpath -w ../lib}"
else
LIB_DIR = "../lib"
endif

LDLIBPATH +=  -L $(LIB_DIR)

LDLIBS += $(ASMPWORKER_LIB)
LIB_ASMPWORKER := $(LIB_DIR)$(DELIM)libasmpw.a

BIN = monw

# Worker stack: default is only 1024B (SDK Make.defs). Snapshot (~350B)
# + format line buffer (512B) + printf depth overflow it -> instant death
# before any shared-memory stage marker. 4KB is plenty (tile memory).
WORKERSTACKSIZE = 4096

CSRCS = monw.c

CELFFLAGS += -O2
CELFFLAGS += ${INCDIR_PREFIX}$(APPDIR)
CELFFLAGS += ${INCDIR_PREFIX}$(SDKDIR)$(DELIM)modules$(DELIM)asmp$(DELIM)worker
CELFFLAGS += ${INCDIR_PREFIX}$(CURDIR)$(DELIM)..$(DELIM)..$(DELIM)include

AOBJS = $(ASRCS:.S=$(OBJEXT))
COBJS = $(CSRCS:.c=$(OBJEXT))

OBJEXT ?= .o

all: $(BIN)

$(COBJS): %$(OBJEXT): %.c
	@echo "CC: $<"
	$(Q) $(CC) -c $(CELFFLAGS) $< -o $@

$(AOBJS): %$(OBJEXT): %.S
	@echo "AS: $<"
	$(Q) $(CC) -c $(AFLAGS) $< -o $@

$(BIN): $(COBJS) $(AOBJS) $(LIB_ASMPWORKER)
	@echo "LD: $@"
	$(Q) $(LD) $(LDRAWELFFLAGS) $(LDLIBPATH) -o $@.debug $^ $(LDLIBS)
	$(Q) $(STRIP) -d -o $@ $@.debug

$(LIB_ASMPWORKER):

clean:
	$(call DELFILE, $(BIN))
	$(call DELFILE, $(BIN).debug)
	$(call CLEAN)
