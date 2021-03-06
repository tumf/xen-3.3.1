XEN_ROOT = ../
include $(XEN_ROOT)/tools/Rules.mk

SUBDIRS-y :=
SUBDIRS-y += check
SUBDIRS-y += include
SUBDIRS-y += libxc
SUBDIRS-y += flask
SUBDIRS-y += xenstore
SUBDIRS-y += misc
SUBDIRS-y += examples
SUBDIRS-y += xentrace
SUBDIRS-$(CONFIG_XCUTILS) += xcutils
SUBDIRS-$(CONFIG_X86) += firmware
SUBDIRS-$(ACM_SECURITY) += security
SUBDIRS-y += console
SUBDIRS-y += xenmon
SUBDIRS-$(VTPM_TOOLS) += vtpm_manager
SUBDIRS-$(VTPM_TOOLS) += vtpm
SUBDIRS-y += xenstat
SUBDIRS-y += libaio
SUBDIRS-y += blktap
SUBDIRS-y += libfsimage
SUBDIRS-$(LIBXENAPI_BINDINGS) += libxen
SUBDIRS-y += fs-back

ifeq (ioemu,$(CONFIG_QEMU))
SUBDIRS-$(CONFIG_IOEMU) += ioemu
else
SUBDIRS-$(CONFIG_IOEMU) += ioemu-dir
endif

# These don't cross-compile
ifeq ($(XEN_COMPILE_ARCH),$(XEN_TARGET_ARCH))
SUBDIRS-$(PYTHON_TOOLS) += python
SUBDIRS-$(PYTHON_TOOLS) += pygrub
endif

# For the sake of linking, set the sys-root
ifneq ($(CROSS_COMPILE),)
CROSS_BIN_PATH ?= /usr/$(CROSS_COMPILE:-=)/bin
CROSS_SYS_ROOT ?= /usr/$(CROSS_COMPILE:-=)/sys-root
export CROSS_SYS_ROOT # exported for check/funcs.sh
export CROSS_BIN_PATH # exported for cross-install.sh
endif

.PHONY: all
all: subdirs-all

.PHONY: install
install: subdirs-install
	$(INSTALL_DIR) $(DESTDIR)/var/xen/dump
	$(INSTALL_DIR) $(DESTDIR)/var/log/xen
	$(INSTALL_DIR) $(DESTDIR)/var/lib/xen

.PHONY: clean distclean
clean distclean: subdirs-clean

ifneq ($(XEN_COMPILE_ARCH),$(XEN_TARGET_ARCH))
IOEMU_CONFIGURE_CROSS ?= --cpu=$(XEN_TARGET_ARCH) \
			 --cross-prefix=$(CROSS_COMPILE) \
			 --interp-prefix=$(CROSS_SYS_ROOT) \
			 --install=$(CURDIR)/cross-install
endif

ioemu/config-host.mak:
	cd ioemu && XEN_TARGET_ARCH=$(XEN_TARGET_ARCH) sh configure \
		--prefix=$(PREFIX) $(IOEMU_CONFIGURE_CROSS)

subdir-all-ioemu subdir-install-ioemu: ioemu/config-host.mak

subdir-clean-ioemu:
	$(MAKE) -C ioemu distclean

ioemu-dir-find:
	set -ex; \
	if test -d $(CONFIG_QEMU); then \
		rm -f ioemu-dir; \
		ln -sf $(CONFIG_QEMU) ioemu-dir; \
	else \
		if [ ! -d ioemu-remote ]; then \
			rm -rf ioemu-remote ioemu-remote.tmp; \
			mkdir ioemu-remote.tmp; rmdir ioemu-remote.tmp; \
			$(GIT) clone $(CONFIG_QEMU) ioemu-remote.tmp; \
			if [ "$(QEMU_TAG)" ]; then			\
				cd ioemu-remote.tmp;			\
				$(GIT) branch -D dummy >/dev/null 2>&1 ||:; \
				$(GIT) checkout -b dummy $(QEMU_TAG);	\
				cd ..;					\
			fi;						\
			mv ioemu-remote.tmp ioemu-remote; \
		fi; \
		rm -f ioemu-dir; \
		ln -sf ioemu-remote ioemu-dir; \
	fi
	set -e; \
		case "$(XEN_ROOT)" in \
		/*)	XEN_ROOT=$(XEN_ROOT) ;; \
		*)	XEN_ROOT=`pwd`/$(XEN_ROOT) ;; \
		esac; \
		export XEN_ROOT; \
		cd ioemu-dir; \
		./xen-setup $(IOEMU_CONFIGURE_CROSS)

subdir-all-ioemu-dir subdir-install-ioemu-dir: ioemu-dir-find

subdir-clean-ioemu-dir:
	if test -d ioemu-dir/.; then \
		$(MAKE) -C ioemu-dir clean; \
	fi
