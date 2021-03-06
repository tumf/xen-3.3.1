# Makefile for QEMU.

include config-host.mak

.PHONY: all clean distclean dvi info install install-doc tar tarbin \
	speed test html dvi info

VPATH=$(SRC_PATH):$(SRC_PATH)/hw

CFLAGS += $(OS_CFLAGS) $(ARCH_CFLAGS)
LDFLAGS += $(OS_LDFLAGS) $(ARCH_LDFLAGS)

CPPFLAGS += -I. -I$(SRC_PATH) -MMD -MP
CPPFLAGS += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
LIBS=
ifdef CONFIG_STATIC
LDFLAGS += -static
endif
ifdef BUILD_DOCS
DOCS=qemu-doc.html qemu-tech.html qemu.1 qemu-img.1
else
DOCS=
endif

LIBS+=$(AIOLIBS)

all: $(TOOLS) $(DOCS) recurse-all 

subdir-%: libqemu_common.a
	$(MAKE) -C $(subst subdir-,,$@) all

recurse-all: $(patsubst %,subdir-%, $(TARGET_DIRS))

tapdisk-ioemu: CPPFLAGS += -I$(XEN_ROOT)/tools/libxc
tapdisk-ioemu: CPPFLAGS += -I$(XEN_ROOT)/tools/blktap/lib
tapdisk-ioemu: CPPFLAGS += -I$(XEN_ROOT)/tools/xenstore
tapdisk-ioemu: CPPFLAGS += -I$(XEN_ROOT)/tools/include
tapdisk-ioemu: tapdisk-ioemu.c cutils.c block.c block-raw.c block-cow.c block-qcow.c aes.c block-vmdk.c block-cloop.c block-dmg.c block-bochs.c block-vpc.c block-vvfat.c block-qcow2.c hw/xen_blktap.c osdep.c
	$(CC) -DQEMU_TOOL $(CFLAGS) $(CPPFLAGS) $(BASE_CFLAGS) $(LDFLAGS) $(BASE_LDFLAGS) -o $@ $^ -lz $(LIBS)

#######################################################################
# BLOCK_OBJS is code used by both qemu system emulation and qemu-img

BLOCK_OBJS=cutils.o osdep.o
BLOCK_OBJS+=block-cow.o block-qcow.o aes.o block-vmdk.o block-cloop.o
BLOCK_OBJS+=block-dmg.o block-bochs.o block-vpc.o block-vvfat.o
BLOCK_OBJS+=block-qcow2.o block-parallels.o

######################################################################
# libqemu_common.a: Target independent part of system emulation. The
# long term path is to suppress *all* target specific code in case of
# system emulation, i.e. a single QEMU executable should support all
# CPUs and machines.

OBJS=$(BLOCK_OBJS)
OBJS+=readline.o console.o
OBJS+=block.o

OBJS+=irq.o
OBJS+=i2c.o smbus.o smbus_eeprom.o max7310.o max111x.o wm8750.o
OBJS+=ssd0303.o ssd0323.o ads7846.o stellaris_input.o twl92230.o
OBJS+=tmp105.o lm832x.o
OBJS+=scsi-disk.o cdrom.o
OBJS+=scsi-generic.o
OBJS+=usb.o usb-hub.o usb-linux.o usb-hid.o usb-msd.o usb-wacom.o usb-serial.o
OBJS+=sd.o ssi-sd.o

ifdef CONFIG_BRLAPI
OBJS+= baum.o
LIBS+=-lbrlapi
endif

ifdef CONFIG_WIN32
OBJS+=tap-win32.o
endif

AUDIO_OBJS = audio.o noaudio.o wavaudio.o mixeng.o
ifdef CONFIG_SDL
AUDIO_OBJS += sdlaudio.o
endif
ifdef CONFIG_OSS
AUDIO_OBJS += ossaudio.o
endif
ifdef CONFIG_COREAUDIO
AUDIO_OBJS += coreaudio.o
AUDIO_PT = yes
endif
ifdef CONFIG_ALSA
AUDIO_OBJS += alsaaudio.o
endif
ifdef CONFIG_DSOUND
AUDIO_OBJS += dsoundaudio.o
endif
ifdef CONFIG_FMOD
AUDIO_OBJS += fmodaudio.o
audio/audio.o audio/fmodaudio.o: CPPFLAGS := -I$(CONFIG_FMOD_INC) $(CPPFLAGS)
endif
ifdef CONFIG_ESD
AUDIO_PT = yes
AUDIO_PT_INT = yes
AUDIO_OBJS += esdaudio.o
endif
ifdef AUDIO_PT
LDFLAGS += -pthread
endif
ifdef AUDIO_PT_INT
AUDIO_OBJS += audio_pt_int.o
endif
AUDIO_OBJS+= wavcapture.o
ifdef CONFIG_AUDIO
OBJS+=$(addprefix audio/, $(AUDIO_OBJS))
endif

ifdef CONFIG_SDL
OBJS+=sdl.o x_keymap.o
endif
ifdef CONFIG_CURSES
OBJS+=curses.o
endif
OBJS+=vnc.o d3des.o

ifdef CONFIG_COCOA
OBJS+=cocoa.o
endif

ifdef CONFIG_SLIRP
CPPFLAGS+=-I$(SRC_PATH)/slirp
SLIRP_OBJS=cksum.o if.o ip_icmp.o ip_input.o ip_output.o \
slirp.o mbuf.o misc.o sbuf.o socket.o tcp_input.o tcp_output.o \
tcp_subr.o tcp_timer.o udp.o bootp.o debug.o tftp.o
OBJS+=$(addprefix slirp/, $(SLIRP_OBJS))
endif

cocoa.o: cocoa.m
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

sdl.o: sdl.c keymaps.c sdl_keysym.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SDL_CFLAGS) -c -o $@ $<

vnc.o: vnc.c keymaps.c sdl_keysym.h vnchextile.h d3des.c d3des.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CONFIG_VNC_TLS_CFLAGS) -c -o $@ $<

curses.o: curses.c keymaps.c curses_keys.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

audio/sdlaudio.o: audio/sdlaudio.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SDL_CFLAGS) -c -o $@ $<

libqemu_common.a: $(OBJS)
	rm -f $@ 
	$(AR) rcs $@ $(OBJS)

QEMU_IMG_BLOCK_OBJS = $(BLOCK_OBJS)
ifdef CONFIG_WIN32
QEMU_IMG_BLOCK_OBJS += qemu-img-block-raw-win32.o
else
QEMU_IMG_BLOCK_OBJS += qemu-img-block-raw-posix.o
endif

######################################################################

qemu-img$(EXESUF): qemu-img.o qemu-img-block.o $(QEMU_IMG_BLOCK_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lz $(LIBS)

qemu-img-%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DQEMU_IMG -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# dyngen host tool
dyngen$(EXESUF): dyngen.o osdep.o
	$(HOST_CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

clean:
# avoid old build problems by removing potentially incorrect old files
	rm -f config.mak config.h op-i386.h opc-i386.h gen-op-i386.h op-arm.h opc-arm.h gen-op-arm.h
	rm -f *.o *.d *.a $(TOOLS) dyngen$(EXESUF) TAGS cscope.* *.pod *~ */*~
	rm -rf dyngen.dSYM
	rm -f slirp/*.o slirp/*.d audio/*.o audio/*.d
	$(MAKE) -C tests clean
	for d in $(TARGET_DIRS); do \
	$(MAKE) -C $$d $@ || exit 1 ; \
        done

distclean: clean
	rm -f config-host.mak config-host.h $(DOCS)
	rm -f qemu-{doc,tech}.{info,aux,cp,dvi,fn,info,ky,log,pg,toc,tp,vr}
	for d in $(TARGET_DIRS); do \
	rm -rf $$d || exit 1 ; \
        done

KEYMAPS=da     en-gb  et  fr     fr-ch  is  lt  modifiers  no  pt-br  sv \
ar      de     en-us  fi  fr-be  hr     it  lv  nl         pl  ru     th \
common  de-ch  es     fo  fr-ca  hu     ja  mk  nl-be      pt  sl     tr

install-doc: $(DOCS)
	mkdir -p "$(DESTDIR)$(docdir)"
	$(INSTALL) -m 644 qemu-doc.html  qemu-tech.html "$(DESTDIR)$(docdir)"
ifndef CONFIG_WIN32
	mkdir -p "$(DESTDIR)$(mandir)/man1"
	$(INSTALL) qemu.1 qemu-img.1 "$(DESTDIR)$(mandir)/man1"
endif

install: all $(if $(BUILD_DOCS),install-doc)
	mkdir -p "$(DESTDIR)$(bindir)"
ifneq ($(TOOLS),)
	$(INSTALL) -m 755 -s $(TOOLS) "$(DESTDIR)$(bindir)"
endif
	mkdir -p "$(DESTDIR)$(datadir)"
	set -e; for x in bios.bin vgabios.bin vgabios-cirrus.bin ppc_rom.bin \
		video.x openbios-sparc32 openbios-sparc64 pxe-ne2k_pci.bin \
		pxe-rtl8139.bin pxe-pcnet.bin; do \
		$(INSTALL) -m 644 $(SRC_PATH)/pc-bios/$$x "$(DESTDIR)$(datadir)"; \
	done
ifndef CONFIG_WIN32
	mkdir -p "$(DESTDIR)$(datadir)/keymaps"
	set -e; for x in $(KEYMAPS); do \
		$(INSTALL) -m 644 $(SRC_PATH)/keymaps/$$x "$(DESTDIR)$(datadir)/keymaps"; \
	done
endif
	for d in $(TARGET_DIRS); do \
	$(MAKE) -C $$d $@ || exit 1 ; \
        done

# various test targets
test speed: all
	$(MAKE) -C tests $@

TAGS:
	etags *.[ch] tests/*.[ch]

cscope:
	rm -f ./cscope.*
	find . -name "*.[ch]" -print > ./cscope.files
	cscope -b

# documentation
%.html: %.texi
	texi2html -monolithic -number $<

%.info: %.texi
	makeinfo $< -o $@

%.dvi: %.texi
	texi2dvi $<

qemu.1: qemu-doc.texi
	$(SRC_PATH)/texi2pod.pl $< qemu.pod
	pod2man --section=1 --center=" " --release=" " qemu.pod > $@

qemu-img.1: qemu-img.texi
	$(SRC_PATH)/texi2pod.pl $< qemu-img.pod
	pod2man --section=1 --center=" " --release=" " qemu-img.pod > $@

info: qemu-doc.info qemu-tech.info

dvi: qemu-doc.dvi qemu-tech.dvi

html: qemu-doc.html qemu-tech.html

VERSION ?= $(shell cat VERSION)
FILE = qemu-$(VERSION)

# tar release (use 'make -k tar' on a checkouted tree)
tar:
	rm -rf /tmp/$(FILE)
	cp -r . /tmp/$(FILE)
	cd /tmp && tar zcvf ~/$(FILE).tar.gz $(FILE) --exclude CVS --exclude .git --exclude .svn
	rm -rf /tmp/$(FILE)

# generate a binary distribution
tarbin:
	cd / && tar zcvf ~/qemu-$(VERSION)-$(ARCH).tar.gz \
	$(bindir)/qemu \
	$(bindir)/qemu-system-ppc \
	$(bindir)/qemu-system-ppc64 \
	$(bindir)/qemu-system-ppcemb \
	$(bindir)/qemu-system-sparc \
	$(bindir)/qemu-system-x86_64 \
	$(bindir)/qemu-system-mips \
	$(bindir)/qemu-system-mipsel \
	$(bindir)/qemu-system-mips64 \
	$(bindir)/qemu-system-mips64el \
	$(bindir)/qemu-system-arm \
	$(bindir)/qemu-system-m68k \
	$(bindir)/qemu-system-sh4 \
	$(bindir)/qemu-system-sh4eb \
	$(bindir)/qemu-system-cris \
	$(bindir)/qemu-i386 \
	$(bindir)/qemu-x86_64 \
        $(bindir)/qemu-arm \
        $(bindir)/qemu-armeb \
        $(bindir)/qemu-sparc \
        $(bindir)/qemu-sparc32plus \
        $(bindir)/qemu-sparc64 \
        $(bindir)/qemu-ppc \
        $(bindir)/qemu-ppc64 \
        $(bindir)/qemu-ppc64abi32 \
        $(bindir)/qemu-mips \
        $(bindir)/qemu-mipsel \
        $(bindir)/qemu-alpha \
        $(bindir)/qemu-m68k \
        $(bindir)/qemu-sh4 \
        $(bindir)/qemu-sh4eb \
        $(bindir)/qemu-cris \
        $(bindir)/qemu-img \
	$(datadir)/bios.bin \
	$(datadir)/vgabios.bin \
	$(datadir)/vgabios-cirrus.bin \
	$(datadir)/ppc_rom.bin \
	$(datadir)/video.x \
	$(datadir)/openbios-sparc32 \
	$(datadir)/openbios-sparc64 \
        $(datadir)/pxe-ne2k_pci.bin \
	$(datadir)/pxe-rtl8139.bin \
        $(datadir)/pxe-pcnet.bin \
	$(docdir)/qemu-doc.html \
	$(docdir)/qemu-tech.html \
	$(mandir)/man1/qemu.1 $(mandir)/man1/qemu-img.1

# Include automatically generated dependency files
-include $(wildcard *.d audio/*.d slirp/*.d)
