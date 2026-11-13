ROOT := ../..
OUTPUT ?= $(ROOT)/out/bf01-wifi-isolated
CC ?= cc
AR ?= ar
CPPFLAGS += -I$(ROOT)/solutions/ipcamera/include -I$(ROOT)/modules/common/include
CFLAGS ?= -O2
CFLAGS += -std=gnu11 -Wall -Wextra -Werror
OBJECTS := $(OUTPUT)/wifi_link.o $(OUTPUT)/wifi_sdio.o $(OUTPUT)/cJSON.o

.PHONY: all
all: $(OUTPUT)/libbf01_wifi_link.a

$(OUTPUT)/libbf01_wifi_link.a: $(OBJECTS)
	$(AR) rcs $@ $^

$(OUTPUT)/wifi_%.o: $(ROOT)/solutions/ipcamera/src/bf01_wifi_%.c $(ROOT)/solutions/ipcamera/include/bf01_wifi_link.h
	@mkdir -p $(OUTPUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUTPUT)/cJSON.o: $(ROOT)/modules/common/cjson/src/cJSON.c
	@mkdir -p $(OUTPUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
