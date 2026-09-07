# beep-i2s — the MBOX-DMA "data plane"

> **✅ DONE (2026-09-05) — this port is complete and plays clean audio on hardware.**
> All of the steps below were carried out in `beep-i2s.c` (kernel-6.6 PCM
> component). Kept as the record of what was ported + two gotchas that aren't in
> the reference: (1) the HW descriptor is built with **explicit bit-shifts, not C
> bitfields** — the vendored struct assumes big-endian, ath79 24.10 is LE; (2) a
> **hard MBOX reset** (RESET module `0x1806001c` bit1) is needed each `prepare`
> or the 2nd+ playback fails `-EIO`, and the ALSA buffer must be forced to an
> integer number of periods (`snd_pcm_hw_constraint_integer`) or a partial buffer
> tail is skipped each loop → a cyclical pop. Playback runs on the **MBOX0 RX**
> channel (`BEEP_PLAYBACK_TX` flips to the stock TX path if ever needed).

## Original port task (for reference)

`beep-i2s.c` is the DT-probed **control plane** (clock/format/enable + GPIO mux),
carried over verbatim from the register map and expected correct. The **data
plane** — the MBOX descriptor-ring DMA feeding the ALSA PCM ring — is the part
the research flagged as genuine bring-up. Reference source (Linux 3.14, works on
old kernels) is vendored in `reference/` (`ath79-mbox.c`, `ath79-pcm.c`).

## What to port (scoped, ordered)
1. **ioremap, not KSEG1.** Reference uses `ath79_mbox_rr/wr` = direct
   `KSEG1ADDR(AR934X_DMA_BASE+off)`. Replace with `readl/writel(b->mbox + off)`
   using the offsets in `ath79-stereo-regs-dt.h` (already done for the DAI).
2. **PCM as a soc-component, not a legacy platform.** Reference registers a
   `snd_soc_platform`; on 6.x fold the PCM ops into `beep_i2s_component`:
   `.pcm_construct` (allocate the coherent DMA buffer via
   `snd_pcm_set_managed_buffer_all(..., SNDRV_DMA_TYPE_DEV, dev, size, size)`),
   `.open` (set `snd_pcm_hardware`), `.hw_params`, `.trigger`, `.pointer`,
   and `.copy` if needed.
3. **Descriptor ring.** Port `ath79_mbox_dma_desc` / `ath79_i2s_init_desc`
   (from the stock ath_i2s.ko — unstripped, symbol `ath_i2s_init_desc` is the
   oracle) — a ring of `{ OWN, size, buf_ptr, next_ptr, ... }` descriptors in
   coherent DMA memory; program `MBOX0_TX_DESCRIPTOR_BASE` (mbox+0x20) with the
   ring head, then `MBOX0_TX_CONTROL` (mbox+0x24) START=bit1 to run,
   RESUME=bit2, STOP=bit0.  (Confirmed START/RESUME/STOP encoding from the oracle.)
4. **IRQ.** Request the DT interrupt (miscintc line 7); in the handler read
   `MBOX_INT_STATUS` (mbox+0x44), ack TX_COMPLETE (bit6), advance the ring, and
   `snd_pcm_period_elapsed()`.
5. **FIFO reset** (mbox+0x58 = 0xff) on open; **DMA policy** (mbox+0x10) TX FIFO
   threshold per the reference.

## Bench checkpoints (do in this order)
- soundcard node appears (`cat /proc/asound/cards`) — franzflasch users stalled here.
- Saleae on CK/WS/MCK: confirm the clocks match hw_params (48k×256) BEFORE analog.
- `speaker-test -Dhw:0,0 -c2 -r48000 -FS16_LE -tsine` → clean tone.
- Verify the WM8524 **mute-GPIO** assumption in the DTS (`wlf,mute-gpios`); if the
  mute pin is hard-strapped, patch wm8524.c to make it optional or use
  `linux,snd-soc-dummy`.
