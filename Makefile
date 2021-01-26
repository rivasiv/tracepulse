SPDK_ROOT_DIR := /root/spdk
#SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

PULSEDEBUG = -g -DDEBUG=1 

COMMON_CFLAGS += $(PULSEDEBUG) -I/root/libtrace/ -I/root/libtrace/lib/

APP = tracepulspdk

C_SRCS := tracepulspdk.c

SPDK_LIB_LIST = $(ALL_MODULES_LIST)
SPDK_LIB_LIST += event_bdev event_accel event_nvmf event_net event_vmd
SPDK_LIB_LIST += nvme bdev_nvme nvmf event log trace conf thread util bdev accel rpc jsonrpc json net sock
SPDK_LIB_LIST += app_rpc log_rpc bdev_rpc notify

SYS_LIBS += -lcrypto -ltrace

ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
SPDK_LIB_LIST += env_dpdk_rpc
endif

ifeq ($(OS),Linux)
SPDK_LIB_LIST += event_nbd nbd
endif

ifeq ($(CONFIG_FC),y)
ifneq ($(strip $(CONFIG_FC_PATH)),)
SYS_LIBS += -L$(CONFIG_FC_PATH)
endif
SYS_LIBS += -lufc
endif

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
