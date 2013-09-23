#
# Makefile for the VPU drivers.
#

obj-$(CONFIG_MXC_VPU)		+= mxc_vpu.o
obj-$(CONFIG_MXC_IRAM)		+= iram_alloc.o

ifeq ($(CONFIG_MXC_VPU_DEBUG),y)
EXTRA_CFLAGS += -DDEBUG
endif
