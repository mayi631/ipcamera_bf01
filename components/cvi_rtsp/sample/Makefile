# Usage:
# local host: make
# riscv64 musl: make CROSS_COMPILE=${HOME}/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-
# riscv64 glibc: make CROSS_COMPILE=${HOME}/host-tools/gcc/riscv64-linux-x86_64/bin/riscv64-unknown-linux-gnu-
# arm64 glibc: make CROSS_COMPILE=${HOME}/host-tools/gcc/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-
# arm32 glibc: make CROSS_COMPILE=${HOME}/host-tools/gcc/gcc-linaro-6.3.1-2017.05-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-
COMMON_DIR ?= $(CURDIR)/../../common
include $(COMMON_DIR)/base.mk


CVILIB_DIR := lib

TARGET_DIR	?= $(CURDIR)/install
TARGET		:= $(TARGET_DIR)/sample_rtsp_server

INCS		+= -I$(CURDIR)/include

RTSP_DIR ?= $(CURDIR)/..
LIBS		+= -L$(RTSP_DIR)/$(CVILIB_DIR) -lcvi_rtsp
INCS		+= -I$(RTSP_DIR)/include

RBUF_DIR	?= $(CURDIR)/../../ringbuffer
LIBS		+= -L$(RBUF_DIR)/$(CVILIB_DIR) -lcvi_ringbuffer
INCS		+= -I$(RBUF_DIR)/include

LOG_DIR		?= $(CURDIR)/../../cvi_log
LIBS		+= -L$(LOG_DIR)/$(CVILIB_DIR) -lcvi_log
INCS		+= -I$(LOG_DIR)/include

OSAL_DIR	?= $(CURDIR)/../../cvi_osal
LIBS		+= -L$(OSAL_DIR)/$(CVILIB_DIR) -lcvi_osal
INCS		+= -I$(OSAL_DIR)/include

INCS		+= -I$(CURDIR)/include

DYN_LIBS	+= -lm -lpthread -ldl -lz

#LDFLAGS_SO	= -shared -fPIC -Wl,--gc-sections

#CFLAGS		+= -Wno-unused-parameter
#CFLAGS		+= -rdynamic  -funwind-tables -ffunction-sections # for stack trace

.PHONY : all clean

all : $(TARGET)

SDIR		:= $(CURDIR)/src
SEXT		:= c
SRCS ?= $(shell find $(SDIR) -type f -name "*.$(SEXT)") # find curdir all *.c

ODIR := $(SDIR)
OBJS := $(addprefix $(ODIR)/,$(notdir $(SRCS:.$(SEXT)=.o)))
DEPS := $(addprefix $(ODIR)/,$(notdir $(SRCS:.$(SEXT)=.d)))

CFLAGS		+= $(INCS)
CXXFLAGS	+= $(INCS)

$(ODIR) :
	$(Q)mkdir -p $@

$(ODIR)/%.o : $(SDIR)/%.c | $(ODIR)
	$(Q)$(CC) $(CFLAGS) -o $@ -c $<
	@echo [$(notdir $(CC))] $(notdir $@)

$(ODIR)/%.o : $(SDIR)/%.cpp | $(ODIR)
	$(Q)$(CXX) $(CXXFLAGS) -o $@ -c $<
	@echo [$(notdir $(CC))] $(notdir $@)

$(TARGET_DIR) :
	mkdir -p $@
	$(Q)$(CC) -dumpmachine | xargs -I {} bash -c 'touch $@/Target:"{}".txt && $(CC) -v 2> $@/Target:"{}".txt'

$(TARGET) : $(OBJS) | $(TARGET_DIR)
	$(CC) $(LDFLAGS) $(ELFFLAGS) -o  $@.static $^ -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic $(DYN_LIBS)
	echo $(CYAN)[LINK]$(END)[$(notdir $(CC))] $(notdir $@.static)
	$(CC) $(LDFLAGS) $(ELFFLAGS) -o  $@ $^ -Wl,-Bdynamic $(DYN_LIBS) $(LIBS)
	echo $(PURPLE)[LINK]$(END)[$(notdir $(CC))] $(notdir $@)

clean:
	$(Q)rm -rf $(OBJS) $(DEPS) $(TARGET_DIR)

-include $(DEPS)
