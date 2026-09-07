# Third-Party Notices

This project is distributed under GPL-2.0-or-later (see [`LICENSE`](LICENSE)). It
includes, derives from, and links against third-party code that is itself licensed
under the GNU General Public License. Copyright remains with the original authors,
listed below. Their license terms are preserved in the source file headers.

## Vendored / referenced kernel audio code

- **`ar9331-i2s-alsa`** — Franz Flasch `<franz.flasch@gmx.at>`.
  GPLv2. The Beep I²S driver (`feed/beep-i2s/src/beep-i2s.c`) is a devicetree-based
  rewrite derived from this work; the register layout and clock table are carried
  over. Original sources are retained for reference under `notes/franzflasch/` and
  `driver-i2s/reference/`.

- **AR9331 / ath79 ASoC platform + PCM/DMA** (`ath79-pcm.c`, `ath79-i2s.c`,
  `ath79-mbox.c`, `ath-carambola2.c`, and related headers under
  `driver-i2s/reference/` and `notes/franzflasch/src/`) —
  Mathieu Olivari `<mathieu@qca.qualcomm.com>`, Qualcomm Atheros, Inc., and other
  contributors. Dual BSD/GPL as marked in the file headers.

- **WM8727 codec driver** (`notes/franzflasch/src/wm8727.c`) —
  Neil Jones `<neil.jones@imgtec.com>`, Imagination Technologies. GPL.

## Platform

- **OpenWrt** and the **Linux kernel** — GPL-2.0. This firmware is built on OpenWrt
  and ships a Linux kernel module; the combined firmware image is a GPL work.
  Corresponding source for the kernel and OpenWrt packages is available from the
  OpenWrt project and this repository.

---

If you believe attribution here is incomplete or incorrect, please open an issue.
