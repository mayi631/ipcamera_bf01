include $(SRCTREE)/build/rule.mk
TARGET_DIR := $(TARGET_OUT_DIR)/lib
TARGET_A := $(TARGET_DIR)/libapp_ircut.a

SEXT	:= c
SDIR	+= $(CURDIR)/src
ODIR 	+= $(CURDIR)/obj
#DEPEND
INCS	+= -I$(CURDIR)/include

ifeq ($(CONFIG_RESOURCE_INSTALL_FBM_CV1815JA_BF01_GC4683),y)
CFLAGS += -DBF01_IRCUT_GPIO_DISABLED
endif

include $(SRCTREE)/build/build.mk
