/*
 * Integer Downscale Filter
 *
 * Fixed-integer GPU downscaling (÷2 / ÷3 / ÷4 / ÷6) that keeps text crisp.
 * Every output pixel i covers exactly the source texels [i*R, i*R+R), and the
 * per-tap weights are phase-locked windowed-sinc decimation kernels (see
 * data/integer_downscale_filter.effect). Unlike generic ratio scalers there is
 * no fractional tap position to blur: text survives downscaling without being
 * smoothed into mush.
 *
 * Pipeline:
 *   pass 0 : stage the source at native resolution        (tex_full)
 *   pass A : K2/K3 kernel -> intermediate stage           (scale ÷4 / ÷6 only)
 *   final  : last kernel pass, composited in item space
 */

#include <stdio.h>
#include <stdlib.h>
#include <util/dstr.h>
#include <obs-module.h>

#define S_SCALE "scale"
#define S_SAMPLING "sampling"

enum id_sampling {
	ID_SMP_AREA = 0,
	ID_SMP_TEXT,
	ID_SMP_LANC
};

struct id_filter_data {
	obs_source_t *context;

	gs_effect_t *effect;
	gs_eparam_t *image_param;
	gs_eparam_t *dimension_param;
	gs_eparam_t *dimension_i_param;
	gs_eparam_t *multiplier_param;

	uint32_t scale; /* 2, 3, 4 or 6 */
	enum id_sampling sampling;

	uint32_t cx_in;
	uint32_t cy_in;
	uint32_t mid_cx;
	uint32_t mid_cy;
	uint32_t out_cx;
	uint32_t out_cy;

	bool valid; /* settings + target present */

	gs_texrender_t *tex_full; /* staged source @ cx_in x cy_in */
	gs_texrender_t *tex_mid; /* intermediate stage (scale ÷4 / ÷6) */
};

static void free_stages(struct id_filter_data *f)
{
	obs_enter_graphics();
	gs_texrender_destroy(f->tex_full);
	f->tex_full = NULL;
	gs_texrender_destroy(f->tex_mid);
	f->tex_mid = NULL;
	obs_leave_graphics();
}

/* Recreate a persistent render target when the color format changes. */
static bool ensure_stage(gs_texrender_t **stage, enum gs_color_format format)
{
	if (*stage && gs_texrender_get_format(*stage) == format)
		return true;

	obs_enter_graphics();
	gs_texrender_destroy(*stage);
	*stage = gs_texrender_create(format, GS_ZS_NONE);
	obs_leave_graphics();

	return !!(*stage);
}

static uint32_t parse_scale(const char *s)
{
	if (!s || !*s)
		return 4; /* default */

	int v = atoi(s);
	return (v == 2 || v == 3 || v == 4 || v == 6) ? (uint32_t)v : 0;
}

static enum id_sampling parse_sampling(const char *s)
{
	if (!s || !*s)
		return ID_SMP_TEXT; /* default */

	if (astrcmpi(s, "area") == 0)
		return ID_SMP_AREA;
	if (astrcmpi(s, "lanczos") == 0)
		return ID_SMP_LANC;

	return ID_SMP_TEXT;
}

static const char *sampling_tag(enum id_sampling sampling)
{
	switch (sampling) {
	case ID_SMP_AREA:
		return "Area";
	case ID_SMP_LANC:
		return "Lanc";
	default:
		return "Text";
	}
}

/*
 * Build the technique name "<kernel><mode>[suffix]" and the output multiplier.
 * The suffix converts between the source color space and the current (canvas)
 * space, mirroring gpu-delay / scale-filter. Intermediate passes always run in
 * one color space, so pass `current_space == source_space` for those.
 */
static void id_build_tech_name(char *out, size_t len, const char *kernel, enum id_sampling sampling,
                               enum gs_color_space current_space, enum gs_color_space source_space, float *multiplier)
{
	const char *suffix = "";

	*multiplier = 1.0f;

	switch (source_space) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		if (current_space == GS_CS_709_SCRGB) {
			suffix = "Multiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
		}
		break;
	case GS_CS_709_EXTENDED:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			suffix = "Tonemap";
			break;
		case GS_CS_709_SCRGB:
			suffix = "Multiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
			break;
		default:
			break;
		}
		break;
	case GS_CS_709_SCRGB:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			suffix = "MultiplyTonemap";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_EXTENDED:
			suffix = "Multiply";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	snprintf(out, len, "%s%s%s", kernel, sampling_tag(sampling), suffix);
}


static const char *id_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("IntegerDownscale");
}

static void id_filter_update(void *data, obs_data_t *settings)
{
	struct id_filter_data *f = data;

	f->scale = parse_scale(obs_data_get_string(settings, S_SCALE));
	if (!f->scale) { /* invalid value: disable the filter */
		f->valid = false;
		return;
	}

	f->sampling = parse_sampling(obs_data_get_string(settings, S_SAMPLING));
	f->valid = true;

	/* full reset of cached state (texrender sizes/formats are rebuilt lazily) */
	f->cx_in = 0;
	f->cy_in = 0;
	f->mid_cx = 0;
	f->mid_cy = 0;
	f->out_cx = 0;
	f->out_cy = 0;
	free_stages(f);
}

static obs_properties_t *id_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, S_SCALE, obs_module_text("IntegerDownscale.Scale"), OBS_COMBO_TYPE_LIST,
	                              OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "÷2", "2");
	obs_property_list_add_string(p, "÷3", "3");
	obs_property_list_add_string(p, "÷4", "4");
	obs_property_list_add_string(p, "÷6", "6");

	p = obs_properties_add_list(props, S_SAMPLING, obs_module_text("IntegerDownscale.Sampling"), OBS_COMBO_TYPE_LIST,
	                              OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Area", "area");
	obs_property_list_add_string(p, "Text Tuned", "texttuned");
	obs_property_list_add_string(p, "Lanczos", "lanczos");

	p = obs_properties_add_text(props, "", obs_module_text("IntegerDownscale.Description"), OBS_TEXT_INFO);

	UNUSED_PARAMETER(data);
	return props;
}

static void *id_filter_create(obs_data_t *settings, obs_source_t *context)
{
	struct id_filter_data *f = bzalloc(sizeof(*f));

	f->context = context;

	char *effect_path = obs_module_file("integer_downscale_filter.effect");
	obs_enter_graphics();
	char *error_string = NULL;
	f->effect = gs_effect_create_from_file(effect_path, &error_string);
	if (!f->effect) {
		blog(LOG_ERROR, "integer_downscale: failed to create effect '%s': %s", effect_path ? effect_path : "(null)",
		     error_string ? error_string : "unknown error");
	} else {
		f->image_param = gs_effect_get_param_by_name(f->effect, "image");
		f->dimension_param = gs_effect_get_param_by_name(f->effect, "base_dimension");
		f->dimension_i_param = gs_effect_get_param_by_name(f->effect, "base_dimension_i");
		f->multiplier_param = gs_effect_get_param_by_name(f->effect, "multiplier");
	}
	obs_leave_graphics();
	bfree(effect_path);
	bfree(error_string);

	id_filter_update(f, settings);
	return f;
}

static void id_filter_destroy(void *data)
{
	struct id_filter_data *f = data;

	free_stages(f);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	obs_leave_graphics();
	bfree(f);
}

/* Recompute stage/output sizes from the current target and settings. Called from
 * the video tick and, defensively, from the render path so the filter never depends
 * on a tick having run first (e.g. headless usage where ticks can be delayed). */
static void id_filter_update_sizes(struct id_filter_data *f)
{
	obs_source_t *target = obs_filter_get_target(f->context);

	if (!target) {
		f->valid = false;
		f->cx_in = 0;
		f->cy_in = 0;
		return;
	}

	uint32_t cx = obs_source_get_base_width(target);
	uint32_t cy = obs_source_get_base_height(target);

	if (!cx || !cy) {
		f->valid = false;
		f->cx_in = 0;
		f->cy_in = 0;
		return;
	}

	f->valid = true;
	f->cx_in = cx;
	f->cy_in = cy;

	if (f->scale == 4 || f->scale == 6) {
		uint32_t stage_div = (f->scale == 4) ? 2 : 3;
		f->mid_cx = cx / stage_div;
		f->mid_cy = cy / stage_div;
	} else {
		f->mid_cx = 0;
		f->mid_cy = 0;
	}

	f->out_cx = cx / f->scale;
	f->out_cy = cy / f->scale;
}

static void id_filter_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct id_filter_data *f = data;
	id_filter_update_sizes(f);
}

static void id_filter_video_render(void *data, gs_effect_t *effect)
{
	struct id_filter_data *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);

	id_filter_update_sizes(f); /* keep sizes fresh even if ticks are delayed */

	if (!f->valid || !target || !f->cx_in || !f->cy_in) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	/* Persistent stages are reused every tick; a texrender is single-use per begin/end cycle */
	gs_texrender_reset(f->tex_full);
	gs_texrender_reset(f->tex_mid);

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	const enum gs_color_space source_space = obs_source_get_color_space(target, OBS_COUNTOF(preferred_spaces), preferred_spaces);
	const enum gs_color_format format = gs_get_format_from_space(source_space);

	uint32_t parent_flags = parent ? obs_source_get_output_flags(parent) : 0;
	bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
	bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;

	/* ---- pass 0: stage the source at native resolution ---- */
	if (!ensure_stage(&f->tex_full, format)) {
		return;
	}

	char tech[32];
	float multiplier;

	if (gs_texrender_begin_with_color_space(f->tex_full, f->cx_in, f->cy_in, source_space)) {
		struct vec4 clear_color;

		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)f->cx_in, 0.0f, (float)f->cy_in, -100.0f, 100.0f);

		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_blend_state_pop();
		gs_texrender_end(f->tex_full);
	} else {
		return;
	}

	struct vec2 src_dimension;
	vec2_set(&src_dimension, (float)f->cx_in, (float)f->cy_in);

	gs_texture_t *tex_src = gs_texrender_get_texture(f->tex_full);
	if (!tex_src)
		return;

	/* ---- Area sampling: single-pass bilinear decimation (no intermediate stage) ---- */
	if (f->sampling == ID_SMP_AREA) {
		const char *area_kernel;

		switch (f->scale) {
		case 2:
			area_kernel = "K2"; /* single bilinear tap */
			break;
		case 3:
			area_kernel = "K3"; /* 9-tap box, unchanged */
			break;
		case 4:
			area_kernel = "K4B";
			break;
		default:
			area_kernel = "K6B";
			break;
		}

		struct vec2 src_dimension_i;
		vec2_set(&src_dimension_i, 1.0f / (float)f->cx_in, 1.0f / (float)f->cy_in);

		id_build_tech_name(tech, sizeof(tech), area_kernel, f->sampling, gs_get_color_space(), source_space, &multiplier);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

		{
			const bool previous = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(false);

			/* sRGB binding: hardware Sample() decodes to linear light, so the box average is
			 * computed in linear space. No framebuffer sRGB (raw write) — together this matches
			 * the established Area color behavior of the reference scaler and the previous
			 * Load-based kernels (verified pixel-identical within ~0.3 levels). */
			gs_effect_set_texture_srgb(f->image_param, tex_src);
			gs_effect_set_vec2(f->dimension_param, &src_dimension);
			if (f->dimension_i_param)
				gs_effect_set_vec2(f->dimension_i_param, &src_dimension_i);
			gs_effect_set_float(f->multiplier_param, multiplier);

			while (gs_effect_loop(f->effect, tech)) {
				gs_draw_sprite(tex_src, 0, f->out_cx, f->out_cy);
			}

			gs_enable_framebuffer_srgb(previous);
		}

		gs_blend_state_pop();
		return;
	}

	/* ---- pass A: intermediate K2/K3 downscale for ÷4 / ÷6 ---- */
	if (f->mid_cx && f->mid_cy) {
		const char *kernel = (f->scale == 4) ? "K2" : "K3";

		id_build_tech_name(tech, sizeof(tech), kernel, f->sampling, source_space, source_space, &multiplier);
		if (!ensure_stage(&f->tex_mid, format)) {
			return;
		}

		if (gs_texrender_begin_with_color_space(f->tex_mid, f->mid_cx, f->mid_cy, source_space)) {
			struct vec4 clear_color;

			gs_blend_state_push();
			gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

			vec4_zero(&clear_color);
			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
			gs_ortho(0.0f, (float)f->mid_cx, 0.0f, (float)f->mid_cy, -100.0f, 100.0f);

			gs_effect_set_texture_srgb(f->image_param, tex_src);
			gs_effect_set_vec2(f->dimension_param, &src_dimension);
			gs_effect_set_float(f->multiplier_param, multiplier);

			while (gs_effect_loop(f->effect, tech)) {
				gs_draw_sprite(tex_src, 0, f->mid_cx, f->mid_cy);
			}

			gs_blend_state_pop();
			gs_texrender_end(f->tex_mid);
		} else {
			return;
		}

		tex_src = gs_texrender_get_texture(f->tex_mid);
		if (!tex_src)
			return;
		vec2_set(&src_dimension, (float)f->mid_cx, (float)f->mid_cy);
	}

	/* ---- final pass: composite in item space at the output size ---- */
	const char *final_kernel = (f->scale == 3) ? "K3" : "K2";

	id_build_tech_name(tech, sizeof(tech), final_kernel, f->sampling, gs_get_color_space(), source_space, &multiplier);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	{
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);

		gs_effect_set_texture_srgb(f->image_param, tex_src);
		gs_effect_set_vec2(f->dimension_param, &src_dimension);
		gs_effect_set_float(f->multiplier_param, multiplier);

		while (gs_effect_loop(f->effect, tech)) {
			gs_draw_sprite(tex_src, 0, f->out_cx, f->out_cy);
		}

		gs_enable_framebuffer_srgb(previous);
	}

	gs_blend_state_pop();

	UNUSED_PARAMETER(effect);
}

static void id_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_SCALE, "4");
	obs_data_set_default_string(settings, S_SAMPLING, "texttuned");
}

static uint32_t id_filter_width(void *data)
{
	struct id_filter_data *f = data;
	return f->out_cx;
}

static uint32_t id_filter_height(void *data)
{
	struct id_filter_data *f = data;
	return f->out_cy;
}

static enum gs_color_space id_filter_get_color_space(void *data, size_t count,
                                                     const enum gs_color_space *preferred_spaces)
{
	struct id_filter_data *f = data;
	obs_source_t *target;

	if (!f || !f->valid || (target = obs_filter_get_target(f->context)) == NULL) {
		return (count > 0) ? preferred_spaces[0] : GS_CS_SRGB;
	}

	const enum gs_color_space potential_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	enum gs_color_space space = obs_source_get_color_space(target, OBS_COUNTOF(potential_spaces), potential_spaces);

	for (size_t i = 0; i < count; ++i) {
		if (preferred_spaces[i] == space)
			return preferred_spaces[i];
	}

	return GS_CS_SRGB;
}

struct obs_source_info integer_downscale_filter = {
	.id = "integer_downscale",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = id_filter_get_name,
	.create = id_filter_create,
	.destroy = id_filter_destroy,
	.update = id_filter_update,
	.get_properties = id_filter_properties,
	.video_tick = id_filter_tick,
	.video_render = id_filter_video_render,
	.get_defaults = id_filter_defaults,
	.get_width = id_filter_width,
	.get_height = id_filter_height,
	.video_get_color_space = id_filter_get_color_space,
};

