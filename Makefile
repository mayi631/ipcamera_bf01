SHELL = /bin/bash
SRCTREE := $(CURDIR)
TARGET_OUT_DIR := $(CURDIR)/out
TOPTARGETS := all clean install clean_all
TOPSUBDIRS := ipcamera

export SOC_SEGMENT := $(shell echo $(CHIP_SEGMENT) | tr a-z A-Z)
export SOC_SEGMENT_LOWER := $(shell echo $(SOC_SEGMENT) | tr A-Z a-z)

ifeq ($(SOC_SEGMENT), CV181X)
export SOC_NICK_NAME := CV181X
export SOC_NICK_NAME_LOWER := $(shell echo $(SOC_NICK_NAME) | tr A-Z a-z)
endif
ifeq ($(SOC_SEGMENT), CV180X)
export SOC_NICK_NAME := CV180X
export SOC_NICK_NAME_LOWER := $(shell echo $(SOC_NICK_NAME) | tr A-Z a-z)
endif

$(info SOC_SEGMENT=$(SOC_SEGMENT))
$(info SOC_NICK_NAME=$(SOC_NICK_NAME))
$(info SOC_NICK_NAME_LOWER=$(SOC_NICK_NAME_LOWER))

## setup path ##
ifeq ($(findstring $(SOC_SEGMENT), CV180X CV181X), )
	$(error UNKNOWN chip series - $(SOC_SEGMENT))
endif

TMP_STRING = $(foreach f,$(TOPSUBDIRS),$(findstring $f,$(MAKECMDGOALS)))
PROJECT = $(strip $(TMP_STRING))
export SRCTREE PROJECT TARGET_OUT_DIR

$(PROJECT): .config
	$(MAKE) -C solutions/$@ $(strip $(subst $@,,$(MAKECMDGOALS)))


$(TOPSUBDIRS): $(PROJECT)

$(TOPTARGETS): $(TOPSUBDIRS)

#Use defconfig cv181x_ipcamera_defconfig or cv180x_ipcamera_defconfig
.config :
ifeq ($(SOC_SEGMENT), CV181X)
	$(MAKE) -f $(SRCTREE)/Kbuild cv181x_ipcamera_defconfig
else ifeq ($(SOC_SEGMENT), CV180X)
	$(MAKE) -f $(SRCTREE)/Kbuild cv180x_ipcamera_defconfig
endif

#.PHONY: $(TOPTARGETS) $(TOPSUBDIRS)
all : $(SRCTREE)/.config $(SUBDIRS) $(TARGET)


menuconfig:
	$(MAKE) -f $(SRCTREE)/Kbuild $@
%_defconfig:
	$(MAKE) -f $(SRCTREE)/Kbuild $@

include $(SRCTREE)/build/build.mk
.PHONY: $(SUBDIRS)
