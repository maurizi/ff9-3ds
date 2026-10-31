# Host-side unit tests of the shared PS1 layer (from ff9-ds). Included by the Makefile.
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

$(BUILD)/tests/window_probe: tests/battle/window_probe.c port/game/window_tween.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pie -no-pie -std=gnu17 -O1 -g -fsanitize=address,undefined -Wno-builtin-declaration-mismatch -Iport/include -I$(DECOMP)/include $^ -Wl,--defsym=psx_ram=0x02000000 -o $@

.PHONY: test-window
test-window: $(BUILD)/tests/window_probe
	$(PYTHON) tools/test_window_tween.py

$(BUILD)/tests/gte_oracle.so: tests/gte/oracle.c port/psx/gte.c port/include/gte.h
	@mkdir -p $(dir $@)
	$(CC) -shared -fPIC -std=gnu17 -O2 -Iport/include $< -o $@

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
