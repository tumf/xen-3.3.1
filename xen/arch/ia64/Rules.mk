########################################
# ia64-specific definitions

ia64 := y
HAS_ACPI := y
HAS_VGA  := y
xenoprof := y
no_warns ?= n
vti_debug ?= n
vmx_panic ?= n
vhpt_disable ?= n
xen_ia64_expose_p2m	?= y
xen_ia64_pervcpu_vhpt	?= y
xen_ia64_tlb_track	?= y
xen_ia64_tlb_track_cnt	?= n
xen_ia64_tlbflush_clock	?= y

# Used only by linux/Makefile.
AFLAGS_KERNEL  += -mconstant-gp -nostdinc $(CPPFLAGS)

CFLAGS	+= -nostdinc -fno-builtin -fno-common
CFLAGS	+= -mconstant-gp
#CFLAGS  += -O3		# -O3 over-inlines making debugging tough!
CFLAGS	+= -O2		# but no optimization causes compile errors!
CFLAGS	+= -fomit-frame-pointer -D__KERNEL__
CFLAGS	+= -iwithprefix include
CPPFLAGS+= -I$(BASEDIR)/include						\
	   -I$(BASEDIR)/include/asm-ia64				\
	   -I$(BASEDIR)/include/asm-ia64/linux 				\
	   -I$(BASEDIR)/include/asm-ia64/linux-xen 			\
	   -I$(BASEDIR)/include/asm-ia64/linux-null 			\
	   -I$(BASEDIR)/arch/ia64/linux -I$(BASEDIR)/arch/ia64/linux-xen
CFLAGS	+= $(CPPFLAGS)
#CFLAGS  += -Wno-pointer-arith -Wredundant-decls
CFLAGS	+= -DIA64 -DXEN -DLINUX_2_6
CFLAGS	+= -ffixed-r13 -mfixed-range=f2-f5,f12-f127,b2-b5
CFLAGS	+= -g
ifeq ($(vti_debug),y)
CFLAGS  += -DVTI_DEBUG
endif
ifeq ($(vmx_panic),y)
CFLAGS  += -DCONFIG_VMX_PANIC
endif
ifeq ($(xen_ia64_expose_p2m),y)
CFLAGS	+= -DCONFIG_XEN_IA64_EXPOSE_P2M
endif
ifeq ($(xen_ia64_pervcpu_vhpt),y)
CFLAGS	+= -DCONFIG_XEN_IA64_PERVCPU_VHPT
ifeq ($(vhpt_disable),y)
$(error "both xen_ia64_pervcpu_vhpt=y and vhpt_disable=y are enabled. they can't be enabled simultaneously. disable one of them.")
endif
endif
ifeq ($(xen_ia64_tlb_track),y)
CFLAGS	+= -DCONFIG_XEN_IA64_TLB_TRACK
endif
ifeq ($(xen_ia64_tlb_track_cnt),y)
CFLAGS	+= -DCONFIG_TLB_TRACK_CNT
endif
ifeq ($(xen_ia64_tlbflush_clock),y)
CFLAGS += -DCONFIG_XEN_IA64_TLBFLUSH_CLOCK
endif
ifeq ($(no_warns),y)
CFLAGS	+= -Wa,--fatal-warnings -Werror -Wno-uninitialized
endif
ifneq ($(vhpt_disable),y)
CFLAGS += -DVHPT_ENABLED=1
else
CFLAGS += -DVHPT_ENABLED=0
endif

LDFLAGS := -g

# Additionnal IA64 include dirs.
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux-null/asm/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux-null/asm/sn/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux-null/linux/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux-xen/asm/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux-xen/asm/sn/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux-xen/linux/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux/asm-generic/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux/asm/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux/byteorder/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/hvm/*.h)

HDRS := $(filter-out %/include/asm-ia64/asm-xsi-offsets.h,$(HDRS))
