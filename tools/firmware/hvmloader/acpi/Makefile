#
# Copyright (c) 2004, Intel Corporation.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms and conditions of the GNU General Public License,
# version 2, as published by the Free Software Foundation.
#
# This program is distributed in the hope it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc., 59 Temple
# Place - Suite 330, Boston, MA 02111-1307 USA.
#

XEN_ROOT = ../../../..
include $(XEN_ROOT)/tools/firmware/Rules.mk

C_SRC = build.c dsdt.c static_tables.c
H_SRC = $(wildcard *.h)
OBJS  = $(patsubst %.c,%.o,$(C_SRC))

IASL_VER = acpica-unix-20080729
IASL_URL = http://acpica.org/download/$(IASL_VER).tar.gz

CFLAGS += -I. -I.. $(CFLAGS_include)

vpath iasl $(PATH)
all: acpi.a

ssdt_tpm.h: ssdt_tpm.asl
	$(MAKE) iasl
	iasl -tc ssdt_tpm.asl
	mv ssdt_tpm.hex ssdt_tpm.h
	rm -f *.aml

dsdt.c: dsdt.asl
	$(MAKE) iasl
	iasl -tc dsdt.asl
	mv dsdt.hex dsdt.c
	echo "int DsdtLen=sizeof(AmlCode);" >> dsdt.c
	rm -f *.aml

iasl:
	@echo
	@echo "ACPI ASL compiler(iasl) is needed"
	@echo "Download Intel ACPI CA"
	@echo "If wget failed, please download and compile manually from"
	@echo "http://acpica.org/downloads/"
	@echo 
	wget $(IASL_URL)
	tar xzf $(IASL_VER).tar.gz
	make -C $(IASL_VER)/compiler
	$(INSTALL_PROG) $(IASL_VER)/compiler/iasl $(DESTDIR)$(BINDIR)/iasl

acpi.a: $(OBJS)
	$(AR) rc $@ $(OBJS)

%.o: %.c $(H_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf *.a *.o $(IASL_VER) $(IASL_VER).tar.gz

install: all
