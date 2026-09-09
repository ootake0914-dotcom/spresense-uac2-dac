############################################################################
# worker/Makefile
# ASMP worker ELF build + ROMFS packing (mirrors SDK examples/asmp)
############################################################################

-include $(TOPDIR)/.config
include $(APPDIR)/Make.defs
include $(APPDIR)/Make.defs

WORKER_ELFS = monw/monw

SUBDIRS = $(dir $(WORKER_ELFS))

define DIR_template
$(1)_$(2):
	+$(Q) $(MAKE) -C $(1) $(3) TOPDIR="$(TOPDIR)" APPDIR="$(APPDIR)" SDKDIR="$(SDKDIR)" CROSSDEV=$(CROSSDEV)
endef

.PHONY: lib depend clean
all: romfs.h

# Create the romfs.img file from the populated romfs directory

romfs.img: $(WORKER_ELFS)
	$(Q) mkdir -p romfs
	$(Q) cp $^ romfs
	$(Q) genromfs -f $@ -d romfs -V "WORKER"

# Create the romfs.h header file from the romfs.img file

romfs.h: romfs.img
	$(Q) xxd -i $< | sed -e "s/^unsigned/static const unsigned/g" >$@

# Build ASMP worker library

lib:
	$(Q) $(MAKE) -C lib TOPDIR="$(TOPDIR)" SDKDIR="$(SDKDIR)" APPDIR="$(APPDIR)" CROSSDEV=$(CROSSDEV)

# Build workers

$(WORKER_ELFS): lib
	$(Q) $(MAKE) -C $(dir $@) TOPDIR="$(TOPDIR)" SDKDIR="$(SDKDIR)" APPDIR="$(APPDIR)" CROSSDEV=$(CROSSDEV)

.depend:
	$(Q) $(MAKE) -C lib TOPDIR="$(TOPDIR)" SDKDIR="$(SDKDIR)" APPDIR="$(APPDIR)" CROSSDEV=$(CROSSDEV) depend
	@touch $@

depend: .depend

# Clean each subdirectory

lib_clean:
	$(Q) $(MAKE) -C lib TOPDIR="$(TOPDIR)" clean

clean: $(foreach DIR, $(SUBDIRS), $(DIR)_clean) lib_clean
	$(Q) rm -rf romfs romfs.img romfs.h

$(foreach DIR, $(SUBDIRS), $(eval $(call DIR_template,$(DIR),clean,clean)))
