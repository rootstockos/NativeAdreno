# Gen8 (Adreno 810) register inventory

The A810 (SM7635) is recognised by its profile and refused at start (`implementedGeneration` is 7). This file lists
what each module that touches registers must change before that guard can go. Every offset below is generated into
`NativeAdrenoA810HwInit.h` by `scripts/derive-a810-hwinit.py` from torvalds/linux `734bef548a96` (pins in
`scripts/a810-inputs.json`; `--fetch` makes the checkout, `--check` verifies the header). `tests/test_a810_hwinit.cpp`
checks the moved offsets, the tables and the power-up list size.

Gen8 has a new register map. Two gen7 offsets now name *other* registers, so running the gen7 paths would write the
wrong registers, not just miss:
- 0x0830, the gen7 `CP_SQE_INSTR_BASE`, is `CP_SCRATCH_PIPE(0)` on gen8;
- 0x0808, the gen7 `CP_SQE_CNTL`, is `CP_RB_RPTR_ADDR_BV` on gen8.

## Status

CP init, IRQ, ring, timer and the power-up list now take their registers from `NativeAdrenoRegisters.h` (gen7 and
gen8 tables; `NativeAdrenoRegisterTable::forGeneration`). The service picks the table from the profile's generation
and hands it to each module before it attaches. `tests/test_gen8.cpp` runs those modules on the gen8 table over a
register peer that models the aperture, and the existing tests still pass on the gen7 default. The start guard
remains: the rows below from *Clock gating* down are not written yet.

## Per module

| Module | Gen7 (A740) | Gen8 (A810) | Change |
| --- | --- | --- | --- |
| CpInit | `CP_SQE_INSTR_BASE` 0x0830, `CP_SQE_CNTL` 0x0808 | 0x0816, 0x0815 | offsets only. `CP_THREAD_CONTROL` (bit 27) and the seven `CP_ME_INIT` words are identical (`a8xx_cp_init`) |
| Irq | `RBBM_INT_0_STATUS` 0x0201, `INT_CLEAR_CMD` 0x0037, `INT_0_MASK` 0x0038 | 0x006a, 0x0061, 0x0062 | offsets. The enable set is the same bits (0x33d283c2). `CP_INTERRUPT_STATUS` 0x0823 and `CP_PROTECT_STATUS` 0x0824 have no gen8 definition. Gen8 reads `CP_INTERRUPT_STATUS_GLOBAL` 0x08b5 (HW and SW fault bits per pipe), then `CP_HW_FAULT_STATUS_PIPE` 0x08b3 and `CP_INTERRUPT_STATUS_PIPE` 0x08b7 through the aperture |
| Ring | `CP_RB_BASE`/`CNTL`/`RPTR_ADDR`/`RPTR`/`WPTR` 0x0800..0x0807 | same | unchanged. BV rptr address 0x0a98 → 0x0808. Fence scratch `CP_SCRATCH(2)` 0x0885 → `CP_SCRATCH_GLOBAL(2)` 0x082c. `RBBM_PERFCTR_SRAM_INIT_CMD`/`STATUS` 0x050e/0x050f → 0x0449/0x019f, plus `RBBM_SLICE_PERFCTR_SRAM_INIT_CMD` 0x05e8. Gen8 submits through `a7xx_submit`, so the PM4 framing carries over |
| Timer | `CP_ALWAYS_ON_COUNTER` 0x0980; GMU counter 0x1f888 | 0x08e7; 0x1f840 | `timerGmuLowOffset` hardcodes 0x1f888. On the 3a (GMU 0x3d37000) the gen8 counter is byte 0x47100 of the GMU window |
| Pwrup | one preemption list, 16 registers | IFPC 72, preemption 45, dynamic 26 entries (44 triplets) | `pack()` already writes all three; 1,480 bytes fit the power-up page. Dynamic triplets are `(PIPEID << 12, offset, value)`, read per pipe through `CP_APERTURE_CNTL_HOST` |
| Aperture | `CP_APERTURE_CNTL_HOST` 0x0a00 | 0x081c, PIPEID bits 12..15, SLICEID 16..18 | all per-pipe writes (non-context, protect, APRIV, CP masks, UBWC) go through it, and the aperture is cleared after |
| Protect | `CP_PROTECT(i)` 0x0850, last span at index `count_max-1` | `CP_PROTECT_GLOBAL(i)` 0x0850, last span again at `CP_PROTECT_PIPE(15)` 0x08af for BR and BV | 47 writes; control word 0xffff000b to `CP_PROTECT_CNTL_PIPE` 0x084e per pipe |
| Clock gating | `a740_hwcg` table + GMU CGC mode | no table: `RBBM_CLOCK_CNTL_GLOBAL`, `CGC_GLOBAL_LOAD_CMD`, `CGC_P2S_TRIG_CMD`, poll `CGC_P2S_STATUS` TXDONE | different procedure, not a table swap |
| hw_init tables | `a740_hwcg`, `a730_protect` (generated, not applied in this tree) | `a810_nonctxt_regs` (39, per pipe), `a840_gbif` (5), bicubic weights (20), UBWC fields | ready in the header; `RB_GC_GMEM_PROTECT` is written only after the secure-mode exit |
| GMU | gen7 boot | `a6xx_gmu.c` gen8 branches (`GMU_GENERAL_8/9/10`, `CX_FAL_INTF`, RPMh power state) | next step; the GMU offsets in the header are not yet relative to the GMU window |

## Not covered yet

- GMU gen8 boot and HFI, RSC/PDC, and the volcano GPU CC.
- The 3a's ADT GPU node needs `qcom,chipid` 0x44010000 (its device tree has none).
- Zap shader under Gunyah.
