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
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/pcm_params.h>

#include "ath79-stereo-regs-dt.h"

#define DRV_NAME "beep-i2s"

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
};

/* divint / divfrac for CLK_DIV (stereo+0x1c) = (divint<<16)|divfrac; posedge=2 */
static const struct { u32 rate, divint, divfrac; } clk_cfg[] = {
	{ 44100, 0x11, 0xB726 },
	{ 48000, 0x10, 0x46AB },
};

static inline u32 sr(struct beep_i2s *b, u32 o)        { return readl(b->stereo + o); }
static inline void sw(struct beep_i2s *b, u32 o, u32 v){ writel(v, b->stereo + o); (void)readl(b->stereo + o); }

static void beep_stereo_reset(struct beep_i2s *b)
{
	sw(b, AR934X_STEREO_REG_CONFIG, sr(b, AR934X_STEREO_REG_CONFIG) | AR934X_STEREO_CONFIG_RESET);
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

static const struct snd_soc_component_driver beep_i2s_component = {
	.name = DRV_NAME,
	/* BENCH/PORTING: attach the MBOX PCM ops here (pcm_construct/open/trigger/
	 * pointer/copy) once ath79-pcm.c is ported — see driver-i2s/PORTING.md. */
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

	ret = beep_pinmux(dev);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, b);
	beep_stereo_reset(b);

	dev_info(dev, "AR9331 I2S DAI ready (stereo=%pR mbox=%pR); PCM/DMA port pending\n",
		 &pdev->resource[0], &pdev->resource[1]);
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
