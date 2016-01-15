#
# Copyright 2011-2016, Bromium, Inc.
# Author: Christian Limpach <Christian.Limpach@gmail.com>
# SPDX-License-Identifier: ISC
#

UXEN_TARGET_FORMAT ?= pe

HOST_NOT_WINE := $(patsubst %,n-,$(WINE_BUILD))
HOST_WINE := $(subst n-n-,,n-$(HOST_NOT_WINE))

$(HOST_NOT_WINE)WINDDK_VER = 7600.16385.1
$(HOST_WINE)WINDDK_VER = 7600.16385.win7_wdk.100208-1538

WINDDK_DIR ?= C:\WinDDK\$(WINDDK_VER)
WINDDK_BUILD ?= build -w

UXEN_WINDOWS_SIGN_FILE ?= $(TOPDIR)/windows/build/uXenDevCert.pfx
UXEN_WINDOWS_SIGN_CERT ?= $(UXEN_WINDOWS_SIGN_FILE:%.pfx=%.cer)

UXEN_WINDOWS_SIGN ?= $(WINDDK_DIR)\bin\x86\signtool sign /q /f $(UXEN_WINDOWS_SIGN_FILE)

$(HOST_WINDOWS)NATIVE_PWD = pwd -W
$(HOST_NOT_WINDOWS)NATIVE_PWD = pwd

dospath = $(subst /,\\,$(shell mkdir -p $(dir $(1)) && cd $(dir $(1)) && $(NATIVE_PWD))/$(notdir $(1)))

$(DEBUG_ONLY)DDKENV ?= chk
$(REL_ONLY)DDKENV ?= fre

CV2PDB ?= cv2pdb.exe
GENERATE_PDB ?= true

# everything below only for builds under {vm-support/,}windows/
ifeq (,$(patsubst $(TARGET_HOST)/%,,$(patsubst vm-support/%,%,$(SUBDIR))))

UXEN_WINDOWS_SIGN_FILE := $(call dospath,$(UXEN_WINDOWS_SIGN_FILE))

# this is CC ?= but honouring CC from the environment
CC := $(if $(subst cc,,$(CC)),$(CC),x86_64-w64-mingw32-gcc)
#CXX := $(if $(subst c++,,$(CXX)),$(CXX),x86_64-w64-mingw32-g++)
AR := $(if $(subst ar,,$(AR)),$(AR),x86_64-w64-mingw32-ar)
RANLIB := $(if $(subst ranlib,,$(RANLIB)),$(RANLIB),x86_64-w64-mingw32-ranlib)
WINDRES := $(if $(subst windres,,$(WINDRES)),$(WINDRES),x86_64-w64-mingw32-windres)
WINDMC := $(if $(subst windmc,,$(WINDMC)),$(WINDMC),x86_64-w64-mingw32-windmc)
STRIP := $(if $(subst strip,,$(STRIP)),$(STRIP),x86_64-w64-mingw32-strip)

ifeq ($(TARGET_HOST_BITS),32)
WINDRES_TARGET_FORMAT_OPTION := --target=pe-i386
else
WINDRES_TARGET_FORMAT_OPTION := 
endif

ifneq (,$(filter-out false 0 n no,$(GENERATE_PDB)))
genpdb = $(CV2PDB) $1 $1 $2
else
genpdb = true
endif

ifeq (,$(HOST_WINDOWS))
sign = ($2 && cmd /c "$(UXEN_WINDOWS_SIGN) $1") || \
	(rm -f $1; false)
link = $(LINK.o) -o $1 $2
install_exe_strip = (dbg=$2; pdb=$${dbg%.*}.pdb;                      \
                     d=$$(dirname $1); f=$$(basename $1);             \
                     $(call genpdb,$$dbg,$$pdb) &&                    \
                     $(STRIP) -o $1 $2 && \
                     (cd "$$d" && cmd /c "$(UXEN_WINDOWS_SIGN) $$f"))
else
sign = $2
link = $(LINK.o) -o $1 $2
endif

CPPFLAGS += -I$(abspath $(TOPDIR)/windows/include)
CPPFLAGS += -I$(abspath $(TOPDIR)/common/include)
CPPFLAGS += -I$(abspath $(TOOLSDIR)/cross-mingw/include)

$(REL_ONLY)LDFLAGS += -Wl,--dynamicbase -Wl,--nxcompat -pie 
ifeq ($(TARGET_HOST_BITS),32)
# For some reason binutils sets the wrong entry address in binaries linked with -pie 
# (it works on 64bit by accident so we probably want to pull this across)
$(REL_ONLY)LDFLAGS += -Wl,-e_mainCRTStartup
endif


LDLIBS_ssp += -lssp
CFLAGS_ssp += -D_FORTIFY_SOURCE=2 -fstack-protector \
	--param ssp-buffer-size=4 -Wformat -Wformat-security
LDLIBS += $(LDLIBS_ssp)
CFLAGS += $(CFLAGS_ssp)

CFLAGS += -fno-strict-aliasing

CFLAGS += -m$(TARGET_HOST_BITS)
LDFLAGS += -m$(TARGET_HOST_BITS)

CFLAGS += -mno-ms-bitfields

endif
