# Build with the same compiler and dependencies as the ipcamera components.
COMMON_DIR ?= $(CURDIR)/../common
CVI_OSAL_DIR ?= $(CURDIR)/../cvi_osal
RINGBUFFER_DIR ?= $(CURDIR)/../ringbuffer

SDIR := $(CURDIR)/src
SRCS_C := $(SDIR)/rtsp.c $(SDIR)/network.c $(SDIR)/rtp.c $(SDIR)/rtsp_client.c

INCS += -I$(COMMON_DIR) -I$(SDIR)/inc -I$(CURDIR)/include
INCS += -I$(CVI_OSAL_DIR)/include -I$(RINGBUFFER_DIR)/include
CFLAGS += -DRTSP_ENABLE_RECV=1

LIB_DIR ?= lib
TARGET_DIR := $(CURDIR)/$(LIB_DIR)
TARGET_A := $(TARGET_DIR)/libcomp_rtsp.a
TARGET_SO := $(TARGET_DIR)/libcomp_rtsp.so

include $(COMMON_DIR)/common_lib.mk

-include $(OBJS:.o=.d)
