# ff9-ds: FF9 (PS1) on the DS, built from the ff9-decomp C source in decomp/.
# Build targets run in the container: tools/docker.sh make [ds|pc]
# `make extract` runs on the host (it drives the decomp's own container).
#
#   make / make ds    build/ff9ds.nds     DSi-mode ROM (BlocksDS)
#   make pc           build/pc/ff9ds      32-bit Linux host build of the same game code and port/psx layer:
#                                         fast iteration, gdb, lockstep comparison with PCSX-Redux
#   AUTORUN=1         DS: run the game straight away instead of waiting for START
#   AUTOPRESS="..."   DS: scripted presses, as the PC's --press ("CROSS@2500,UP@2900+3")
#   SOFTDISPLAY=0     DS: experimental GX renderer (software display is the default)

DECOMP   := decomp
VERSION  := us
BUILD    := build
GEN      := $(BUILD)/gen
PYTHON   := python3
,        := ,

# The decomp's binaries: main + the FF9.IMG dir-0 code overlays.
OVLS     := $(shell awk '/^[0-9a-f]/ {print $$1}' $(DECOMP)/config/overlays.$(VERSION).txt)
BINS     := main $(addprefix ovl_,$(OVLS))

# Decomp C, as paths relative to decomp/ without .c, in priority order (the link keeps the first definition):
#   src/<bin>/*.c              matched C
#   src_equiv/<bin>/*.c        port-only equivalents of asm routines (weak)
#   docs/wip/<bin>_<addr>.c    near-miss drafts of the last non-matching functions (weak, lowest priority)
src_of    = $(patsubst $(DECOMP)/%.c,%,$(wildcard $(DECOMP)/src/$(1)/*.c) $(wildcard $(DECOMP)/src_equiv/$(1)/*.c))
WIP_SRCS  := $(patsubst $(DECOMP)/%.c,%,$(wildcard $(DECOMP)/docs/wip/*_[0-9A-F]*.c))
DECOMP_SRCS := $(foreach b,$(BINS),$(call src_of,$(b))) $(WIP_SRCS)
wip_bin   = $(patsubst %_$(lastword $(subst _, ,$(1))),%,$(1))
bin_of    = $(if $(filter docs/wip/%,$(1)),$(call wip_bin,$(notdir $(1))),$(word 2,$(subst /, ,$(1))))
weak_src  = $(filter src_equiv/% docs/wip/%,$(1))
header_of = $(if $(filter main,$(1)),ff9.h,$(1).h)

PORT_HDRS := $(wildcard port/include/*.h)
PSX_SRCS  := $(wildcard port/psx/*.c port/game/*.c)

# PS1 semantics: signed char, no strict aliasing, wrapping overflow; K&R-era C (gcc 2.8) needs gnu89 + permissive.
DECOMP_CFLAGS := -std=gnu89 -fpermissive -O2 -g -fsigned-char -fno-strict-aliasing -fwrapv \
                 -fno-delete-null-pointer-checks -fno-builtin-memcpy -ffunction-sections -fdata-sections -w \
                 -I$(DECOMP)/include -Iport/include -include port_prelude.h \
                 $(addprefix -I,$(wildcard $(DECOMP)/src_equiv/*/)) # their "../src_equiv.h" resolves from here
PORT_CFLAGS   := -std=gnu17 -O2 -g -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -Wno-builtin-declaration-mismatch -ffunction-sections -fdata-sections \
                 -Iport/include -I$(DECOMP)/include

# port/game/ includes the decomp's headers, whose inline helpers call functions declared only for other binaries
GAME_CFLAGS   := -Wno-error=implicit-function-declaration -Wno-implicit-function-declaration

.PHONY: all ds ds-r4 pc decomp-objs extract clean
.SECONDARY:

all: ds

# --- shared front half: portified sources and the generated link data ----------------------------------------
# tools/portify.py adapts each decomp file into build/portified/ (MIPS padding out, PS1 addresses mapped).
$(BUILD)/portified/%.c: $(DECOMP)/%.c tools/portify.py config/addr_plain.txt config/patches.txt
	@mkdir -p $(dir $@)
	@$(PYTHON) tools/portify.py $< $@ --report $@.addr.txt

# gen_link.py writes the data images, the PS1 data symbol addresses, the overlay namespace maps, the
# resident-overlay trampolines (ARM and x86) and the overlay table, from the decomp's splat output (the user's
# disc) and the ARM objects. Both targets share them.
$(GEN)/stamp: $(addprefix $(BUILD)/ds/obj/,$(addsuffix .o,$(DECOMP_SRCS))) tools/gen_link.py tools/ff9sym.py \
              $(wildcard config/xrefs.txt)
	@mkdir -p $(GEN)
	@$(PYTHON) tools/gen_link.py $(BUILD)/ds/obj
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
$(1)_LINK     := $$($(1)_NSOBJS) $$($(1)_GENOBJS) $$($(1)_PORTOBJS)

$$($(1)_DIR)/obj/%.o: $(BUILD)/portified/%.c $(PORT_HDRS)
	@mkdir -p $$(dir $$@)
	@echo "CC   [$(1)] $$*"
	@$(2) $(4) $(DECOMP_CFLAGS) -DPORT_HEADER='"$$(call header_of,$$(call bin_of,$$*))"' \
		-DPORT_BIN_$$(call bin_of,$$*) -c $$< -o $$@ $$(if $$(filter docs/wip/%,$$*),2> $$@.log || \
		{ echo "draft $$* doesn't compile against current headers (skipped, see $$@.log)"; \
		  echo "" | $(2) $(4) -x c -c - -o $$@; })

# src_equiv/ is weakened first, so a matched src/ definition of the same function always wins; then overlay
# functions get their namespace prefix and cross-binary references are pointed at the right overlay; last, the
# functions the port replaces (config/overrides.txt) are weakened.
$$($(1)_DIR)/nsobj/%.o: $$($(1)_DIR)/obj/%.o $(GEN)/stamp config/overrides.txt
	@mkdir -p $$(dir $$@)
	@$(3) $$(if $$(call weak_src,$$*),--weaken) $$< $$@.tmp
	@$(3) --redefine-syms=$(GEN)/redefine_$$(call bin_of,$$*).txt $$@.tmp $$@.tmp2
	@$(3) --weaken-symbols=config/overrides.txt $$@.tmp2 $$@
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

$$($(1)_DIR)/ff9ds.elf: $$($(1)_LINK) $$($(1)_DIR)/stubs.o
	@echo "LD   $$@"
	@$(2) $(4) -o $$@ $$($(1)_LINK) $$($(1)_DIR)/stubs.o $(GEN)/psx_syms.ld $(7) -Wl,-Map,$$($(1)_DIR)/ff9ds.map \
		-Wl,--start-group $(8) -Wl,--end-group
endef

# DS: BlocksDS, DSi-mode memory map, Thumb.
# Preserve complete PSX textures and effects while GX resource/blend issues remain.
SOFTDISPLAY ?= 1
AUDIO ?= 1
SPUARM ?= 1
SPUITCM ?= 1
# ARM9 timing probe verifies these pixel loops in instruction TCM; keep O2.
PORT_ARCH_ds_psx/softgpu := -marm -DPORT_RASTER_ITCM
PORT_ARCH_ds_ds/display_scale := -marm -DPORT_SCALE_ITCM
PORT_ARCH_ds_ds/itcm_guard := -marm
# ARMv5TE DSP code generation; keep Thumb available for measured comparisons.
ifeq ($(SPUARM),1)
PORT_ARCH_ds_psx/spu_core := -marm
PORT_ARCH_ds_psx/spu_voice := -marm
PORT_ARCH_ds_psx/spu_source := -marm
PORT_ARCH_ds_psx/spu_adpcm := -marm
PORT_ARCH_ds_psx/spu_envelope := -marm
PORT_ARCH_ds_psx/spu_reverb := -marm
PORT_ARCH_ds_psx/spu_noise := -marm
endif
SPU_IMPL_NAMES := spu_core spu_voice spu_source spu_adpcm spu_envelope spu_reverb spu_noise
$(foreach target,ds pc,$(addprefix $(BUILD)/$(target)/port/psx/,$(addsuffix .o,$(SPU_IMPL_NAMES)))): port/psx/spu_hot.h
$(addprefix $(BUILD)/tests/spu_,$(addsuffix _test,adpcm source envelope interpolation voice noise core clock reverb)) $(BUILD)/tests/spu_journal_fixture $(BUILD)/tests/spu_throughput_host: port/psx/spu_hot.h
$(BUILD)/ds/port/psx/softgpu.o: Makefile
$(BUILD)/ds/port/ds/video_gl.o: third_party/libnds/videoGL.c
$(BUILD)/ds/port/ds/render_gx.o $(BUILD)/tests/gx_backend.o: port/ds/gx_shadow_lazy.h port/include/profile.h
SPECS    := $(BLOCKSDS)/sys/crts/dsi_arm9.specs
DS_ARCH  := -mcpu=arm946e-s -mthumb -mthumb-interwork
PORT_EXTRA_ds := $(if $(filter 1,$(SPUITCM)),-DPORT_SPU_ITCM) $(if $(filter 1,$(AUDIO)),-DPORT_AUDIO) $(if $(filter 1,$(AUDIOHASH)),-DPORT_AUDIO_HASH) $(if $(AUDIOCAPTURE),-DPORT_AUDIO_CAPTURE=$(AUDIOCAPTURE)) -I$(BLOCKSDS)/libs/maxmod/include -specs=$(SPECS) -I$(BLOCKSDS)/libs/libnds/include $(if $(AUTORUN),-DPORT_AUTORUN) \
                 $(if $(CAPTURE_FRAME),-DPORT_CAPTURE_FRAME=$(CAPTURE_FRAME)) \
                 $(if $(filter 1,$(CAPTURE_READBACK)),-DPORT_CAPTURE_READBACK) \
                 $(if $(AUTOPRESS),-DPORT_AUTOPRESS='"$(AUTOPRESS)"') $(if $(SCENARIO),-DPORT_SCENARIO='"$(SCENARIO)"') $(if $(filter 1,$(SOFTDISPLAY)),-DPORT_SOFT_DISPLAY) $(if $(filter 0,$(GXMERGE)),-DPORT_GX_NO_MERGE) $(if $(filter 1,$(GXMERGE)),-DPORT_GX_MERGE) $(if $(filter 1,$(GXEXPERIMENTAL)),-DPORT_GX_EXPERIMENTAL) $(if $(filter 1,$(GXSHADOW)),-DPORT_GX_SHADOW) $(if $(filter 1,$(GXLAZY)),-DPORT_GX_LAZY) $(if $(filter 1,$(GXPLAN)),-DPORT_GX_PLAN) $(if $(filter 1,$(GXSHARE)),-DPORT_GX_SHARE_INDEXED) $(if $(filter 1,$(ITCMGUARD)),-DPORT_ITCM_GUARD) $(if $(filter 1,$(PROFILE)),-DPORT_PROFILE)
$(eval $(call target_rules,ds,arm-none-eabi-gcc,arm-none-eabi-objcopy,$(DS_ARCH),ds,arm,\
	-specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib -L$(BLOCKSDS)/libs/maxmod/lib -Wl$(,)--gc-sections,-lmm9 -lnds9 -lc))

# Changing a DS runtime option must rebuild its users even when sources are unchanged.
.PHONY: FORCE_DS_CONFIG
$(BUILD)/ds/runtime-config: FORCE_DS_CONFIG
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(AUTORUN)' '$(AUTOPRESS)' '$(SOFTDISPLAY)' '$(AUDIO)' '$(AUDIOHASH)' '$(AUDIOCAPTURE)' '$(SPUARM)' '$(SPUITCM)' '$(GXMERGE)' '$(GXEXPERIMENTAL)' '$(GXPLAN)' '$(GXSHADOW)' '$(GXSHARE)' '$(ITCMGUARD)' '$(SCENARIO)' '$(PROFILE)' '$(CAPTURE_FRAME)' '$(GXLAZY)' '$(CAPTURE_READBACK)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm -f $@.tmp
$(ds_PORTOBJS): $(BUILD)/ds/runtime-config
$(BUILD)/ds/port/ds/audio.o: port/ds/audio.h port/ds/pcm_ring.h
$(BUILD)/ds/port/ds/pcm_ring.o: port/ds/pcm_ring.h
$(BUILD)/ds/port/ds/platform.o: port/ds/audio.h

.PHONY: test-gx test-libnds-allocator
test-libnds-allocator:
	$(PYTHON) tools/test_libnds_allocator.py
test-gx: $(BUILD)/tests/gx_texture_lazy_test $(BUILD)/tests/gx_texture_shared_shadow_test $(BUILD)/tests/gx_texture_shadow_test $(BUILD)/tests/gx_texture_test $(BUILD)/tests/gx_texture_default_test $(BUILD)/tests/gx_texture_merge_test
	@$(BUILD)/tests/gx_texture_test
	@$(BUILD)/tests/gx_texture_default_test
	@$(BUILD)/tests/gx_texture_merge_test
	@$(BUILD)/tests/gx_texture_shadow_test
	@$(BUILD)/tests/gx_texture_shared_shadow_test
	@$(BUILD)/tests/gx_texture_lazy_test

$(BUILD)/tests/gx_texture_lazy_test: tests/gx/texture_test.c tests/gx/nds.h port/ds/render_gx.c port/ds/gx_shadow_lazy.h port/include/gp0.h port/include/profile.h port/psx/softgpu.c port/include/psx_raster.h
	@mkdir -p $(dir $@)
	$(CC) -DPORT_GX_LAZY -DPORT_GX_SHADOW -DPORT_GX_SHARE_INDEXED -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Itests/gx -Iport/include -I$(DECOMP)/include $< port/psx/softgpu.c -o $@
$(BUILD)/tests/gx_texture_test: tests/gx/texture_test.c tests/gx/nds.h port/ds/render_gx.c port/include/gp0.h port/psx/softgpu.c port/include/psx_raster.h
	@mkdir -p $(dir $@)
	$(CC) -DPORT_GX_EXPERIMENTAL -DPORT_GX_SHARE_INDEXED -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Itests/gx -Iport/include -I$(DECOMP)/include $< port/psx/softgpu.c -o $@
$(BUILD)/tests/gx_texture_default_test: tests/gx/texture_test.c tests/gx/nds.h port/ds/render_gx.c port/include/gp0.h port/psx/softgpu.c port/include/psx_raster.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Itests/gx -Iport/include -I$(DECOMP)/include $< port/psx/softgpu.c -o $@

$(BUILD)/tests/gx_texture_merge_test: tests/gx/texture_test.c tests/gx/nds.h port/ds/render_gx.c port/include/gp0.h port/psx/softgpu.c port/include/psx_raster.h
	@mkdir -p $(dir $@)
	$(CC) -DPORT_GX_MERGE -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Itests/gx -Iport/include -I$(DECOMP)/include $< port/psx/softgpu.c -o $@

$(BUILD)/tests/gx_texture_shadow_test: tests/gx/texture_test.c tests/gx/nds.h port/ds/render_gx.c port/include/gp0.h port/psx/softgpu.c port/include/psx_raster.h
	@mkdir -p $(dir $@)
	$(CC) -DPORT_GX_SHADOW -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Itests/gx -Iport/include -I$(DECOMP)/include $< port/psx/softgpu.c -o $@

$(BUILD)/tests/gx_texture_shared_shadow_test: tests/gx/texture_test.c tests/gx/nds.h port/ds/render_gx.c port/include/gp0.h port/psx/softgpu.c port/include/psx_raster.h
	@mkdir -p $(dir $@)
	$(CC) -DPORT_GX_SHADOW -DPORT_GX_SHARE_INDEXED -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Itests/gx -Iport/include -I$(DECOMP)/include $< port/psx/softgpu.c -o $@

.PHONY: test-spu
.PHONY: test-spu-transfer
test-spu-transfer: $(BUILD)/tests/spu_transfer_test
	@$<
$(BUILD)/tests/spu_transfer_test: tests/spu/transfer_test.c port/psx/spu.c port/psx/spu_trace.c port/include/psyq_port.h port/include/psx_spu.h port/include/psx_spu_trace.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include $< port/psx/spu.c port/psx/spu_trace.c -Wl,--gc-sections -o $@
# End-to-end journal writer fixture uses the production 32-bit PsyQ ABI.
.PHONY: test-spu-journal
test-spu-journal: $(BUILD)/tests/spu_journal_fixture
	@python3 tests/spu/test_journal.py
$(BUILD)/tests/spu_journal_fixture: tests/spu/journal_fixture.c port/pc/spu_trace.c port/psx/spu.c port/psx/spu_core.c port/psx/spu_trace.c port/psx/spu_reverb.c port/psx/spu_noise.c port/psx/spu_voice.c port/psx/spu_source.c port/psx/spu_adpcm.c port/psx/spu_envelope.c $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include $< port/pc/spu_trace.c port/psx/spu.c port/psx/spu_core.c port/psx/spu_trace.c port/psx/spu_reverb.c port/psx/spu_noise.c port/psx/spu_voice.c port/psx/spu_source.c port/psx/spu_adpcm.c port/psx/spu_envelope.c -Wl,--gc-sections -o $@
# SDK structures use the game's 32-bit ABI, like the transfer suite.
.PHONY: test-spu-sdk-reverb
test-spu-sdk-reverb: $(BUILD)/tests/spu_sdk_reverb_test
	@$<
$(BUILD)/tests/spu_sdk_reverb_test: tests/spu/sdk_reverb_test.c tests/spu/sdk_reverb_reference.h port/psx/spu.c port/psx/spu_trace.c port/include/psx_spu_presets.h port/include/psx_spu_trace.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include $< port/psx/spu.c port/psx/spu_trace.c -Wl,--gc-sections -o $@
test-spu: $(BUILD)/tests/spu_adpcm_test $(BUILD)/tests/spu_source_test $(BUILD)/tests/spu_envelope_test $(BUILD)/tests/spu_interpolation_test $(BUILD)/tests/spu_voice_test $(BUILD)/tests/spu_noise_test $(BUILD)/tests/spu_core_test $(BUILD)/tests/spu_clock_test $(BUILD)/tests/spu_reverb_test
	@$<
	@$(BUILD)/tests/spu_source_test
	@$(BUILD)/tests/spu_envelope_test
	@$(BUILD)/tests/spu_interpolation_test
	@$(BUILD)/tests/spu_voice_test
	@$(BUILD)/tests/spu_noise_test
	@$(BUILD)/tests/spu_core_test
	@$(BUILD)/tests/spu_clock_test
	@$(BUILD)/tests/spu_reverb_test
$(BUILD)/tests/spu_reverb_test: tests/spu/reverb_test.c tests/spu/reverb_cases.h tests/spu/reverb_presets.h tests/spu/reverb_reference.h port/psx/spu_reverb.c port/include/psx_spu_reverb.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iport/include $< port/psx/spu_reverb.c -o $@
$(BUILD)/tests/spu_clock_test: tests/spu/clock_test.c port/psx/irq.c port/psx/spu_core.c port/psx/spu_trace.c port/psx/spu_reverb.c port/psx/spu_noise.c port/psx/spu_voice.c port/psx/spu_source.c port/psx/spu_adpcm.c port/psx/spu_envelope.c $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include $< port/psx/irq.c port/psx/spu_core.c port/psx/spu_trace.c port/psx/spu_reverb.c port/psx/spu_noise.c port/psx/spu_voice.c port/psx/spu_source.c port/psx/spu_adpcm.c port/psx/spu_envelope.c -Wl,--gc-sections -o $@
$(BUILD)/tests/spu_core_test: tests/spu/core_test.c port/psx/spu_core.c port/psx/spu_trace.c port/psx/spu_reverb.c port/psx/spu_noise.c port/psx/spu_voice.c port/psx/spu_source.c port/psx/spu_adpcm.c port/psx/spu_envelope.c port/include/psx_spu_reverb.h port/include/psx_spu.h port/include/psx_spu_voice.h port/include/psx_spu_source.h port/include/psx_spu_gauss.h port/include/psx_spu_adpcm.h port/include/psx_spu_envelope.h port/include/platform.h port/include/psx_spu_trace.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iport/include $< port/psx/spu_core.c port/psx/spu_trace.c port/psx/spu_reverb.c port/psx/spu_noise.c port/psx/spu_voice.c port/psx/spu_source.c port/psx/spu_adpcm.c port/psx/spu_envelope.c -o $@
$(BUILD)/tests/spu_noise_test: tests/spu/noise_test.c tests/spu/noise_reference.h port/psx/spu_noise.c port/include/psx_spu.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iport/include $< port/psx/spu_noise.c -o $@
$(BUILD)/tests/spu_voice_test: tests/spu/voice_test.c tests/spu/voice_cases.h tests/spu/voice_reference.h port/psx/spu_voice.c port/psx/spu_source.c port/psx/spu_adpcm.c port/psx/spu_envelope.c port/include/psx_spu_voice.h port/include/psx_spu_source.h port/include/psx_spu_gauss.h port/include/psx_spu_adpcm.h port/include/psx_spu_envelope.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iport/include $< port/psx/spu_voice.c port/psx/spu_source.c port/psx/spu_adpcm.c port/psx/spu_envelope.c -o $@
$(BUILD)/tests/spu_interpolation_test: tests/spu/interpolation_test.c tests/spu/interpolation_cases.h tests/spu/interpolation_reference.h port/psx/spu_source.c port/psx/spu_adpcm.c port/include/psx_spu_source.h port/include/psx_spu_gauss.h port/include/psx_spu_adpcm.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iport/include $< port/psx/spu_source.c port/psx/spu_adpcm.c -o $@
$(BUILD)/tests/spu_envelope_test: tests/spu/envelope_test.c tests/spu/envelope_cases.h tests/spu/envelope_reference.h port/psx/spu_envelope.c port/include/psx_spu_envelope.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iport/include $< port/psx/spu_envelope.c -o $@
$(BUILD)/tests/spu_source_test: tests/spu/source_test.c port/psx/spu_source.c port/psx/spu_adpcm.c port/include/psx_spu_source.h port/include/psx_spu_gauss.h port/include/psx_spu_adpcm.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iport/include $< port/psx/spu_source.c port/psx/spu_adpcm.c -o $@
$(BUILD)/tests/spu_adpcm_test: tests/spu/adpcm_test.c tests/spu/adpcm_cases.h tests/spu/adpcm_reference.h port/psx/spu_adpcm.c port/include/psx_spu_adpcm.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iport/include $< port/psx/spu_adpcm.c -o $@

# Match the production pointer width when exercising 24-bit OT links.
.PHONY: test-gpu
test-gpu: $(BUILD)/tests/gpu_ordering_test $(BUILD)/tests/gpu_env_probe $(BUILD)/tests/gpu_env_render $(BUILD)/tests/gpu_replay
	@$<
	@$(BUILD)/tests/gpu_env_render
	@python3 tools/test_gpu_env.py
	@python3 tools/test_gpu_dump.py

$(BUILD)/tests/gpu_env_render: tests/gpu/env_render.c tests/gpu/overlap_reference.h port/psx/gpu.c port/psx/gp0.c port/psx/softgpu.c port/include/gp0.h port/include/psyq_port.h port/include/psx_raster.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include $< port/psx/gpu.c port/psx/gp0.c port/psx/softgpu.c -Wl,--gc-sections -o $@

$(BUILD)/tests/gpu_replay: tests/gpu/replay.c port/psx/gpu.c port/psx/gp0.c port/psx/softgpu.c port/include/gp0.h port/include/psyq_port.h port/include/psx_raster.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include $< port/psx/gpu.c port/psx/gp0.c port/psx/softgpu.c -Wl,--gc-sections -o $@

$(BUILD)/tests/gpu_env_probe: tests/gpu/env_probe.c tests/gpu/ordering_test.c port/psx/gpu.c port/include/gp0.h port/include/psyq_port.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include $< port/psx/gpu.c -Wl,--gc-sections -o $@
$(BUILD)/tests/gpu_ordering_test: tests/gpu/ordering_test.c port/psx/gpu.c port/include/gp0.h port/include/psyq_port.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/gpu/ordering_test.c port/psx/gpu.c -Wl,--gc-sections -o $@

.PHONY: test-battle
test-battle: $(BUILD)/tests/battle_math_probe $(BUILD)/tests/battle_background_probe $(BUILD)/tests/battle_pose_probe $(BUILD)/tests/battle_packets_probe $(BUILD)/tests/battle_angles_probe $(BUILD)/tests/battle_motion_probe $(BUILD)/tests/battle_quads_probe
	@python3 tools/test_battle_math.py
	@python3 tools/test_battle_background.py
	@python3 tools/test_battle_pose.py
	@python3 tools/test_battle_packets.py
	@python3 tools/test_battle_angles.py
	@python3 tools/test_battle_motion.py
	@python3 tools/test_battle_quads.py
$(BUILD)/tests/battle_angles_probe: tests/battle/angles_probe.c tests/battle/pose_probe.c tests/battle/background_probe.c port/game/battle_angles.c port/game/battle_pose.c port/game/anim_key.c port/game/battle_background.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/battle/angles_probe.c port/game/battle_angles.c port/game/battle_pose.c port/game/anim_key.c port/game/battle_background.c -Wl,--defsym=psx_ram=0x02000000 -o $@
$(BUILD)/tests/battle_packets_probe: tests/battle/packets_probe.c tests/battle/background_probe.c port/game/battle_packets.c port/game/battle_background.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/battle/packets_probe.c port/game/battle_packets.c port/game/battle_background.c -Wl,--defsym=psx_ram=0x02000000 -Wl,--defsym=g_vtxScreenXY=0x020711A8 -Wl,--defsym=g_vtxScreenZ=0x02070520 -o $@
$(BUILD)/tests/battle_pose_probe: tests/battle/pose_probe.c tests/battle/background_probe.c port/game/battle_pose.c port/game/anim_key.c port/game/battle_background.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/battle/pose_probe.c port/game/battle_pose.c port/game/anim_key.c port/game/battle_background.c -Wl,--defsym=psx_ram=0x02000000 -o $@
$(BUILD)/tests/battle_background_probe: tests/battle/background_probe.c port/game/battle_background.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/battle/background_probe.c port/game/battle_background.c -Wl,--defsym=psx_ram=0x02000000 -o $@
$(BUILD)/tests/battle_math_probe: tests/battle/math_probe.c port/game/battle_math.c port/include/psx_mem.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/battle/math_probe.c port/game/battle_math.c -o $@

# PC: 32-bit, non-PIE (psx_ram is mapped at a fixed address, like the DS's main RAM), 8-byte doubles like
# ARM/MIPS. psx_ram is placed with --defsym and mapped by port/pc before anything runs.
PC_ARCH  := -m32 -malign-double -fno-pie
PORT_EXTRA_pc :=
$(eval $(call target_rules,pc,gcc,objcopy,$(PC_ARCH),pc,x86,-no-pie -Wl$(,)-z$(,)noexecstack -Wl$(,)--defsym=psx_ram=0x02000000,-lm))

# Diagnostic host run of the production GX translator (allocator model, no pixels).
.PHONY: pc-gx
pc-gx: $(BUILD)/tests/gx_host.elf
$(BUILD)/tests/gx_host.elf: $(pc_LINK) $(BUILD)/pc/stubs.o $(BUILD)/tests/gx_runtime.o $(BUILD)/tests/gx_backend.o
	$(CC) $(PC_ARCH) -o $@ $(pc_LINK) $(BUILD)/pc/stubs.o $(BUILD)/tests/gx_runtime.o $(BUILD)/tests/gx_backend.o $(GEN)/psx_syms.ld -no-pie -Wl,-z,noexecstack -Wl,--defsym=psx_ram=0x02000000 -lm
$(BUILD)/tests/gx_runtime.o: tests/gx/runtime_probe.c tests/gx/nds.h
	@mkdir -p $(dir $@)
	$(CC) $(PC_ARCH) $(PORT_CFLAGS) -Itests/gx -c $< -o $@
$(BUILD)/tests/gx_backend.o: port/ds/render_gx.c tests/gx/nds.h $(BUILD)/ds/runtime-config
	@mkdir -p $(dir $@)
	$(CC) $(PC_ARCH) $(PORT_CFLAGS) $(if $(filter 1,$(GXSHADOW)),-DPORT_GX_SHADOW) $(if $(filter 1,$(GXLAZY)),-DPORT_GX_LAZY) $(if $(filter 1,$(GXPLAN)),-DPORT_GX_PLAN) $(if $(filter 1,$(GXSHARE)),-DPORT_GX_SHARE_INDEXED) $(if $(filter 1,$(GXMERGE)),-DPORT_GX_MERGE) -Itests/gx -c $< -o $@

ARM7ELF  := $(BLOCKSDS)/sys/arm7/main_core/arm7_maxmod.elf
NDSTOOL  := $(BLOCKSDS)/tools/ndstool/ndstool

ds: $(BUILD)/ff9ds.nds
pc: $(BUILD)/pc/ff9ds

# The ROM patched with the R4 DLDI driver, for DeSmuME's emulated R4 flashcart (--slot1=r4 --slot1-fat-dir=DIR),
# so the game can read ff9ds/ from a host directory.
ds-r4: $(BUILD)/ff9ds_r4.nds
$(BUILD)/ff9ds_r4.nds: $(BUILD)/ff9ds.nds
	@cp $< $@
	@$(BLOCKSDS)/tools/dlditool/dlditool $(BLOCKSDS)/sys/dldi_r4/r4tf.dldi $@ > /dev/null

$(BUILD)/ff9ds.nds: $(BUILD)/ds/ff9ds.elf $(ARM7ELF)
	@echo "NDS  $@"
	@$(NDSTOOL) -c $@ -7 $(ARM7ELF) -9 $< -b $(BLOCKSDS)/sys/icon.bmp "FF9 DS;port build;not for distribution"

$(BUILD)/pc/ff9ds: $(BUILD)/pc/ff9ds.elf
	@cp $< $@

decomp-objs: $(ds_DOBJS)

# --- disc data (host) ----------------------------------------------------------------------------------------
# Needs the four disc images in disks/us/raw/ ("FF9 CD1.img" ... as in the decomp). The decomp's split only needs
# the exe and the code overlays; they are copied into the submodule, whose container can't follow links.
extract:
	$(MAKE) -C $(DECOMP) extract extract-img extract-ovl DISC_DIR=$(CURDIR)/disks/$(VERSION)/raw
	cd $(DECOMP) && tools/docker.sh make split

clean:
	rm -rf $(BUILD)

$(BUILD)/tests/battle_motion_probe: tests/battle/motion_probe.c tests/battle/pose_probe.c tests/battle/background_probe.c port/game/battle_motion.c port/game/battle_pose.c port/game/anim_key.c port/game/battle_background.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/battle/motion_probe.c port/game/battle_motion.c port/game/battle_pose.c port/game/anim_key.c port/game/battle_background.c -Wl,--defsym=psx_ram=0x02000000 -o $@

$(BUILD)/tests/battle_quads_probe: tests/battle/quads_probe.c tests/battle/background_probe.c port/game/battle_quads.c port/game/battle_background.c port/game/effect_vm.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/battle/quads_probe.c port/game/battle_quads.c port/game/battle_background.c port/game/effect_vm.c -Wl,--gc-sections -Wl,--defsym=psx_ram=0x02000000 -o $@

$(BUILD)/tests/battle_strips_probe: tests/battle/strips_probe.c tests/battle/background_probe.c port/game/battle_quads.c port/game/battle_background.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/battle/strips_probe.c port/game/battle_quads.c port/game/battle_background.c -Wl,--gc-sections -Wl,--defsym=psx_ram=0x02000000 -o $@

.PHONY: test-battle-strips
test-battle-strips: $(BUILD)/tests/battle_strips_probe
	$(PYTHON) tools/test_battle_strips.py

$(BUILD)/tests/effect_vm_probe: tests/battle/effect_vm_probe.c tests/battle/background_probe.c port/game/effect_vm.c port/game/battle_background.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/battle/effect_vm_probe.c port/game/effect_vm.c port/game/battle_background.c -Wl,--defsym=psx_ram=0x02000000 -o $@

.PHONY: test-effect-vm
test-effect-vm: $(BUILD)/tests/effect_vm_probe
	$(PYTHON) tools/test_effect_vm.py

$(BUILD)/tests/battle_trails_probe: tests/battle/trails_probe.c port/game/battle_trails.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include $^ -Wl,--defsym=psx_ram=0x02000000 -o $@

.PHONY: test-battle-trails
test-battle-trails: $(BUILD)/tests/battle_trails_probe
	$(PYTHON) tools/test_battle_trails.py

$(BUILD)/tests/window_probe: tests/battle/window_probe.c port/game/window_tween.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include $^ -Wl,--defsym=psx_ram=0x02000000 -o $@

.PHONY: test-window
test-window: $(BUILD)/tests/window_probe
	$(PYTHON) tools/test_window_tween.py

$(BUILD)/tests/battle_morph_probe: tests/battle/morph_probe.c port/game/battle_morph.c tests/gte/oracle.c port/psx/gte.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/battle/morph_probe.c port/game/battle_morph.c tests/gte/oracle.c -Wl,--defsym=psx_ram=0x02000000 -Wl,--defsym=g_vtxScreenXY=0x020711a8 -Wl,--defsym=g_vtxScreenZ=0x02070520 -o $@

.PHONY: test-battle-morph
test-battle-morph: $(BUILD)/tests/gte_oracle.so $(BUILD)/tests/battle_morph_probe
	$(PYTHON) tools/test_battle_morph.py

$(BUILD)/tests/gte_oracle.so: tests/gte/oracle.c port/psx/gte.c port/include/gte.h
	@mkdir -p $(dir $@)
	$(CC) -shared -fPIC -std=gnu17 -O2 -Iport/include $< -o $@

$(BUILD)/tests/battle_ribbons_probe: tests/battle/ribbons_probe.c port/game/battle_ribbons.c port/psx/gte.c port/game/effect_vm.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/battle/ribbons_probe.c port/game/battle_ribbons.c port/game/effect_vm.c -Wl,--gc-sections -Wl,--defsym=psx_ram=0x02000000 -o $@

.PHONY: test-battle-ribbons
test-battle-ribbons: $(BUILD)/tests/gte_oracle.so $(BUILD)/tests/battle_ribbons_probe
	$(PYTHON) tools/test_battle_ribbons.py

.PHONY: test-gte
test-gte: $(BUILD)/tests/gte_op_test $(BUILD)/tests/gte_probe
	@$(BUILD)/tests/gte_op_test
	@echo 'Production GTE cross-product regression checks passed'
$(BUILD)/tests/gte_op_test: tests/gte/op_test.c port/psx/gte.c port/include/gte.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -fsanitize=address,undefined -Iport/include tests/gte/op_test.c port/psx/gte.c -o $@
$(BUILD)/tests/gte_probe: tests/gte/probe.c port/psx/gte.c port/include/gte.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -fsanitize=address,undefined -Iport/include $< -o $@

.PHONY: test-raster
.PHONY: test-low-ram test-bios test-cd test-kernel
test-kernel: $(BUILD)/tests/kernel
	@$<

$(BUILD)/tests/kernel: tests/bios/kernel.c port/psx/kernel.c port/include/psx_mem.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Iport/include $< port/psx/kernel.c -o $@

test-cd: $(BUILD)/tests/cd_sectors
	@$<

$(BUILD)/tests/cd_sectors: tests/bios/cd_sectors.c port/psx/cd.c port/include/psx_mem.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Iport/include -I$(DECOMP)/include $< port/psx/cd.c -Wl,--gc-sections -o $@

test-bios: $(BUILD)/tests/bios_strings
	@$<

$(BUILD)/tests/bios_strings: tests/bios/strings.c port/psx/bios.c port/include/psx_mem.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -Iport/include $< port/psx/bios.c -Wl,--gc-sections -o $@

test-low-ram: $(BUILD)/tests/low_ram_test
	@$(BUILD)/tests/low_ram_test

$(BUILD)/tests/low_ram_test: tests/battle/low_ram_test.c port/include/psx_mem.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Iport/include $< -o $@

test-raster: $(BUILD)/tests/raster_differential
	@$<
$(BUILD)/tests/raster_psx_probe: tests/raster/psx_probe.c port/psx/softgpu.c port/psx/gp0.c port/include/gp0.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/raster/psx_probe.c port/psx/softgpu.c port/psx/gp0.c -o $@
$(BUILD)/tests/raster_differential: tests/raster/differential.c tests/raster/reference.c tests/raster/interpolation_golden.h port/psx/softgpu.c port/include/gp0.h port/include/psx_raster.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include tests/raster/differential.c tests/raster/reference.c port/psx/softgpu.c -o $@

# Disc-free comparison of generic and optimised Thumb/ARM pixel loops on ARM9.
.PHONY: raster-ds
raster-ds: $(BUILD)/tests/raster_ds.nds
RASTER_DS_FLAGS := $(DS_ARCH) -specs=$(SPECS) -I$(BLOCKSDS)/libs/libnds/include -Iport/include -I$(DECOMP)/include -std=gnu17 -O2 -Wno-builtin-declaration-mismatch
# Exercise the same instruction mode as production in the DSP component ROMs.
SPU_DS_IMPL_OBJS := $(addprefix $(BUILD)/tests/spu_ds_,core.o source.o envelope.o voice.o noise.o reverb.o integration.o)
$(SPU_DS_IMPL_OBJS): RASTER_DS_FLAGS += $(if $(filter 1,$(SPUARM)),-marm) $(if $(filter 1,$(SPUITCM)),-DPORT_SPU_ITCM)
$(SPU_DS_IMPL_OBJS): $(BUILD)/ds/runtime-config port/psx/spu_hot.h
.PHONY: spu-ds
spu-ds: $(BUILD)/tests/spu_ds.nds
.PHONY: test-akao-pitch spu-pitch-ds
$(BUILD)/tests/akao_pitch_callee.c $(BUILD)/tests/akao_pitch_caller.c &: $(BUILD)/portified/src/main/main.c tools/akao_pitch_abi_fixture.py
	$(PYTHON) tools/akao_pitch_abi_fixture.py $< $(BUILD)/tests
test-akao-pitch: $(BUILD)/tests/akao_pitch_host
	@$<
$(BUILD)/tests/akao_pitch_host: $(BUILD)/tests/akao_pitch_callee.c $(BUILD)/tests/akao_pitch_caller.c
	$(CC) -m32 -std=gnu17 -O2 -fsigned-char -fno-strict-aliasing -fwrapv -Wno-builtin-declaration-mismatch -Wno-incompatible-pointer-types -I$(DECOMP)/include $^ -o $@
spu-pitch-ds: $(BUILD)/tests/spu_pitch_ds.nds
$(BUILD)/tests/akao_pitch_ds_callee.o: $(BUILD)/tests/akao_pitch_callee.c
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -fsigned-char -fno-strict-aliasing -fwrapv -c $< -o $@
$(BUILD)/tests/akao_pitch_ds_caller.o: $(BUILD)/tests/akao_pitch_caller.c
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Wno-incompatible-pointer-types -c $< -o $@
$(BUILD)/tests/spu_pitch_ds_main.o: tests/spu/pitch_ds_main.c
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/spu_pitch_ds.elf: $(BUILD)/tests/spu_pitch_ds_main.o $(BUILD)/tests/akao_pitch_ds_callee.o $(BUILD)/tests/akao_pitch_ds_caller.o
	arm-none-eabi-gcc $(DS_ARCH) -specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib $^ -Wl,--gc-sections -Wl,--start-group -lnds9 -lc -Wl,--end-group -o $@
$(BUILD)/tests/spu_pitch_ds.nds: $(BUILD)/tests/spu_pitch_ds.elf $(ARM7ELF)
	$(NDSTOOL) -c $@ -7 $(ARM7ELF) -9 $< -b $(BLOCKSDS)/sys/icon.bmp "AKAO pitch ABI;ARM9;Synthetic data"
.PHONY: test-pcm-ring spu-stream-ds
test-pcm-ring: $(BUILD)/tests/pcm_ring_test
	@$<
$(BUILD)/tests/pcm_ring_test: tests/spu/pcm_ring_test.c port/ds/pcm_ring.c port/ds/pcm_ring.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iport/ds $< port/ds/pcm_ring.c -o $@
spu-stream-ds: $(BUILD)/tests/spu_stream_ds.nds
$(BUILD)/tests/pcm_ring_ds_test.o: tests/spu/pcm_ring_test.c port/ds/pcm_ring.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Iport/ds -Dmain=pcm_ring_tests -c $< -o $@
$(BUILD)/tests/spu_stream_ds_main.o: tests/spu/stream_ds_main.c port/ds/audio.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Iport/ds -c $< -o $@
$(BUILD)/tests/spu_stream_ds_audio.o: port/ds/audio.c port/ds/audio.h port/ds/pcm_ring.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -DPORT_AUDIO -I$(BLOCKSDS)/libs/maxmod/include -c $< -o $@
$(BUILD)/tests/spu_stream_ds_ring.o: port/ds/pcm_ring.c port/ds/pcm_ring.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/spu_stream_ds.elf: $(BUILD)/tests/spu_stream_ds_main.o $(BUILD)/tests/pcm_ring_ds_test.o $(BUILD)/tests/spu_stream_ds_audio.o $(BUILD)/tests/spu_stream_ds_ring.o
	arm-none-eabi-gcc $(DS_ARCH) -specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib -L$(BLOCKSDS)/libs/maxmod/lib $^ -Wl,--gc-sections -Wl,--start-group -lmm9 -lnds9 -lc -Wl,--end-group -o $@
$(BUILD)/tests/spu_stream_ds.nds: $(BUILD)/tests/spu_stream_ds.elf $(ARM7ELF)
	$(NDSTOOL) -c $@ -7 $(ARM7ELF) -9 $< -b $(BLOCKSDS)/sys/icon.bmp "SPU PCM stream;ARM9/ARM7;No disc data"
.PHONY: spu-throughput-ds
spu-throughput-ds: $(BUILD)/tests/spu_throughput_ds.nds
$(BUILD)/tests/spu_throughput_ds_main.o: tests/spu/throughput.c port/include/psx_spu_presets.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/spu_throughput_ds.elf: $(BUILD)/tests/spu_throughput_ds_main.o $(BUILD)/tests/spu_ds_noise.o $(BUILD)/tests/spu_ds_reverb.o $(BUILD)/tests/spu_ds_integration.o $(BUILD)/tests/spu_ds_core.o $(BUILD)/tests/spu_ds_source.o $(BUILD)/tests/spu_ds_envelope.o $(BUILD)/tests/spu_ds_voice.o
	arm-none-eabi-gcc $(DS_ARCH) -specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib $^ -Wl,--gc-sections -Wl,--start-group -lnds9 -lc -Wl,--end-group -o $@
$(BUILD)/tests/spu_throughput_ds.nds: $(BUILD)/tests/spu_throughput_ds.elf $(ARM7ELF)
	$(NDSTOOL) -c $@ -7 $(ARM7ELF) -9 $< -b $(BLOCKSDS)/sys/icon.bmp "SPU throughput;ARM9;Synthetic data"
$(BUILD)/tests/spu_throughput_host: tests/spu/throughput.c $(addprefix port/psx/,$(addsuffix .c,$(SPU_IMPL_NAMES))) port/psx/spu_trace.c $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O2 -Iport/include $< $(addprefix port/psx/,$(addsuffix .c,$(SPU_IMPL_NAMES))) port/psx/spu_trace.c -o $@
.PHONY: spu-core-ds
spu-core-ds: $(BUILD)/tests/spu_core_ds.nds
$(BUILD)/tests/spu_core_ds_main.o: tests/spu/core_ds_main.c
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/spu_core_ds_test.o: tests/spu/core_test.c port/include/psx_spu.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Dmain=spu_core_tests -c $< -o $@
$(BUILD)/tests/spu_ds_noise_test.o: tests/spu/noise_test.c tests/spu/noise_reference.h port/include/psx_spu.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Dmain=spu_noise_tests -c $< -o $@
$(BUILD)/tests/spu_ds_noise.o: port/psx/spu_noise.c port/include/psx_spu.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/spu_ds_integration.o: port/psx/spu_core.c port/include/psx_spu_reverb.h port/include/psx_spu.h port/include/psx_spu_voice.h port/include/psx_spu_source.h port/include/psx_spu_adpcm.h port/include/psx_spu_envelope.h port/include/platform.h port/include/psx_spu_trace.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/spu_ds_reverb_test.o: tests/spu/reverb_test.c tests/spu/reverb_cases.h tests/spu/reverb_reference.h tests/spu/reverb_presets.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Dmain=spu_reverb_tests -c $< -o $@
$(BUILD)/tests/spu_ds_reverb.o: port/psx/spu_reverb.c port/include/psx_spu_reverb.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/spu_core_ds.elf: $(BUILD)/tests/spu_ds_reverb_test.o $(BUILD)/tests/spu_core_ds_main.o $(BUILD)/tests/spu_core_ds_test.o $(BUILD)/tests/spu_ds_noise_test.o $(BUILD)/tests/spu_ds_noise.o $(BUILD)/tests/spu_ds_reverb.o $(BUILD)/tests/spu_ds_integration.o $(BUILD)/tests/spu_ds_core.o $(BUILD)/tests/spu_ds_source.o $(BUILD)/tests/spu_ds_envelope.o $(BUILD)/tests/spu_ds_voice.o
	arm-none-eabi-gcc $(DS_ARCH) -specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib $^ -Wl,--gc-sections -Wl,--start-group -lnds9 -lc -Wl,--end-group -o $@
$(BUILD)/tests/spu_core_ds.nds: $(BUILD)/tests/spu_core_ds.elf
	$(NDSTOOL) -c $@ -7 $(ARM7ELF) -9 $< -b $(BLOCKSDS)/sys/icon.bmp "SPU integration;ARM9;No disc data"
$(BUILD)/tests/spu_ds_main.o: tests/spu/ds_main.c
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/spu_ds_test.o: tests/spu/adpcm_test.c tests/spu/adpcm_cases.h tests/spu/adpcm_reference.h port/include/psx_spu_adpcm.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Dmain=spu_adpcm_tests -c $< -o $@
$(BUILD)/tests/spu_ds_core.o: port/psx/spu_adpcm.c port/include/psx_spu_adpcm.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/spu_ds_source_test.o: tests/spu/source_test.c port/include/psx_spu_source.h port/include/psx_spu_adpcm.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Dmain=spu_source_tests -c $< -o $@
$(BUILD)/tests/spu_ds_source.o: port/psx/spu_source.c port/include/psx_spu_source.h port/include/psx_spu_adpcm.h port/include/psx_spu_gauss.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/spu_ds_envelope_test.o: tests/spu/envelope_test.c tests/spu/envelope_cases.h tests/spu/envelope_reference.h port/include/psx_spu_envelope.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Dmain=spu_envelope_tests -c $< -o $@
$(BUILD)/tests/spu_ds_envelope.o: port/psx/spu_envelope.c port/include/psx_spu_envelope.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/spu_ds_interpolation_test.o: tests/spu/interpolation_test.c tests/spu/interpolation_cases.h tests/spu/interpolation_reference.h port/include/psx_spu_source.h port/include/psx_spu_adpcm.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Dmain=spu_interpolation_tests -c $< -o $@
$(BUILD)/tests/spu_ds_voice_test.o: tests/spu/voice_test.c tests/spu/voice_cases.h tests/spu/voice_reference.h port/include/psx_spu_voice.h port/include/psx_spu_source.h port/include/psx_spu_adpcm.h port/include/psx_spu_envelope.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Dmain=spu_voice_tests -c $< -o $@
$(BUILD)/tests/spu_ds_voice.o: port/psx/spu_voice.c port/include/psx_spu_voice.h port/include/psx_spu_source.h port/include/psx_spu_adpcm.h port/include/psx_spu_envelope.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/spu_ds.elf: $(BUILD)/tests/spu_ds_main.o $(BUILD)/tests/spu_ds_test.o $(BUILD)/tests/spu_ds_core.o $(BUILD)/tests/spu_ds_source_test.o $(BUILD)/tests/spu_ds_source.o $(BUILD)/tests/spu_ds_envelope_test.o $(BUILD)/tests/spu_ds_envelope.o $(BUILD)/tests/spu_ds_interpolation_test.o $(BUILD)/tests/spu_ds_voice_test.o $(BUILD)/tests/spu_ds_voice.o
	arm-none-eabi-gcc $(DS_ARCH) -specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib $^ -Wl,--gc-sections -Wl,--start-group -lnds9 -lc -Wl,--end-group -o $@
$(BUILD)/tests/spu_ds.nds: $(BUILD)/tests/spu_ds.elf
	$(NDSTOOL) -c $@ -7 $(ARM7ELF) -9 $< -b $(BLOCKSDS)/sys/icon.bmp "SPU components;ARM9;No disc data"
.PHONY: gpu-ds
gpu-ds: $(BUILD)/tests/gpu_ds.nds
$(BUILD)/tests/gpu_ds_main.o: tests/gpu/ds_main.c
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/gpu_ds_env.o: tests/gpu/env_render.c tests/gpu/overlap_reference.h port/include/gp0.h port/include/psyq_port.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Dmain=gpu_transfer_tests -c $< -o $@
$(BUILD)/tests/gpu_ds_gpu.o: port/psx/gpu.c port/include/gp0.h port/include/psyq_port.h port/include/profile.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -ffunction-sections -fdata-sections -c $< -o $@
$(BUILD)/tests/gpu_ds_gp0.o: port/psx/gp0.c port/include/gp0.h port/include/profile.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/gpu_ds.elf: $(BUILD)/tests/gpu_ds_main.o $(BUILD)/tests/gpu_ds_env.o $(BUILD)/tests/gpu_ds_gpu.o $(BUILD)/tests/gpu_ds_gp0.o $(BUILD)/tests/raster_ds_fast.o
	arm-none-eabi-gcc $(DS_ARCH) -specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib $^ -Wl,--gc-sections -Wl,--start-group -lnds9 -lc -Wl,--end-group -o $@
$(BUILD)/tests/gpu_ds.nds: $(BUILD)/tests/gpu_ds.elf
	$(NDSTOOL) -c $@ -7 $(ARM7ELF) -9 $< -b $(BLOCKSDS)/sys/icon.bmp "GPU transfers;ARM9;No disc data"
$(BUILD)/tests/raster_ds_main.o: tests/raster/ds_benchmark.c tests/raster/interpolation_golden.h port/include/gp0.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/raster_ds_reference.o: tests/raster/reference.c port/psx/softgpu.c
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/raster_ds_fast.o: port/psx/softgpu.c
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -c $< -o $@
$(BUILD)/tests/raster_ds_arm.o: port/psx/softgpu.c
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -marm -Dpsx_softgpu=psx_softgpu_arm -Dpsx_softgpu_enable=psx_softgpu_arm_enable -Dpsx_softgpu_shadow_poly=psx_softgpu_arm_shadow_poly -Dpsx_softgpu_shadow_poly_region=psx_softgpu_arm_shadow_poly_region -Dpsx_softgpu_shadow_rect=psx_softgpu_arm_shadow_rect -c $< -o $@
$(BUILD)/tests/raster_ds.elf: $(BUILD)/tests/raster_ds_main.o $(BUILD)/tests/raster_ds_reference.o $(BUILD)/tests/raster_ds_fast.o $(BUILD)/tests/raster_ds_arm_itcm.o
	arm-none-eabi-gcc $(DS_ARCH) -specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib $^ -Wl,--start-group -lnds9 -lc -Wl,--end-group -o $@
$(BUILD)/tests/raster_ds.nds: $(BUILD)/tests/raster_ds.elf
	$(NDSTOOL) -c $@ -7 $(ARM7ELF) -9 $< -b $(BLOCKSDS)/sys/icon.bmp "Raster timing;ARM9;No disc data"

$(BUILD)/tests/raster_ds_arm_itcm.o: port/psx/softgpu.c
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -marm -DPORT_RASTER_ITCM -Dpsx_softgpu=psx_softgpu_arm_itcm -Dpsx_softgpu_enable=psx_softgpu_arm_itcm_enable -Dpsx_softgpu_shadow_poly=psx_softgpu_arm_itcm_shadow_poly -Dpsx_softgpu_shadow_poly_region=psx_softgpu_arm_itcm_shadow_poly_region -Dpsx_softgpu_shadow_rect=psx_softgpu_arm_itcm_shadow_rect -c $< -o $@

.PHONY: test-display scale-ds
test-display: $(BUILD)/tests/display_scale_test $(BUILD)/tests/display_glyph_test
	@$(BUILD)/tests/display_scale_test
	@$(BUILD)/tests/display_glyph_test
$(BUILD)/tests/display_glyph_test: tests/display/glyph_test.c port/psx/gp0.c port/include/gp0.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -fsanitize=address,undefined -Iport/include tests/display/glyph_test.c port/psx/gp0.c -o $@
$(BUILD)/tests/display_scale_test: tests/display/scale_test.c port/ds/display_scale.c port/ds/display_scale.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -O1 -g -fsanitize=address,undefined -Iport/ds tests/display/scale_test.c port/ds/display_scale.c -o $@

scale-ds: $(BUILD)/tests/display_scale.nds
$(BUILD)/tests/display_scale_main.o: tests/display/ds_benchmark.c port/ds/display_scale.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -Iport/ds -c $< -o $@
$(BUILD)/tests/display_scale.o: port/ds/display_scale.c port/ds/display_scale.h
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(RASTER_DS_FLAGS) -marm -DPORT_SCALE_ITCM -c $< -o $@
$(BUILD)/tests/display_scale.elf: $(BUILD)/tests/display_scale_main.o $(BUILD)/tests/display_scale.o
	arm-none-eabi-gcc $(DS_ARCH) -specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib $^ -Wl,--start-group -lnds9 -lc -Wl,--end-group -o $@
$(BUILD)/tests/display_scale.nds: $(BUILD)/tests/display_scale.elf
	$(NDSTOOL) -c $@ -7 $(ARM7ELF) -9 $< -b $(BLOCKSDS)/sys/icon.bmp "Display scaling;ARM9;No disc data"
