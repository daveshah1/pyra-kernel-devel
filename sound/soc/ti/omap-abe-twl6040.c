// SPDX-License-Identifier: GPL-2.0-only

/*
 * this is a mix of upstream omap-abe-twl6040.c from 4.18ff with TI code
 * done by H. N. Schaller during porting to 4.18 API for OMAP5 based Pyra-Handheld
 *
 * questionable code areas:
 *
 * are omap_abe_hw_params() and omap_abe_mcpdm_hw_params() the same? then keep upstream omap_abe_hw_params() only
 * what should we do with OMAP4_SDP and tps6130x stuff?
 * the extension to handle more DMICs seems not to be upstream!
 * support for spdif-dit unclear
 * what is this "legacy" stuff? Is this still needed for anything?
 * error path in probe() and to err_unregister isn't sane
 * here, we have better error messages than upstream
 * dmic_codec_dev is duplicated
 * there is a problem with "mcbsp-ame" dai_link which is defined nowhere else
 * no support for McBSP3?
 * how does this twl6040_dapm_widgets[] relate to the one defined in sound/soc/codecs/twl6040.c? If at all...
 *
 * this comment should be removed if all questions have been answered and addressed
 */

/*
 * omap-abe-twl6040.c  --  SoC audio for TI OMAP based boards with ABE and
 *			   twl6040 codec
 *
 * Authors: Misael Lopez Cruz <misael.lopez@ti.com>
 *          Peter Ujfalusi <peter.ujfalusi@ti.com>
 *          Liam Girdwood <lrg@ti.com>
 *          Sebastien Guiriec <s-guiriec@ti.com>
 *
 * Contact: Misael Lopez Cruz <misael.lopez@ti.com>
 *          Peter Ujfalusi <peter.ujfalusi@ti.com>
 *
 */

#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/mfd/twl6040.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/of.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include <sound/soc-dpcm.h>
#include <sound/dmaengine_pcm.h>

#include "omap-dmic.h"
#include "omap-mcpdm.h"
#include "aess/omap-aess.h"
#include "omap-mcbsp-priv.h"
#include "omap-mcbsp.h"
#include "omap-dmic.h"
#include "../codecs/twl6040.h"

SND_SOC_DAILINK_DEFS(link0,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC("twl6040-codec",
				      "twl6040-legacy")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(link1,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC("dmic-codec",
				      "dmic-hifi")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

#define AESS_FW_NAME	"omap_aess-adfw.bin"

struct abe_twl6040 {
	int	jack_detection;	/* board can detect jack events */
	int	mclk_freq;	/* MCLK frequency speed for twl6040 */
	int	has_abe;
	int	has_dmic;
	int	twl6040_power_mode;
	int 	mcbsp_cfg;
	struct omap_aess *aess;
#ifdef CONFIG_OMAP4_SDP
	struct i2c_client *tps6130x;
	struct i2c_adapter *adapter;
#endif
	struct platform_device *dmic_codec_dev;
	struct platform_device *spdif_codec_dev;
};

static struct platform_device *dmic_codec_dev;

static int omap_abe_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_card *card = rtd->card;
	struct abe_twl6040 *priv = snd_soc_card_get_drvdata(card);
	int clk_id, freq;
	int ret;

#ifdef CHECKME // do we need these changes?? They seem to modify cpu_dai and not codec_dai

	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	unsigned int be_id, channels;

	be_id = rtd->dai_link->id;

	/* FM + MODEM + Bluetooth all use I2S config */
	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_I2S |
					   SND_SOC_DAIFMT_NB_IF |
					   SND_SOC_DAIFMT_CBM_CFM);
	if (ret < 0) {
		dev_err(card->dev, "can't set DAI %d configuration\n", be_id);
		return ret;
	}

	if (params != NULL) {
		struct omap_mcbsp *mcbsp = snd_soc_dai_get_drvdata(cpu_dai);
		/* Configure McBSP internal buffer usage */
		/* this need to be done for playback and/or record */
		channels = params_channels(params);
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			omap_mcbsp_set_tx_threshold(mcbsp, channels);
		else
			omap_mcbsp_set_rx_threshold(mcbsp, channels);
	}

	clk_id = OMAP_MCBSP_SYSCLK_CLKS_FCLK;
	freq = 64 * params_rate(params);

#else	// upstream code

	clk_id = twl6040_get_clk_id(codec_dai->component);
	if (clk_id == TWL6040_SYSCLK_SEL_HPPLL)
		freq = priv->mclk_freq;
	else if (clk_id == TWL6040_SYSCLK_SEL_LPPLL)
		freq = 32768;
	else
		return -EINVAL;


#endif

	/* set the codec mclk */
	ret = snd_soc_dai_set_sysclk(codec_dai, clk_id, freq,
				SND_SOC_CLOCK_IN);
	if (ret < 0)
		dev_err(card->dev, "can't set codec system clock\n");

	return ret;
}

static const struct snd_soc_ops omap_abe_ops = {
	.hw_params = omap_abe_hw_params,
};

#ifdef CONFIG_OMAP4_SDP

static struct i2c_board_info tps6130x_hwmon_info = {
	I2C_BOARD_INFO("tps6130x", 0x33),
};

/* configure the TPS6130x Handsfree Boost Converter */
static int omap_abe_tps6130x_configure(struct abe_twl6040 *sdp4403)
{
	struct i2c_client *tps6130x = sdp4403->tps6130x;
	u8 data[2];

	data[0] = 0x01;
	data[1] = 0x60;
	if (i2c_master_send(tps6130x, data, 2) != 2)
		dev_err(&tps6130x->dev, "I2C write to TPS6130x failed\n");

	data[0] = 0x02;
	if (i2c_master_send(tps6130x, data, 2) != 2)
		dev_err(&tps6130x->dev, "I2C write to TPS6130x failed\n");
	return 0;
}
#endif

static int mcpdm_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				    struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);

	rate->min = rate->max = 96000;

	return 0;
}

/* FIXME: what is the difference to upstream omap_abe_hw_params? */

static int omap_abe_mcpdm_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct abe_twl6040 *priv = snd_soc_card_get_drvdata(card);
	int clk_id, freq;
	int ret;

	clk_id = twl6040_get_clk_id(codec_dai->component);
	if (clk_id == TWL6040_SYSCLK_SEL_HPPLL)
		freq = priv->mclk_freq;
	else if (clk_id == TWL6040_SYSCLK_SEL_LPPLL) {
		freq = 32768;
	} else {
		dev_err(card->dev, "invalid clock\n");
		return -EINVAL;
	}

	/* set the codec mclk */
	ret = snd_soc_dai_set_sysclk(codec_dai, clk_id, freq,
				     SND_SOC_CLOCK_IN);
	if (ret)
		dev_err(card->dev, "can't set codec system clock\n");

	return ret;
}

static struct snd_soc_ops omap_abe_mcpdm_ops = {
	.hw_params = omap_abe_mcpdm_hw_params,
};

static int omap_abe_dmic_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;

	ret = snd_soc_dai_set_sysclk(cpu_dai, OMAP_DMIC_SYSCLK_PAD_CLKS,
				     19200000, SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(rtd->card->dev, "can't set DMIC in system clock\n");
		return ret;
	}
	ret = snd_soc_dai_set_sysclk(cpu_dai, OMAP_DMIC_ABE_DMIC_CLK, 2400000,
				     SND_SOC_CLOCK_OUT);
	if (ret < 0)
		dev_err(rtd->card->dev, "can't set DMIC output clock\n");

	return ret;
}

static struct snd_soc_ops omap_abe_dmic_ops = {
	.hw_params = omap_abe_dmic_hw_params,
};

static int mcbsp_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				    struct snd_pcm_hw_params *params)
{
	struct snd_interval *channels = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_CHANNELS);
	unsigned int be_id = rtd->dai_link->id;

	if (be_id == OMAP_AESS_BE_ID_MM_FM || be_id == OMAP_AESS_BE_ID_BT_VX)
		channels->min = 2;

	snd_mask_set(&params->masks[SNDRV_PCM_HW_PARAM_FORMAT -
				    SNDRV_PCM_HW_PARAM_FIRST_MASK],
				    SNDRV_PCM_FORMAT_S16_LE);
	return 0;
}

static int dmic_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);

	/* The ABE will covert the FE rate to 96k */
	rate->min = rate->max = 96000;

	snd_mask_set(&params->masks[SNDRV_PCM_HW_PARAM_FORMAT -
				    SNDRV_PCM_HW_PARAM_FIRST_MASK],
				    SNDRV_PCM_FORMAT_S32_LE);
	return 0;
}
/* Headset jack */
static struct snd_soc_jack hs_jack;

/*Headset jack detection DAPM pins */
static struct snd_soc_jack_pin hs_jack_pins[] = {
	{
		.pin = "Headset Mic",
		.mask = SND_JACK_MICROPHONE,
	},
	{
		.pin = "Headset Stereophone",
		.mask = SND_JACK_HEADPHONE,
	},
};

static int omap_abe_get_power_mode(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct abe_twl6040 *priv = snd_soc_card_get_drvdata(card);

	ucontrol->value.integer.value[0] = priv->twl6040_power_mode;
	return 0;
}

static int omap_abe_set_power_mode(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct abe_twl6040 * priv = snd_soc_card_get_drvdata(card);

	if (priv->twl6040_power_mode == ucontrol->value.integer.value[0])
		return 0;

	priv->twl6040_power_mode = ucontrol->value.integer.value[0];
	omap_aess_pm_set_mode(priv->aess, priv->twl6040_power_mode);

	return 1;
}

static const char *power_texts[] = {"Low-Power", "High-Performance"};

static const struct soc_enum omap_abe_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, power_texts),
};

static const struct snd_kcontrol_new omap_abe_controls[] = {
	SOC_ENUM_EXT("TWL6040 Power Mode", omap_abe_enum[0],
		     omap_abe_get_power_mode, omap_abe_set_power_mode),
};

/* OMAP ABE TWL6040 machine DAPM */
static const struct snd_soc_dapm_widget twl6040_dapm_widgets[] = {
	/* Outputs */
	SND_SOC_DAPM_HP("Headset Stereophone", NULL),
	SND_SOC_DAPM_SPK("Earphone Spk", NULL),
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
	SND_SOC_DAPM_LINE("Line Out", NULL),
	SND_SOC_DAPM_SPK("Vibrator", NULL),

	/* Inputs */
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Main Handset Mic", NULL),
	SND_SOC_DAPM_MIC("Sub Handset Mic", NULL),
	SND_SOC_DAPM_LINE("Line In", NULL),

	/* Digital microphones */
	SND_SOC_DAPM_MIC("Digital Mic", NULL),
};

static const struct snd_soc_dapm_route audio_map[] = {
	/* Routings for outputs */
	{"Headset Stereophone", NULL, "HSOL"},
	{"Headset Stereophone", NULL, "HSOR"},

	{"Earphone Spk", NULL, "EP"},

	{"Ext Spk", NULL, "HFL"},
	{"Ext Spk", NULL, "HFR"},

	{"Line Out", NULL, "AUXL"},
	{"Line Out", NULL, "AUXR"},

	{"Vibrator", NULL, "VIBRAL"},
	{"Vibrator", NULL, "VIBRAR"},

	/* Routings for inputs */
	{"HSMIC", NULL, "Headset Mic Bias"},
	{"Headset Mic Bias", NULL, "Headset Mic"},

	{"MAINMIC", NULL, "Main Mic Bias"},
	{"Main Mic Bias", NULL, "Main Handset Mic"},

	{"SUBMIC", NULL, "Main Mic Bias"},
	{"Main Mic Bias", NULL, "Sub Handset Mic"},

	{"AFML", NULL, "Line In"},
	{"AFMR", NULL, "Line In"},

	/* Connections between twl6040 and ABE */
	{"Headset Playback", NULL, "PDM_DL1"},
	{"Handsfree Playback", NULL, "PDM_DL2"},
	{"PDM_UL1", NULL, "Analog Capture"},

	/* Bluetooth <--> ABE*/
	{"omap-mcbsp.1 Playback", NULL, "BT_VX_DL"},
	{"BT_VX_UL", NULL, "omap-mcbsp.1 Capture"},

	/* FM <--> ABE */
	{"omap-mcbsp.2 Playback", NULL, "MM_EXT_DL"},
	{"MM_EXT_UL", NULL, "omap-mcbsp.2 Capture"},
};

static int omap_abe_stream_event(struct snd_soc_dapm_context *dapm, int event)
{
	struct snd_soc_card *card = dapm->card;
	struct snd_soc_component *component = dapm->component;
	struct abe_twl6040 * priv = snd_soc_card_get_drvdata(card);

	int gain;
#if 0
[    7.406936] [<c06a4040>] (dapm_find_widget) from [<c06a40b8>] (snd_soc_dapm_get_pin_status+0xc/0x1c)
[    7.406967] [<c06a40b8>] (snd_soc_dapm_get_pin_status) from [<bf295170>] (twl6040_get_dl1_gain+0x18/0x7c [snd_soc_twl6040])
[    7.407031] [<bf295170>] (twl6040_get_dl1_gain [snd_soc_twl6040]) from [<bf2b94c8>] (omap_abe_stream_event+0x14/0x30 [snd_soc_omap_abe_twl6040])
[    7.407070] [<bf2b94c8>] (omap_abe_stream_event [snd_soc_omap_abe_twl6040]) from [<c06a70bc>] (dapm_power_widgets+0x790/0x96c)
[    7.407095] [<c06a70bc>] (dapm_power_widgets) from [<c06a8fc0>] (snd_soc_dapm_new_widgets+0x418/0x47c)
[    7.407117] [<c06a8fc0>] (snd_soc_dapm_new_widgets) from [<c06a34d0>] (snd_soc_register_card+0xadc/0xbf4)
[    7.407144] [<c06a34d0>] (snd_soc_register_card) from [<bf2b9aec>] (omap_abe_probe+0x4ac/0x59c [snd_soc_omap_abe_twl6040])

[    7.407175] [<bf2b9aec>] (omap_abe_probe [snd_soc_omap_abe_twl6040]) from [<c04feb40>] (platform_drv_probe+0x48/0x98)

	/*
	 * set DL1 gains dynamically according to the active output
	 * (Headset, Earpiece) and HSDAC power mode
	 */

	gain = twl6040_get_dl1_gain(component) * 100;

	omap_aess_set_dl1_gains(priv->aess, gain, gain);
#endif

	return 0;
}

static int omap_abe_twl6040_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component = rtd->codec_dai->component;
	struct snd_soc_card *card = rtd->card;
	struct abe_twl6040 *priv = snd_soc_card_get_drvdata(card);
	int hs_trim;
	u32 hsotrim, left_offset, right_offset, step_mV;
	int ret = 0;

	/*
	 * Configure McPDM offset cancellation based on the HSOTRIM value from
	 * twl6040.
	 */
	hs_trim = twl6040_get_trim_value(component, TWL6040_TRIM_HSOTRIM);
	omap_mcpdm_configure_dn_offsets(rtd, TWL6040_HSF_TRIM_LEFT(hs_trim),
					TWL6040_HSF_TRIM_RIGHT(hs_trim));

//FIXME:	card->dapm.stream_event = omap_abe_stream_event;

	/* allow audio paths from the audio modem to run during suspend */
	snd_soc_dapm_ignore_suspend(&card->dapm, "Ext Spk");
	snd_soc_dapm_ignore_suspend(&card->dapm, "AFML");
	snd_soc_dapm_ignore_suspend(&card->dapm, "AFMR");
	snd_soc_dapm_ignore_suspend(&card->dapm, "Headset Mic");
	snd_soc_dapm_ignore_suspend(&card->dapm, "Headset Stereophone");

	/* DC offset cancellation computation only if ABE is enabled */
	if (priv->has_abe) {
		hsotrim = twl6040_get_trim_value(component, TWL6040_TRIM_HSOTRIM);
		right_offset = TWL6040_HSF_TRIM_RIGHT(hsotrim);
		left_offset = TWL6040_HSF_TRIM_LEFT(hsotrim);

		step_mV = twl6040_get_hs_step_size(component);
		omap_aess_dc_set_hs_offset(priv->aess, left_offset,
					   right_offset, step_mV);

		/* ABE power control */
		ret = snd_soc_add_card_controls(card, omap_abe_controls,
						ARRAY_SIZE(omap_abe_controls));
		if (ret)
			return ret;
	}

	/* Headset jack detection only if it is supported */
	if (priv->jack_detection) {
		ret = snd_soc_card_jack_new(rtd->card, "Headset Jack",
					    SND_JACK_HEADSET, &hs_jack,
					    hs_jack_pins,
					    ARRAY_SIZE(hs_jack_pins));
		if (ret)
			return ret;

		twl6040_hs_jack_detect(component, &hs_jack, SND_JACK_HEADSET);
	}

#ifdef CONFIG_OMAP4_SDP
	if (of_machine_is_compatible("ti,omap4-sdp")) {
		priv->adapter = i2c_get_adapter(1);
		if (! priv->adapter) {
			dev_err(card->dev, "can't get i2c adapter\n");
			return -ENODEV;
		}

		priv->tps6130x = i2c_new_device(priv->adapter,
						     &tps6130x_hwmon_info);
		if (! priv->tps6130x) {
			dev_err(card->dev, "can't add i2c device\n");
			i2c_put_adapter(priv->adapter);
			return -ENODEV;
		}

		omap_abe_tps6130x_configure(priv);
	}
#endif
	return ret;
}

static const struct snd_soc_dapm_widget dmic_dapm_widgets[] = {
	SND_SOC_DAPM_MIC("Digital Mic 0", NULL),
	SND_SOC_DAPM_MIC("Digital Mic 1", NULL),
	SND_SOC_DAPM_MIC("Digital Mic 2", NULL),
};

static const struct snd_soc_dapm_route dmic_audio_map[] = {
	/* Digital Mics: DMic0, DMic1, DMic2 with bias */
	{"DMIC0", NULL, "omap-dmic-abe Capture"},
	{"omap-dmic-abe Capture", NULL, "Digital Mic1 Bias"},
	{"Digital Mic1 Bias", NULL, "Digital Mic 0"},

	{"DMIC1", NULL, "omap-dmic-abe Capture"},
	{"omap-dmic-abe Capture", NULL, "Digital Mic1 Bias"},
	{"Digital Mic1 Bias", NULL, "Digital Mic 1"},

	{"DMIC2", NULL, "omap-dmic-abe Capture"},
	{"omap-dmic-abe Capture", NULL, "Digital Mic1 Bias"},
	{"Digital Mic1 Bias", NULL, "Digital Mic 2"},
};

static int omap_abe_dmic_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dapm_context *dapm = &rtd->card->dapm;
	struct snd_soc_card *card = rtd->card;
	struct abe_twl6040 * priv = snd_soc_card_get_drvdata(card);
	int ret;

	if (! priv->has_abe)
		return 0;

	ret = snd_soc_dapm_new_controls(dapm, dmic_dapm_widgets,
					ARRAY_SIZE(dmic_dapm_widgets));
	if (ret)
		return ret;

	ret = snd_soc_dapm_add_routes(dapm, dmic_audio_map,
				      ARRAY_SIZE(dmic_audio_map));
	if (ret < 0)
		return ret;

	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic 0");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic 1");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic 2");
	return 0;
}

static int omap_abe_twl6040_dl2_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_component *component = codec_dai->component;
	struct abe_twl6040 * priv = snd_soc_card_get_drvdata(card);
	u32 hfotrim, left_offset, right_offset;

	/* DC offset cancellation computation only if ABE is enabled */
	if (priv->has_abe) {
		/* DC offset cancellation computation */
		hfotrim = twl6040_get_trim_value(component, TWL6040_TRIM_HFOTRIM);
		right_offset = TWL6040_HSF_TRIM_RIGHT(hfotrim);
		left_offset = TWL6040_HSF_TRIM_LEFT(hfotrim);

		omap_aess_dc_set_hf_offset(priv->aess, left_offset,
					   right_offset);
	}

	return 0;
}

static int omap_abe_twl6040_fe_init(struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}

/* TODO Expand these custom macros */
#define SND_SOC_DAI_CONNECT(xname, xcodec, xplatform, xcodec_dai, xcpu_dai) \
.name = xname, .codec_name = xcodec, \
.platform_name = xplatform, .codec_dai_name = xcodec_dai,\
.cpu_dai_name = xcpu_dai
#define SND_SOC_DAI_OPS(xops, xinit) \
.ops = xops, .init = xinit
#define SND_SOC_DAI_IGNORE_SUSPEND .ignore_suspend = 1
#define SND_SOC_DAI_IGNORE_PMDOWN .ignore_pmdown_time = 1
#define SND_SOC_DAI_BE_LINK(xid, xfixup) \
.id = xid, .be_hw_params_fixup = xfixup, .no_pcm = 1
#define SND_SOC_DAI_FE_LINK(xname, xplatform, xcpu_dai) \
.name = xname, .platform_name = xplatform, \
.cpu_dai_name = xcpu_dai, .dynamic = 1, \
.codec_name = "snd-soc-dummy", .codec_dai_name = "snd-soc-dummy-dai"
#define SND_SOC_DAI_FE_TRIGGER(xplay, xcapture) \
.trigger = {xplay, xcapture}
//#define SND_SOC_DAI_LINK_NO_HOST	.no_host_mode = 1

/*
FIXME: must store infos in:

struct snd_soc_dai_link_component {
	const char *name;
	struct device_node *of_node;
	const char *dai_name;
};

and add to "modern style" indirect pointers
	struct snd_soc_dai_link_component *cpus;
	unsigned int num_cpus;
	struct snd_soc_dai_link_component *codecs;
	unsigned int num_codecs;
	struct snd_soc_dai_link_component *platforms;
	unsigned int num_platforms;

At the moment I have no better idea than redefining the macros - which breaks initialization
*/

#define SND_SOC_DAI_CONNECT(xname, xcodec, xplatform, xcodec_dai, xcpu_dai) \
.name = xname
#define SND_SOC_DAI_FE_LINK(xname, xplatform, xcpu_dai) \
.name = xname

/* Digital audio interface glue - connects codec <--> CPU */
static struct snd_soc_dai_link legacy_dmic_dai = {
	/* Legacy DMIC */
	SND_SOC_DAI_CONNECT("Legacy DMIC", "dmic-codec", "omap-pcm-audio",
			    "dmic-hifi", "omap-dmic"),
	SND_SOC_DAI_OPS(&omap_abe_dmic_ops, omap_abe_dmic_init),
};

static struct snd_soc_dai_link legacy_mcpdm_dai = {
	/* Legacy McPDM */
	SND_SOC_DAI_CONNECT("Legacy McPDM", "twl6040-codec", "omap-pcm-audio",
			    "twl6040-legacy", "mcpdm-legacy"),
	SND_SOC_DAI_OPS(&omap_abe_mcpdm_ops, NULL),
};

static struct snd_soc_dai_link legacy_mcbsp_dai = {
	/* Legacy McBSP */
	SND_SOC_DAI_CONNECT("Legacy McBSP", "snd-soc-dummy", "omap-pcm-audio",
			    "snd-soc-dummy-dai", "omap-mcbsp.2"),
	SND_SOC_DAI_OPS(&omap_abe_ops, NULL),
	SND_SOC_DAI_IGNORE_SUSPEND,
};

static struct snd_soc_dai_link legacy_mcasp_dai = {
	/* Legacy SPDIF */
	SND_SOC_DAI_CONNECT("Legacy SPDIF", "spdif-dit", "omap-pcm-audio",
			    "dit-hifi", "omap-mcasp"),
	SND_SOC_DAI_IGNORE_SUSPEND,
};

static struct snd_soc_dai_link abe_fe_dai[] = {

/*
 * Frontend DAIs - i.e. userspace visible interfaces (ALSA PCMs)
 */
{
	/* ABE Media Playback/Capture */
	SND_SOC_DAI_FE_LINK("OMAP ABE Media1", "omap-pcm-audio", "MultiMedia1"),
	SND_SOC_DAI_FE_TRIGGER(SND_SOC_DPCM_TRIGGER_BESPOKE,
			       SND_SOC_DPCM_TRIGGER_BESPOKE),
	SND_SOC_DAI_OPS(NULL, omap_abe_twl6040_fe_init),
	SND_SOC_DAI_IGNORE_PMDOWN,
	.dpcm_playback = 1,
	.dpcm_capture = 1,
},
{
	/* ABE Media Capture */
	SND_SOC_DAI_FE_LINK("OMAP ABE Media2", "omap-pcm-audio", "MultiMedia2"),
	SND_SOC_DAI_FE_TRIGGER(SND_SOC_DPCM_TRIGGER_BESPOKE,
			       SND_SOC_DPCM_TRIGGER_BESPOKE),
	.dpcm_capture = 1,
},
{
	/* ABE Voice */
	SND_SOC_DAI_FE_LINK("OMAP ABE Voice", "omap-pcm-audio", "Voice"),
	SND_SOC_DAI_FE_TRIGGER(SND_SOC_DPCM_TRIGGER_BESPOKE,
			       SND_SOC_DPCM_TRIGGER_BESPOKE),
	.dpcm_playback = 1,
	.dpcm_capture = 1,
},
{
	/* ABE Tones */
	SND_SOC_DAI_FE_LINK("OMAP ABE Tones", "omap-pcm-audio", "Tones"),
	SND_SOC_DAI_FE_TRIGGER(SND_SOC_DPCM_TRIGGER_BESPOKE,
			       SND_SOC_DPCM_TRIGGER_BESPOKE),
	.dpcm_playback = 1,
},
{
	/* MODEM */
	SND_SOC_DAI_FE_LINK("OMAP ABE MODEM", "aess", "MODEM"),
	SND_SOC_DAI_FE_TRIGGER(SND_SOC_DPCM_TRIGGER_BESPOKE,
			       SND_SOC_DPCM_TRIGGER_BESPOKE),
	SND_SOC_DAI_OPS(NULL, omap_abe_twl6040_fe_init),
	SND_SOC_DAI_IGNORE_SUSPEND, SND_SOC_DAI_IGNORE_PMDOWN,
	// TODO port 34ad3dfcf615 ("ASoC: core: Add no host support.")?
	//SND_SOC_DAI_LINK_NO_HOST,
	.dpcm_playback = 1,
	.dpcm_capture = 1,
},
{
	/* Low power ping - pong */
	SND_SOC_DAI_FE_LINK("OMAP ABE Media LP", "aess", "MultiMedia1 LP"),
	SND_SOC_DAI_FE_TRIGGER(SND_SOC_DPCM_TRIGGER_BESPOKE,
			       SND_SOC_DPCM_TRIGGER_BESPOKE),
	.dpcm_playback = 1,
},
};

/*
 * Backend DAIs - i.e. dynamically matched interfaces, invisible to userspace.
 * Matched to above interfaces at runtime, based upon use case.
 */

static struct snd_soc_dai_link abe_be_mcpdm_dai[] = {
{
	/* McPDM DL1 - Headset */
	SND_SOC_DAI_CONNECT("McPDM-DL1", "twl6040-codec", "aess",
// FIXME: there is no cpu_dai_name="mcpdm-abe" so this will not match!
			    "twl6040-dl1", "mcpdm-abe"),
	SND_SOC_DAI_BE_LINK(OMAP_AESS_BE_ID_PDM_DL1, mcpdm_be_hw_params_fixup),
	SND_SOC_DAI_OPS(&omap_abe_mcpdm_ops, omap_abe_twl6040_init),
	SND_SOC_DAI_IGNORE_SUSPEND, SND_SOC_DAI_IGNORE_PMDOWN,
	.dpcm_playback = 1,
},
{
	/* McPDM UL1 - Analog Capture */
	SND_SOC_DAI_CONNECT("McPDM-UL1", "twl6040-codec", "aess",
			    "twl6040-ul", "mcpdm-abe"),
	SND_SOC_DAI_BE_LINK(OMAP_AESS_BE_ID_PDM_UL, mcpdm_be_hw_params_fixup),
	SND_SOC_DAI_OPS(&omap_abe_mcpdm_ops, NULL),
	SND_SOC_DAI_IGNORE_SUSPEND, SND_SOC_DAI_IGNORE_PMDOWN,
	.dpcm_capture = 1,
},
{
	/* McPDM DL2 - Handsfree */
	SND_SOC_DAI_CONNECT("McPDM-DL2", "twl6040-codec", "aess",
			    "twl6040-dl2", "mcpdm-abe"),
	SND_SOC_DAI_BE_LINK(OMAP_AESS_BE_ID_PDM_DL2, mcpdm_be_hw_params_fixup),
	SND_SOC_DAI_OPS(&omap_abe_mcpdm_ops, omap_abe_twl6040_dl2_init),
	SND_SOC_DAI_IGNORE_SUSPEND, SND_SOC_DAI_IGNORE_PMDOWN,
	.dpcm_playback = 1,
},
};

static struct snd_soc_dai_link abe_be_mcbsp1_dai = {
	/* McBSP 1 - Bluetooth */
	SND_SOC_DAI_CONNECT("McBSP-1", "snd-soc-dummy", "aess",
			    "snd-soc-dummy-dai", "omap-mcbsp.1"),
	SND_SOC_DAI_BE_LINK(OMAP_AESS_BE_ID_BT_VX, mcbsp_be_hw_params_fixup),
	SND_SOC_DAI_OPS(&omap_abe_ops, NULL),
	SND_SOC_DAI_IGNORE_SUSPEND, SND_SOC_DAI_IGNORE_PMDOWN,
	.dpcm_playback = 1,
	.dpcm_capture = 1,
};

static struct snd_soc_dai_link abe_be_mcbsp2_dai = {
	/* McBSP 2 - MODEM or FM */
	SND_SOC_DAI_CONNECT("McBSP-2", "snd-soc-dummy", "aess",
			    "snd-soc-dummy-dai", "omap-mcbsp.2"),
	SND_SOC_DAI_BE_LINK(OMAP_AESS_BE_ID_MM_FM, mcbsp_be_hw_params_fixup),
	SND_SOC_DAI_OPS(&omap_abe_ops, NULL),
	SND_SOC_DAI_IGNORE_SUSPEND, SND_SOC_DAI_IGNORE_PMDOWN,
	.dpcm_playback = 1,
	.dpcm_capture = 1,
};

// FIXME: we have a McBSP3 connected to MODEM and McBSP2 to (optional) FM
// why is this hard coded here and not defined by DT records?

static struct snd_soc_dai_link abe_be_dmic_dai[] = {
{
	/* DMIC0 */
	SND_SOC_DAI_CONNECT("DMIC-0", "dmic-codec", "aess",
			    "dmic-hifi", "omap-dmic-abe-dai"),
	SND_SOC_DAI_BE_LINK(OMAP_AESS_BE_ID_DMIC0, dmic_be_hw_params_fixup),
	SND_SOC_DAI_OPS(&omap_abe_dmic_ops, NULL),
	.dpcm_capture = 1,
},
{
	/* DMIC1 */
	SND_SOC_DAI_CONNECT("DMIC-1", "dmic-codec", "aess",
			    "dmic-hifi", "omap-dmic-abe-dai"),
	SND_SOC_DAI_BE_LINK(OMAP_AESS_BE_ID_DMIC1, dmic_be_hw_params_fixup),
	SND_SOC_DAI_OPS(&omap_abe_dmic_ops, NULL),
	.dpcm_capture = 1,
},
{
	/* DMIC2 */
	SND_SOC_DAI_CONNECT("DMIC-2", "dmic-codec", "aess",
			    "dmic-hifi", "omap-dmic-abe-dai"),
	SND_SOC_DAI_BE_LINK(OMAP_AESS_BE_ID_DMIC2, dmic_be_hw_params_fixup),
	SND_SOC_DAI_OPS(&omap_abe_dmic_ops, NULL),
	.dpcm_capture = 1,
},
};

/* copied from "ASoC: core: Add convenience function to add DAI links" by Liam Girdwood <lrg@ti.com> */
static int snd_soc_card_new_dai_links(struct snd_soc_card *card,
	struct snd_soc_dai_link *new, int count)
{
	struct snd_soc_dai_link *links;
	size_t bytes;

	bytes = (count + card->num_links) * sizeof(struct snd_soc_dai_link);
	links = devm_kzalloc(card->dev, bytes, GFP_KERNEL);
	if (!links)
		return -ENOMEM;

	if (card->dai_link) {
		memcpy(links, card->dai_link,
			card->num_links * sizeof(struct snd_soc_dai_link));
		devm_kfree(card->dev, card->dai_link);
	}
	memcpy(links + card->num_links, new,
		count * sizeof(struct snd_soc_dai_link));
	card->dai_link = links;
	card->num_links += count;

	return 0;
}

static void snd_soc_card_reset_dai_links(struct snd_soc_card *card)
{
	card->dai_link = NULL;
	card->num_links = 0;
}

/* TODO: Peter - this will need some logic for DTS DAI link creation */
static int omap_abe_add_dai_links(struct snd_soc_card *card)
{
	struct abe_twl6040 * priv = snd_soc_card_get_drvdata(card);
	struct device_node *node = card->dev->of_node;
	struct device_node *dai_node, *aess_node;
	int ret, i;


	aess_node = of_parse_phandle(node, "ti,aess", 0);
	if (!aess_node) {
		dev_err(card->dev, "AESS node is missing\n");
		return -EINVAL;
	}

#if 0 // BROKEN
	for (i = 4; i < ARRAY_SIZE(abe_fe_dai); i++) {
		abe_fe_dai[i].platform_name  = NULL;
		abe_fe_dai[i].platform_of_node = aess_node;
	}
#endif

	dai_node = of_parse_phandle(node, "ti,mcpdm", 0);
	if (!dai_node) {
		dev_err(card->dev, "McPDM node is missing\n");
		return -EINVAL;
	}

#if 0 // BROKEN
	for (i = 0; i < ARRAY_SIZE(abe_be_mcpdm_dai); i++) {
		abe_be_mcpdm_dai[i].platform_name  = NULL;
		abe_be_mcpdm_dai[i].platform_of_node = aess_node;
	}

	for (i = 0; i < ARRAY_SIZE(abe_be_dmic_dai); i++) {
		abe_be_dmic_dai[i].platform_name  = NULL;
		abe_be_dmic_dai[i].platform_of_node = aess_node;
	}
#endif

	dai_node = of_parse_phandle(node, "ti,mcbsp1", 0);
	if (!dai_node) {
		dev_err(card->dev,"McBSP1 node is missing\n");
		return -EINVAL;
	}

#if 0 // BROKEN
	abe_be_mcbsp1_dai.cpu_dai_name  = NULL;
	abe_be_mcbsp1_dai.cpu_of_node = dai_node;
	abe_be_mcbsp1_dai.platform_name  = NULL;
	abe_be_mcbsp1_dai.platform_of_node = aess_node;
#endif

	dai_node = of_parse_phandle(node, "ti,mcbsp2", 0);
	if (!dai_node) {
		dev_err(card->dev,"McBSP2 node is missing\n");
		return -EINVAL;
	}

#if 0 // BROKEN
	abe_be_mcbsp2_dai.cpu_dai_name  = NULL;
	abe_be_mcbsp2_dai.cpu_of_node = dai_node;
	abe_be_mcbsp2_dai.platform_name  = NULL;
	abe_be_mcbsp2_dai.platform_of_node = aess_node;
#endif

	/* Add the ABE FEs */
	ret = snd_soc_card_new_dai_links(card, abe_fe_dai,
					 ARRAY_SIZE(abe_fe_dai));
	if (ret < 0)
		return ret;

#if 0
/*
 * there is no "mcpdm-abe" to match???
 * and trying ends in
 * [   28.445332] omap-abe-twl6040 sound: ASoC: CPU DAI mcpdm-abe not registered
 *
 */

	/* McPDM BEs */
	ret = snd_soc_card_new_dai_links(card, abe_be_mcpdm_dai,
					 ARRAY_SIZE(abe_be_mcpdm_dai));
	if (ret < 0)
		return ret;
#endif

#if 0	// this comflicts with our "simple sound cards" for mcbsp1, 2, 3
	// it should be configurable either by DT or config or automatically
	// if the mcbsps are used by abe/aess or separately

	/* McBSP1 BEs */
	ret = snd_soc_card_new_dai_links(card, &abe_be_mcbsp1_dai, 1);
	if (ret < 0)
		return ret;

	/* McBSP2 BEs */
	ret = snd_soc_card_new_dai_links(card, &abe_be_mcbsp2_dai, 1);
	if (ret < 0)
		return ret;
#endif

	/* DMIC BEs */
	if (priv->has_dmic) {
		ret = snd_soc_card_new_dai_links(card, abe_be_dmic_dai,
						 ARRAY_SIZE(abe_be_dmic_dai));
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* TODO: Peter - this will need some logic for DTS DAI link creation */
static int omap_abe_add_legacy_dai_links(struct snd_soc_card *card)
{
	struct abe_twl6040 * priv = snd_soc_card_get_drvdata(card);
	struct device_node *node = card->dev->of_node;
	struct device_node *dai_node;
	int has_mcasp = 0;
	int ret;

return 0;	// completely broken...

	dai_node = of_parse_phandle(node, "ti,mcpdm", 0);
	if (!dai_node) {
			dev_err(card->dev, "McPDM node is missing\n");
			return -EINVAL;
	}
#if 0 // BROKEN
	legacy_mcpdm_dai.cpu_dai_name  = NULL;
	legacy_mcpdm_dai.cpu_of_node = dai_node;
#endif


#if 0	// legacy?
	dai_node = of_parse_phandle(node, "ti,mcbsp2", 0);
	if (!dai_node) {
		dev_err(card->dev,"McBSP2 node is missing\n");
		return -EINVAL;
	}
	legacy_mcbsp_dai.cpu_dai_name  = NULL;
	legacy_mcbsp_dai.cpu_of_node = dai_node;
#endif

	dai_node = of_parse_phandle(node, "ti,mcasp", 0);
	if (dai_node) {
#if 0 // BROKEN
		legacy_mcasp_dai.cpu_dai_name  = NULL;
		legacy_mcasp_dai.cpu_of_node = dai_node;
#endif
		has_mcasp = 1;
	}

	/* Add the Legacy McPDM */
	ret = snd_soc_card_new_dai_links(card, &legacy_mcpdm_dai, 1);
	if (ret < 0)
		return ret;

#if 0
	/* Add the Legacy McBSP */
	ret = snd_soc_card_new_dai_links(card, &legacy_mcbsp_dai, 1);
	if (ret < 0)
		return ret;
#endif

	/* Add the Legacy McASP */
	if (has_mcasp) {
		ret = snd_soc_card_new_dai_links(card, &legacy_mcasp_dai, 1);
		if (ret < 0)
			return ret;
	}

	/* Add the Legacy DMICs */
	if (priv->has_dmic) {
		ret = snd_soc_card_new_dai_links(card, &legacy_dmic_dai, 1);
		if (ret < 0)
			return ret;
	}

	return 0;
}

#if IS_BUILTIN(CONFIG_SND_OMAP_SOC_OMAP_ABE_TWL6040)
static void omap_abe_fw_ready(const struct firmware *fw, void *context)
{
	struct platform_device *pdev = (struct platform_device *)context;
	struct snd_soc_card *card = &omap_abe_card;
	struct abe_twl6040 * priv = snd_soc_card_get_drvdata(card);
	int ret;

	if (unlikely(!fw))
		dev_warn(&pdev->dev, "%s firmware is not loaded.\n",
			 AESS_FW_NAME);

	priv->aess = omap_aess_get_handle();
	if (! priv->aess)
		dev_err(&pdev->dev, "AESS is not yet available\n");

	ret = omap_aess_load_firmware(priv->aess, AESS_FW_NAME);
	if (ret) {
		dev_err(&pdev->dev, "%s firmware was not loaded.\n",
			AESS_FW_NAME);
		omap_aess_put_handle(priv->aess);
		priv->aess = NULL;
		priv->has_abe = 0;
	}

	/* Release the FW here. */
	release_firmware(fw);

	if (priv->has_abe) {
		ret = omap_abe_add_dai_links(card);
		if (ret < 0)
			goto err_unregister;
	}

	ret = omap_abe_add_legacy_dai_links(card);
	if (ret < 0)
		goto err_unregister;

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret)
		dev_err(&pdev->dev, "devm_snd_soc_register_card() failed: %d\n",
			ret);
	return;

err_unregister:
	if (!IS_ERR(priv->spdif_codec_dev))
		platform_device_unregister(priv->spdif_codec_dev);

	if (!IS_ERR(priv->dmic_codec_dev))
		platform_device_unregister(priv->dmic_codec_dev);

	snd_soc_card_reset_dai_links(card);
	return;
}

#else /* IS_BUILTIN(CONFIG_SND_OMAP_SOC_OMAP_ABE_TWL6040) */
static int omap_abe_load_fw(struct snd_soc_card *card)
{
	struct abe_twl6040 * priv = snd_soc_card_get_drvdata(card);
	const struct firmware *fw;
	int ret;

	priv->aess = omap_aess_get_handle();
	if (! priv->aess) {
		dev_err(card->dev, "AESS is not yet available\n");
		return -EPROBE_DEFER;
	}

	ret = request_firmware(&fw, AESS_FW_NAME, card->dev);
	if (ret) {
		dev_err(card->dev, "FW request failed: %d\n", ret);
		return ret;
	}

	ret = omap_aess_load_firmware(priv->aess, AESS_FW_NAME);
	if (ret) {
		dev_err(card->dev, "%s firmware was not loaded.\n",
			AESS_FW_NAME);
		omap_aess_put_handle(priv->aess);
		priv->aess = NULL;
		priv->has_abe = 0;
		ret = 0;
	}

	if (priv->has_abe)
		ret = omap_abe_add_dai_links(card);

	/* Release the FW here. */
	release_firmware(fw);

	return ret;
}
#endif /* IS_BUILTIN(CONFIG_SND_OMAP_SOC_OMAP_ABE_TWL6040) */

/* Digital audio interface glue - connects codec <--> CPU */
static struct snd_soc_dai_link abe_twl6040_dai_links[] = {
	{
		.name = "TWL6040",
		.stream_name = "TWL6040",
#if 0 // BROKEN
		.codec_dai_name = "twl6040-legacy",
		.codec_name = "twl6040-codec",
#endif
		.init = omap_abe_twl6040_init,
		.ops = &omap_abe_ops,
	},
	{
		.name = "DMIC",
		.stream_name = "DMIC Capture",
#if 0 // BROKEN
		.codec_dai_name = "dmic-hifi",
		.codec_name = "dmic-codec",
#endif
		.init = omap_abe_dmic_init,
		.ops = &omap_abe_dmic_ops,
	},
};

/* Audio machine driver */
static struct snd_soc_card omap_abe_card = {
	.owner = THIS_MODULE,

	.dapm_widgets = twl6040_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(twl6040_dapm_widgets),
	.dapm_routes = audio_map,
	.num_dapm_routes = ARRAY_SIZE(audio_map),
};

static int omap_abe_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct snd_soc_card *card = &omap_abe_card;
	struct device_node *dai_node;
	struct abe_twl6040 *priv;
	int num_links = 0;
	int ret = 0;

	if (!node) {
		dev_err(&pdev->dev, "of node is missing.\n");
		return -ENODEV;
	}

	card->dev = &pdev->dev;

	priv = devm_kzalloc(&pdev->dev, sizeof(struct abe_twl6040), GFP_KERNEL);
	if (priv == NULL)
		return -ENOMEM;

	priv->dmic_codec_dev = ERR_PTR(-EINVAL);
	priv->spdif_codec_dev = ERR_PTR(-EINVAL);

	if (snd_soc_of_parse_card_name(card, "ti,model")) {
		dev_err(&pdev->dev, "Card name is not provided\n");
		return -ENODEV;
	}

	ret = snd_soc_of_parse_audio_routing(card, "ti,audio-routing");
	if (ret) {
		dev_err(&pdev->dev, "Error while parsing DAPM routing\n");
		return ret;
	}

	dai_node = of_parse_phandle(node, "ti,mcpdm", 0);
	if (!dai_node) {
		dev_err(&pdev->dev, "McPDM node is not provided\n");
		return -EINVAL;
	}

#if 0 // BROKEN
	abe_twl6040_dai_links[0].cpu_of_node = dai_node;
	abe_twl6040_dai_links[0].platform_of_node = dai_node;

	priv->dai_links[0].name = "DMIC";
	priv->dai_links[0].stream_name = "TWL6040";
	priv->dai_links[0].cpus = link0_cpus;
	priv->dai_links[0].num_cpus = 1;
	priv->dai_links[0].cpus->of_node = dai_node;
	priv->dai_links[0].platforms = link0_platforms;
	priv->dai_links[0].num_platforms = 1;
	priv->dai_links[0].platforms->of_node = dai_node;
	priv->dai_links[0].codecs = link0_codecs;
	priv->dai_links[0].num_codecs = 1;
	priv->dai_links[0].init = omap_abe_twl6040_init;
	priv->dai_links[0].ops = &omap_abe_ops;

	abe_twl6040_dai_links[0].cpu_of_node = dai_node;
	abe_twl6040_dai_links[0].platform_of_node = dai_node;
#endif

	dai_node = of_parse_phandle(node, "ti,dmic", 0);
	if (dai_node) {
		num_links = 2;
#if 0 // BROKEN
		priv->dai_links[1].name = "TWL6040";
		priv->dai_links[1].stream_name = "DMIC Capture";
		priv->dai_links[1].cpus = link1_cpus;
		priv->dai_links[1].num_cpus = 1;
		priv->dai_links[1].cpus->of_node = dai_node;
		priv->dai_links[1].platforms = link1_platforms;
		priv->dai_links[1].num_platforms = 1;
		priv->dai_links[1].platforms->of_node = dai_node;
		priv->dai_links[1].codecs = link1_codecs;
		priv->dai_links[1].num_codecs = 1;
		priv->dai_links[1].init = omap_abe_dmic_init;
		priv->dai_links[1].ops = &omap_abe_dmic_ops;

		abe_twl6040_dai_links[1].cpu_of_node = dai_node;
		abe_twl6040_dai_links[1].platform_of_node = dai_node;
#endif
	} else {
		num_links = 1;
	}
	dai_node = of_parse_phandle(node, "ti,aess", 0);
	if (dai_node)
		priv->has_abe = 1;
	else
		dev_dbg(&pdev->dev, "AESS node is missing\n");

	dai_node = of_parse_phandle(node, "ti,dmic", 0);
	if (dai_node) {
		num_links = 2;
#if 0 // BROKEN
		abe_twl6040_dai_links[1].cpu_of_node = dai_node;
		abe_twl6040_dai_links[1].platform_of_node = dai_node;
#endif
		priv->has_dmic = 1;
	} else {
		num_links = 1;
	}

#ifdef FIXME
	pdev_tmp = platform_device_register_simple("spdif-dit", -1,
							NULL, 0);
	if (IS_ERR(pdev_tmp)) {
		dev_err(&pdev->dev, "Can't instantiate spdif-dit\n");
		ret = PTR_ERR(pdev_tmp);
		goto err_dmic_unregister;
	}
	priv->spdif_codec_dev = pdev_tmp;
#endif

	priv->jack_detection = of_property_read_bool(node, "ti,jack-detection");
	of_property_read_u32(node, "ti,mclk-freq", &priv->mclk_freq);
	if (!priv->mclk_freq) {
		dev_err(&pdev->dev, "MCLK frequency not provided\n");
		return -EINVAL;
	}

	card->fully_routed = 1;

	if (!priv->mclk_freq) {
		dev_err(&pdev->dev, "MCLK frequency missing\n");
		return -ENODEV;
	}

	snd_soc_card_new_dai_links(card, abe_twl6040_dai_links, num_links);

	snd_soc_card_set_drvdata(card, priv);

	if (priv->has_abe) {
#if IS_BUILTIN(CONFIG_SND_OMAP_SOC_OMAP_ABE_TWL6040)
		/* When ABE is in use the AESS needs firmware */
		ret = request_firmware_nowait(THIS_MODULE, 1, AESS_FW_NAME,
					      &pdev->dev, GFP_KERNEL, pdev,
					      omap_abe_fw_ready);
#else
		ret = omap_abe_load_fw(card);
#endif
		if (ret < 0) {
			dev_err(&pdev->dev, "Failed to load firmware %s: %d\n",
				AESS_FW_NAME, ret);
			goto err_unregister;
		}

#if IS_BUILTIN(CONFIG_SND_OMAP_SOC_OMAP_ABE_TWL6040)
		return ret;
#endif
	}

	ret = omap_abe_add_legacy_dai_links(card);
	if (ret < 0)
		goto err_unregister;

	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "card registration failed: %d\n", ret);
		goto err_unregister;
	}

	return ret;

err_unregister:
	if (!IS_ERR(priv->spdif_codec_dev))
		platform_device_unregister(priv->spdif_codec_dev);
err_dmic_unregister:
	if (!IS_ERR(priv->dmic_codec_dev))
		platform_device_unregister(priv->dmic_codec_dev);

	snd_soc_card_reset_dai_links(card);
	return ret;
}

static int omap_abe_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct abe_twl6040 * priv = snd_soc_card_get_drvdata(card);

	omap_aess_put_handle(priv->aess);
#ifdef CONFIG_OMAP4_SDP
	if (of_machine_is_compatible("ti,omap4-sdp")) {
		i2c_unregister_device(priv->tps6130x);
		i2c_put_adapter(priv->adapter);
	}
#endif
	if (!IS_ERR(priv->dmic_codec_dev))
		platform_device_unregister(priv->dmic_codec_dev);
	if (!IS_ERR(priv->spdif_codec_dev))
		platform_device_unregister(priv->spdif_codec_dev);
	snd_soc_card_reset_dai_links(card);

	return 0;
}

static const struct of_device_id omap_abe_of_match[] = {
	{.compatible = "ti,abe-twl6040", },
	{ },
};
MODULE_DEVICE_TABLE(of, omap_abe_of_match);

static struct platform_driver omap_abe_driver = {
	.driver = {
		.name = "omap-abe-twl6040",
		.pm = &snd_soc_pm_ops,
		.of_match_table = omap_abe_of_match,
	},
	.probe = omap_abe_probe,
	.remove = omap_abe_remove,
};

static int __init omap_abe_init(void)
{
	int ret;

	dmic_codec_dev = platform_device_register_simple("dmic-codec", -1, NULL,
							 0);
	if (IS_ERR(dmic_codec_dev)) {
		pr_err("%s: dmic-codec device registration failed\n", __func__);
		return PTR_ERR(dmic_codec_dev);
	}

	ret = platform_driver_register(&omap_abe_driver);
	if (ret) {
		pr_err("%s: platform driver registration failed\n", __func__);
		platform_device_unregister(dmic_codec_dev);
	}

	return ret;
}
module_init(omap_abe_init);

static void __exit omap_abe_exit(void)
{
	platform_driver_unregister(&omap_abe_driver);
	platform_device_unregister(dmic_codec_dev);
}
module_exit(omap_abe_exit);

MODULE_AUTHOR("Misael Lopez Cruz <misael.lopez@ti.com>");
MODULE_DESCRIPTION("ALSA SoC for OMAP boards with ABE and twl6040 codec");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:omap-abe-twl6040");
