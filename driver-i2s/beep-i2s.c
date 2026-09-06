// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * beep-i2s.c — AR9331 I2S CPU DAI + pinmux/config for the Beep (ASoC, DT-probed).
 *
 * Modern (kernel 6.x, devicetree) rewrite of franzflasch/ar9331-i2s-alsa's
 * ath79-i2s.c + the Carambola2 board init in ath-carambola2.c. Register layout
 * and clock table are carried over verbatim (see driver-i2s/ath79-stereo-regs.h)
 * and cross-checked against the stock, unstripped ath_i2s.ko (MBOX TX control
 * @ mbox+0x24: START=bit1 / RESUME=bit2 / STOP=bit0).
 *
 * SCOPE / STATUS:
 *   This file is the "control plane" — clock/format/enable + the GPIO function
 *   mux — which is fully specifiable from the register map and is expected to be
 *   correct.  The "data plane" (the MBOX descriptor-ring DMA + ALSA PCM) is the
 *   part the research flagged as genuine bring-up work; the reference for it is
 *   vendored at driver-i2s/ath79-mbox.c and ath79-pcm.c, and the port task is
 *   spelled out in driver-i2s/PORTING.md.  Until that PCM component lands, this
 *   driver registers the CPU DAI so `aplay -D hw` fails cleanly with "no PCM"
 *   rather than silently — which is the correct Phase-1 checkpoint.
 *
 * Bench verification order (RESEARCH-SYNTHESIS.md): RAM-boot -> confirm the
 * soundcard node appears -> Saleae on CK/WS/MCK to prove the clocks match
 * hw_params BEFORE trusting analog -> then speaker-test.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/pcm_params.h>

#include "ath79-stereo-regs-dt.h"

#define DRV_NAME "beep-i2s"

/*
 * ---- MBOX-DMA register bits missing from the DT-offsets header ----
 * (playback uses MBOX0; see BEEP_PLAYBACK_TX for the RX-vs-TX channel choice)
 */
#define AR934X_DMA_MBOX0_INT_RX_COMPLETE		BIT(10)
#define AR934X_DMA_MBOX_INT_STATUS_RX_DMA_COMPLETE	BIT(10)
#define AR934X_DMA_MBOX_DMA_POLICY_RX_QUANTUM		BIT(1)
#define AR934X_DMA_MBOX_DMA_POLICY_TX_QUANTUM		BIT(3)
#define AR934X_DMA_MBOX_DMA_POLICY_TX_FIFO_THRESH_SHIFT	4

/*
 * PLAYBACK DMA CHANNEL — the one real ambiguity in this port.
 *   0 = MBOX0 *RX* regs (0x18 base / 0x1c ctrl / int bit10) — the franzflasch
 *       working reference's choice ("RX regs for playback").
 *   1 = MBOX0 *TX* regs (0x20 base / 0x24 ctrl / int bit6)  — what the Beep's
 *       own stock ath_i2s.ko used per the disassembly (PORTING.md).
 * Both are internally consistent; a wrong pick = silence (no crash). Flip this
 * one line and rebuild if RX is silent.
 */
#define BEEP_PLAYBACK_TX 0

#if BEEP_PLAYBACK_TX
#  define PB_DESC_BASE     AR934X_DMA_REG_MBOX0_DMA_TX_DESCRIPTOR_BASE  /* 0x20 */
#  define PB_CONTROL       AR934X_DMA_REG_MBOX0_DMA_TX_CONTROL          /* 0x24 */
#  define PB_INT_ENABLE_B  AR934X_DMA_MBOX0_INT_TX_COMPLETE             /* BIT6 */
#  define PB_INT_STATUS_B  AR934X_DMA_MBOX_INT_STATUS_TX_DMA_COMPLETE   /* BIT6 */
#  define PB_POLICY_QUANTUM AR934X_DMA_MBOX_DMA_POLICY_TX_QUANTUM       /* BIT3 */
#else
#  define PB_DESC_BASE     AR934X_DMA_REG_MBOX0_DMA_RX_DESCRIPTOR_BASE  /* 0x18 */
#  define PB_CONTROL       AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL          /* 0x1c */
#  define PB_INT_ENABLE_B  AR934X_DMA_MBOX0_INT_RX_COMPLETE             /* BIT10 */
#  define PB_INT_STATUS_B  AR934X_DMA_MBOX_INT_STATUS_RX_DMA_COMPLETE   /* BIT10 */
#  define PB_POLICY_QUANTUM AR934X_DMA_MBOX_DMA_POLICY_RX_QUANTUM       /* BIT1 */
#endif

/* ALSA ring limits. Descriptor size/length fields are 12-bit -> one period must
 * fit in a single descriptor (<=4095B); keep it frame-aligned (4B, S16 stereo). */
#define BEEP_PERIOD_BYTES_MAX	4092
#define BEEP_PERIOD_BYTES_MIN	256
#define BEEP_PERIODS_MAX	64
#define BEEP_BUFFER_BYTES_MAX	(BEEP_PERIODS_MAX * BEEP_PERIOD_BYTES_MAX)

/*
 * AR9331 MBOX DMA descriptor — HARDWARE layout, built with explicit shifts (NOT
 * C bitfields) so it is endianness-independent (OpenWrt ath79 24.10 is little-
 * endian; the vendored reference's bitfield struct assumed big-endian):
 *   ctrl:    [31]OWN [30]EOM [23:12]size [11:0]length
 *   bufptr:  [27:0] audio-buffer physical address
 *   nextptr: [27:0] next descriptor physical address
 *   vuca[36]: Va/Ua/Ca/Vb/Ub/Cb voice-channel words (unused for audio; zeroed
 *             but kept so the hw descriptor stride matches the stock/ref layout).
 * CPU and DMA engine are the same SoC sharing bus endianness, so no byteswap.
 */
struct beep_hwdesc {
	u32 ctrl;
	u32 bufptr;
	u32 nextptr;
	u32 vuca[36];
};

#define DESC_CTRL(own, size, len) \
	(((own) << 31) | (((size) & 0xfff) << 12) | ((len) & 0xfff))
#define DESC_OWN(ctrl)	(((ctrl) >> 31) & 1)

/* AR9331 RESET module — one register we poke to hard-reset the MBOX DMA engine
 * between streams (0x1806001c, MBOX = BIT(1); same across ath79, used by the
 * franzflasch ref on AR9331). Without this the 2nd+ playback fails: the engine
 * keeps stale internal state from the previous stream. */
#define AR9331_RESET_MODULE_PHYS	0x1806001c
#define AR9331_RESET_MBOX		BIT(1)

/* AR9331 GPIO function block (for the I2S pin mux — no mainline pinctrl path) */
#define AR9331_GPIO_BASE	0x18040000
#define AR9331_GPIO_OE		0x00
#define AR9331_GPIO_FUNC	0x28
#define AR9331_GPIO_FUNC2	0x30
#define FUNC_I2S_GPIO_18_22_EN	BIT(29)
#define FUNC_I2S_REFCLKEN	BIT(28)
#define FUNC_I2S_MCKEN		BIT(27)
#define FUNC_I2S0_EN		BIT(26)
#define I2S_PIN_SCK   18
#define I2S_PIN_WS    19
#define I2S_PIN_SD    20
#define I2S_PIN_MCLK  21
#define I2S_PIN_MIC   22

struct beep_i2s {
	struct device *dev;
	void __iomem  *stereo;   /* 0x180b0000 */
	void __iomem  *mbox;     /* 0x180a0000 */
	void __iomem  *reset;    /* 0x1806001c (MBOX reset bit) */
	int            irq;

	/* data plane (playback) */
	struct snd_pcm_substream *ss;   /* active playback substream, or NULL */
	struct beep_hwdesc *ring;       /* coherent descriptor ring */
	dma_addr_t     ring_dma;
	unsigned int   nperiods;
	unsigned int   period_bytes;
	unsigned int   hw_idx;          /* descriptor the DMA is on (advanced in ISR) */
};

/* divint / divfrac for CLK_DIV (stereo+0x1c) = (divint<<16)|divfrac; posedge=2 */
static const struct { u32 rate, divint, divfrac; } clk_cfg[] = {
	{ 44100, 0x11, 0xB726 },
	{ 48000, 0x10, 0x46AB },
};

static inline u32 sr(struct beep_i2s *b, u32 o)        { return readl(b->stereo + o); }
static inline void sw(struct beep_i2s *b, u32 o, u32 v){ writel(v, b->stereo + o); (void)readl(b->stereo + o); }
static inline u32 mr(struct beep_i2s *b, u32 o)        { return readl(b->mbox + o); }
static inline void mw(struct beep_i2s *b, u32 o, u32 v){ writel(v, b->mbox + o); (void)readl(b->mbox + o); }

static void beep_stereo_reset(struct beep_i2s *b)
{
	sw(b, AR934X_STEREO_REG_CONFIG, sr(b, AR934X_STEREO_REG_CONFIG) | AR934X_STEREO_CONFIG_RESET);
}

/* Hard-reset the MBOX DMA engine via the RESET module — clears all internal
 * engine state so a fresh stream starts clean (fixes 2nd+ playback failing). */
static void beep_mbox_reset(struct beep_i2s *b)
{
	u32 t;
	if (!b->reset)
		return;
	t = readl(b->reset);
	writel(t | AR9331_RESET_MBOX, b->reset); readl(b->reset);
	udelay(100);
	writel(t & ~AR9331_RESET_MBOX, b->reset); readl(b->reset);
	udelay(100);
}

/* One-time pin mux: enable the I2S function on GPIO18-22 + MCLK, SPDIF on GPIO23,
 * output-enable SCK/WS/SD/MCLK, input MIC. (from ath-carambola2.c) */
static int beep_pinmux(struct device *dev)
{
	void __iomem *g = devm_ioremap(dev, AR9331_GPIO_BASE, 0x40);
	u32 v;
	if (!g)
		return -ENOMEM;
	v = readl(g + AR9331_GPIO_FUNC);
	v |= FUNC_I2S_GPIO_18_22_EN | FUNC_I2S_MCKEN | FUNC_I2S0_EN;
	writel(v, g + AR9331_GPIO_FUNC); readl(g + AR9331_GPIO_FUNC);
	writel(readl(g + AR9331_GPIO_FUNC2) | BIT(2), g + AR9331_GPIO_FUNC2); /* SPDIF/GPIO23 */
	v = readl(g + AR9331_GPIO_OE);
	v |= BIT(I2S_PIN_SCK) | BIT(I2S_PIN_WS) | BIT(I2S_PIN_SD) | BIT(I2S_PIN_MCLK);
	v &= ~BIT(I2S_PIN_MIC);
	writel(v, g + AR9331_GPIO_OE); readl(g + AR9331_GPIO_OE);
	return 0;
}

static int beep_i2s_startup(struct snd_pcm_substream *ss, struct snd_soc_dai *dai)
{
	struct beep_i2s *b = snd_soc_dai_get_drvdata(dai);
	if (!snd_soc_dai_active(dai)) {
		sw(b, AR934X_STEREO_REG_CONFIG,
		   AR934X_STEREO_CONFIG_SPDIF_ENABLE | AR934X_STEREO_CONFIG_I2S_ENABLE |
		   AR934X_STEREO_CONFIG_SAMPLE_CNT_CLEAR_TYPE | AR934X_STEREO_CONFIG_MASTER);
		beep_stereo_reset(b);
	}
	return 0;
}

static void beep_i2s_shutdown(struct snd_pcm_substream *ss, struct snd_soc_dai *dai)
{
	struct beep_i2s *b = snd_soc_dai_get_drvdata(dai);
	if (!snd_soc_dai_active(dai))
		sw(b, AR934X_STEREO_REG_CONFIG, 0);
}

static int beep_i2s_hw_params(struct snd_pcm_substream *ss,
			      struct snd_pcm_hw_params *p, struct snd_soc_dai *dai)
{
	struct beep_i2s *b = snd_soc_dai_get_drvdata(dai);
	u32 mask = 0, t;
	int i;

	for (i = 0; i < ARRAY_SIZE(clk_cfg); i++)
		if (params_rate(p) == clk_cfg[i].rate)
			break;
	if (i == ARRAY_SIZE(clk_cfg)) {
		dev_err(b->dev, "unsupported rate %d\n", params_rate(p));
		return -EINVAL;
	}
	sw(b, AR934X_STEREO_CONFIG_CLK_DIV, (clk_cfg[i].divint << 16) | clk_cfg[i].divfrac);

	switch (params_format(p)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		mask |= AR934X_STEREO_CONFIG_PCM_SWAP;
		fallthrough;
	case SNDRV_PCM_FORMAT_S16_BE:
		mask |= AR934X_STEREO_CONFIG_DATA_WORD_16 << AR934X_STEREO_CONFIG_DATA_WORD_SIZE_SHIFT;
		break;
	default:
		dev_err(b->dev, "unsupported format %d\n", params_format(p));
		return -EINVAL;
	}

	t = sr(b, AR934X_STEREO_REG_CONFIG);
	t &= ~(AR934X_STEREO_CONFIG_DATA_WORD_SIZE_MASK << AR934X_STEREO_CONFIG_DATA_WORD_SIZE_SHIFT);
	t &= ~AR934X_STEREO_CONFIG_I2S_WORD_SIZE;
	t |= mask;
	t |= (2 & AR934X_STEREO_CONFIG_POSEDGE_MASK);   /* posedge = 2 */
	sw(b, AR934X_STEREO_REG_CONFIG, t);
	beep_stereo_reset(b);
	return 0;
}

static const struct snd_soc_dai_ops beep_i2s_dai_ops = {
	.startup   = beep_i2s_startup,
	.shutdown  = beep_i2s_shutdown,
	.hw_params = beep_i2s_hw_params,
};

static struct snd_soc_dai_driver beep_i2s_dai = {
	.name = "beep-i2s",
	.playback = {
		.stream_name  = "Playback",
		.channels_min = 2, .channels_max = 2,
		.rates   = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S16_BE,
	},
	.ops = &beep_i2s_dai_ops,
};

/* =========================================================================
 *  DATA PLANE — MBOX descriptor-ring DMA + ALSA PCM component (kernel-6.x port
 *  of reference/ath79-pcm.c + ath79-mbox.c). Playback only.
 * ========================================================================= */

static const struct snd_pcm_hardware beep_pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats          = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S16_BE,
	.rates            = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
	.rate_min         = 44100,
	.rate_max         = 48000,
	.channels_min     = 2,
	.channels_max     = 2,
	.buffer_bytes_max = BEEP_BUFFER_BYTES_MAX,
	.period_bytes_min = BEEP_PERIOD_BYTES_MIN,
	.period_bytes_max = BEEP_PERIOD_BYTES_MAX,
	.periods_min      = 2,
	.periods_max      = BEEP_PERIODS_MAX,
};

/* Build the circular descriptor ring over the (already allocated) managed audio
 * buffer. One descriptor per period; all initially OWNed by the DMA engine. */
static void beep_ring_build(struct beep_i2s *b, dma_addr_t audio, u32 pb, u32 np)
{
	unsigned int i;

	for (i = 0; i < np; i++) {
		b->ring[i].ctrl    = DESC_CTRL(1, pb, pb);
		b->ring[i].bufptr  = (u32)(audio + i * pb);
		b->ring[i].nextptr = (u32)(b->ring_dma +
					   ((i + 1) % np) * sizeof(struct beep_hwdesc));
		memset(b->ring[i].vuca, 0, sizeof(b->ring[i].vuca));
	}
}

static irqreturn_t beep_pcm_isr(int irq, void *dev_id)
{
	struct beep_i2s *b = dev_id;
	u32 status = mr(b, AR934X_DMA_REG_MBOX_INT_STATUS);
	bool handled = false;

	if (status & PB_INT_STATUS_B) {
		/* ack at the MBOX level (the miscintc irqchip acks the SoC line) */
		mw(b, AR934X_DMA_REG_MBOX_INT_STATUS, PB_INT_STATUS_B);

		/* The HW clears OWN on each finished descriptor. Re-own every
		 * completed one from hw_idx forward, advancing + elapsing a period
		 * for each (handles >1 period completing between IRQs). */
		if (b->ring && b->ss) {
			while (DESC_OWN(b->ring[b->hw_idx].ctrl) == 0) {
				b->ring[b->hw_idx].ctrl =
					DESC_CTRL(1, b->period_bytes, b->period_bytes);
				b->hw_idx = (b->hw_idx + 1) % b->nperiods;
				snd_pcm_period_elapsed(b->ss);
			}
		}
		handled = true;
	}
	return handled ? IRQ_HANDLED : IRQ_NONE;
}

static int beep_pcm_open(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	int ret;

	snd_soc_set_runtime_hwparams(ss, &beep_pcm_hw);
	/* Our ring is N whole-period descriptors, so the ALSA buffer MUST be an
	 * integer number of periods — otherwise the leftover tail bytes are never
	 * covered by a descriptor and get skipped each loop → a periodic pop.
	 * (aplay picked buffer=22050 / period=1023 = 21.55 periods, hence the pops.) */
	ret = snd_pcm_hw_constraint_integer(ss->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	return ret < 0 ? ret : 0;
}

static int beep_pcm_hw_params(struct snd_soc_component *c,
			      struct snd_pcm_substream *ss,
			      struct snd_pcm_hw_params *p)
{
	struct beep_i2s *b = snd_soc_component_get_drvdata(c);
	struct snd_pcm_runtime *rt = ss->runtime;
	u32 pb = params_period_bytes(p);
	u32 bb = params_buffer_bytes(p);
	u32 np = bb / pb;

	if (np < 2 || np > BEEP_PERIODS_MAX || pb > BEEP_PERIOD_BYTES_MAX)
		return -EINVAL;

	/* free a prior ring if hw_params is re-entered without hw_free */
	if (b->ring) {
		dma_free_coherent(b->dev, b->nperiods * sizeof(struct beep_hwdesc),
				  b->ring, b->ring_dma);
		b->ring = NULL;
	}

	/* the managed buffer (SNDRV_DMA_TYPE_DEV) is already allocated: rt->dma_addr */
	b->ring = dma_alloc_coherent(b->dev, np * sizeof(struct beep_hwdesc),
				     &b->ring_dma, GFP_KERNEL);
	if (!b->ring)
		return -ENOMEM;

	b->period_bytes = pb;
	b->nperiods = np;
	b->ss = ss;
	beep_ring_build(b, rt->dma_addr, pb, np);
	return 0;
}

static int beep_pcm_hw_free(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	struct beep_i2s *b = snd_soc_component_get_drvdata(c);

	/* stop the engine, mask its IRQ, and let any in-flight ISR finish before
	 * the ring is freed (ISR touches b->ring / b->ss) */
	mw(b, PB_CONTROL, AR934X_DMA_MBOX_DMA_CONTROL_STOP);
	mdelay(20);   /* let the engine finish its current descriptor before we free the ring */
	mw(b, AR934X_DMA_REG_MBOX_INT_ENABLE,
	   mr(b, AR934X_DMA_REG_MBOX_INT_ENABLE) & ~PB_INT_ENABLE_B);
	synchronize_irq(b->irq);
	if (b->ring) {
		dma_free_coherent(b->dev, b->nperiods * sizeof(struct beep_hwdesc),
				  b->ring, b->ring_dma);
		b->ring = NULL;
	}
	b->ss = NULL;
	return 0;
}

static int beep_pcm_prepare(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	struct beep_i2s *b = snd_soc_component_get_drvdata(c);
	unsigned int i;
	u32 t;

	/* hard-reset the engine first so no stale state survives from a prior stream */
	beep_mbox_reset(b);

	/* FIFO reset (also resets the stereo block per the datasheet) */
	mw(b, AR934X_DMA_REG_MBOX_FIFO_RESET, AR934X_DMA_MBOX_FIFO_RESET_ALL);
	udelay(50);
	beep_stereo_reset(b);

	/* DMA policy: request the channel + TX FIFO threshold = 6 */
	t = mr(b, AR934X_DMA_REG_MBOX_DMA_POLICY);
	t |= PB_POLICY_QUANTUM | (6 << AR934X_DMA_MBOX_DMA_POLICY_TX_FIFO_THRESH_SHIFT);
	mw(b, AR934X_DMA_REG_MBOX_DMA_POLICY, t);

	/* (re)arm the whole ring for the engine and point it at descriptor 0 */
	for (i = 0; i < b->nperiods; i++)
		b->ring[i].ctrl = DESC_CTRL(1, b->period_bytes, b->period_bytes);
	b->hw_idx = 0;
	mw(b, PB_DESC_BASE, (u32)b->ring_dma);

	/* enable the per-descriptor completion interrupt */
	mw(b, AR934X_DMA_REG_MBOX_INT_ENABLE,
	   mr(b, AR934X_DMA_REG_MBOX_INT_ENABLE) | PB_INT_ENABLE_B);
	return 0;
}

static int beep_pcm_trigger(struct snd_soc_component *c,
			    struct snd_pcm_substream *ss, int cmd)
{
	struct beep_i2s *b = snd_soc_component_get_drvdata(c);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		mw(b, PB_CONTROL, AR934X_DMA_MBOX_DMA_CONTROL_START);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		mw(b, PB_CONTROL, AR934X_DMA_MBOX_DMA_CONTROL_STOP);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static snd_pcm_uframes_t beep_pcm_pointer(struct snd_soc_component *c,
					  struct snd_pcm_substream *ss)
{
	struct beep_i2s *b = snd_soc_component_get_drvdata(c);

	return bytes_to_frames(ss->runtime, b->hw_idx * b->period_bytes);
}

static int beep_pcm_construct(struct snd_soc_component *c,
			      struct snd_soc_pcm_runtime *rtd)
{
	/* coherent (uncached) DMA buffer the MBOX engine reads directly */
	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV, c->dev,
				       BEEP_BUFFER_BYTES_MAX, BEEP_BUFFER_BYTES_MAX);
	return 0;
}

static const struct snd_soc_component_driver beep_i2s_component = {
	.name          = DRV_NAME,
	.open          = beep_pcm_open,
	.hw_params     = beep_pcm_hw_params,
	.hw_free       = beep_pcm_hw_free,
	.prepare       = beep_pcm_prepare,
	.trigger       = beep_pcm_trigger,
	.pointer       = beep_pcm_pointer,
	.pcm_construct = beep_pcm_construct,
};

static int beep_i2s_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct beep_i2s *b;
	int ret;

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	b->stereo = devm_platform_ioremap_resource_byname(pdev, "stereo");
	if (IS_ERR(b->stereo))
		return PTR_ERR(b->stereo);
	b->mbox = devm_platform_ioremap_resource_byname(pdev, "mbox");
	if (IS_ERR(b->mbox))
		return PTR_ERR(b->mbox);
	b->reset = devm_ioremap(dev, AR9331_RESET_MODULE_PHYS, 4);
	if (!b->reset)
		return -ENOMEM;

	b->irq = platform_get_irq(pdev, 0);
	if (b->irq < 0)
		return b->irq;
	ret = devm_request_irq(dev, b->irq, beep_pcm_isr, 0, DRV_NAME, b);
	if (ret)
		return ret;

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ret = beep_pinmux(dev);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, b);
	beep_stereo_reset(b);

	dev_info(dev, "AR9331 I2S ready (stereo=%pR mbox=%pR irq=%d); MBOX-DMA playback %s\n",
		 &pdev->resource[0], &pdev->resource[1], b->irq,
		 BEEP_PLAYBACK_TX ? "on TX chan" : "on RX chan");
	return devm_snd_soc_register_component(dev, &beep_i2s_component, &beep_i2s_dai, 1);
}

static const struct of_device_id beep_i2s_of_match[] = {
	{ .compatible = "qca,ar9331-i2s" },
	{ }
};
MODULE_DEVICE_TABLE(of, beep_i2s_of_match);

static struct platform_driver beep_i2s_driver = {
	.probe = beep_i2s_probe,
	.driver = { .name = DRV_NAME, .of_match_table = beep_i2s_of_match },
};
module_platform_driver(beep_i2s_driver);

MODULE_DESCRIPTION("Beep AR9331 I2S CPU DAI (control plane)");
MODULE_AUTHOR("Beep revival project; ported from franzflasch/ar9331-i2s-alsa");
MODULE_LICENSE("Dual BSD/GPL");
