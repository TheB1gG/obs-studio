/******************************************************************************
    Copyright (C) 2025 by OBS Project

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>
#include <obs-module.h>
#include <opts-parser.h>

#ifndef _STDINT_H_INCLUDED
#define _STDINT_H_INCLUDED
#endif

#ifdef _WIN32
#define X265_API_IMPORTS
#endif
#include <x265.h>

#define do_log_enc(level, encoder, format, ...) \
	blog(level, "[x265 encoder: '%s'] " format, obs_encoder_get_name(encoder), ##__VA_ARGS__)
#define do_log(level, format, ...) do_log_enc(level, obsx265->encoder, format, ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define warn_enc(encoder, format, ...) do_log_enc(LOG_WARNING, encoder, format, ##__VA_ARGS__)
#define err(format, ...) do_log(LOG_ERROR, format, ##__VA_ARGS__)
#define err_enc(encoder, format, ...) do_log_enc(LOG_ERROR, encoder, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

/* ------------------------------------------------------------------------- */

struct obs_x265 {
	obs_encoder_t *encoder;

	x265_param params;
	x265_encoder *context;
	const x265_api *api; /* resolved via x265_api_get(bitdepth) at create */

	/* Effective fps (num/den) the x265 context was last OPENED with. The rate
	 * control's per-frame bit budget is derived from this at open time and is NOT
	 * rebuilt by encoder_reconfig, so a live FPS change requires re-opening the
	 * context. Tracked separately from params.fpsNum/fpsDenom (which update() may
	 * already have advanced via reconfig) so the re-init is detected regardless of
	 * whether the deferred update() runs before or after this check on the encode
	 * thread. */
	uint32_t inited_fps_num;
	uint32_t inited_fps_den;

	/* User's keyframe interval in seconds (0 = use the encoder default). Stored so a
	 * live FPS change can recompute keyframeMax for the new rate without depending on
	 * whether a deferred update() has already rescaled it from the new fps. */
	int keyint_sec;

	DARRAY(uint8_t) packet_data;

	uint8_t *extra_data;
	uint8_t *sei;

	size_t extra_data_size;
	size_t sei_size;

	os_performance_token_t *performance_token;

	uint32_t roi_increment;
	float *quant_offsets;

	/* Color settings chosen by the user. These drive both the
	 * x265 context parameters and get_video_info, so libobs delivers frames
	 * in exactly this format/space/range. Changes apply on next start. */
	enum video_format color_format;
	enum video_colorspace color_space;
	enum video_range_type color_range;

	/* The csp/bitdepth the x265 context is currently built for (from create).
	 * Used to detect settings changes in update() and keep live encode data
	 * consistent with what libobs delivers until a restart. */
	int active_csp;         /* input/delivery csp (pic->colorSpace): can be 0-8 */
	int internal_csp;        /* planar csp for params.internalCsp: must be 0-3 */
	uint32_t active_bitdepth;

	/* 8-bit RGB (BGRA): libobs delivers packed GBRA; we de-interleave it into planar
	 * [G][B][R] u8 in the encode path and feed x265 as YUV 4:4:4 with an identity (GBR)
	 * matrix. The scratch is cached across frames, reallocated only on resolution change. */
	uint8_t *gbr_deinter;
	size_t gbr_deinter_size;
};

/* Forward declaration */
static void obs_x265_video_info(void *data, struct video_scale_info *info);

/* ------------------------------------------------------------------------- */

static enum video_format color_format_from_name(const char *name)
{
	if (!name || strcmp(name, "NV12") == 0)
		return VIDEO_FORMAT_NV12;
	else if (strcmp(name, "P010") == 0)
		return VIDEO_FORMAT_P010;
	else if (strcmp(name, "I422") == 0)
		return VIDEO_FORMAT_I422;
	else if (strcmp(name, "YUV420P12") == 0)
		return VIDEO_FORMAT_YUV420P12;
	else if (strcmp(name, "YUV422P12") == 0)
		return VIDEO_FORMAT_YUV422P12;
	else if (strcmp(name, "YUV444P12") == 0)
		return VIDEO_FORMAT_YUV444P12;
	else if (strcmp(name, "GBRP12") == 0)
		return VIDEO_FORMAT_GBRP12;
	else if (strcmp(name, "I444") == 0)
		return VIDEO_FORMAT_I444;
	else if (strcmp(name, "P216") == 0)
		return VIDEO_FORMAT_P216;
	else if (strcmp(name, "BGRA") == 0)
		return VIDEO_FORMAT_BGRA;
	else if (strcmp(name, "R10I") == 0)
		return VIDEO_FORMAT_R10L;
	else if (strcmp(name, "R10P") == 0)
		return VIDEO_FORMAT_R10P;
	else if (strcmp(name, "I412") == 0)
		return VIDEO_FORMAT_I412;
	else if (strcmp(name, "I420") == 0)
		return VIDEO_FORMAT_NV12;
	else if (strcmp(name, "I010") == 0)
		return VIDEO_FORMAT_P010;

	return VIDEO_FORMAT_NONE;
}

static void publish_preferred_settings(obs_encoder_t *encoder, enum video_format format, enum video_colorspace cs,
				       enum video_range_type range)
{
	obs_encoder_set_preferred_video_format(encoder, format);
	obs_encoder_set_preferred_color_space(encoder, cs);
	obs_encoder_set_preferred_range(encoder, range);
}

static void read_color_settings(obs_data_t *settings, enum video_format *format, enum video_colorspace *cs,
				enum video_range_type *range)
{
	const char *fmt_name = obs_data_get_string(settings, "color_format");
	enum video_colorspace cs_val = (enum video_colorspace)obs_data_get_int(settings, "color_space");
	enum video_range_type rg_val = (enum video_range_type)obs_data_get_int(settings, "color_range");

	/* Color format: fall back to P010 when unset / legacy "(Default)" / unrecognized. */
	if (!fmt_name || !*fmt_name || strcmp(fmt_name, "Default") == 0) {
		*format = VIDEO_FORMAT_P010;
	} else {
		enum video_format f = color_format_from_name(fmt_name);
		*format = (f != VIDEO_FORMAT_NONE) ? f : VIDEO_FORMAT_P010;
	}

	/* Color space: fall back to Rec. 709 when unset / legacy "(Default)". */
	*cs = (cs_val == VIDEO_CS_DEFAULT) ? VIDEO_CS_709 : cs_val;

	/* Range: fall back to Partial (Limited) when unset / legacy "(Default)". */
	*range = (rg_val == VIDEO_RANGE_DEFAULT) ? VIDEO_RANGE_PARTIAL : rg_val;

	/* Identity-RGB (BGRA): RGB screen content is full-range sRGB. A YUV colorspace or a
	 * limited range would be wrong for it, so force sRGB + full range regardless of the
	 * user's picks - matching NVENC/QSV's BGRA->GBRA override. */
	if (*format == VIDEO_FORMAT_BGRA) {
		*cs = VIDEO_CS_SRGB;
		*range = VIDEO_RANGE_FULL;
	}
}

/* Maps a user-selected color format to:
 * - delivery: what libobs delivers (the OBS video_format)
 * - internal_csp: planar csp for params.internalCsp (must be 0-3: I400/I420/I422/I444)
 * - input_csp: csp for pic->colorSpace
 * - bitdepth: pixel bit depth
 *
 * IMPORTANT: this libx265 build requires params.internalCsp to EQUAL the input
 * picture's colorSpace (see x265.h: "internalCsp ... must match color space of
 * input pictures") and only ingests planar pictures - packed NV12/BGRA ingest is
 * not implemented and crashes with a null internal buffer. So every format maps to
 * a PLANAR delivery format and input_csp always equals internal_csp. */
static bool obs_x265_delivery_csp(enum video_format fmt, enum video_format *delivery,
                                   int *internal_csp, int *input_csp, uint32_t *bitdepth)
{
	switch (fmt) {
	case VIDEO_FORMAT_NV12:
		/* Deliver planar I420 and feed x265 I420 directly. The output HEVC is identical
		 * to an NV12 feed, but avoids the unsupported packed-chroma ingest path that
		 * crashes (null internal buffer) in this libx265 build. */
		*delivery = VIDEO_FORMAT_I420;
		*internal_csp = X265_CSP_I420;
		*input_csp = X265_CSP_I420;
		*bitdepth = 8;
		break;
	case VIDEO_FORMAT_P010:
		*delivery = VIDEO_FORMAT_I010; /* libobs delivers planar I010 for P010 */
		*internal_csp = X265_CSP_I420;
		*input_csp = X265_CSP_I420;
		*bitdepth = 10;
		break;
	case VIDEO_FORMAT_YUV420P12:
		*delivery = VIDEO_FORMAT_YUV420P12; /* planar Y/U/V u16 (YUV 4:2:0, 12-bit) */
		*internal_csp = X265_CSP_I420;
		*input_csp = X265_CSP_I420;
		*bitdepth = 12;
		break;
	case VIDEO_FORMAT_I444:
		*delivery = VIDEO_FORMAT_I444;
		*internal_csp = X265_CSP_I444;
		*input_csp = X265_CSP_I444;
		*bitdepth = 8;
		break;
	case VIDEO_FORMAT_I412:
		*delivery = VIDEO_FORMAT_I412;
		*internal_csp = X265_CSP_I444;
		*input_csp = X265_CSP_I444;
		*bitdepth = 10;
		break;
	case VIDEO_FORMAT_YUV444P12:
		*delivery = VIDEO_FORMAT_YUV444P12; /* planar Y/U/V u16 (YUV 4:4:4, 12-bit) */
		*internal_csp = X265_CSP_I444;
		*input_csp = X265_CSP_I444;
		*bitdepth = 12;
		break;
	case VIDEO_FORMAT_R10P:
		*delivery = VIDEO_FORMAT_R10P;
		*internal_csp = X265_CSP_I444; /* planar G/B/R u16 ingested as I444 + bitDepth 10 */
		*input_csp = X265_CSP_I444;
		*bitdepth = 10;
		break;
	case VIDEO_FORMAT_GBRP12:
		*delivery = VIDEO_FORMAT_GBRP12; /* planar G/B/R u16 ingested as I444 + bitDepth 12 */
		*internal_csp = X265_CSP_I444;
		*input_csp = X265_CSP_I444;
		*bitdepth = 12;
		break;
	case VIDEO_FORMAT_P216:
		*delivery = VIDEO_FORMAT_I210; /* libobs delivers planar I210 for P216 */
		*internal_csp = X265_CSP_I422;
		*input_csp = X265_CSP_I422;
		*bitdepth = 10;
		break;
	case VIDEO_FORMAT_I422:
		*delivery = VIDEO_FORMAT_I422; /* planar Y/U/V u8 (YUV 4:2:2, 8-bit) */
		*internal_csp = X265_CSP_I422;
		*input_csp = X265_CSP_I422;
		*bitdepth = 8;
		break;
	case VIDEO_FORMAT_YUV422P12:
		*delivery = VIDEO_FORMAT_YUV422P12; /* planar Y/U/V u16 (YUV 4:2:2, 12-bit) */
		*internal_csp = X265_CSP_I422;
		*input_csp = X265_CSP_I422;
		*bitdepth = 12;
		break;
	case VIDEO_FORMAT_BGRA:
		/* Packed RGB has no ingest path in this libx265 build. libobs delivers packed BGRA
		 * on the CPU path (GBRA is only produced on the GPU-texture path used by NVENC/QSV,
		 * not for software encoders); we de-interleave it to planar G/B/R in the encode path
		 * (init_pic_data), fed as YUV 4:4:4 with an identity (GBR) matrix. */
		*delivery = VIDEO_FORMAT_BGRA;
		*internal_csp = X265_CSP_I444;
		*input_csp = X265_CSP_I444;
		*bitdepth = 8;
		break;
	default:
		return false;
	}
	return true;
}

static const char *obs_x265_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "x265";
}

static void clear_data(struct obs_x265 *obsx265)
{
	if (obsx265->context && obsx265->api) {
		obsx265->api->encoder_close(obsx265->context);
		bfree(obsx265->sei);
		bfree(obsx265->extra_data);
		bfree(obsx265->quant_offsets);

		obsx265->context = NULL;
		obsx265->sei = NULL;
		obsx265->extra_data = NULL;
	}

	bfree(obsx265->gbr_deinter);
	obsx265->gbr_deinter = NULL;
	obsx265->gbr_deinter_size = 0;
}

static void obs_x265_destroy(void *data)
{
	struct obs_x265 *obsx265 = data;

	if (obsx265) {
		os_end_high_performance(obsx265->performance_token);
		clear_data(obsx265);
		da_free(obsx265->packet_data);
		bfree(obsx265);
	}
}

static void obs_x265_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_bool(settings, "use_bufsize", false);
	obs_data_set_default_int(settings, "buffer_size", 2500);
	obs_data_set_default_int(settings, "keyint_sec", 0);
	obs_data_set_default_int(settings, "crf", 17);
	obs_data_set_default_string(settings, "rate_control", "CRF");

	obs_data_set_default_string(settings, "preset", "veryfast");
	obs_data_set_default_string(settings, "profile", "");
	obs_data_set_default_string(settings, "tune", "");
	obs_data_set_default_string(settings, "x265opts", "");
	obs_data_set_default_bool(settings, "repeat_headers", false);

	/* Color settings: x265 defaults to 10-bit P010 (libx265 falls back to NV12 if no 10-bit API). */
	obs_data_set_default_string(settings, "color_format", "P010");
	obs_data_set_default_int(settings, "color_space", VIDEO_CS_709);
	obs_data_set_default_int(settings, "color_range", VIDEO_RANGE_PARTIAL);
}


static inline void add_strings(obs_property_t *list, const char *const *strings)
{
	while (*strings) {
		obs_property_list_add_string(list, *strings, *strings);
		strings++;
	}
}

#define TEXT_RATE_CONTROL obs_module_text("RateControl")
#define TEXT_BITRATE obs_module_text("Bitrate")
#define TEXT_CUSTOM_BUF obs_module_text("CustomBufsize")
#define TEXT_BUF_SIZE obs_module_text("BufferSize")
#define TEXT_CRF obs_module_text("CRF")
#define TEXT_KEYINT_SEC obs_module_text("KeyframeIntervalSec")
#define TEXT_PRESET obs_module_text("CPUPreset")
#define TEXT_PROFILE obs_module_text("Profile")
#define TEXT_TUNE obs_module_text("Tune")
#define TEXT_NONE obs_module_text("None")
#define TEXT_X265_OPTS obs_module_text("EncoderOptions")
#define TEXT_COLOR_FORMAT obs_module_text("ColorFormat")
#define TEXT_COLOR_SPACE obs_module_text("ColorSpace")
#define TEXT_COLOR_RANGE obs_module_text("ColorRange")

static bool use_bufsize_modified(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	bool use_bufsize = obs_data_get_bool(settings, "use_bufsize");
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool rc_quality = astrcmpi(rc, "CRF") == 0 || astrcmpi(rc, "CQP") == 0;

	p = obs_properties_get(ppts, "buffer_size");
	obs_property_set_visible(p, use_bufsize && !rc_quality);
	return true;
}

static bool rate_control_modified(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool use_bufsize = obs_data_get_bool(settings, "use_bufsize");
	bool abr = astrcmpi(rc, "CBR") == 0 || astrcmpi(rc, "ABR") == 0;
	bool rc_quality = astrcmpi(rc, "CRF") == 0 || astrcmpi(rc, "CQP") == 0;

	p = obs_properties_get(ppts, "crf");
	obs_property_set_visible(p, !abr);

	p = obs_properties_get(ppts, "bitrate");
	obs_property_set_visible(p, !rc_quality);
	p = obs_properties_get(ppts, "use_bufsize");
	obs_property_set_visible(p, !rc_quality);
	p = obs_properties_get(ppts, "buffer_size");
	obs_property_set_visible(p, !rc_quality && use_bufsize);
	return true;
}

/* The formats x265 can encode (see obs_x265_delivery_csp): YUV 4:2:0, 4:2:2 and 4:4:4 in 8-bit,
 * 10-bit and 12-bit, plus RGB. Offer exactly those; the shared injection builds the dropdown from
 * this set. */
static bool x265_is_color_format_supported(void *type_data, enum video_format format)
{
	UNUSED_PARAMETER(type_data);
	switch (format) {
	case VIDEO_FORMAT_NV12: /* YUV 4:2:0 8-bit */
	case VIDEO_FORMAT_P010: /* YUV 4:2:0 10-bit */
	case VIDEO_FORMAT_YUV420P12: /* YUV 4:2:0 12-bit */
	case VIDEO_FORMAT_I422: /* YUV 4:2:2 8-bit */
	case VIDEO_FORMAT_P216: /* YUV 4:2:2 10-bit */
	case VIDEO_FORMAT_YUV422P12: /* YUV 4:2:2 12-bit */
	case VIDEO_FORMAT_I444: /* YUV 4:4:4 8-bit */
	case VIDEO_FORMAT_I412: /* YUV 4:4:4 10-bit */
	case VIDEO_FORMAT_YUV444P12: /* YUV 4:4:4 12-bit */
	case VIDEO_FORMAT_BGRA: /* RGB 8-bit */
	case VIDEO_FORMAT_R10P: /* RGB 10-bit (planar) */
	case VIDEO_FORMAT_GBRP12: /* RGB 12-bit */
		return true;
	default:
		return false;
	}
}


static obs_properties_t *obs_x265_props(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *list;
	obs_property_t *p;
	obs_property_t *headers;

	list = obs_properties_add_list(props, "rate_control", TEXT_RATE_CONTROL, OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, "CBR", "CBR");
	obs_property_list_add_string(list, "ABR", "ABR");
	obs_property_list_add_string(list, "VBR", "VBR");
	obs_property_list_add_string(list, "CRF", "CRF");
	obs_property_list_add_string(list, "CQP", "CQP");

	obs_property_set_modified_callback(list, rate_control_modified);

	p = obs_properties_add_int(props, "bitrate", TEXT_BITRATE, 50, 10000000, 50);
	obs_property_int_set_suffix(p, " Kbps");

	p = obs_properties_add_bool(props, "use_bufsize", TEXT_CUSTOM_BUF);
	obs_property_set_modified_callback(p, use_bufsize_modified);
	obs_properties_add_int(props, "buffer_size", TEXT_BUF_SIZE, 0, 10000000, 1);

	obs_properties_add_int(props, "crf", TEXT_CRF, 0, 51, 1);

	p = obs_properties_add_int(props, "keyint_sec", TEXT_KEYINT_SEC, 0, 20, 1);
	obs_property_int_set_suffix(p, " s");

	static const char *const x265_presets[] = {"ultrafast", "superfast", "veryfast", "faster", "fast",
						    "medium",   "slow",    "slower",  "veryslow", "placebo", NULL};
	list = obs_properties_add_list(props, "preset", TEXT_PRESET, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	add_strings(list, x265_presets);

	list = obs_properties_add_list(props, "profile", TEXT_PROFILE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, TEXT_NONE, "");
	obs_property_list_add_string(list, "main", "main");
	obs_property_list_add_string(list, "main10", "main10");
	obs_property_list_add_string(list, "rext", "rext");

	static const char *const x265_tunes[] = {"psnr", "ssim", "grain", "fastdecode", "zerolatency", NULL};
	list = obs_properties_add_list(props, "tune", TEXT_TUNE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, TEXT_NONE, "");
	add_strings(list, x265_tunes);

	/* Color format / space / range are provided by the shared libobs injection (see
	 * add_encoder_color_properties in obs-encoder.c); x265 declares its allowed formats via
	 * is_color_format_supported. */

	obs_properties_add_text(props, "x265opts", TEXT_X265_OPTS, OBS_TEXT_DEFAULT);

	headers = obs_properties_add_bool(props, "repeat_headers", "repeat_headers");
	obs_property_set_visible(headers, false);

	return props;
}


static const char *validate(struct obs_x265 *obsx265, const char *val, const char *name, const char *const *list)
{
	if (!val || !*val)
		return val;

	while (*list) {
		if (strcmp(val, *list) == 0)
			return val;

		list++;
	}

	warn("Invalid %s: %s", name, val);
	return NULL;
}

static inline void set_param(struct obs_x265 *obsx265, struct obs_option option)
{
	const char *name = option.name;
	const char *val = option.value;
	if (strcmp(name, "preset") != 0 && strcmp(name, "profile") != 0 && strcmp(name, "tune") != 0 &&
	    strcmp(name, "fps") != 0 && strcmp(name, "force-cfr") != 0 && strcmp(name, "width") != 0 &&
	    strcmp(name, "height") != 0 && strcmp(name, "stats") != 0 && strcmp(name, "qpfile") != 0 &&
	    strcmp(name, "pass") != 0) {
		if (obsx265->api->param_parse(&obsx265->params, name, val) != 0)
			warn("x265 param: %s=%s failed", name, val);
	}
}

static inline int get_x265_cs_val(const char *const name, const char *const names[])
{
	int idx = 0;
	do {
		if (strcmp(names[idx], name) == 0)
			return idx;
	} while (!!names[++idx]);

	return 0;
}

static bool reset_x265_params(struct obs_x265 *obsx265, const char *preset, const char *tune)
{
	int ret = obsx265->api->param_default_preset(&obsx265->params, preset && *preset ? preset : NULL,
						     tune && *tune ? tune : NULL);
	if (ret != 0) {
		warn("Failed to set x265 preset '%s' / tune '%s'", preset ? preset : "(none)", tune ? tune : "(none)");
		return false;
	}
	return true;
}

enum rate_control { RATE_CONTROL_CBR, RATE_CONTROL_VBR, RATE_CONTROL_ABR, RATE_CONTROL_CRF, RATE_CONTROL_CQP };

static void update_params(struct obs_x265 *obsx265, obs_data_t *settings, const struct obs_options *options,
			  bool update)
{
	video_t *video = obs_encoder_video(obsx265->encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	struct video_scale_info info;

	info.format = obsx265->color_format;
	info.colorspace = obsx265->color_space;
	info.range = obsx265->color_range;

	obs_x265_video_info(obsx265, &info);

	const char *rate_control = obs_data_get_string(settings, "rate_control");

	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	int buffer_size = (int)obs_data_get_int(settings, "buffer_size");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	obsx265->keyint_sec = keyint_sec;
	int crf = (int)obs_data_get_int(settings, "crf");
	int width = (int)obs_encoder_get_width(obsx265->encoder);
	int height = (int)obs_encoder_get_height(obsx265->encoder);
	bool use_bufsize = obs_data_get_bool(settings, "use_bufsize");
	enum rate_control rc;

	/* Determine the rate control mode and normalize bitrate/buffer_size/crf for it.
	 * Mirrors obs-x264: quality-based modes (CRF/CQP) must NOT carry a VBV/bitrate cap,
	 * otherwise x265 clamps the output to the VBV max rate even though CRF is selected. */
	if (astrcmpi(rate_control, "ABR") == 0) {
		rc = RATE_CONTROL_ABR;
		crf = 0;

	} else if (astrcmpi(rate_control, "VBR") == 0) {
		rc = RATE_CONTROL_VBR;

	} else if (astrcmpi(rate_control, "CRF") == 0) {
		rc = RATE_CONTROL_CRF;
		bitrate = 0;
		buffer_size = 0;

	} else if (astrcmpi(rate_control, "CQP") == 0) {
		rc = RATE_CONTROL_CQP;
		bitrate = 0;
		buffer_size = 0;

	} else { /* CBR */
		rc = RATE_CONTROL_CBR;
		crf = 0;
	}

	if (keyint_sec)
		obsx265->params.keyframeMax = keyint_sec * voi->fps_num / voi->fps_den;

	if (!use_bufsize)
		buffer_size = bitrate;

	obsx265->params.rc.vbvMaxBitrate = bitrate;
	obsx265->params.rc.vbvBufferSize = buffer_size;
	obsx265->params.rc.bitrate = bitrate;
	obsx265->params.sourceWidth = width;
	obsx265->params.sourceHeight = height;
	obsx265->params.fpsNum = voi->fps_num;
	obsx265->params.fpsDenom = voi->fps_den;
	obsx265->params.logLevel = X265_LOG_WARNING;

	if (obs_data_has_user_value(settings, "bf"))
		obsx265->params.bframes = (int)obs_data_get_int(settings, "bf");


	/* VUI / Color metadata */
	static const char *const smpte170m = "smpte170m";
	static const char *const bt709 = "bt709";
	const char *colorprim = bt709;
	const char *transfer = bt709;
	const char *colmatrix = bt709;
	switch (info.colorspace) {
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_709:
		colorprim = bt709;
		transfer = bt709;
		colmatrix = bt709;
		break;
	case VIDEO_CS_601:
		colorprim = smpte170m;
		transfer = smpte170m;
		colmatrix = smpte170m;
		break;
	case VIDEO_CS_SRGB:
		colorprim = bt709;
		transfer = "iec61966-2-1";
		colmatrix = bt709;
		break;
	case VIDEO_CS_2100_PQ:
		colorprim = "bt2020";
		transfer = "smpte2084";
		colmatrix = "bt2020nc";
		break;
	case VIDEO_CS_2100_HLG:
		colorprim = "bt2020";
		transfer = "arib-std-b67";
		colmatrix = "bt2020nc";
		break;
	default:
		break;
	}

	/* Formats genuinely encoded as planar GBR/RGB get an identity (GBR) matrix VUI. BGRA is
	 * one of these: obs_x265_delivery_csp() delivers it as packed BGRA and init_pic_data
	 * de-interleaves it to planar G/B/R, so its bitstream is raw RGB components - it must
	 * carry an identity/GBR VUI (a YUV/BT.709 tag would make decoders read the G/B/R planes
	 * as Y/Cb/Cr and produce wrong colors). BGRA is detected via the user's stable
	 * color_format choice (its delivery format is also BGRA here). */
	bool is_rgb_family = (obsx265->color_format == VIDEO_FORMAT_BGRA) || (info.format == VIDEO_FORMAT_R10L) || (info.format == VIDEO_FORMAT_R10P) ||
	                        (info.format == VIDEO_FORMAT_GBRP12);
	if (is_rgb_family)
		colmatrix = "gbr";

	obsx265->params.vui.bEnableVideoSignalTypePresentFlag = true;
	obsx265->params.vui.videoFormat = 5; /* progressive */
	obsx265->params.vui.bEnableVideoFullRangeFlag = is_rgb_family || info.range == VIDEO_RANGE_FULL;
	obsx265->params.vui.bEnableColorDescriptionPresentFlag = true;

	if (is_rgb_family) {
		/* Identity-RGB: leave primaries/transfer unspecified and use the identity matrix, so
		 * the stream reads as raw RGB with no YUV transform and no BT.709/sRGB tags. */
		obsx265->params.vui.colorPrimaries = 2; /* unspecified */
		obsx265->params.vui.transferCharacteristics = 2; /* unspecified */
	} else {
		obsx265->params.vui.colorPrimaries = get_x265_cs_val(colorprim, x265_colorprim_names);
		obsx265->params.vui.transferCharacteristics = get_x265_cs_val(transfer, x265_transfer_names);
	}
	obsx265->params.vui.matrixCoeffs = get_x265_cs_val(colmatrix, x265_colmatrix_names);

	/* Rate control - mirror obs-x264: CBR/ABR use the ABR method, VBR/CRF use the
	 * constant-ratefactor method (VBR keeps a VBV cap, CRF has none), and CQP uses
	 * constant QP. */
	if (rc == RATE_CONTROL_CBR || rc == RATE_CONTROL_ABR) {
		obsx265->params.rc.rateControlMode = X265_RC_ABR;

		if (rc == RATE_CONTROL_CBR) {
			/* CBR: pin the VBV max rate and buffer to the target bitrate so the output stays flat */
			obsx265->params.rc.vbvMaxBitrate = bitrate;
			obsx265->params.rc.vbvBufferSize = bitrate;
		}
	} else if (rc == RATE_CONTROL_CQP) {
		obsx265->params.rc.rateControlMode = X265_RC_CQP;
		obsx265->params.rc.qp = crf;
	} else { /* VBR and CRF both use the constant-ratefactor method */
		obsx265->params.rc.rateControlMode = X265_RC_CRF;
		obsx265->params.rc.rfConstant = (float)crf;
	}

	/* Derive x265 csp/bitdepth from the user's chosen color format */
	enum video_format delivery_fmt;
	int internal_csp, input_csp;
	uint32_t bitdepth;
	if (!obs_x265_delivery_csp(obsx265->color_format, &delivery_fmt, &internal_csp, &input_csp, &bitdepth)) {
		err("internal error: unsupported color format (%d) reached param setup", (int)obsx265->color_format);
		return;
	}
	obsx265->params.internalCsp = internal_csp;
	obsx265->params.internalBitDepth = (int)bitdepth;

	for (size_t i = 0; i < options->ignored_word_count; ++i)
		warn("ignoring invalid x265 option: %s", options->ignored_words[i]);
	for (size_t i = 0; i < options->count; ++i)
		set_param(obsx265, options->options[i]);

	if (!update) {
		info("settings:\n"
		     "\trate_control: %s\n"
		     "\tbitrate:      %d\n"
		     "\tbuffer size:  %d\n"
		     "\tcrf:          %d\n"
		     "\tFPS:          %g (%u/%u)\n"
		     "\twidth:        %d\n"
		     "\theight:       %d\n"
		     "\tkeyint:       %d\n"
		     "\tcolor format: %d\n"
		     "\tbit depth:    %u\n",
		     rate_control, bitrate, buffer_size, crf, obs_encoder_get_effective_fps(obsx265->encoder),
		     obs_encoder_get_fps_num(obsx265->encoder), obs_encoder_get_fps_den(obsx265->encoder) * obs_encoder_get_frame_rate_divisor(obsx265->encoder), width, height,
		     obsx265->params.keyframeMax, (int)obsx265->color_format, bitdepth);
	}

	if (!update) {
		size_t buffer_size = 1;
		for (size_t i = 0; i < options->count; ++i)
			buffer_size += strlen(options->options[i].name) + strlen(options->options[i].value) + 4;

		char *settings_string = bmalloc(buffer_size);
		char *p = settings_string;
		size_t remaining_buffer_size = buffer_size;

		*p++ = '\0';
		remaining_buffer_size--;

		for (size_t i = 0; i < options->count; ++i) {
			int chars_written = snprintf(p, remaining_buffer_size, "\n\t%s = %s", options->options[i].name,
						     options->options[i].value);
			assert(chars_written >= 0);
			assert((size_t)chars_written <= remaining_buffer_size);
			p += chars_written;
			remaining_buffer_size -= (size_t)chars_written;
		}
		assert(remaining_buffer_size == 1);
		assert(*p == '\0');
		info("custom settings: %s", settings_string);
		bfree(settings_string);
	}
}


static bool update_settings(struct obs_x265 *obsx265, obs_data_t *settings, bool update)
{
	char *preset = bstrdup(obs_data_get_string(settings, "preset"));
	char *profile = bstrdup(obs_data_get_string(settings, "profile"));
	char *tune = bstrdup(obs_data_get_string(settings, "tune"));
	struct obs_options options = obs_parse_options(obs_data_get_string(settings, "x265opts"));
	bool repeat_headers = obs_data_get_bool(settings, "repeat_headers");

	bool success = true;

	if (!update)
		blog(LOG_INFO, "---------------------------------");

	if (!obsx265->context) {
		if (preset && *preset)
			info("preset: %s", preset);
		if (profile && *profile)
			info("profile: %s", profile);
		if (tune && *tune)
			info("tune: %s", tune);

		success = reset_x265_params(obsx265, preset, tune);
	}

	if (repeat_headers) {
		obsx265->params.bRepeatHeaders = 1;
	}

	if (success) {
		update_params(obsx265, settings, &options, update);
	}

	obs_free_options(options);
	bfree(preset);
	bfree(profile);
	bfree(tune);

	return success;
}

static bool obs_x265_update(void *data, obs_data_t *settings)
{
	struct obs_x265 *obsx265 = data;

	enum video_format format;
	enum video_colorspace cs;
	enum video_range_type range;
	read_color_settings(settings, &format, &cs, &range);

	enum video_format delivery_fmt;
	int internal_csp, input_csp;
	uint32_t bitdepth;
	bool ok_csp = obs_x265_delivery_csp(format, &delivery_fmt, &internal_csp, &input_csp, &bitdepth);
	if (!ok_csp) {
		err("color format '%s' is not supported by x265; refusing to fall back to another format",
		    obs_data_get_string(settings, "color_format"));
		return false;
	}

	/* Fall back to NV12 if the linked libx265 has no API for the requested bit depth.
	 * A multi-bit-depth build exposes one x265_api per depth via x265_api_get(); a
	 * single-depth build returns NULL for unsupported depths. */
	if (!x265_api_get((int)bitdepth)) {
		warn("requested color format requires %u-bit but libx265 has no %u-bit API; using NV12",
		     bitdepth, bitdepth);
		format = VIDEO_FORMAT_NV12;
		cs = VIDEO_CS_709;
		range = VIDEO_RANGE_PARTIAL;
		obs_x265_delivery_csp(format, &delivery_fmt, &internal_csp, &input_csp, &bitdepth);
	}

	/* Publish the format actually encoded so the MP4 colr box matches the stream's real VUI.
	 * For 8-bit BGRA this is BGRA itself (de-interleaved to planar R/G/B + identity matrix),
	 * so it is tagged raw RGB / identity; other formats publish their planar delivery format. */
	publish_preferred_settings(obsx265->encoder, delivery_fmt, cs, range);

	/* Keep the stored color settings in sync so update_params()/get_video_info() use the
	 * current values. Previously only create() set these, so after a settings change the
	 * published format (colr box) reflected the new value while the VUI stayed stale. */
	obsx265->color_format = format;
	obsx265->color_space = cs;
	obsx265->color_range = range;

	bool csp_changed = obsx265->active_csp != input_csp || obsx265->internal_csp != internal_csp ||
	                        obsx265->active_bitdepth != bitdepth;

	if (csp_changed) {
		info("color format/space/range changed; the new settings apply when the encoder next starts");
		obsx265->active_csp = input_csp;
		obsx265->internal_csp = internal_csp;
		obsx265->active_bitdepth = bitdepth;
	} else {
		bool success = update_settings(obsx265, settings, true);
		int ret = 0;
		if (success) {
			ret = obsx265->api->encoder_reconfig(obsx265->context, &obsx265->params);
			if (ret != 0)
				warn("Failed to reconfigure: %d", ret);
		}
		return success && ret == 0;
	}

	update_settings(obsx265, settings, true);
	return true;
}

static void load_headers(struct obs_x265 *obsx265)
{
	x265_nal *nals;
	uint32_t nal_count;
	DARRAY(uint8_t) header;
	DARRAY(uint8_t) sei;

	da_init(header);
	da_init(sei);

	obsx265->api->encoder_headers(obsx265->context, &nals, &nal_count);

	for (uint32_t i = 0; i < nal_count; i++) {
		x265_nal *nal = nals + i;

		if (nal->type == NAL_UNIT_PREFIX_SEI || nal->type == NAL_UNIT_SUFFIX_SEI)
			da_push_back_array(sei, nal->payload, nal->sizeBytes);
		else
			da_push_back_array(header, nal->payload, nal->sizeBytes);
	}

	obsx265->extra_data = header.array;
	obsx265->extra_data_size = header.num;
	obsx265->sei = sei.array;
	obsx265->sei_size = sei.num;
}


static void *obs_x265_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	struct obs_x265 *obsx265 = bzalloc(sizeof(struct obs_x265));
	obsx265->encoder = encoder;

	enum video_format format;
	enum video_colorspace cs;
	enum video_range_type range;
	read_color_settings(settings, &format, &cs, &range);

	enum video_format delivery_fmt;
	int internal_csp, input_csp;
	uint32_t bitdepth;
	if (!obs_x265_delivery_csp(format, &delivery_fmt, &internal_csp, &input_csp, &bitdepth)) {
		err_enc(encoder, "color format '%s' is not supported by x265; refusing to fall back to another format",
		        obs_data_get_string(settings, "color_format"));
		obs_encoder_set_last_error(encoder, obs_module_text("ColorFormatUnsupported"));
		bfree(obsx265);
		return NULL;
	}

	/* Resolve the x265 API table for the requested bit depth. A multi-bit-depth
	 * libx265 exposes one entry point per depth via x265_api_get(); a single-depth
	 * build returns NULL for unsupported depths, so we fall back to 8-bit/NV12. */
	const x265_api *api = x265_api_get((int)bitdepth);
	if (!api) {
		warn_enc(encoder, "requested color format '%s' requires %u-bit depth but libx265 has no %u-bit API; falling back to NV12",
		        obs_data_get_string(settings, "color_format"), bitdepth, bitdepth);
		format = VIDEO_FORMAT_NV12;
		cs = VIDEO_CS_709;
		range = VIDEO_RANGE_PARTIAL;
		obs_x265_delivery_csp(format, &delivery_fmt, &internal_csp, &input_csp, &bitdepth);
		api = x265_api_get((int)bitdepth);
	}
	if (!api) {
		err_enc(encoder, "libx265 provides no usable API (not even 8-bit); is libx265.dll present?");
		bfree(obsx265);
		return NULL;
	}
	obsx265->api = api;

	obsx265->color_format = format;
	obsx265->color_space = cs;
	obsx265->color_range = range;

	/* Publish the encoded (planar delivery) format so the MP4 colr box matches the
	 * stream's real VUI - see obs_x265_update() for why. */
	publish_preferred_settings(encoder, delivery_fmt, cs, range);
	obsx265->active_csp = input_csp;
	obsx265->internal_csp = internal_csp;
	obsx265->active_bitdepth = bitdepth;

	if (update_settings(obsx265, settings, false)) {
		info("opening x265 encoder: %dx%d, csp=%d, bitDepth=%d, fps=%d/%d, rc=%d",
		     obsx265->params.sourceWidth, obsx265->params.sourceHeight,
		     obsx265->params.internalCsp, obsx265->params.internalBitDepth,
		     obsx265->params.fpsNum, obsx265->params.fpsDenom,
		     obsx265->params.rc.rateControlMode);
		obsx265->context = obsx265->api->encoder_open(&obsx265->params);

		if (obsx265->context == NULL)
			warn("x265 failed to load");
		else {
			load_headers(obsx265);
			/* Record the fps the context was opened with so a later live FPS change
			 * can be detected against it (see obs_x265_check_fps). */
			obsx265->inited_fps_num = obsx265->params.fpsNum;
			obsx265->inited_fps_den = obsx265->params.fpsDenom;
		}
	} else {
		warn("bad settings specified");
	}

	if (!obsx265->context) {
		bfree(obsx265);
		return NULL;
	}

	obsx265->performance_token = os_request_high_performance("x265 encoding");

	return obsx265;
}

static void parse_packet(struct obs_x265 *obsx265, struct encoder_packet *packet, x265_nal *nals, int nal_count,
			 const x265_picture *pic_out)
{
	if (!nal_count)
		return;

	da_resize(obsx265->packet_data, 0);

	for (int i = 0; i < nal_count; i++) {
		x265_nal *nal = nals + i;
		da_push_back_array(obsx265->packet_data, nal->payload, nal->sizeBytes);
	}

	packet->data = obsx265->packet_data.array;
	packet->size = obsx265->packet_data.num;
	packet->type = OBS_ENCODER_VIDEO;
	packet->pts = pic_out->pts;
	/* Use the encoder's decode-order timestamp (reordered pts), not the display
	 * pts. With B-frames the two differ, and stamping DTS with display pts makes
	 * the DTS sequence non-monotonic; the MP4 muxer then computes a bogus track
	 * duration (>UINT32_MAX -> malformed mvhd). x264 does the same via i_dts. */
	packet->dts = pic_out->dts;
	packet->keyframe = IS_X265_TYPE_I(pic_out->sliceType);
}

/* De-interleaves a packed BGRA frame (B,G,R,A per pixel) into three planar u8 buffers
 * [G][B][R] so it can be fed to x265 as YUV 4:4:4 with an identity (GBR) matrix. The
 * scratch is cached on the encoder and only reallocated when the resolution changes, so
 * steady-state frames cost one pass and no allocation. */
static uint8_t *deinterleave_gbra(struct obs_x265 *obsx265, struct encoder_frame *frame, int *stride_out)
{
	const uint32_t width = obs_encoder_get_width(obsx265->encoder);
	const uint32_t height = obs_encoder_get_height(obsx265->encoder);
	const size_t plane_size = (size_t)width * height;

	if (!obsx265->gbr_deinter || obsx265->gbr_deinter_size < 3 * plane_size) {
		bfree(obsx265->gbr_deinter);
		obsx265->gbr_deinter = bzalloc(3 * plane_size);
		obsx265->gbr_deinter_size = 3 * plane_size;
	}

	const uint8_t *src = frame->data[0];
	const int src_stride = (int)frame->linesize[0];
	uint8_t *dG = obsx265->gbr_deinter; /* plane0 = G */
	uint8_t *dR = dG + plane_size;      /* plane1 = R */
	uint8_t *dB = dR + plane_size;      /* plane2 = B */

	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *srow = src + (size_t)y * src_stride;
		for (uint32_t x = 0; x < width; x++) {
			dG[(size_t)y * width + x] = srow[x * 4 + 1]; /* G (BGRA byte 1) */
			dR[(size_t)y * width + x] = srow[x * 4 + 2]; /* R (BGRA byte 2) */
			dB[(size_t)y * width + x] = srow[x * 4 + 0]; /* B (BGRA byte 0) */
		}
	}

	*stride_out = (int)width;
	return obsx265->gbr_deinter;
}

static inline void init_pic_data(struct obs_x265 *obsx265, x265_picture *pic, struct encoder_frame *frame)
{
	obsx265->api->picture_init(&obsx265->params, pic);

	pic->pts = frame->pts;
	pic->colorSpace = obsx265->active_csp;
	pic->bitDepth = (int)obsx265->active_bitdepth;

	if (obsx265->color_format == VIDEO_FORMAT_BGRA) {
		/* 8-bit RGB: OBS delivers packed BGRA. De-interleave to planar G/R/B and feed as
		 * YUV 4:4:4 with an identity matrix (see deinterleave_gbra). */
		const uint32_t width = obs_encoder_get_width(obsx265->encoder);
		const uint32_t height = obs_encoder_get_height(obsx265->encoder);
		const size_t plane_size = (size_t)width * height;
		int stride;
		uint8_t *base = deinterleave_gbra(obsx265, frame, &stride);

		pic->planes[0] = base;                  /* G */
		pic->stride[0] = stride;
		pic->planes[1] = base + plane_size;     /* R */
		pic->stride[1] = stride;
		pic->planes[2] = base + 2 * plane_size; /* B */
		pic->stride[2] = stride;
		return;
	}

	if (obsx265->active_csp == X265_CSP_NV12) {
		pic->planes[0] = frame->data[0];
		pic->stride[0] = (int)frame->linesize[0];
		pic->planes[1] = frame->data[1];
		pic->stride[1] = (int)frame->linesize[1];
	} else if (obsx265->active_csp == X265_CSP_I420) {
		pic->planes[0] = frame->data[0];
		pic->stride[0] = (int)frame->linesize[0];
		pic->planes[1] = frame->data[1];
		pic->stride[1] = (int)frame->linesize[1];
		pic->planes[2] = frame->data[2];
		pic->stride[2] = (int)frame->linesize[2];
	} else if (obsx265->active_csp == X265_CSP_I422) {
		pic->planes[0] = frame->data[0];
		pic->stride[0] = (int)frame->linesize[0];
		pic->planes[1] = frame->data[1];
		pic->stride[1] = (int)frame->linesize[1];
		pic->planes[2] = frame->data[2];
		pic->stride[2] = (int)frame->linesize[2];
	} else if (obsx265->active_csp == X265_CSP_I444) {
		pic->planes[0] = frame->data[0];
		pic->stride[0] = (int)frame->linesize[0];
		pic->planes[1] = frame->data[1];
		pic->stride[1] = (int)frame->linesize[1];
		pic->planes[2] = frame->data[2];
		pic->stride[2] = (int)frame->linesize[2];
	} else if (obsx265->active_csp == X265_CSP_BGRA) {
		pic->planes[0] = frame->data[0];
		pic->stride[0] = (int)frame->linesize[0];
	}
}


/* H.265 uses 16x16 CTUs for quant offset mapping (same as x264's 16x16 MBs) */
static const uint32_t MB_SIZE = 16;

struct roi_params {
	uint32_t mb_width;
	uint32_t mb_height;
	float *map;
};

static void roi_cb(void *param, struct obs_encoder_roi *roi)
{
	const struct roi_params *rp = param;

	const uint32_t roi_left = roi->left / MB_SIZE;
	const uint32_t roi_top = roi->top / MB_SIZE;
	const uint32_t roi_right = (roi->right - 1) / MB_SIZE;
	const uint32_t roi_bottom = (roi->bottom - 1) / MB_SIZE;
	/* QP range is 0..51 */
	const float qp_offset = -51.0f * roi->priority;

	for (uint32_t mb_y = 0; mb_y < rp->mb_height; mb_y++) {
		if (mb_y < roi_top || mb_y > roi_bottom)
			continue;

		for (uint32_t mb_x = 0; mb_x < rp->mb_width; mb_x++) {
			if (mb_x < roi_left || mb_x > roi_right)
				continue;

			rp->map[mb_y * rp->mb_width + mb_x] = qp_offset;
		}
	}
}

static void add_roi(struct obs_x265 *obsx265, x265_picture *pic)
{
	const uint32_t increment = obs_encoder_get_roi_increment(obsx265->encoder);

	if (obsx265->quant_offsets && obsx265->roi_increment == increment) {
		pic->quantOffsets = obsx265->quant_offsets;
		return;
	}

	const uint32_t width = obs_encoder_get_width(obsx265->encoder);
	const uint32_t height = obs_encoder_get_height(obsx265->encoder);
	const uint32_t mb_width = (width + MB_SIZE - 1) / MB_SIZE;
	const uint32_t mb_height = (height + MB_SIZE - 1) / MB_SIZE;
	const size_t map_size = sizeof(float) * mb_width * mb_height;

	float *map = bzalloc(map_size);

	struct roi_params par = {mb_width, mb_height, map};

	obs_encoder_enum_roi(obsx265->encoder, roi_cb, &par);

	pic->quantOffsets = map;
	obsx265->quant_offsets = map;
	obsx265->roi_increment = increment;
}

/* Detect a live effective-FPS change (base rate or frame-rate divisor) and force
 * x265 to re-initialise so its ABR per-frame bit budget is rebuilt for the new rate.
 * libobs does not call update() when only the frame-rate divisor changes, and even
 * when it does, x265 encoder_reconfig does NOT rebuild the rate control's fps-derived
 * state (reconfig cannot rewrite the SPS VUI either - see the csp-change path above).
 * Left alone the encoder keeps targeting bits/frame for the old rate, so the total
 * bitrate scales with the delivery rate (e.g. 30->60 fps doubles it, 30->15 halves it).
 * Re-opening the context re-runs the rate-control init with the new fps and emits a
 * fresh IDR + VPS/SPS/PPS. Output timing is driven by the caller's pts (frame->pts),
 * which libobs keeps continuous via the frame-rate divisor, so DTS/PTS do not regress
 * across the re-init. This runs on the encode thread - the same thread libobs uses for
 * deferred update() - so no extra lock is needed. */
static void obs_x265_reinit_fps(struct obs_x265 *obsx265)
{
	/* params.fpsNum/fpsDenom and keyframeMax are already updated by the caller. */
	obsx265->api->encoder_close(obsx265->context);
	obsx265->context = NULL;
	bfree(obsx265->extra_data);
	obsx265->extra_data = NULL;
	bfree(obsx265->sei);
	obsx265->sei = NULL;

	obsx265->context = obsx265->api->encoder_open(&obsx265->params);
	if (!obsx265->context) {
		warn("failed to re-initialise x265 after live FPS change");
		return;
	}
	load_headers(obsx265);
}

static void obs_x265_check_fps(struct obs_x265 *obsx265)
{
	uint32_t num = obs_encoder_get_fps_num(obsx265->encoder);
	uint32_t den = obs_encoder_get_fps_den(obsx265->encoder) *
	                obs_encoder_get_frame_rate_divisor(obsx265->encoder);

	if (num == 0 || den == 0)
		return;
	/* Compare against the fps the context was actually opened with, not params.fpsNum:
	 * a deferred update() may already have advanced params.fpsNum via reconfig without
	 * rebuilding the rate control, in which case a params-based comparison would miss
	 * the change and the re-init would never fire. */
	if (num == obsx265->inited_fps_num && den == obsx265->inited_fps_den)
		return;

	info("live FPS change %u/%u -> %u/%u, re-initialising x265", obsx265->inited_fps_num,
	     obsx265->inited_fps_den, num, den);

	/* Keep the keyframe interval (in seconds) constant across the rate change. When the
	 * user set an explicit interval in seconds, recompute it for the new rate directly
	 * (order-independent of any deferred update()); otherwise scale the current frame
	 * count from the fps the context was opened with. */
	if (obsx265->keyint_sec > 0) {
		obsx265->params.keyframeMax = obsx265->keyint_sec * (int)num / (int)den;
	} else if (obsx265->params.keyframeMax > 0 && obsx265->inited_fps_num != 0) {
		int64_t knew = (int64_t)obsx265->params.keyframeMax * obsx265->inited_fps_den * num /
		               ((int64_t)obsx265->inited_fps_num * den);
		if (knew > 0)
			obsx265->params.keyframeMax = (int)knew;
	}

	obsx265->params.fpsNum = num;
	obsx265->params.fpsDenom = den;

	obs_x265_reinit_fps(obsx265);
	obsx265->inited_fps_num = num;
	obsx265->inited_fps_den = den;
}

static bool obs_x265_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
			     bool *received_packet)
{
	struct obs_x265 *obsx265 = data;
	x265_nal *nals;
	uint32_t nal_count;
	int ret;
	x265_picture pic, pic_out;

	if (!frame || !packet || !received_packet)
		return false;

	/* A failed live-FPS re-init leaves the context NULL; do not hammer a broken
	 * encoder (and avoid retrying encoder_open every frame). */
	if (!obsx265->context)
		return false;

	obs_x265_check_fps(obsx265);

	init_pic_data(obsx265, &pic, frame);

	if (obs_encoder_has_roi(obsx265->encoder))
		add_roi(obsx265, &pic);

	ret = obsx265->api->encoder_encode(obsx265->context, &nals, &nal_count, (frame ? &pic : NULL), &pic_out);
	if (ret < 0) {
		warn("encode failed");
		return false;
	}

	*received_packet = (nal_count != 0);
	parse_packet(obsx265, packet, nals, nal_count, &pic_out);

	return true;
}

static bool obs_x265_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct obs_x265 *obsx265 = data;

	if (!obsx265->context)
		return false;

	*extra_data = obsx265->extra_data;
	*size = obsx265->extra_data_size;
	return true;
}

static bool obs_x265_sei(void *data, uint8_t **sei, size_t *size)
{
	struct obs_x265 *obsx265 = data;

	if (!obsx265->context)
		return false;

	*sei = obsx265->sei;
	*size = obsx265->sei_size;
	return true;
}

static void obs_x265_video_info(void *data, struct video_scale_info *info)
{
	struct obs_x265 *obsx265 = data;
	enum video_format delivery_fmt;
	int internal_csp, input_csp;
	uint32_t bitdepth;

	if (!obs_x265_delivery_csp(obsx265->color_format, &delivery_fmt, &internal_csp, &input_csp, &bitdepth)) {
		err("unsupported color format (%d): x265 cannot encode it; delivering NV12 as last resort",
		    (int)obsx265->color_format);
		delivery_fmt = VIDEO_FORMAT_NV12;
	}
	info->format = delivery_fmt;
	info->colorspace = obsx265->color_space;
	info->range = obsx265->color_range;
}

struct obs_encoder_info obs_x265_encoder = {
	.id = "obs_x265",
	.type = OBS_ENCODER_VIDEO,
	.codec = "hevc",
	.get_name = obs_x265_getname,
	.create = obs_x265_create,
	.destroy = obs_x265_destroy,
	.encode = obs_x265_encode,
	.update = obs_x265_update,
	.get_properties = obs_x265_props,
	.get_defaults = obs_x265_defaults,
	.get_extra_data = obs_x265_extra_data,
	.get_sei_data = obs_x265_sei,
	.get_video_info = obs_x265_video_info,
	.is_color_format_supported = x265_is_color_format_supported,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_ROI,
};

