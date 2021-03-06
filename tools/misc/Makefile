XEN_ROOT=../..
include $(XEN_ROOT)/tools/Rules.mk

CFLAGS   += -Werror

INCLUDES += -I $(XEN_XC)
INCLUDES += -I $(XEN_LIBXC)
INCLUDES += -I $(XEN_INCLUDE)
CFLAGS   += $(INCLUDES)

HDRS     = $(wildcard *.h)

TARGETS-y := xenperf
TARGETS-$(CONFIG_X86) += xen-detect xenpm
TARGETS := $(TARGETS-y)

SUBDIRS-$(CONFIG_LOMOUNT) += lomount
SUBDIRS-$(CONFIG_MINITERM) += miniterm
SUBDIRS := $(SUBDIRS-y)

INSTALL_BIN-y := xencons
INSTALL_BIN-$(CONFIG_X86) += xen-detect
INSTALL_BIN := $(INSTALL_BIN-y)

INSTALL_SBIN-y := netfix xm xen-bugtool xen-python-path xend xenperf xsview
INSTALL_SBIN-$(CONFIG_X86) += xenpm
INSTALL_SBIN := $(INSTALL_SBIN-y)

DEFAULT_PYTHON_PATH := $(shell $(XEN_ROOT)/tools/python/get-path)
PYTHON_PATH ?= $(DEFAULT_PYTHON_PATH)
INSTALL_PYTHON_PROG = $(XEN_ROOT)/tools/python/install-wrap \
"$(PYTHON_PATH)" $(INSTALL_PROG)

.PHONY: all
all: build

.PHONY: build
build: $(TARGETS)
	set -e; for d in $(SUBDIRS); do $(MAKE) -C $$d; done

.PHONY: install
install: build
	$(INSTALL_DIR) $(DESTDIR)$(BINDIR)
	$(INSTALL_DIR) $(DESTDIR)$(SBINDIR)
	$(INSTALL_PYTHON_PROG) $(INSTALL_BIN) $(DESTDIR)$(BINDIR)
	$(INSTALL_PYTHON_PROG) $(INSTALL_SBIN) $(DESTDIR)$(SBINDIR)
	set -e; for d in $(SUBDIRS); do $(MAKE) -C $$d install-recurse; done

.PHONY: clean
clean:
	$(RM) *.o $(TARGETS) *~
	set -e; for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done

%.o: %.c $(HDRS) Makefile
	$(CC) -c $(CFLAGS) -o $@ $<

xenperf xenpm: %: %.o Makefile
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDFLAGS_libxenctrl)
