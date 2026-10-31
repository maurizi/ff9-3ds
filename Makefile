# ff9-3ds: FF9 (PS1, NTSC-U) on the Nintendo 3DS, built from the ff9-decomp C source in decomp/.
# Build targets run in the container: tools/docker.sh make [3ds|pc]
#
#   make / make 3ds   build/ff9.3dsx      3DS homebrew executable (devkitARM, libctru, citro3d)
#   make pc           build/pc/ff9ds      32-bit Linux host build of the same game code and port layer: fast
#                                         iteration, gdb, frame dumps, lockstep comparison with PCSX-Redux
#   AUTOPRESS="..."   3DS: scripted presses, as the PC's --press ("CROSS@2500,UP@2900+3")
#   AUTOTOUCH="..."  3DS: scripted taps on the bottom screen ("X,Y@FRAME ...")
#   SCENARIO=path     3DS: boot straight into a scenario (docs/scenario-testing.md)
#   PROFILE=1         3DS: frame-time and per-category timers on the bottom screen
#   DUMP_AT=N         3DS: at frame N write the VRAM mirror, shadow and textures to sdmc:/3ds/ff9/dump/
#   SOFT=1            3DS: the software renderer, copied to the top screen by the CPU (reference)

DECOMP   := decomp
VERSION  := us
BUILD    := build
GEN      := $(BUILD)/gen
PYTHON   := python3
,        := ,

# The decomp's binaries: main + the FF9.IMG dir-0 code overlays.
OVLS     := $(shell awk '/^[0-9a-f]/ {print $$1}' $(DECOMP)/config/overlays.$(VERSION).txt)
# Battle effect modules (FF9.IMG dir 13) built natively; the rest run in the effect VM (port/game/effect_bridge.c).
# Only complete C (no INCLUDE_ASM) with an extracted image is used (tools/ff9sym.py native_fxmods).
FXNATIVE ?= $(notdir $(wildcard $(DECOMP)/src/fxmod/fx_*))
export FXNATIVE
FXMODS   := $(foreach m,$(FXNATIVE),$(if $(and $(wildcard $(DECOMP)/src/fxmod/$(m)/*.c),$(wildcard $(DECOMP)/disks/us/fxmod/$(m).bin)),\
              $(if $(shell grep -l INCLUDE_ASM $(DECOMP)/src/fxmod/$(m)/*.c),,$(m))))
BINS     := main $(addprefix ovl_,$(OVLS)) $(FXMODS)

# Decomp C, as paths relative to decomp/ without .c, in priority order (the link keeps the first definition):
#   src/<bin>/*.c              matched C
#   src_equiv/<bin>/*.c        port-only equivalents of asm routines (weak)
#   docs/wip/<bin>_<addr>.c    near-miss drafts of the last non-matching functions (weak, lowest priority)
src_of    = $(patsubst $(DECOMP)/%.c,%,$(wildcard $(DECOMP)/src/$(1)/*.c) $(wildcard $(DECOMP)/src/fxmod/$(1)/*.c) \
                                        $(wildcard $(DECOMP)/src_equiv/$(1)/*.c))
wip_bin   = $(patsubst %_$(lastword $(subst _, ,$(1))),%,$(1))
bin_of    = $(if $(filter docs/wip/%,$(1)),$(call wip_bin,$(notdir $(1))),$(if $(filter src/fxmod/%,$(1)),$(word 3,$(subst /, ,$(1))),$(word 2,$(subst /, ,$(1)))))
WIP_SRCS  := $(foreach f,$(patsubst $(DECOMP)/%.c,%,$(wildcard $(DECOMP)/docs/wip/*_[0-9A-F]*.c)),\
                 $(if $(filter $(call wip_bin,$(notdir $(f))),$(BINS)),$(f)))
DECOMP_SRCS := $(foreach b,$(BINS),$(call src_of,$(b))) $(WIP_SRCS)
# the effect modules' sources, all and per module (per module: fx_srcs_of)
FX_SRCS   := $(foreach f,$(DECOMP_SRCS),$(if $(filter fx_%,$(call bin_of,$(f))),$(f)))
fx_srcs_of = $(foreach f,$(FX_SRCS),$(if $(filter $(1),$(call bin_of,$(f))),$(f)))
weak_src  = $(filter src_equiv/% docs/wip/%,$(1))
header_of = $(if $(filter main,$(1)),ff9.h,$(if $(filter fx_%,$(1)),btlfx_mod.h,$(1).h))

PORT_HDRS := $(wildcard port/include/*.h)
# window_tween.c reimplemented func_80063118 before the decomp matched it; only its PSX comparison test uses it now.
PSX_SRCS  := $(filter-out port/game/window_tween.c,$(wildcard port/psx/*.c port/game/*.c))

# PS1 semantics: signed char, no strict aliasing, wrapping overflow; K&R-era C (gcc 2.8) needs gnu89 + permissive.
DECOMP_CFLAGS := -std=gnu89 -fpermissive -O2 -g -fsigned-char -fno-strict-aliasing -fwrapv \
                 -fno-delete-null-pointer-checks -fno-builtin-memcpy -ffunction-sections -fdata-sections -w \
                 -I$(DECOMP)/include -Iport/include -include port_prelude.h \
                 $(addprefix -I,$(wildcard $(DECOMP)/src_equiv/*/)) # their "../src_equiv.h" resolves from here
PORT_CFLAGS   := -std=gnu17 -O2 -g -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -Wno-builtin-declaration-mismatch -ffunction-sections -fdata-sections \
                 -Iport/include -I$(DECOMP)/include

# port/game/ includes the decomp's headers, whose inline helpers call functions declared only for other binaries
GAME_CFLAGS   := -Wno-error=implicit-function-declaration -Wno-implicit-function-declaration

.PHONY: all 3ds pc decomp-objs extract clean FORCE
.SECONDARY:

all: 3ds

# --- shared front half: portified sources and the generated link data ----------------------------------------
# tools/portify.py adapts each decomp file into build/portified/ (MIPS padding out, PS1 addresses mapped).
$(BUILD)/portified/%.c: $(DECOMP)/%.c tools/portify.py config/addr_plain.txt config/patches.txt
	@mkdir -p $(dir $@)
	@$(PYTHON) tools/portify.py $< $@ --report $@.addr.txt

# gen_link.py writes the data images, the PS1 data symbol addresses, the overlay namespace maps, the
# resident-overlay trampolines (ARM and x86) and the overlay table, from the decomp's splat output (the user's
# disc) and the ARM objects. Both targets share them.
# The native effect module list: rewritten only when it changes, so gen_link reruns exactly then.
$(GEN)/fxmods.list: FORCE
	@mkdir -p $(GEN)
	@echo '$(FXMODS)' | cmp -s - $@ || echo '$(FXMODS)' > $@
FORCE:
$(GEN)/stamp: $(addprefix $(BUILD)/3ds/obj/,$(addsuffix .o,$(DECOMP_SRCS))) tools/gen_link.py tools/ff9sym.py \
              $(wildcard config/xrefs.txt) $(GEN)/fxmods.list
	@mkdir -p $(GEN)
	@$(PYTHON) tools/gen_link.py $(BUILD)/3ds/obj
	@touch $@
$(GEN)/%.s: $(GEN)/stamp ;

# --- per-target back half ------------------------------------------------------------------------------------
# $(1) target, $(2) compiler, $(3) objcopy, $(4) arch flags, $(5) port platform dir, $(6) trampoline flavour,
# $(7) link flags, $(8) libs
define target_rules
$(1)_DIR      := $(BUILD)/$(1)
$(1)_DOBJS    := $$(addprefix $$($(1)_DIR)/obj/,$$(addsuffix .o,$(DECOMP_SRCS)))
$(1)_NSOBJS   := $$(patsubst $$($(1)_DIR)/obj/%,$$($(1)_DIR)/nsobj/%,$$($(1)_DOBJS))
$(1)_GENOBJS  := $$(addprefix $$($(1)_DIR)/gen/,$$(addsuffix .o,$$(addprefix img_,$(BINS)) tramp_$(6)))
$(1)_PORTOBJS := $$(patsubst port/%.c,$$($(1)_DIR)/port/%.o,$(PSX_SRCS) $$(wildcard port/$(5)/*.c))
$(1)_LINK     := $$($(1)_NSOBJS) $$($(1)_GENOBJS) $$($(1)_PORTOBJS) $$($(1)_EXTRAOBJS)
# With $(1)_FXOVL the effect modules' code stays out of the executable (loaded from SD: effect_native.c)
$(1)_FXOBJS   := $$(if $$($(1)_FXOVL),$$(addprefix $$($(1)_DIR)/nsobj/,$$(addsuffix .o,$(FX_SRCS))) \
                   $$(addprefix $$($(1)_DIR)/gen/img_,$$(addsuffix .o,$(FXMODS))))
$(1)_MAINLINK := $$(filter-out $$($(1)_FXOBJS),$$($(1)_LINK))

$$($(1)_DIR)/obj/%.o: $(BUILD)/portified/%.c $(PORT_HDRS)
	@mkdir -p $$(dir $$@)
	@echo "CC   [$(1)] $$*"
	@$(2) $(4) $(DECOMP_CFLAGS) $$(DECOMP_OPT_$(1)_$$(call bin_of,$$*)) \
		$$(if $$(filter $(DECOMP_GTE_BINS),$$(call bin_of,$$*)),$$(DECOMP_GTE_$(1))) -DPORT_HEADER='"$$(call header_of,$$(call bin_of,$$*))"' \
		-DPORT_BIN_$$(call bin_of,$$*) -c $$< -o $$@ $$(if $$(filter docs/wip/%,$$*),2> $$@.log || \
		{ echo "draft $$* doesn't compile against current headers (skipped, see $$@.log)"; \
		  echo "" | $(2) $(4) -x c -c - -o $$@; })

# src_equiv/ is weakened first, so a matched src/ definition of the same function always wins; then overlay
# functions get their namespace prefix and cross-binary references are pointed at the right overlay; last, the
# functions the port wraps are renamed in their defining object (config/renames.txt) and the ones it replaces
# (config/overrides.txt) are weakened.
$$($(1)_DIR)/nsobj/%.o: $$($(1)_DIR)/obj/%.o $(GEN)/stamp config/overrides.txt config/renames.txt
	@mkdir -p $$(dir $$@)
	@$(3) $$(if $$(call weak_src,$$*),--weaken) $$< $$@.tmp
	@$(3) --redefine-syms=$(GEN)/redefine_$$(call bin_of,$$*).txt $$@.tmp $$@.tmp2
	@$(3) $$(shell awk '$$$$1 == "$$*" { printf "--redefine-sym %s=%s ", $$$$2, $$$$3 }' config/renames.txt) $$@.tmp2 $$@.tmp
	@$(3) --weaken-symbols=config/overrides.txt $$@.tmp $$@ \
		$$(if $$($(1)_FXOVL),$$(if $$(filter ovl_%,$$(call bin_of,$$*)),&& $(3) --prefix-alloc-sections=.psxovl_$$(patsubst ovl_%,%,$$(call bin_of,$$*)) $$@))
	@rm -f $$@.tmp $$@.tmp2

$$($(1)_DIR)/gen/%.o: $(GEN)/%.s
	@mkdir -p $$(dir $$@)
	@$(2) $(4) -c $$< -o $$@

$$($(1)_DIR)/port/%.o: port/%.c $(PORT_HDRS)
	@mkdir -p $$(dir $$@)
	@echo "CC   [$(1)] port/$$*"
	@$(2) $(4) $(PORT_CFLAGS) $$(PORT_EXTRA_$(1)) $$(PORT_ARCH_$(1)_$$*) $$(if $$(filter game/%,$$*),$(GAME_CFLAGS)) -c $$< -o $$@

# Two-phase link: the first pass lists what is still undefined; gen_stubs.py turns those into stubs that stop
# with their name (the to-do list is $$($(1)_DIR)/stubs.txt).
$$($(1)_DIR)/stubs.c: $$($(1)_LINK) tools/gen_stubs.py
	@$(2) $(4) -o $$($(1)_DIR)/scan.elf $$($(1)_LINK) $(GEN)/psx_syms.ld $(7) -Wl,--warn-unresolved-symbols \
		-Wl,--start-group $(8) -Wl,--end-group 2> $$($(1)_DIR)/scan.log || true
	@$(PYTHON) tools/gen_stubs.py $$($(1)_DIR)/scan.log $$@

$$($(1)_DIR)/stubs.o: $$($(1)_DIR)/stubs.c
	@$(2) $(4) $(PORT_CFLAGS) $$(PORT_EXTRA_$(1)) -w -c $$< -o $$@

# With $(1)_FXOVL: the main executable keeps what the modules use (fx_extern.ld) and gets a new build id each
# link (fxid.s), which the module pack carries so a stale pack is refused.
$$($(1)_DIR)/fx_extern.ld: $$($(1)_FXOBJS) tools/fx_extern.py port/include/ovl0b_layout.h
	@$(PYTHON) tools/fx_extern.py $$(subst gcc,nm,$(2)) $$@ $$($(1)_FXOBJS)

$$($(1)_DIR)/ff9ds.elf: $$($(1)_LINK) $$($(1)_DIR)/stubs.o $$(if $$($(1)_FXOVL),$$($(1)_DIR)/fx_extern.ld)
	@echo "LD   $$@"
	$$(if $$($(1)_FXOVL),@$(PYTHON) tools/fx_extern.py --build-id $$($(1)_DIR)/fxid.s && $(2) $(4) -c $$($(1)_DIR)/fxid.s -o $$($(1)_DIR)/fxid.o)
	@$(2) $(4) -o $$@ $$($(1)_MAINLINK) $$($(1)_DIR)/stubs.o $$(if $$($(1)_FXOVL),$$($(1)_DIR)/fxid.o $$($(1)_DIR)/fx_extern.ld) \
		$(GEN)/psx_syms.ld $(7) -Wl,-Map,$$($(1)_DIR)/ff9ds.map -Wl,--start-group $(8) -Wl,--end-group \
		2> $$($(1)_DIR)/link.log; status=$$$$?; grep -v "allocated section .\.\(psx\|itcm\)ovl_.* not in segment" $$($(1)_DIR)/link.log >&2; \
		exit $$$$status
endef
# --- 3DS: devkitARM + libctru + citro3d, ARM11 MPCore (ARMv6K, VFPv2) ---------------------------------------
# ARM state throughout: ARMv6's Thumb is Thumb-1 (no hardware divide either way), and ARM code is faster on the
# ARM11. 3dsx relocations are word relocations only (-mword-relocations: no movw/movt pairs).
DEVKITPRO ?= /opt/devkitpro
CTRULIB   := $(DEVKITPRO)/libctru
CTR_ARCH  := -D__3DS__ -DPORT_3DS -DPORT_GTE_DEPTH -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -marm -mword-relocations
PORT_EXTRA_3ds := -DPORT_DISC_PACK -DPORT_SPU_WORKER -I$(CTRULIB)/include \
                  $(if $(AUTOPRESS),-DPORT_AUTOPRESS='"$(AUTOPRESS)"') $(if $(SCENARIO),-DPORT_SCENARIO='"$(SCENARIO)"') \
                  $(if $(filter 1,$(PROFILE)),-DPORT_PROFILE) $(if $(filter 1,$(SOFT)),-DPORT_SOFT_PRESENT) $(if $(DUMP_AT),-DPORT_DUMP_AT=$(DUMP_AT)) $(if $(LOGUI_AT),-DPORT_LOGUI_AT=$(LOGUI_AT)) $(if $(AUTOTOUCH),-DPORT_AUTOTOUCH='"$(AUTOTOUCH)"') -I$(BUILD)/3ds/shaders
# Vertex shaders (PICA200 assembly, picasso) are embedded as binaries.
SHADERS := $(wildcard port/3ds/*.v.pica)
$(BUILD)/3ds/shaders/%.shbin: port/3ds/%.v.pica
	@mkdir -p $(dir $@)
	@echo "PICA $<"
	@picasso -o $@ $<
$(BUILD)/3ds/shaders/%_shbin.o: $(BUILD)/3ds/shaders/%.shbin
	@cd $(dir $@) && bin2s -a 4 -H $*_shbin.h $(notdir $<) > $*_shbin.s && arm-none-eabi-gcc $(CTR_ARCH) -c $*_shbin.s -o $(notdir $@)
3ds_EXTRAOBJS := $(patsubst port/3ds/%.v.pica,$(BUILD)/3ds/shaders/%_shbin.o,$(SHADERS))

$(eval $(call target_rules,3ds,arm-none-eabi-gcc,arm-none-eabi-objcopy,$(CTR_ARCH),3ds,arm,\
	-specs=3dsx.specs -L$(CTRULIB)/lib -Wl$(,)--gc-sections,-lcitro3d -lctru -lm))

# Changing a runtime option must rebuild the port objects even when sources are unchanged.
.PHONY: FORCE_3DS_CONFIG
$(BUILD)/3ds/runtime-config: FORCE_3DS_CONFIG
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(AUTOPRESS)' '$(SCENARIO)' '$(PROFILE)' '$(SOFT)' '$(DUMP_AT)' '$(LOGUI_AT)' '$(AUTOTOUCH)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm -f $@.tmp
$(3ds_PORTOBJS): $(BUILD)/3ds/runtime-config $(3ds_EXTRAOBJS)

APP_TITLE  := Final Fantasy IX
APP_DESC   := Native 3DS port (ff9-decomp)
APP_AUTHOR := ff9-3ds
$(BUILD)/3ds/icon.png: tools/make_icon.py
	@$(PYTHON) tools/make_icon.py $@
$(BUILD)/ff9.smdh: $(BUILD)/3ds/icon.png
	@smdhtool --create "$(APP_TITLE)" "$(APP_DESC)" "$(APP_AUTHOR)" $< $@
$(BUILD)/ff9.3dsx: $(BUILD)/3ds/ff9ds.elf $(BUILD)/ff9.smdh
	@echo "3DSX $@"
	@3dsxtool $< $@ --smdh=$(BUILD)/ff9.smdh

3ds: $(BUILD)/ff9.3dsx

# --- PC host build -----------------------------------------------------------------------------------------
# 32-bit, non-PIE: psx_ram is mapped at a fixed address (port/pc/platform.c), so OT links rebuild as on the 3DS.
PC_ARCH  := -m32 -malign-double -fno-pie
PORT_EXTRA_pc := -DPORT_DISC_PACK -DNUI_DEBUG
PORT_ARCH_pc_psx/softgpu := $(if $(filter 1,$(RASTER_REFERENCE)),-DPORT_SOFTGPU_REFERENCE)
$(eval $(call target_rules,pc,gcc,objcopy,$(PC_ARCH),pc,x86,-no-pie -Wl$(,)-z$(,)noexecstack -Wl$(,)--defsym=psx_ram=0x02000000,-lm))
pc: $(BUILD)/pc/ff9ds
$(BUILD)/pc/ff9ds: $(BUILD)/pc/ff9ds.elf
	@cp $< $@

decomp-objs: $(3ds_DOBJS)

# --- disc data (host) ----------------------------------------------------------------------------------------
# Needs the four disc images in disks/us/raw/ ("FF9 CD1.img" ... as in the decomp). The decomp's split only needs
# the exe and the code overlays; they are copied into the submodule, whose container can't follow links.
extract:
	$(MAKE) -C $(DECOMP) extract extract-img extract-ovl DISC_DIR=$(CURDIR)/disks/$(VERSION)/raw
	cd $(DECOMP) && tools/docker.sh make split

clean:
	rm -rf $(BUILD)

include mk/host-tests.mk
