/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

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
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>
#include <obs-module.h>
#include <opts-parser.h>

#ifndef _STDINT_H_INCLUDED
#define _STDINT_H_INCLUDED
#endif

#include <x264.h>

#define do_log_enc(level, encoder, format, ...) \
	blog(level, "[x264 encoder: '%s'] " format, obs_encoder_get_name(encoder), ##__VA_ARGS__)
#define do_log(level, format, ...) do_log_enc(level, obsx264->encoder, format, ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define warn_enc(encoder, format, ...) do_log_enc(LOG_WARNING, encoder, format, ##__VA_ARGS__)
#define err(format, ...) do_log(LOG_ERROR, format, ##__VA_ARGS__)
#define err_enc(encoder, format, ...) do_log_enc(LOG_ERROR, encoder, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

//#define ENABLE_VFR

/* ------------------------------------------------------------------------- */

struct obs_x264 {
	obs_encoder_t *encoder;

	x264_param_t params;
	x264_t *context;

	DARRAY(uint8_t) packet_data;

	uint8_t *extra_data;
	uint8_t *sei;

	size_t extra_data_size;
	size_t sei_size;

	os_performance_token_t *performance_token;

	uint32_t roi_increment;
	float *quant_offsets;

	/* Color settings chosen by the user (Option 1). These drive both the
	 * x264 context parameters and get_video_info, so libobs delivers frames
	 * in exactly this format/space/range. Changes apply on next start. */
	enum video_format color_format;
	enum video_colorspace color_space;
	enum video_range_type color_range;

	/* The csp/bitdepth the x264 context is currently built for (from create).
	 * Used to detect settings changes in update() and keep live encode data
	 * consistent with what libobs delivers until a restart. */
	int active_csp;
	uint32_t active_bitdepth;

	/* R10I (RGB 10-bit): libobs delivers three planar u16 textures [G][B][R]
	 * (VIDEO_FORMAT_R10P), which x264 ingests directly as YUV444P | HIGH_DEPTH -
	 * no per-frame scratch/unpack is needed on this side. */
};

/* ------------------------------------------------------------------------- */

static enum video_format color_format_from_name(const char *name)
{
	if (!name || strcmp(name, "NV12") == 0)
		return VIDEO_FORMAT_NV12;
	else if (strcmp(name, "P010") == 0)
		return VIDEO_FORMAT_P010;
	else if (strcmp(name, "I444") == 0)
		return VIDEO_FORMAT_I444;
	else if (strcmp(name, "P216") == 0)
		return VIDEO_FORMAT_P216;
	else if (strcmp(name, "BGRA") == 0)
		return VIDEO_FORMAT_BGRA;
	else if (strcmp(name, "R10I") == 0)
		return VIDEO_FORMAT_R10L; /* RGB 10-bit: libobs delivers planar G/B/R u16 (R10P) */
	else if (strcmp(name, "R10P") == 0)
		return VIDEO_FORMAT_R10P; /* planar RGB 10-bit: delivered as [G][B][R] u16 planes directly */
	else if (strcmp(name, "I412") == 0)
		return VIDEO_FORMAT_I412; /* planar YUV 4:4:4 u16 (YUV444P12LE); x264 I444 + HIGH_DEPTH */
	else if (strcmp(name, "I420") == 0)
		return VIDEO_FORMAT_NV12; /* legacy: planar 4:2:0 collapses to the same NV12 stream */
	else if (strcmp(name, "I010") == 0)
		return VIDEO_FORMAT_P010; /* legacy: planar P010 collapses to the same P010 stream */

	return VIDEO_FORMAT_NONE; /* unrecognized value: create/update rejects it loudly instead of silently downgrading to NV12 */
}

/* Maps an arbitrary base (Advanced -> Video) format onto one x264 can actually encode,
 * so that a "Default" color-format choice still produces a valid encoder context. */
static enum video_format map_base_format_for_x264(enum video_format fmt)
{
	switch (fmt) {
	case VIDEO_FORMAT_NV12:
		return VIDEO_FORMAT_NV12;
	case VIDEO_FORMAT_P010:
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_Y410: /* packed YUV 4:2:0 10-bit -> lossless as planar I010 (x264 I420 | HIGH_DEPTH) */
		return VIDEO_FORMAT_P010;
	case VIDEO_FORMAT_I444:
		return VIDEO_FORMAT_I444;
	case VIDEO_FORMAT_I412:
		return VIDEO_FORMAT_I412; /* planar 4:4:4 u16, x264 encodes it as I444 + HIGH_DEPTH */
	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416: /* packed YUV 4:2:2 10-bit -> lossless as planar I210 (x264 I422 | HIGH_DEPTH) */
		return VIDEO_FORMAT_P216;
	case VIDEO_FORMAT_R10L:
	case VIDEO_FORMAT_R10P: /* RGB 10-bit -> lossless as planar [G][B][R] u16 (x264 I444 | HIGH_DEPTH) */
		return VIDEO_FORMAT_R10P;
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_AYUV:
		return VIDEO_FORMAT_BGRA; /* RGB-family content is stored as raw GBR components by x264 */
	default:
		return VIDEO_FORMAT_NV12; /* planar 8-bit (I420/I422, etc.) collapses to NV12 */
	}
}

/* Publishes the effective color settings on the encoder so consumers that inspect
 * the encoder (e.g. the mp4 muxer writing its nclx box) see exactly what x264
 * encodes instead of only the base video info. */
static void publish_preferred_settings(obs_encoder_t *encoder, enum video_format format, enum video_colorspace cs,
				       enum video_range_type range)
{
	obs_encoder_set_preferred_video_format(encoder, format);
	obs_encoder_set_preferred_color_space(encoder, cs);
	obs_encoder_set_preferred_range(encoder, range);
}

/* Reads the user's per-output color settings and resolves them into a concrete
 * format/space/range. "Default" entries fall back to the base video info (Advanced -> Video).
 * If any one of the three is explicitly chosen, all three are resolved (explicit value
 * or base) forming a complete override that wins over the global video settings. */
static void read_color_settings(obs_data_t *settings, enum video_format *format, enum video_colorspace *cs,
				enum video_range_type *range)
{
	struct obs_video_info ovi;
	bool have_base = obs_get_video_info(&ovi);

	const char *fmt_name = obs_data_get_string(settings, "color_format");
	bool fmt_explicit = !obs_data_has_user_value(settings, "color_format") || strcmp(fmt_name, "Default") != 0;
	enum video_colorspace cs_val = (enum video_colorspace)obs_data_get_int(settings, "color_space");
	enum video_range_type rg_val = (enum video_range_type)obs_data_get_int(settings, "color_range");

	if (!have_base) {
		/* Base info unavailable: honor explicit picks, otherwise plain defaults */
		*format = fmt_explicit ? color_format_from_name(fmt_name) : VIDEO_FORMAT_NV12;
		*cs = cs_val == VIDEO_CS_DEFAULT ? VIDEO_CS_709 : cs_val;
		*range = rg_val == VIDEO_RANGE_DEFAULT ? VIDEO_RANGE_PARTIAL : rg_val;
		return;
	}

	enum video_format fmt = map_base_format_for_x264(ovi.output_format);
	enum video_colorspace spc = ovi.colorspace;
	enum video_range_type rg = ovi.range;

	if (fmt_explicit)
		fmt = color_format_from_name(fmt_name);
	if (cs_val != VIDEO_CS_DEFAULT)
		spc = cs_val;
	if (rg_val != VIDEO_RANGE_DEFAULT)
		rg = rg_val;

	*format = fmt;
	*cs = spc;
	*range = rg;
}

static const char *obs_x264_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "x264";
}

static void clear_data(struct obs_x264 *obsx264)
{
	if (obsx264->context) {
		x264_encoder_close(obsx264->context);
		bfree(obsx264->sei);
		bfree(obsx264->extra_data);
		bfree(obsx264->quant_offsets);

		obsx264->context = NULL;
		obsx264->sei = NULL;
		obsx264->extra_data = NULL;
	}
}

static void obs_x264_destroy(void *data)
{
	struct obs_x264 *obsx264 = data;

	if (obsx264) {
		os_end_high_performance(obsx264->performance_token);
		clear_data(obsx264);
		da_free(obsx264->packet_data);
		bfree(obsx264);
	}
}

static void obs_x264_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_bool(settings, "use_bufsize", false);
	obs_data_set_default_int(settings, "buffer_size", 2500);
	obs_data_set_default_int(settings, "keyint_sec", 0);
	obs_data_set_default_int(settings, "crf", 23);
#ifdef ENABLE_VFR
	obs_data_set_default_bool(settings, "vfr", false);
#endif
	obs_data_set_default_string(settings, "rate_control", "CBR");

	obs_data_set_default_string(settings, "preset", "veryfast");
	obs_data_set_default_string(settings, "profile", "");
	obs_data_set_default_string(settings, "tune", "");
	obs_data_set_default_string(settings, "x264opts", "");
	obs_data_set_default_bool(settings, "repeat_headers", false);

	/* Color settings (Option 1): per-output color format/space/range */
	obs_data_set_default_string(settings, "color_format", "NV12");
	obs_data_set_default_int(settings, "color_space", VIDEO_CS_DEFAULT);
	obs_data_set_default_int(settings, "color_range", VIDEO_RANGE_DEFAULT);
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
#define TEXT_VFR obs_module_text("VFR")
#define TEXT_CRF obs_module_text("CRF")
#define TEXT_KEYINT_SEC obs_module_text("KeyframeIntervalSec")
#define TEXT_PRESET obs_module_text("CPUPreset")
#define TEXT_PROFILE obs_module_text("Profile")
#define TEXT_TUNE obs_module_text("Tune")
#define TEXT_NONE obs_module_text("None")
#define TEXT_X264_OPTS obs_module_text("EncoderOptions")
#define TEXT_COLOR_FORMAT obs_module_text("ColorFormat")
#define TEXT_COLOR_SPACE obs_module_text("ColorSpace")
#define TEXT_COLOR_RANGE obs_module_text("ColorRange")

static bool use_bufsize_modified(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	bool use_bufsize = obs_data_get_bool(settings, "use_bufsize");
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool rc_crf = astrcmpi(rc, "CRF") == 0;

	p = obs_properties_get(ppts, "buffer_size");
	obs_property_set_visible(p, use_bufsize && !rc_crf);
	return true;
}

static bool rate_control_modified(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool use_bufsize = obs_data_get_bool(settings, "use_bufsize");
	bool abr = astrcmpi(rc, "CBR") == 0 || astrcmpi(rc, "ABR") == 0;
	bool rc_crf = astrcmpi(rc, "CRF") == 0;

	p = obs_properties_get(ppts, "crf");
	obs_property_set_visible(p, !abr);

	p = obs_properties_get(ppts, "bitrate");
	obs_property_set_visible(p, !rc_crf);
	p = obs_properties_get(ppts, "use_bufsize");
	obs_property_set_visible(p, !rc_crf);
	p = obs_properties_get(ppts, "buffer_size");
	obs_property_set_visible(p, !rc_crf && use_bufsize);
	return true;
}

static bool color_format_modified(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const char *format = obs_data_get_string(settings, "color_format");
	bool bgra = strcmp(format, "BGRA") == 0;

	/* x264 stores BGRA as raw RGB components (G/B/R in the Y/Cb/Cr slots), no
	 * YUV conversion: only GBR/identity matrix + full range is valid for it.
	 * Disable the two dropdowns when BGRA is selected so an invalid combo
	 * cannot be chosen; update_params() forces the same values regardless. */
	p = obs_properties_get(ppts, "color_space");
	obs_property_set_enabled(p, !bgra);

	p = obs_properties_get(ppts, "color_range");
	obs_property_set_enabled(p, !bgra);
	return true;
}

static void add_cs_entry(obs_property_t *list, const char *name, int value)
{
	obs_property_list_add_int(list, name, (long long)value);
}

static obs_properties_t *obs_x264_props(void *unused)
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

	obs_property_set_modified_callback(list, rate_control_modified);

	p = obs_properties_add_int(props, "bitrate", TEXT_BITRATE, 50, 10000000, 50);
	obs_property_int_set_suffix(p, " Kbps");

	p = obs_properties_add_bool(props, "use_bufsize", TEXT_CUSTOM_BUF);
	obs_property_set_modified_callback(p, use_bufsize_modified);
	obs_properties_add_int(props, "buffer_size", TEXT_BUF_SIZE, 0, 10000000, 1);

	obs_properties_add_int(props, "crf", TEXT_CRF, 0, 51, 1);

	p = obs_properties_add_int(props, "keyint_sec", TEXT_KEYINT_SEC, 0, 20, 1);
	obs_property_int_set_suffix(p, " s");

	list = obs_properties_add_list(props, "preset", TEXT_PRESET, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	add_strings(list, x264_preset_names);

	list = obs_properties_add_list(props, "profile", TEXT_PROFILE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, TEXT_NONE, "");
	obs_property_list_add_string(list, "baseline", "baseline");
	obs_property_list_add_string(list, "main", "main");
	obs_property_list_add_string(list, "high", "high");

	list = obs_properties_add_list(props, "tune", TEXT_TUNE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, TEXT_NONE, "");
	add_strings(list, x264_tune_names);

#ifdef ENABLE_VFR
	obs_properties_add_bool(props, "vfr", TEXT_VFR);
#endif

	/* Color format / color space / color range (Option 1) */
	list = obs_properties_add_list(props, "color_format", TEXT_COLOR_FORMAT, OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, "(Default - Advanced -> Video)", "Default");
	obs_property_list_add_string(list, "NV12 (8-bit)", "NV12");
	obs_property_list_add_string(list, "P010 (10-bit)", "P010");
	obs_property_list_add_string(list, "I444 (8-bit)", "I444");
	obs_property_list_add_string(list, "I412 (YUV 4:4:4, 10-bit planar)", "I412");
	obs_property_list_add_string(list, "P216 (10-bit)", "P216");
	obs_property_list_add_string(list, "BGRA (RGB 8-bit)", "BGRA");
	/* Planar RGB 10-bit replaces the removed packed R10l ("R10I") option; that name still maps to a
	 * working planar path here so profiles saved before it was dropped keep encoding. */
	obs_property_list_add_string(list, "R10p (planar RGB 10-bit)", "R10P");

	list = obs_properties_add_list(props, "color_space", TEXT_COLOR_SPACE, OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	add_cs_entry(list, "(Default)", VIDEO_CS_DEFAULT);
	add_cs_entry(list, "Rec. 709", VIDEO_CS_709);
	add_cs_entry(list, "Rec. 601", VIDEO_CS_601);
	add_cs_entry(list, "sRGB", VIDEO_CS_SRGB);

	list = obs_properties_add_list(props, "color_range", TEXT_COLOR_RANGE, OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, "(Default)", (long long)VIDEO_RANGE_DEFAULT);
	obs_property_list_add_int(list, "Partial (Limited)", (long long)VIDEO_RANGE_PARTIAL);
	obs_property_list_add_int(list, "Full", (long long)VIDEO_RANGE_FULL);

	/* BGRA locks the color space / range dropdowns to their only valid combo */
	obs_property_set_modified_callback(obs_properties_get(props, "color_format"), color_format_modified);

	obs_properties_add_text(props, "x264opts", TEXT_X264_OPTS, OBS_TEXT_DEFAULT);

	headers = obs_properties_add_bool(props, "repeat_headers", "repeat_headers");
	obs_property_set_visible(headers, false);

	return props;
}

static const char *validate(struct obs_x264 *obsx264, const char *val, const char *name, const char *const *list)
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

static void override_base_param(struct obs_x264 *obsx264, struct obs_option option, char **preset, char **profile,
				char **tune)
{
	const char *name = option.name;
	const char *val = option.value;
	if (astrcmpi(name, "preset") == 0) {
		const char *valid_name = validate(obsx264, val, "preset", x264_preset_names);
		if (valid_name) {
			bfree(*preset);
			*preset = bstrdup(val);

		} else if (astrcmpi(name, "profile") == 0) {
			const char *valid_name = validate(obsx264, val, "profile", x264_profile_names);
			if (valid_name) {
				bfree(*profile);
				*profile = bstrdup(val);

			} else if (astrcmpi(name, "tune") == 0) {
				const char *valid_name = validate(obsx264, val, "tune", x264_tune_names);
				if (valid_name) {
					bfree(*tune);
					*tune = bstrdup(val);
				}
			}
		}
	}
}

static inline void override_base_params(struct obs_x264 *obsx264, const struct obs_options *options, char **preset,
					char **profile, char **tune)
{
	for (size_t i = 0; i < options->count; ++i)
		override_base_param(obsx264, options->options[i], preset, profile, tune);
}

#define OPENCL_ALIAS "opencl_is_experimental_and_potentially_unstable"

static inline void set_param(struct obs_x264 *obsx264, struct obs_option option)
{
	const char *name = option.name;
	const char *val = option.value;
	if (strcmp(name, "preset") != 0 && strcmp(name, "profile") != 0 && strcmp(name, "tune") != 0 &&
	    strcmp(name, "fps") != 0 && strcmp(name, "force-cfr") != 0 && strcmp(name, "width") != 0 &&
	    strcmp(name, "height") != 0 && strcmp(name, "opencl") != 0 && strcmp(name, "stats") != 0 &&
	    strcmp(name, "qpfile") != 0 && strcmp(name, "pass") != 0) {
		if (strcmp(option.name, OPENCL_ALIAS) == 0)
			name = "opencl";
		if (x264_param_parse(&obsx264->params, name, val) != 0)
			warn("x264 param: %s=%s failed", name, val);
	}
}

static inline void apply_x264_profile(struct obs_x264 *obsx264, const char *profile)
{
	if (!obsx264->context && profile && *profile) {
		int ret = x264_param_apply_profile(&obsx264->params, profile);
		if (ret != 0)
			warn("Failed to set x264 profile '%s'", profile);
	}
}

static inline const char *validate_preset(struct obs_x264 *obsx264, const char *preset)
{
	const char *new_preset = validate(obsx264, preset, "preset", x264_preset_names);
	return new_preset ? new_preset : "veryfast";
}

static bool reset_x264_params(struct obs_x264 *obsx264, const char *preset, const char *tune)
{
	int ret = x264_param_default_preset(&obsx264->params, validate_preset(obsx264, preset),
					    validate(obsx264, tune, "tune", x264_tune_names));
	return ret == 0;
}

static void log_x264(void *param, int level, const char *format, va_list args)
{
	static const int level_map[] = {
		LOG_ERROR,
		LOG_WARNING,
		LOG_INFO,
		LOG_DEBUG,
	};

	UNUSED_PARAMETER(param);
	if (level < X264_LOG_ERROR)
		level = X264_LOG_ERROR;
	else if (level > X264_LOG_DEBUG)
		level = X264_LOG_DEBUG;

	blogva(level_map[level], format, args);
}

static inline int get_x264_cs_val(const char *const name, const char *const names[])
{
	int idx = 0;
	do {
		if (strcmp(names[idx], name) == 0)
			return idx;
	} while (!!names[++idx]);

	return 0;
}

static void obs_x264_video_info(void *data, struct video_scale_info *info);

enum rate_control { RATE_CONTROL_CBR, RATE_CONTROL_VBR, RATE_CONTROL_ABR, RATE_CONTROL_CRF };

/* Maps the user's chosen color format to the x264 csp + bitdepth.
 * Returns false for formats x264 cannot encode (e.g. Y410/P416). */
static bool obs_x264_format_to_csp(enum video_format format, int *csp, uint32_t *bitdepth)
{
	switch (format) {
	case VIDEO_FORMAT_NV12:
		*csp = X264_CSP_NV12;
		*bitdepth = 8;
		break;
	case VIDEO_FORMAT_P010:
		*csp = X264_CSP_NV12; /* x264 uses NV12 csp with i_bitdepth=10 for P010 */
		*bitdepth = 10;
		break;
	case VIDEO_FORMAT_I444:
		*csp = X264_CSP_I444;
		*bitdepth = 8;
		break;
	case VIDEO_FORMAT_I412:
		*csp = X264_CSP_I444; /* planar Y/U/V u16 (YUV444P12LE container); HIGH_DEPTH via i_bitdepth */
		*bitdepth = 10;
		break;
	case VIDEO_FORMAT_R10P:
		*csp = X264_CSP_I444; /* planar G/B/R u16 (R10p delivery); HIGH_DEPTH via i_bitdepth */
		*bitdepth = 10;
		break;
	case VIDEO_FORMAT_P216:
		*csp = X264_CSP_NV16; /* x264 uses NV16 csp with i_bitdepth=10 for P216 */
		*bitdepth = 10;
		break;
	case VIDEO_FORMAT_BGRA:
		*csp = X264_CSP_BGRA;
		*bitdepth = 8;
		break;
	default:
		return false; /* Y410, P416, etc. are not supported by x264 */
	}
	return true;
}

/* Maps the user's chosen color format to (a) the libobs delivery format and
 * (b) the x264 csp/bitdepth. The packed 10-bit formats are delivered in their
 * PLANAR equivalents - P010 as YUV420P10LE (I010), P216 as YUV422P10LE (I210) -
 * NOT interleaved: libobs's ffmpeg build writes/reads the P010LE path in a
 * non-standard cv8x256 fixed-point convention that corrupts 8-bit chroma, so x264
 * would receive invalid values. Planar delivery carries proper [0..1023] values and
 * x264 interleaves U/V itself during its frame copy (I420 + HIGH_DEPTH) - the same
 * input layout ffmpeg's own libx264 wrapper uses for yuv420p10. Formats x264 can encode
 * directly from planar delivery (NV12, I444, and I412 = YUV444P12LE) pass through
 * unchanged - the plugin does no conversion; x264 ingests libobs's planes as-is. */
static bool obs_x264_delivery_csp(enum video_format fmt, enum video_format *delivery,
		int *csp, uint32_t *bitdepth)
{
	switch (fmt) {
	case VIDEO_FORMAT_P010:
		*delivery = VIDEO_FORMAT_I010;
		*csp = X264_CSP_I420; /* planar Y/U/V, interleaved internally by x264 */
		*bitdepth = 10;
		return true;
	case VIDEO_FORMAT_P216:
		*delivery = VIDEO_FORMAT_I210;
		*csp = X264_CSP_I422; /* planar Y/U/V 4:2:2, interleaved internally by x264 */
		*bitdepth = 10;
		return true;
	case VIDEO_FORMAT_R10L:
		/* R10I (RGB 10-bit): libobs delivers three full-resolution planar u16 textures
		 * [G][B][R] as the R10P format. x264 ingests them as YUV444P | HIGH_DEPTH with
		 * G→plane[0], B→plane[1], R→plane[2] — the same values/planes the old packed
		 * X2BGR10LE path produced, minus the per-frame CPU unpack. */
		*delivery = VIDEO_FORMAT_R10P;
		*csp = X264_CSP_I444;
		*bitdepth = 10;
		return true;
	case VIDEO_FORMAT_R10P:
		/* R10p (planar RGB 10-bit): libobs delivers the three full-resolution planar u16
		 * textures [G][B][R] as-is; x264 ingests them directly. */
		*delivery = VIDEO_FORMAT_R10P;
		*csp = X264_CSP_I444;
		*bitdepth = 10;
		return true;
	default:
		if (!obs_x264_format_to_csp(fmt, csp, bitdepth))
			return false;
		*delivery = fmt;
		return true;
	}
}

static void update_params(struct obs_x264 *obsx264, obs_data_t *settings, const struct obs_options *options,
			  bool update)
{
	video_t *video = obs_encoder_video(obsx264->encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	struct video_scale_info info;

	/* The user's chosen color settings drive the x264 params. libobs will
	 * deliver frames in exactly this format/space/range (see get_video_info). */
	info.format = obsx264->color_format;
	info.colorspace = obsx264->color_space;
	info.range = obsx264->color_range;

	obs_x264_video_info(obsx264, &info);

	const char *rate_control = obs_data_get_string(settings, "rate_control");

	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	int buffer_size = (int)obs_data_get_int(settings, "buffer_size");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	int crf = (int)obs_data_get_int(settings, "crf");
	int width = (int)obs_encoder_get_width(obsx264->encoder);
	int height = (int)obs_encoder_get_height(obsx264->encoder);
	int bf = (int)obs_data_get_int(settings, "bf");
	bool use_bufsize = obs_data_get_bool(settings, "use_bufsize");
	bool cbr_override = obs_data_get_bool(settings, "cbr");
	enum rate_control rc;

#ifdef ENABLE_VFR
	bool vfr = obs_data_get_bool(settings, "vfr");
#endif

	/* XXX: "cbr" setting has been deprecated */
	if (cbr_override) {
		warn("\"cbr\" setting has been deprecated for all encoders!  "
		     "Please set \"rate_control\" to \"CBR\" instead.  "
		     "Forcing CBR mode.  "
		     "(Note to all: this is why you shouldn't use strings for "
		     "common settings)");
		rate_control = "CBR";
	}

	if (astrcmpi(rate_control, "ABR") == 0) {
		rc = RATE_CONTROL_ABR;
		crf = 0;

	} else if (astrcmpi(rate_control, "VBR") == 0) {
		rc = RATE_CONTROL_VBR;

	} else if (astrcmpi(rate_control, "CRF") == 0) {
		rc = RATE_CONTROL_CRF;
		bitrate = 0;
		buffer_size = 0;

	} else { /* CBR */
		rc = RATE_CONTROL_CBR;
		crf = 0;
	}

	if (keyint_sec)
		obsx264->params.i_keyint_max = keyint_sec * voi->fps_num / voi->fps_den;

	if (!use_bufsize)
		buffer_size = bitrate;

#ifdef ENABLE_VFR
	obsx264->params.b_vfr_input = vfr;
#else
	obsx264->params.b_vfr_input = false;
#endif
	obsx264->params.rc.i_vbv_max_bitrate = bitrate;
	obsx264->params.rc.i_vbv_buffer_size = buffer_size;
	obsx264->params.rc.i_bitrate = bitrate;
	obsx264->params.i_width = width;
	obsx264->params.i_height = height;
	obsx264->params.i_fps_num = voi->fps_num;
	obsx264->params.i_fps_den = voi->fps_den;
	obsx264->params.i_timebase_num = voi->fps_den;
	obsx264->params.i_timebase_den = voi->fps_num;
	obsx264->params.pf_log = log_x264;
	obsx264->params.p_log_private = obsx264;
	obsx264->params.i_log_level = X264_LOG_WARNING;

	if (obs_data_has_user_value(settings, "bf"))
		obsx264->params.i_bframe = bf;

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
	default:
		break;
	}

	/* BGRA and R10I are stored by x264 as raw RGB components (G/B/R in the Y/Cb/Cr slots,
	 * alpha discarded for BGRA) with no YUV conversion. The only valid VUI for that data
	 * is identity (GBR) colormatrix + full range: a bt.709 matrix or limited
	 * range would be wrong for it. Force both regardless of the user's picks. */
	if (info.format == VIDEO_FORMAT_BGRA || info.format == VIDEO_FORMAT_R10L ||
	    info.format == VIDEO_FORMAT_R10P) {
		colmatrix =
			"GBR"; /* SPS col_matrix_coef 0, identity matrix (x264_colmatrix_names[0]): R=plane[2], G=plane[0](luma slot), B=plane[1] */
	}

	obsx264->params.vui.i_sar_height = 1;
	obsx264->params.vui.i_sar_width = 1;
	bool is_rgb_family = (info.format == VIDEO_FORMAT_BGRA) || (info.format == VIDEO_FORMAT_R10L) ||
		              (info.format == VIDEO_FORMAT_R10P);
	obsx264->params.vui.b_fullrange = is_rgb_family || info.range == VIDEO_RANGE_FULL;
	if (is_rgb_family) {
		/* Raw RGB components: no YUV transform, so primaries/transfer are unspecified
		 * (all-zero VUI fields) - the same signalling a standalone x264 RGB encode produces. */
		obsx264->params.vui.i_colorprim = 0;
		obsx264->params.vui.i_transfer = 0;
	} else {
		obsx264->params.vui.i_colorprim = get_x264_cs_val(colorprim, x264_colorprim_names);
		obsx264->params.vui.i_transfer = get_x264_cs_val(transfer, x264_transfer_names);
	}
	obsx264->params.vui.i_colmatrix = get_x264_cs_val(colmatrix, x264_colmatrix_names);

	/* use the new filler method for CBR to allow real-time adjusting of
	 * the bitrate */
	if (rc == RATE_CONTROL_CBR || rc == RATE_CONTROL_ABR) {
		obsx264->params.rc.i_rc_method = X264_RC_ABR;

		if (rc == RATE_CONTROL_CBR) {
#if X264_BUILD >= 139
			obsx264->params.rc.b_filler = true;
#else
			obsx264->params.i_nal_hrd = X264_NAL_HRD_CBR;
#endif
		}
	} else {
		obsx264->params.rc.i_rc_method = X264_RC_CRF;
		obsx264->params.rc.f_rf_constant = (float)crf;
	}

	/* Derive x264 csp/bitdepth from the user's chosen color format */
	enum video_format delivery_fmt;
	int csp;
	uint32_t bitdepth;
	if (!obs_x264_delivery_csp(obsx264->color_format, &delivery_fmt, &csp, &bitdepth)) {
		err("internal error: unsupported color format (%d) reached param setup", (int)obsx264->color_format);
		return;
	}
	/* x264 consumes csp/bitdepth; libobs delivers delivery_fmt (P010 -> planar I010). */
	obsx264->params.i_csp = csp;
	obsx264->params.i_bitdepth = bitdepth;

	for (size_t i = 0; i < options->ignored_word_count; ++i)
		warn("ignoring invalid x264 option: %s", options->ignored_words[i]);
	for (size_t i = 0; i < options->count; ++i)
		set_param(obsx264, options->options[i]);

	if (!update) {
		info("settings:\n"
		     "\trate_control: %s\n"
		     "\tbitrate:      %d\n"
		     "\tbuffer size:  %d\n"
		     "\tcrf:          %d\n"
		     "\tfps_num:      %d\n"
		     "\tfps_den:      %d\n"
		     "\twidth:        %d\n"
		     "\theight:       %d\n"
		     "\tkeyint:       %d\n"
		     "\tcolor format: %d\n"
		     "\tbit depth:    %u\n",
		     rate_control, obsx264->params.rc.i_vbv_max_bitrate, obsx264->params.rc.i_vbv_buffer_size,
		     (int)obsx264->params.rc.f_rf_constant, voi->fps_num, voi->fps_den, width, height,
		     obsx264->params.i_keyint_max, (int)obsx264->color_format, bitdepth);
	}
}

static void log_custom_options(struct obs_x264 *obsx264, const struct obs_options *options)
{
	if (options->count == 0) {
		return;
	}
	size_t settings_string_length = 0;
	for (size_t i = 0; i < options->count; ++i)
		settings_string_length += strlen(options->options[i].name) + strlen(options->options[i].value) + 5;
	size_t buffer_size = settings_string_length + 1;
	char *settings_string = bmalloc(settings_string_length + 1);
	char *p = settings_string;
	size_t remaining_buffer_size = buffer_size;
	for (size_t i = 0; i < options->count; ++i) {
		int chars_written = snprintf(p, remaining_buffer_size, "\n\t%s = %s", options->options[i].name,
					     options->options[i].value);
		assert(chars_written >= 0);
		assert((size_t)chars_written <= remaining_buffer_size);
		p += chars_written;
		remaining_buffer_size -= chars_written;
	}
	assert(remaining_buffer_size == 1);
	assert(*p == '\0');
	info("custom settings: %s", settings_string);
	bfree(settings_string);
}

static bool update_settings(struct obs_x264 *obsx264, obs_data_t *settings, bool update)
{
	char *preset = bstrdup(obs_data_get_string(settings, "preset"));
	char *profile = bstrdup(obs_data_get_string(settings, "profile"));
	char *tune = bstrdup(obs_data_get_string(settings, "tune"));
	struct obs_options options = obs_parse_options(obs_data_get_string(settings, "x264opts"));
	bool repeat_headers = obs_data_get_bool(settings, "repeat_headers");

	bool success = true;

	if (!update)
		blog(LOG_INFO, "---------------------------------");

	if (!obsx264->context) {
		override_base_params(obsx264, &options, &preset, &profile, &tune);

		if (preset && *preset)
			info("preset: %s", preset);
		if (profile && *profile)
			info("profile: %s", profile);
		if (tune && *tune)
			info("tune: %s", tune);

		success = reset_x264_params(obsx264, preset, tune);
	}

	if (repeat_headers) {
		obsx264->params.b_repeat_headers = 1;
		obsx264->params.b_annexb = 1;
		obsx264->params.b_aud = 1;
	}

	if (success) {
		update_params(obsx264, settings, &options, update);
		if (!update) {
			log_custom_options(obsx264, &options);
		}

		if (!obsx264->context)
			apply_x264_profile(obsx264, profile);
	}

	obs_free_options(options);
	bfree(preset);
	bfree(profile);
	bfree(tune);

	return success;
}

static bool obs_x264_update(void *data, obs_data_t *settings)
{
	struct obs_x264 *obsx264 = data;

	/* Read the user's color settings into the struct */
	enum video_format format;
	enum video_colorspace cs;
	enum video_range_type range;
	read_color_settings(settings, &format, &cs, &range);

	enum video_format delivery_fmt;
	int csp;
	uint32_t bitdepth;
	bool ok_csp = obs_x264_delivery_csp(format, &delivery_fmt, &csp, &bitdepth);
	if (!ok_csp) {
		err("color format '%s' is not supported by x264; refusing to fall back to another format",
		    obs_data_get_string(settings, "color_format"));
		return false;
	}
	publish_preferred_settings(obsx264->encoder, format, cs, range);

	/* Detect if the chosen color format changed since the context was built.
	 * x264_encoder_reconfig cannot change i_csp/i_bitdepth/vui, so a context
	 * rebuild is needed. libobs only re-negotiates raw input delivery on
	 * stop/start (remove/add connection), so we log that and let the next
	 * start pick up the new format — same model as svt-av1. */
	bool csp_changed = obsx264->active_csp != csp || obsx264->active_bitdepth != bitdepth;

	if (csp_changed) {
		info("color format/space/range changed; the new settings apply when the encoder next starts");
	} else {
		bool success = update_settings(obsx264, settings, true);
		int ret = 0;
		if (success) {
			ret = x264_encoder_reconfig(obsx264->context, &obsx264->params);
			if (ret != 0)
				warn("Failed to reconfigure: %d", ret);
		}
		return success && ret == 0;
	}

	/* csp changed: still update params so the next create() has correct values */
	update_settings(obsx264, settings, true);
	return true;
}

static void load_headers(struct obs_x264 *obsx264)
{
	x264_nal_t *nals;
	int nal_count;
	DARRAY(uint8_t) header;
	DARRAY(uint8_t) sei;

	da_init(header);
	da_init(sei);

	x264_encoder_headers(obsx264->context, &nals, &nal_count);

	for (int i = 0; i < nal_count; i++) {
		x264_nal_t *nal = nals + i;

		if (nal->i_type == NAL_SEI)
			da_push_back_array(sei, nal->p_payload, nal->i_payload);
		else
			da_push_back_array(header, nal->p_payload, nal->i_payload);
	}

	obsx264->extra_data = header.array;
	obsx264->extra_data_size = header.num;
	obsx264->sei = sei.array;
	obsx264->sei_size = sei.num;
}

static void *obs_x264_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	video_t *video = obs_encoder_video(encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	/* Reject PQ/HLG pipeline (cannot be encoded by x264 at all) */
	if (voi->colorspace == VIDEO_CS_2100_PQ || voi->colorspace == VIDEO_CS_2100_HLG) {
		obs_encoder_set_last_error(encoder, obs_module_text("HdrUnsupported"));
		warn_enc(encoder, "OBS does not support using x264 with Rec. 2100");
		return NULL;
	}

	struct obs_x264 *obsx264 = bzalloc(sizeof(struct obs_x264));
	obsx264->encoder = encoder;

	/* Read the user's color settings */
	enum video_format format;
	enum video_colorspace cs;
	enum video_range_type range;
	read_color_settings(settings, &format, &cs, &range);

	enum video_format delivery_fmt;
	int csp;
	uint32_t bitdepth;
	if (!obs_x264_delivery_csp(format, &delivery_fmt, &csp, &bitdepth)) {
		err_enc(encoder, "color format '%s' is not supported by x264; refusing to fall back to another format",
		        obs_data_get_string(settings, "color_format"));
		obs_encoder_set_last_error(encoder, obs_module_text("ColorFormatUnsupported"));
		bfree(obsx264);
		return NULL;
	}
	obsx264->color_format = format;
	obsx264->color_space = cs;
	obsx264->color_range = range;

	publish_preferred_settings(encoder, format, cs, range);
	obsx264->active_csp = csp;
	obsx264->active_bitdepth = bitdepth;

	if (update_settings(obsx264, settings, false)) {
		obsx264->context = x264_encoder_open(&obsx264->params);

		if (obsx264->context == NULL)
			warn("x264 failed to load");
		else
			load_headers(obsx264);
	} else {
		warn("bad settings specified");
	}

	if (!obsx264->context) {
		bfree(obsx264);
		return NULL;
	}

	obsx264->performance_token = os_request_high_performance("x264 encoding");

	return obsx264;
}

static void parse_packet(struct obs_x264 *obsx264, struct encoder_packet *packet, x264_nal_t *nals, int nal_count,
			 x264_picture_t *pic_out)
{
	if (!nal_count)
		return;

	da_resize(obsx264->packet_data, 0);

	for (int i = 0; i < nal_count; i++) {
		x264_nal_t *nal = nals + i;
		da_push_back_array(obsx264->packet_data, nal->p_payload, nal->i_payload);
	}

	packet->data = obsx264->packet_data.array;
	packet->size = obsx264->packet_data.num;
	packet->type = OBS_ENCODER_VIDEO;
	packet->pts = pic_out->i_pts;
	packet->dts = pic_out->i_dts;
	packet->keyframe = pic_out->b_keyframe != 0;
}

static inline void init_pic_data(struct obs_x264 *obsx264, x264_picture_t *pic, struct encoder_frame *frame)
{
	x264_picture_init(pic);

	pic->i_pts = frame->pts;
	/* Use active_csp (what libobs is actually delivering right now), not
	 * params.i_csp (which may already reflect a pending color change). */
	pic->img.i_csp = obsx264->active_csp;
	/* This build of x264 is templated: i_bitdepth=10 selects the HIGH_BIT_DEPTH flavor,
     * whose frame copy requires X264_CSP_HIGH_DEPTH on every input csp. libobs already
     * delivers 10-bit formats (P010/P216) as uint16 planes; flag the csp so x264 reads them
     * correctly. 8-bit formats use the unflagged 8-bit flavor and must NOT be flagged. */
	if (obsx264->active_bitdepth == 10)
		pic->img.i_csp |= X264_CSP_HIGH_DEPTH;

	if (obsx264->active_csp == X264_CSP_NV12)
		pic->img.i_plane = 2; /* NV12: Y + interleaved UV */
	else if (obsx264->active_csp == X264_CSP_I420)
		pic->img.i_plane = 3; /* P010 delivered as planar I010: Y/U/V (interleaved internally by x264) */
	else if (obsx264->active_csp == X264_CSP_I422)
		pic->img.i_plane = 3; /* P216 delivered as planar I210: Y/U/V full-height (interleaved internally by x264) */
	else if (obsx264->active_csp == X264_CSP_I444)
		pic->img.i_plane = 3;
	else if (obsx264->active_csp == X264_CSP_NV16)
		pic->img.i_plane = 2; /* P216: Y + interleaved UV */
	else if (obsx264->active_csp == X264_CSP_BGRA)
		pic->img.i_plane = 1;

	for (int i = 0; i < pic->img.i_plane; i++) {
		pic->img.i_stride[i] = (int)frame->linesize[i];
		pic->img.plane[i] = frame->data[i];
	}
}

/* H.264 always uses 16x16 macroblocks */
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

static void add_roi(struct obs_x264 *obsx264, x264_picture_t *pic)
{
	const uint32_t increment = obs_encoder_get_roi_increment(obsx264->encoder);

	if (obsx264->quant_offsets && obsx264->roi_increment == increment) {
		pic->prop.quant_offsets = obsx264->quant_offsets;
		return;
	}

	const uint32_t width = obs_encoder_get_width(obsx264->encoder);
	const uint32_t height = obs_encoder_get_height(obsx264->encoder);
	const uint32_t mb_width = (width + MB_SIZE - 1) / MB_SIZE;
	const uint32_t mb_height = (height + MB_SIZE - 1) / MB_SIZE;
	const size_t map_size = sizeof(float) * mb_width * mb_height;

	float *map = bzalloc(map_size);

	struct roi_params par = {mb_width, mb_height, map};

	obs_encoder_enum_roi(obsx264->encoder, roi_cb, &par);

	pic->prop.quant_offsets = map;
	obsx264->quant_offsets = map;
	obsx264->roi_increment = increment;
}

static bool obs_x264_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
			    bool *received_packet)
{
	struct obs_x264 *obsx264 = data;
	x264_nal_t *nals;
	int nal_count;
	int ret;
	x264_picture_t pic, pic_out;

	if (!frame || !packet || !received_packet)
		return false;

	init_pic_data(obsx264, &pic, frame);

	if (obs_encoder_has_roi(obsx264->encoder))
		add_roi(obsx264, &pic);

	ret = x264_encoder_encode(obsx264->context, &nals, &nal_count, (frame ? &pic : NULL), &pic_out);
	if (ret < 0) {
		warn("encode failed");
		return false;
	}

	*received_packet = (nal_count != 0);
	parse_packet(obsx264, packet, nals, nal_count, &pic_out);

	return true;
}

static bool obs_x264_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct obs_x264 *obsx264 = data;

	if (!obsx264->context)
		return false;

	*extra_data = obsx264->extra_data;
	*size = obsx264->extra_data_size;
	return true;
}

static bool obs_x264_sei(void *data, uint8_t **sei, size_t *size)
{
	struct obs_x264 *obsx264 = data;

	if (!obsx264->context)
		return false;

	*sei = obsx264->sei;
	*size = obsx264->sei_size;
	return true;
}

static void obs_x264_video_info(void *data, struct video_scale_info *info)
{
	struct obs_x264 *obsx264 = data;
	enum video_format delivery_fmt;
	int csp;
	uint32_t bitdepth;

	/* Request the user's chosen format/space/range from libobs.
	 * libobs will CPU-convert to this per-input (pinned at connect time),
	 * so the x264 context and delivered frames always match.
	 * P010 is requested as planar I010 (YUV420P10LE) - see obs_x264_delivery_csp. */
	if (!obs_x264_delivery_csp(obsx264->color_format, &delivery_fmt, &csp, &bitdepth)) {
		/* Unreachable after create/update validation; NV12 is the last-resort delivery so libobs
		 * still has a valid format. Logged loudly - never silent. */
		err("unsupported color format (%d): x264 cannot encode it; delivering NV12 as last resort",
		    (int)obsx264->color_format);
		delivery_fmt = VIDEO_FORMAT_NV12;
	}
	info->format = delivery_fmt;
	info->colorspace = obsx264->color_space;
	info->range = obsx264->color_range;
}

struct obs_encoder_info obs_x264_encoder = {
	.id = "obs_x264",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.get_name = obs_x264_getname,
	.create = obs_x264_create,
	.destroy = obs_x264_destroy,
	.encode = obs_x264_encode,
	.update = obs_x264_update,
	.get_properties = obs_x264_props,
	.get_defaults = obs_x264_defaults,
	.get_extra_data = obs_x264_extra_data,
	.get_sei_data = obs_x264_sei,
	.get_video_info = obs_x264_video_info,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_ROI,
};
