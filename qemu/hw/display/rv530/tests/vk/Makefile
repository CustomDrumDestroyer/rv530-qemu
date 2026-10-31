# Differential test: CPU reference path vs Vulkan backend (needs a Vulkan
# 1.3 GPU with pixel interlock and VK_EXT_external_memory_host).
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter
TOP     := ../..
CPPFLAGS += -I$(TOP) $(shell pkg-config --cflags vulkan shaderc)
SRCS := test_vk.c $(TOP)/r5xx_vk.c $(TOP)/r5xx_vk_state.c $(TOP)/r5xx_raster.c \
        $(TOP)/r5xx_core.c $(TOP)/r5xx_tex.c $(TOP)/r500_us.c
LIBS := $(shell pkg-config --libs vulkan shaderc) -lm
ifeq ($(shell uname -s),Darwin)
# r5xx_vk_create() checks the Metal devices before loading MoltenVK
SRCS += $(TOP)/r5xx_vk_mac.m
LIBS += -framework Metal -framework Foundation
endif

all: test_vk

test_vk: $(SRCS) $(wildcard $(TOP)/*.h)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRCS) $(LIBS)

check: test_vk
	./test_vk

clean:
	rm -f test_vk

.PHONY: all check clean
