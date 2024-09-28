#
# Copyright 2016-2019, Bromium, Inc.
# SPDX-License-Identifier: ISC
#

ifneq ($(KERNELRELEASE),)
# kbuild part of makefile
include Kbuild

else
#normal makefile
KERNEL_VERSION ?= $(shell uname -r)
KERNEL_SRC ?= /lib/modules/${KERNEL_VERSION}/build
UXENDIR ?= $(shell pwd)/include/uxen
INSTALL_HDR_PATH ?= /usr

LX_TARGET_FLAGS= -DLX_TARGET_STANDARDVM
LX_TARGET_ATTOVM=n
LX_TARGET_STANDARDVM=y
ifeq ($(TARGET_SECURE_AX),1)
    LX_TARGET_FLAGS= -DLX_TARGET_ATTOVM
    LX_TARGET_ATTOVM=y
    LX_TARGET_STANDARDVM=n
endif
LX_TARGET=LX_TARGET_ATTOVM=$(LX_TARGET_ATTOVM) LX_TARGET_STANDARDVM=$(LX_TARGET_STANDARDVM)
EXTRA_CFLAGS=$(LX_TARGET_FLAGS) -g -Wall
NOSTDINC_FLAGS=-I$(shell pwd)/include/ -I$(UXENDIR) -I$(UXENDIR)/xen

all:
	make -C $(KERNEL_SRC) $(LX_TARGET) M=$(shell pwd) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" NOSTDINC_FLAGS="$(NOSTDINC_FLAGS)"
clean:
	make -C $(KERNEL_SRC) $(LX_TARGET) M=$(shell pwd) clean
modules_install:
	make -C $(KERNEL_SRC) $(LX_TARGET) M=$(shell pwd) modules_install
endif
