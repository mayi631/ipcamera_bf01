include $(SRCTREE)/build/rule.mk
TARGET_DIR := $(TARGET_OUT_DIR)/lib
TARGET_A := $(TARGET_DIR)/libapp_preview.a
SEXT := c
SDIR += $(CURDIR)/src
ODIR += $(CURDIR)/obj
INCS += -I$(CURDIR)/include
.DEFAULT_GOAL := all
ATBM_DIR := $(TOP_DIR)/osdrv/extdrv/wireless/realtek/atbm6461
AP_TARGET := $(TARGET_OUT_DIR)/bin/bf01_atbm_ap
AP_FLAGS := -I$(ATBM_DIR)/tools -I$(ATBM_DIR)/hal_apollo -DARES_B=10 -DHERA=11 -DRHEA=12 -DEROS=13 -DPROJ_TYPE=RHEA

all: $(TARGET_A) $(AP_TARGET)

$(AP_TARGET): tools/bf01_atbm_ap.c $(ATBM_DIR)/tools/atbm_tool.h $(ATBM_DIR)/hal_apollo/atbm_ioctl_ext.h $(ATBM_DIR)/cv181x.config
	$(Q)! grep -q '^CONFIG_ATBM_SDIO_ATCMD=y' $(ATBM_DIR)/cv181x.config
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) $(AP_FLAGS) $< -o $@

clean: clean_preview_tool

clean_preview_tool:
	$(Q)$(RM) $(AP_TARGET)

.PHONY: all clean_preview_tool
include $(SRCTREE)/build/build.mk
