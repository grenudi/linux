// SPDX-License-Identifier: GPL-2.0
/*
 * Based on sdm845.c
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * MSM8998 ASoC driver
 * Copyright (c) 2021, AngeloGioacchino Del Regno
 *                     <angelogioacchino.delregno@somainline.org>
 */

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <linux/soundwire/sdw.h>
#include <uapi/linux/input-event-codes.h>
#include "common.h"
#include "qdsp6/q6afe.h"

#define DRIVER_NAME		"msm8998"
#define DEFAULT_SAMPLE_RATE_48K	48000
#define DEFAULT_MCLK_RATE	24576000
#define TDM_BCLK_RATE		6144000
#define MI2S_BCLK_RATE		1536000
#define LEFT_SPK_TDM_TX_MASK	0x30
#define RIGHT_SPK_TDM_TX_MASK	0xC0
#define SPK_TDM_RX_MASK		0x03
#define NUM_TDM_SLOTS		8
#define SLIM_MAX_TX_PORTS	17
#define SLIM_MAX_RX_PORTS	14
#define WCD934X_DEFAULT_MCLK_RATE 9600000

struct msm8998_snd_data {
	struct snd_soc_jack jack;
	bool jack_setup;
	bool stream_prepared[SLIM_MAX_RX_PORTS];
	struct snd_soc_card *card;
	uint32_t quat_tdm_clk_count;
	struct sdw_stream_runtime *sruntime[SLIM_MAX_RX_PORTS];
};

static unsigned int tdm_slot_offset[8] = {0, 4, 8, 12, 16, 20, 24, 28};

/* Runtime knob so the mic channel count can be swept without a rebuild. */
static unsigned int slim_tx_channels = 1;
module_param(slim_tx_channels, uint, 0644);
MODULE_PARM_DESC(slim_tx_channels, "channel count for the SLIMBUS_0_TX backend");

/*
 * Backend rate, swept the same way. Every backend was hard-pinned to 48 kHz,
 * which is also the only rate that can reach the codec from userspace -- so
 * there was no way to ask whether the codec's 2:1 sample loss follows the
 * programmed rate or the hardware's own clock. Default is the previous
 * hard-coded value, so leaving it alone changes nothing.
 */
static unsigned int be_rate = DEFAULT_SAMPLE_RATE_48K;
module_param(be_rate, uint, 0644);
MODULE_PARM_DESC(be_rate, "sample rate forced on every backend");

static int msm8998_slim_snd_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	struct msm8998_snd_data *pdata = snd_soc_card_get_drvdata(rtd->card);
	u32 rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	struct sdw_stream_runtime *sruntime;
	u32 rx_ch_cnt = 0, tx_ch_cnt = 0;
	int ret = 0, i;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		sruntime = snd_soc_dai_get_stream(codec_dai,
						      substream->stream);
		if (sruntime != ERR_PTR(-ENOTSUPP))
			pdata->sruntime[cpu_dai->id] = sruntime;

		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt, rx_ch);

		if (ret != 0 && ret != -ENOTSUPP) {
			pr_err("failed to get codec chan map, err:%d\n", ret);
			return ret;
		} else if (ret == -ENOTSUPP) {
			/* Ignore unsupported */
			continue;
		}

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
							  rx_ch_cnt, rx_ch);
		} else {
			ret = snd_soc_dai_set_channel_map(cpu_dai, tx_ch_cnt,
							  tx_ch, 0, NULL);
		}
	}

	return 0;
}

static int msm8998_tdm_snd_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	int ret = 0, j;
	int channels, slot_width;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		slot_width = 16;
		break;
	default:
		dev_err(rtd->dev, "%s: invalid param format 0x%x\n",
				__func__, params_format(params));
		return -EINVAL;
	}

	channels = params_channels(params);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0, 0x3,
				8, slot_width);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set tdm slot, err:%d\n",
					__func__, ret);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
				channels, tdm_slot_offset);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set channel map, err:%d\n",
					__func__, ret);
			goto end;
		}
	} else {
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0xf, 0,
				8, slot_width);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set tdm slot, err:%d\n",
					__func__, ret);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, channels,
				tdm_slot_offset, 0, NULL);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set channel map, err:%d\n",
					__func__, ret);
			goto end;
		}
	}

	for_each_rtd_codec_dais(rtd, j, codec_dai) {

		if (!strcmp(codec_dai->component->name_prefix, "Left")) {
			ret = snd_soc_dai_set_tdm_slot(
					codec_dai, LEFT_SPK_TDM_TX_MASK,
					SPK_TDM_RX_MASK, NUM_TDM_SLOTS,
					slot_width);
			if (ret < 0) {
				dev_err(rtd->dev,
					"DEV0 TDM slot err:%d\n", ret);
				return ret;
			}
		}

		if (!strcmp(codec_dai->component->name_prefix, "Right")) {
			ret = snd_soc_dai_set_tdm_slot(
					codec_dai, RIGHT_SPK_TDM_TX_MASK,
					SPK_TDM_RX_MASK, NUM_TDM_SLOTS,
					slot_width);
			if (ret < 0) {
				dev_err(rtd->dev,
					"DEV1 TDM slot err:%d\n", ret);
				return ret;
			}
		}
	}

end:
	return ret;
}

static int msm8998_snd_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int ret = 0;

	switch (cpu_dai->id) {
	case QUATERNARY_TDM_RX_0:
	case QUATERNARY_TDM_TX_0:
		ret = msm8998_tdm_snd_hw_params(substream, params);
		break;
	case SLIMBUS_0_RX...SLIMBUS_6_TX:
		ret = msm8998_slim_snd_hw_params(substream, params);
		break;
	case QUATERNARY_MI2S_RX:
		break;
	default:
		pr_err("%s: invalid dai id 0x%x\n", __func__, cpu_dai->id);
		break;
	}
	return ret;
}

static int msm8998_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct msm8998_snd_data *pdata = snd_soc_card_get_drvdata(card);
	struct snd_jack *jack;
	/*
	 * Codec SLIMBUS configuration
	 * RX1, RX2, RX3, RX4, RX5, RX6, RX7, RX8, RX9, RX10, RX11, RX12, RX13
	 * TX1, TX2, TX3, TX4, TX5, TX6, TX7, TX8, TX9, TX10, TX11, TX12, TX13,
	 * TX14, TX15, TX16
	 *
	 * These must be sized by what the codec actually has, not by
	 * SLIM_MAX_*_PORTS: those also size arrays indexed by DAI id, and using
	 * them here overran both codecs. tx_ch was declared [17] with only 16
	 * initialisers, so ARRAY_SIZE() handed set_channel_map a 17th, zeroed
	 * channel; rx_ch listed 14. Both wcd9335 and wcd934x top out at 16 TX
	 * and 13 RX. wcd9335_set_channel_map() only null-checks its arguments
	 * so it tolerated the overflow, but wcd934x validates the counts and
	 * rejected it with "Invalid tx 17 or rx 14 channel count", failing the
	 * SLIM link init and taking the whole sound card down.
	 */
	unsigned int rx_ch[13] = {144, 145, 146, 147, 148, 149, 150,
				  151, 152, 153, 154, 155, 156};
	unsigned int tx_ch[16] = {128, 129, 130, 131, 132, 133, 134, 135,
				  136, 137, 138, 139, 140, 141, 142, 143};
	int ret, i;

	if (!pdata->jack_setup) {
		ret = snd_soc_card_jack_new(card, "Headset Jack",
					    SND_JACK_HEADSET |
					    SND_JACK_HEADPHONE |
					    SND_JACK_BTN_0 | SND_JACK_BTN_1 |
					    SND_JACK_BTN_2 | SND_JACK_BTN_3,
					    &pdata->jack);

		if (ret < 0) {
			dev_err(card->dev, "Unable to add Headphone Jack\n");
			return ret;
		}

		jack = pdata->jack.jack;

		snd_jack_set_key(jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
		snd_jack_set_key(jack, SND_JACK_BTN_1, KEY_VOICECOMMAND);
		snd_jack_set_key(jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
		snd_jack_set_key(jack, SND_JACK_BTN_3, KEY_VOLUMEDOWN);
		pdata->jack_setup = true;
	}

	switch (cpu_dai->id) {
	case SLIMBUS_0_RX...SLIMBUS_6_TX:
		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			ret = snd_soc_dai_set_channel_map(codec_dai,
							  ARRAY_SIZE(tx_ch), tx_ch,
							  ARRAY_SIZE(rx_ch), rx_ch);
			if (ret != 0 && ret != -ENOTSUPP)
				return ret;

			snd_soc_dai_set_sysclk(codec_dai, 0,
					       WCD934X_DEFAULT_MCLK_RATE,
					       SNDRV_PCM_STREAM_PLAYBACK);

			ret = snd_soc_component_set_jack(codec_dai->component,
							  &pdata->jack, NULL);
			if (ret != 0 && ret != -ENOTSUPP) {
				dev_warn(card->dev, "Failed to set jack: %d\n", ret);
				return ret;
			}
		}
		break;
	default:
		break;
	}

	return 0;
}

static int msm8998_snd_startup(struct snd_pcm_substream *substream)
{
	unsigned int codec_dai_fmt = SND_SOC_DAIFMT_CBC_CFC;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_card *card = rtd->card;
	struct msm8998_snd_data *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	int j;
	int ret;

	switch (cpu_dai->id) {
	case QUATERNARY_TDM_RX_0:
	case QUATERNARY_TDM_TX_0:
		if (++(data->quat_tdm_clk_count) == 1) {
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_QUAD_TDM_IBIT,
				TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		}

		codec_dai_fmt |= SND_SOC_DAIFMT_IB_NF | SND_SOC_DAIFMT_DSP_B;

		for_each_rtd_codec_dais(rtd, j, codec_dai) {

			if (!strcmp(codec_dai->component->name_prefix,
				    "Left")) {
				ret = snd_soc_dai_set_fmt(
						codec_dai, codec_dai_fmt);
				if (ret < 0) {
					dev_err(rtd->dev,
						"Left TDM fmt err:%d\n", ret);
					return ret;
				}
			}

			if (!strcmp(codec_dai->component->name_prefix,
				    "Right")) {
				ret = snd_soc_dai_set_fmt(
						codec_dai, codec_dai_fmt);
				if (ret < 0) {
					dev_err(rtd->dev,
						"Right TDM slot err:%d\n", ret);
					return ret;
				}
			}
		}
		break;
	case QUATERNARY_MI2S_RX:
		/*
		 * The SoC is the bit and frame clock master here (the TAS2557
		 * amps on taimen are I2S slaves), so the backend has to program
		 * the LPASS quaternary bit clock itself. Without this the port
		 * fell straight through to the error print below and no clock
		 * was ever generated.
		 */
		ret = snd_soc_dai_set_sysclk(cpu_dai,
					     Q6AFE_LPASS_CLK_ID_QUAD_MI2S_IBIT,
					     MI2S_BCLK_RATE,
					     SNDRV_PCM_STREAM_PLAYBACK);
		if (ret) {
			dev_err(rtd->dev, "QUAT MI2S bclk err:%d\n", ret);
			return ret;
		}
		ret = snd_soc_dai_set_fmt(cpu_dai,
					  SND_SOC_DAIFMT_CBC_CFC |
					  SND_SOC_DAIFMT_NB_NF |
					  SND_SOC_DAIFMT_I2S);
		if (ret) {
			dev_err(rtd->dev, "QUAT MI2S fmt err:%d\n", ret);
			return ret;
		}
		break;
	case SLIMBUS_0_RX...SLIMBUS_6_TX:
		break;

	default:
		pr_err("%s: invalid dai id 0x%x\n", __func__, cpu_dai->id);
		break;
	}
	return 0;
}

static void  msm8998_snd_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_card *card = rtd->card;
	struct msm8998_snd_data *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case QUATERNARY_TDM_RX_0:
	case QUATERNARY_TDM_TX_0:
		if (--(data->quat_tdm_clk_count) == 0) {
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_QUAD_TDM_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		}
		break;
	case QUATERNARY_MI2S_RX:
		/* release the bit clock the startup path took */
		snd_soc_dai_set_sysclk(cpu_dai,
				       Q6AFE_LPASS_CLK_ID_QUAD_MI2S_IBIT,
				       0, SNDRV_PCM_STREAM_PLAYBACK);
		break;
	case SLIMBUS_0_RX...SLIMBUS_6_TX:
		break;

	default:
		pr_err("%s: invalid dai id 0x%x\n", __func__, cpu_dai->id);
		break;
	}
}

static int msm8998_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct msm8998_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sdw_stream_runtime *sruntime = data->sruntime[cpu_dai->id];
	int ret;

	if (!sruntime)
		return 0;

	if (data->stream_prepared[cpu_dai->id]) {
		sdw_disable_stream(sruntime);
		sdw_deprepare_stream(sruntime);
		data->stream_prepared[cpu_dai->id] = false;
	}

	ret = sdw_prepare_stream(sruntime);
	if (ret)
		return ret;

	/**
	 * NOTE: there is a strict hw requirement about the ordering of port
	 * enables and actual WSA881x PA enable. PA enable should only happen
	 * after soundwire ports are enabled if not DC on the line is
	 * accumulated resulting in Click/Pop Noise
	 * PA enable/mute are handled as part of codec DAPM and digital mute.
	 */

	ret = sdw_enable_stream(sruntime);
	if (ret) {
		sdw_deprepare_stream(sruntime);
		return ret;
	}
	data->stream_prepared[cpu_dai->id] = true;

	return ret;
}

static int msm8998_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct msm8998_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sdw_stream_runtime *sruntime = data->sruntime[cpu_dai->id];

	if (sruntime && data->stream_prepared[cpu_dai->id]) {
		sdw_disable_stream(sruntime);
		sdw_deprepare_stream(sruntime);
		data->stream_prepared[cpu_dai->id] = false;
	}

	return 0;
}

static const struct snd_soc_ops msm8998_be_ops = {
	.hw_params = msm8998_snd_hw_params,
	.hw_free = msm8998_snd_hw_free,
	.prepare = msm8998_snd_prepare,
	.startup = msm8998_snd_startup,
	.shutdown = msm8998_snd_shutdown,
};

static int msm8998_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	rate->min = rate->max = be_rate;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	/*
	 * Forcing every backend to stereo is wrong for the mic.
	 *
	 * The handset mic is a single DMIC on ONE SLIM TX channel: the codec's
	 * slim_ch_list holds exactly one entry, so wcd934x_get_channel_map()
	 * returns tx_num=1 and q6slim_set_channel_map() configures the AFE port
	 * with num_channels=1 (q6slim_hw_params() never touches the count).
	 * But this fixup then told the BE it was stereo, and q6routing feeds
	 * that straight into the ADM -- measured as an ADM open with ch=2 against a
	 * one-channel AFE port and a mono frontend. The DSP ran and delivered a
	 * constant value rather than audio.
	 *
	 * Downstream agrees: slim_tx_cfg[SLIM_TX_0] in msm8998_wahoo.c is 1
	 * channel, and mixer_paths_tavil_taimen.xml sets "SLIM_0_TX Channels" to
	 * "One" -- a control mainline does not implement, which is why nothing
	 * here ever corrected it.
	 */
	switch (cpu_dai->id) {
	case SLIMBUS_0_TX:
		channels->min = channels->max = slim_tx_channels;
		break;
	default:
		channels->min = channels->max = 2;
		break;
	}

	return 0;
}

static const struct snd_soc_dapm_widget msm8998_snd_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_SPK("Left Spk", NULL),
	SND_SOC_DAPM_SPK("Right Spk", NULL),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
	/*
	 * The board routes each MIC BIAS to one of these, so without them the
	 * bias widgets have no input and DAPM leaves the path incomplete --
	 * measured as "MIC BIAS1: On in 0 out 0" while capture returned a
	 * constant. Names and the bias pairing are taken verbatim from the
	 * factory devicetree overlay (qcom,audio-routing on sound-tavil):
	 * DMIC0/1 -> BIAS1, DMIC2/3 -> BIAS3, DMIC4/5 -> BIAS4.
	 */
	SND_SOC_DAPM_MIC("Handset Mic", NULL),
	SND_SOC_DAPM_MIC("Handset 2nd Mic", NULL),
	SND_SOC_DAPM_MIC("Handset 3rd Mic", NULL),
	SND_SOC_DAPM_MIC("Digital Mic0", NULL),
	SND_SOC_DAPM_MIC("Digital Mic1", NULL),
	SND_SOC_DAPM_MIC("Digital Mic2", NULL),
	SND_SOC_DAPM_MIC("Digital Mic3", NULL),
	SND_SOC_DAPM_MIC("Digital Mic4", NULL),
	SND_SOC_DAPM_MIC("Digital Mic5", NULL),
};

static void msm8998_add_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->ops = &msm8998_be_ops;
			link->be_hw_params_fixup = msm8998_be_hw_params_fixup;
		}
		link->init = msm8998_dai_init;
	}
}

static int msm8998_snd_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct msm8998_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->driver_name = DRIVER_NAME;
	card->dapm_widgets = msm8998_snd_widgets;
	card->num_dapm_widgets = ARRAY_SIZE(msm8998_snd_widgets);
	card->dev = dev;
	card->owner = THIS_MODULE;
	dev_set_drvdata(dev, card);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	data->card = card;
	snd_soc_card_set_drvdata(card, data);

	msm8998_add_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id msm8998_snd_device_id[]  = {
	{ .compatible = "qcom,msm8998-sndcard" },
	{},
};
MODULE_DEVICE_TABLE(of, msm8998_snd_device_id);

static struct platform_driver msm8998_snd_driver = {
	.probe = msm8998_snd_platform_probe,
	.driver = {
		.name = "msm-snd-msm8998",
		.of_match_table = msm8998_snd_device_id,
	},
};
module_platform_driver(msm8998_snd_driver);

MODULE_DESCRIPTION("msm8998 ASoC Machine Driver");
MODULE_LICENSE("GPL v2");
