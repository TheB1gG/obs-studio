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

#include "obs.h"
#include "obs-internal.h"
#include "obs-video-reuse.h"
#include "util/util_uint64.h"

#include <stdio.h>

#define encoder_active(encoder) os_atomic_load_bool(&encoder->active)
#define set_encoder_active(encoder, val) os_atomic_set_bool(&encoder->active, val)

#define get_weak(encoder) ((obs_weak_encoder_t *)encoder->context.control)

static void encoder_set_video(obs_encoder_t *encoder, video_t *video);

struct obs_encoder_info *find_encoder(const char *id)
{
	for (size_t i = 0; i < obs->encoder_types.num; i++) {
		struct obs_encoder_info *info = obs->encoder_types.array + i;

		if (strcmp(info->id, id) == 0)
			return info;
	}

	return NULL;
}

const char *obs_encoder_get_display_name(const char *id)
{
	struct obs_encoder_info *ei = find_encoder(id);
	return ei ? ei->get_name(ei->type_data) : NULL;
}

static bool init_encoder(struct obs_encoder *encoder, const char *name, obs_data_t *settings, obs_data_t *hotkey_data)
{
	pthread_mutex_init_value(&encoder->init_mutex);
	pthread_mutex_init_value(&encoder->callbacks_mutex);
	pthread_mutex_init_value(&encoder->outputs_mutex);
	pthread_mutex_init_value(&encoder->pause.mutex);
	pthread_mutex_init_value(&encoder->roi_mutex);

	if (!obs_context_data_init(&encoder->context, OBS_OBJ_TYPE_ENCODER, settings, name, NULL, hotkey_data, false))
		return false;
	if (pthread_mutex_init_recursive(&encoder->init_mutex) != 0)
		return false;
	if (pthread_mutex_init_recursive(&encoder->callbacks_mutex) != 0)
		return false;
	if (pthread_mutex_init(&encoder->outputs_mutex, NULL) != 0)
		return false;
	if (pthread_mutex_init(&encoder->pause.mutex, NULL) != 0)
		return false;
	if (pthread_mutex_init(&encoder->roi_mutex, NULL) != 0)
		return false;

	if (encoder->orig_info.get_defaults) {
		encoder->orig_info.get_defaults(encoder->context.settings);
	}
	if (encoder->orig_info.get_defaults2) {
		encoder->orig_info.get_defaults2(encoder->context.settings, encoder->orig_info.type_data);
	}

	return true;
}

static struct obs_encoder *create_encoder(const char *id, enum obs_encoder_type type, const char *name,
					  obs_data_t *settings, size_t mixer_idx, obs_data_t *hotkey_data)
{
	struct obs_encoder *encoder;
	struct obs_encoder_info *ei = find_encoder(id);
	bool success;

	if (ei && ei->type != type)
		return NULL;

	encoder = bzalloc(sizeof(struct obs_encoder));
	encoder->mixer_idx = mixer_idx;

	if (!ei) {
		blog(LOG_ERROR, "Encoder ID '%s' not found", id);

		encoder->info.id = bstrdup(id);
		encoder->info.type = type;
		encoder->owns_info_id = true;
		encoder->orig_info = encoder->info;
	} else {
		encoder->info = *ei;
		encoder->orig_info = *ei;
	}

	success = init_encoder(encoder, name, settings, hotkey_data);
	if (!success) {
		blog(LOG_ERROR, "creating encoder '%s' (%s) failed", name, id);
		obs_encoder_destroy(encoder);
		return NULL;
	}

	obs_context_init_control(&encoder->context, encoder, (obs_destroy_cb)obs_encoder_destroy);
	obs_context_data_insert(&encoder->context, &obs->data.encoders_mutex, &obs->data.first_encoder);

	if (type == OBS_ENCODER_VIDEO) {
		encoder->frame_rate_divisor = 1;
	}

	blog(LOG_DEBUG, "encoder '%s' (%s) created", name, id);

	if (ei && ei->caps & OBS_ENCODER_CAP_DEPRECATED) {
		blog(LOG_WARNING, "Encoder ID '%s' is deprecated and may be removed in a future version.", id);
	}

	return encoder;
}

obs_encoder_t *obs_video_encoder_create(const char *id, const char *name, obs_data_t *settings, obs_data_t *hotkey_data)
{
	if (!name || !id)
		return NULL;
	return create_encoder(id, OBS_ENCODER_VIDEO, name, settings, 0, hotkey_data);
}

obs_encoder_t *obs_audio_encoder_create(const char *id, const char *name, obs_data_t *settings, size_t mixer_idx,
					obs_data_t *hotkey_data)
{
	if (!name || !id)
		return NULL;
	return create_encoder(id, OBS_ENCODER_AUDIO, name, settings, mixer_idx, hotkey_data);
}

static void receive_video(void *param, struct video_data *frame);
static void receive_audio(void *param, size_t mix_idx, struct audio_data *data);

static inline void get_audio_info(const struct obs_encoder *encoder, struct audio_convert_info *info)
{
	const struct audio_output_info *aoi;
	aoi = audio_output_get_info(encoder->media);

	if (info->format == AUDIO_FORMAT_UNKNOWN)
		info->format = aoi->format;
	if (!info->samples_per_sec)
		info->samples_per_sec = aoi->samples_per_sec;
	if (info->speakers == SPEAKERS_UNKNOWN)
		info->speakers = aoi->speakers;

	if (encoder->info.get_audio_info)
		encoder->info.get_audio_info(encoder->context.data, info);
}

static inline void get_video_info(struct obs_encoder *encoder, struct video_scale_info *info)
{
	const struct video_output_info *voi;
	voi = video_output_get_info(encoder->media);

	info->format = voi->format;
	info->colorspace = voi->colorspace;
	info->range = voi->range;
	info->width = obs_encoder_get_width(encoder);
	info->height = obs_encoder_get_height(encoder);

	/* The encoder's get_video_info callback requires a live context; before create() has
	 * run (context.data == NULL) we only use the base-canvas fields filled above. */
	if (encoder->context.data && encoder->info.get_video_info)
		encoder->info.get_video_info(encoder->context.data, info);

	/**
	 * Prevent video output from performing an actual scale. If GPU scaling is
	 * enabled, the voi will contain the scaled size. Therefore, GPU scaling
	 * takes priority over self-scaling functionality.
	 */
	if ((encoder->info.caps & OBS_ENCODER_CAP_SCALING) != 0) {
		info->width = voi->width;
		info->height = voi->height;
	}
}

static inline bool gpu_encode_available(const struct obs_encoder *encoder)
{
	struct obs_core_video_mix *video = get_mix_for_video(encoder->media);
	if (!video)
		return false;
	return (encoder->info.caps & OBS_ENCODER_CAP_PASS_TEXTURE) != 0 &&
	       video->encoder_texture_format != VIDEO_FORMAT_NONE;
}

/**
 * GPU based rescaling is currently implemented via core video mixes,
 * i.e. a core mix with matching width/height/format/colorspace/range
 * will be created if it doesn't exist already to generate encoder
 * input
 */
enum encoder_mix_acquire {
	MIX_ACQUIRE_SUCCESS,
	MIX_ACQUIRE_NOT_NEEDED, // nothing to scale or convert: consume the source as-is
	MIX_ACQUIRE_FAILED,
};

/* CPU/raw encoders can be handed already-converted YUV through GPU readback when their
 * requested delivery format has a GPU conversion path (convert_textures allocated in
 * obs_init_gpu_conversion) plus matching raw staging surfaces. This removes the per-frame
 * CPU swscale color-matrix pass: the GPU converts base->YUV and the encoder reads back the
 * smaller YUV buffer instead of full BGRA. Only planar/semi-planar YUV formats with a GPU
 * conversion are eligible; packed-RGB and alpha formats fall back to CPU swscale. */
static bool raw_gpu_conversion_supported(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_I412:
	case VIDEO_FORMAT_R10P:
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_P010:
	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416:
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I210:
	case VIDEO_FORMAT_YUV420P12:
	case VIDEO_FORMAT_YUV422P12:
	case VIDEO_FORMAT_YUV444P12:
	case VIDEO_FORMAT_GBRP12:
	case VIDEO_FORMAT_GBRP:
	case VIDEO_FORMAT_YUV444P10:
		return true;
	default:
		return false;
	}
}

/**
 * Find or create an encoder-only mix matching this encoder's current scaled size,
 * scale type and output format/colorspace/range for the view of its media.
 *
 * On success the mix is returned through out_mix with a refcount already
 * incremented; MIX_ACQUIRE_NOT_NEEDED means the encoder can consume its source
 * as-is (no scaling or conversion required); MIX_ACQUIRE_FAILED means no mix
 * could be acquired.
 */
static enum encoder_mix_acquire acquire_encoder_only_mix(struct obs_encoder *encoder, struct obs_core_video_mix **out_mix)
{
	*out_mix = NULL;
	struct obs_core_video_mix *mix = NULL, *current_mix;
	const bool is_tex_encoder = (encoder->info.caps & OBS_ENCODER_CAP_PASS_TEXTURE) != 0;
	bool conversion_requested = false;
	uint32_t width;
	uint32_t height;
	enum video_format format;
	enum video_colorspace space;
	enum video_range_type range;

	if (!encoder->media)
		return MIX_ACQUIRE_FAILED;

	/* Base the delivery decision on the original source canvas, not whatever mix this encoder is
	 * currently bound to. On a live rescale the media may already be a converted encoder-only mix
	 * (e.g. P010); using its format/size as the "base" would hide that the real base (e.g. RGBA16F)
	 * still needs conversion and would report the old mix's size instead of the canvas size. */
	video_t *base_video = encoder->source_video ? encoder->source_video : encoder->media;
	const struct video_output_info *info = video_output_get_info(base_video);

	if (is_tex_encoder) {
		format = encoder->preferred_format;
		space = encoder->preferred_space;
		range = encoder->preferred_range;

		struct video_scale_info encoder_info = {0};

		if (format == VIDEO_FORMAT_NONE) {
			/* No explicit preference: use the normal path (base canvas format → encoder callback). */
			get_video_info(encoder, &encoder_info);
			format = encoder_info.format;
			if (space == VIDEO_CS_DEFAULT)
				space = encoder_info.colorspace;
			if (range == VIDEO_RANGE_DEFAULT)
				range = encoder_info.range;
		} else {
			/* User explicitly requested a format: ask the encoder what texture format it
			 * would use for THAT specific input. This lets the encoder remap e.g. I444→AYUV,
			 * BGRA→GBRA, R10L→GBR10, etc. */
			encoder_info.format = format;
			encoder_info.colorspace = (space != VIDEO_CS_DEFAULT) ? space : info->colorspace;
			encoder_info.range = (range != VIDEO_RANGE_DEFAULT) ? range : info->range;
			encoder_info.width = obs_encoder_get_width(encoder);
			encoder_info.height = obs_encoder_get_height(encoder);

			/* Ask the encoder to remap our format to its texture equivalent (e.g. I444->AYUV).
			 * This needs a live context; before create() we keep the requested format as-is,
			 * which is already a texture-encodable layout for the explicit-preference path. */
			if (encoder->context.data && encoder->info.get_video_info)
				encoder->info.get_video_info(encoder->context.data, &encoder_info);

			/* The encoder callback may have remapped our format to its texture equivalent. */
			if (encoder_info.format != format) {
				if (format_conversion_is_lossless(format, encoder_info.format)) {
					/* Lossless repack: use the encoder's actual texture format. */
					blog(LOG_INFO,
					     "Encoder \"%s\": texture input uses %s layout (requested %s, lossless equivalent).",
					     obs_encoder_get_name(encoder),
					     get_video_format_name(encoder_info.format),
					     get_video_format_name(format));
					format = encoder_info.format;
				} else {
					/* Lossy conversion: the encoder cannot handle the requested format via texture. */
					blog(LOG_ERROR,
					     "Encoder \"%s\": requested video format %s is not supported for GPU texture "
					     "encoding. The encoder requires %s, which would require a lossy conversion "
					     "(chroma subsampling or bit-depth reduction). No scaling mix will be created.",
					     obs_encoder_get_name(encoder),
					     get_video_format_name(format),
					     get_video_format_name(encoder_info.format));
					return MIX_ACQUIRE_FAILED;
				}
			}

			if (space == VIDEO_CS_DEFAULT)
				space = encoder_info.colorspace;
			if (range == VIDEO_RANGE_DEFAULT)
				range = encoder_info.range;
		}

		conversion_requested = format != info->format || space != info->colorspace || range != info->range;
	} else {
		/* CPU/raw encoder. Default to the base canvas format, but if the encoder's delivery
		 * callback requests a GPU-convertible YUV format that differs from the base, honor it:
		 * we then deliver already-converted YUV via GPU readback instead of doing a per-frame CPU
		 * swscale conversion. The format is taken from get_video_info (the exact value
		 * start_raw_video requests) so the mix output always matches the raw connection and no
		 * CPU scaler is created for it. Guarded on context.data: before the encoder's create() has
		 * run, the callback may not be safe to invoke, so we fall back to base-format delivery. */
		format = info->format;
		space = info->colorspace;
		range = info->range;

		if (encoder->context.data != NULL) {
			struct video_scale_info encoder_info = {0};
			get_video_info(encoder, &encoder_info);

			if (encoder_info.format != VIDEO_FORMAT_NONE && raw_gpu_conversion_supported(encoder_info.format) &&
			    encoder_info.format != info->format) {
				format = encoder_info.format;
				space = (encoder_info.colorspace != VIDEO_CS_DEFAULT) ? encoder_info.colorspace : info->colorspace;
				range = (encoder_info.range != VIDEO_RANGE_DEFAULT) ? encoder_info.range : info->range;

				blog(LOG_INFO,
				     "Encoder '%s': delivering %s via GPU conversion (base %s) to avoid per-frame CPU swscale",
				     obs_encoder_get_name(encoder), get_video_format_name(format), get_video_format_name(info->format));
			}
		}

		conversion_requested = format != info->format || space != info->colorspace || range != info->range;
	}

	/* No-fake-high-fidelity guard: refuse to create a mix whose target format needs MORE
	 * fidelity than the base canvas actually holds (higher bit depth or finer chroma). Such a
	 * conversion would only pad/interpolate, producing output that over-claims its fidelity.
	 * Surface this as a hard error so the bad selection blocks start instead of silently
	 * mis-encoding at the base canvas's lower fidelity. */
	if (format_needs_higher_fidelity(info->format, format)) {
		char err[512];
		snprintf(err, sizeof(err),
		         "Encoder '%s' cannot deliver %s from the current video output format (%s): that would be a "
		         "fake high-fidelity stream (padded bit depth and/or interpolated chroma). Change the video "
		         "output format (Settings -> Video) to a matching or higher-fidelity format such as RGBA16F.",
		         obs_encoder_get_name(encoder), get_video_format_name(format), get_video_format_name(info->format));
		blog(LOG_ERROR, "%s", err);
		obs_encoder_set_last_error(encoder, err);
		return MIX_ACQUIRE_FAILED;
	}

	width = encoder->scaled_width ? encoder->scaled_width : info->width;
	height = encoder->scaled_height ? encoder->scaled_height : info->height;
	const bool scaling_needed = (width != info->width || height != info->height);

	if (!scaling_needed && !conversion_requested)
		return MIX_ACQUIRE_NOT_NEEDED;

	current_mix = get_mix_for_video(encoder->media);
	if (!current_mix)
	{
		blog(LOG_ERROR, "Encoder '%s': no video mix found for the current source", obs_encoder_get_name(encoder));
		return MIX_ACQUIRE_FAILED;
	}

	/* OBS_SCALE_DISABLE means "no GPU scaler configured". If the encoder's scaled size
	 * differs from the source, some scaler must be used to honor the requested resolution -
	 * fall back to the default one in that case. The actual GPU downscale happens from the
	 * full base canvas texture (current_mix->ovi.base_width/height), not from the media's
	 * output-scaled size reported by video_output_get_info(). The fallback is stored on the
	 * encoder so every consumer (mix reuse matching, obs_encoder_gpu_scaling_enabled(),
	 * encoder plugins choosing their input path) sees a consistent scale type. */
	if (encoder->gpu_scale_type == OBS_SCALE_DISABLE && scaling_needed) {
		encoder->gpu_scale_type = OBS_SCALE_BICUBIC;
		blog(LOG_INFO,
		     "Encoder '%s': scaled size %ux%u differs from source output %ux%u (base canvas %ux%u is the "
		     "actual downscale source), using default scaler (BICUBIC)",
		     obs_encoder_get_name(encoder), width, height, info->width, info->height, current_mix->ovi.base_width,
		     current_mix->ovi.base_height);
	}
	enum obs_scale_type mix_scale_type = encoder->gpu_scale_type;

	pthread_mutex_lock(&obs->video.mixes_mutex);
	for (size_t i = 0; i < obs->video.mixes.num; i++) {
		struct obs_core_video_mix *current = obs->video.mixes.array[i];
		const struct video_output_info *voi = video_output_get_info(current->video);
		if (current_mix->view != current->view)
			continue;

		if (current->ovi.scale_type != mix_scale_type)
			continue;

		if (voi->width != width || voi->height != height)
			continue;

		if (voi->format != format || voi->colorspace != space || voi->range != range)
			continue;

		current->encoder_refs += 1;
		mix = current;
		break;
	}
	pthread_mutex_unlock(&obs->video.mixes_mutex);

	if (mix) {
		*out_mix = mix;
		return MIX_ACQUIRE_SUCCESS;
	}

	struct obs_video_info ovi = current_mix->ovi;

	ovi.output_format = format;
	ovi.colorspace = space;
	ovi.range = range;

	ovi.output_height = height;
	ovi.output_width = width;
	ovi.scale_type = mix_scale_type;

	ovi.gpu_conversion = true;

	struct obs_core_video_mix *created = obs_create_video_mix(&ovi);
	if (!created)
	{
		blog(LOG_ERROR, "Encoder '%s': failed to create scaling mix for %ux%u", obs_encoder_get_name(encoder), width,
		     height);
		return MIX_ACQUIRE_FAILED;
	}

	/* Check that mix actually has the GPU texture format we want */
	if (is_tex_encoder && created->encoder_texture_format != format) {
		blog(LOG_ERROR,
		     "Encoder \"%s\": GPU texture format %s is not available on this system/GPU. "
		     "Cannot create scaling mix.",
		     obs_encoder_get_name(encoder), get_video_format_name(format));
		obs_free_video_mix(created);
		return MIX_ACQUIRE_FAILED;
	}

	/* For CPU/raw encoders, make sure GPU conversion was actually set up for the requested format
	 * (i.e. convert_textures were allocated). If not (e.g. software/no-GPU mode), drop this mix and
	 * let the encoder fall back to base-format delivery + CPU swscale. */
	if (!is_tex_encoder && format != info->format && !created->convert_textures[0]) {
		blog(LOG_WARNING, "Encoder '%s': GPU conversion unavailable for %s, falling back to CPU conversion",
		     obs_encoder_get_name(encoder), get_video_format_name(format));
		obs_free_video_mix(created);
		return MIX_ACQUIRE_NOT_NEEDED;
	}

	created->encoder_only_mix = true;
	created->encoder_refs = 1;
	created->view = current_mix->view;

	pthread_mutex_lock(&obs->video.mixes_mutex);

	// double check that nobody else added a matching mix while we created it
	for (size_t i = 0; i < obs->video.mixes.num; i++) {
		struct obs_core_video_mix *current = obs->video.mixes.array[i];
		const struct video_output_info *voi = video_output_get_info(current->video);
		if (current->view != current_mix->view)
			continue;

		if (current->ovi.scale_type != mix_scale_type)
			continue;

		if (voi->width != width || voi->height != height)
			continue;

		if (voi->format != format || voi->colorspace != space || voi->range != range)
			continue;

		current->encoder_refs += 1;
		mix = current;
		break;
	}

	if (!mix) {
		da_push_back(obs->video.mixes, &created);
		mix = created;
	} else {
		obs_free_video_mix(created);
	}

	pthread_mutex_unlock(&obs->video.mixes_mutex);

	*out_mix = mix;
	return MIX_ACQUIRE_SUCCESS;
}

/* Log the texture-reuse path (Option A/B) an encoder-only mix will take relative to its source
 * composite. Whether the same-view composite is aliased (B) or blit-down (A) depends on color
 * space and output size vs base size - computed with the same inputs render_main_texture() uses. */
static void log_encoder_only_mix_reuse(const char *action, const char *name,
                                       const struct obs_core_video_mix *src_mix,
                                       const struct obs_core_video_mix *mix)
{
	if (!mix || !src_mix)
		return;

	const struct video_output_info *voi = video_output_get_info(mix->video);
	const bool can_reuse = render_space_reusable(src_mix->render_space, mix->render_space);
	const enum mix_reuse_path reuse_path =
		can_reuse ? get_mix_reuse_path(src_mix->render_space, mix->render_space,
		                              mix->ovi.output_width, mix->ovi.output_height,
		                              mix->ovi.base_width, mix->ovi.base_height)
		             : MIX_REUSE_BLIT_DOWN;

	blog(LOG_INFO,
	     "%s \"%s\"\n"
	     "\tresolution: %dx%d\n"
	     "\tscaling:    %s\n"
	     "\tformat:     %s\n"
	     "\tspace:      %s\n"
	     "\trange:      %s\n"
	     "\treuse:      %s (%s -> %s)\n",
	     action, name, voi->width, voi->height, get_scale_type_name(mix->ovi.scale_type),
	     get_video_format_name(voi->format), get_video_colorspace_name(voi->colorspace),
	     get_video_range_name(voi->format, voi->range),
	     can_reuse ? mix_reuse_path_name(reuse_path) : "none (full re-render)",
	     mix_reuse_space_name(src_mix->render_space), mix_reuse_space_name(mix->render_space));
}

/**
 * Set up GPU based rescaling for this encoder (init-time path). Binds the
 * encoder to an existing or newly created encoder-only mix.
 */
static bool maybe_set_up_gpu_rescale(struct obs_encoder *encoder)
{
	struct obs_core_video_mix *mix = NULL;
	enum encoder_mix_acquire result = acquire_encoder_only_mix(encoder, &mix);

	if (result == MIX_ACQUIRE_FAILED)
		return false;

	if (result == MIX_ACQUIRE_SUCCESS) {
		/* Use the internal setter so source_video keeps pointing at the original canvas. */
		encoder_set_video(encoder, mix->video);
		log_encoder_only_mix_reuse("Bound encoder-only mix for", obs_encoder_get_name(encoder),
		                            get_mix_for_video(encoder->source_video), mix);
	}
	/* MIX_ACQUIRE_NOT_NEEDED: no scaling/conversion required, consume the source as-is. */
	return true;
}

/**
 * Pre-create/bind the encoder-only texture mix BEFORE the encoder's create() runs.
 *
 * Texture encoders (e.g. nvenc) decide at create() time whether to use the GPU
 * shared-texture path by querying obs_encoder_video_tex_active(), which only
 * returns true if a mix exposing that format is already bound to the encoder.
 * When the base canvas is a high-fidelity non-texture format (e.g. RGBA16F, used
 * as the multitrack master), no such mix exists yet: it would normally be created
 * by maybe_set_up_gpu_rescale() *after* create(), which is too late - the encoder
 * has already committed to its CPU/soft path.
 *
 * For a texture encoder with an explicit preferred format we can determine the
 * target mix without context.data, so we bind it here (pre-create). The encoder's
 * create() then sees an active shared-texture path and takes the GPU fast path,
 * while the high-fidelity master keeps its full fidelity. This works for every
 * texture-encodable format (P010/NV12/Y410/GBR10/GBRA/AYUV/R10L), so each
 * multitrack track can pick its own output format and still encode on the GPU.
 */
static bool maybe_pre_bind_texture_mix(struct obs_encoder *encoder)
{
	if (encoder->orig_info.type != OBS_ENCODER_VIDEO)
		return false;
	if ((encoder->info.caps & OBS_ENCODER_CAP_PASS_TEXTURE) == 0)
		return false;
	if (encoder->preferred_format == VIDEO_FORMAT_NONE)
		return false;

	struct obs_core_video_mix *mix = NULL;
	if (acquire_encoder_only_mix(encoder, &mix) != MIX_ACQUIRE_SUCCESS)
		return false;

	/* Use the internal setter so source_video keeps pointing at the original canvas. */
	encoder_set_video(encoder, mix->video);
	log_encoder_only_mix_reuse("Pre-bound encoder-only mix for", obs_encoder_get_name(encoder),
	                            get_mix_for_video(encoder->source_video), mix);
	return true;
}

static void add_connection(struct obs_encoder *encoder)
{
	if (encoder->info.type == OBS_ENCODER_AUDIO) {
		struct audio_convert_info audio_info = {0};
		get_audio_info(encoder, &audio_info);

		audio_output_connect(encoder->media, encoder->mixer_idx, &audio_info, receive_audio, encoder);
	} else {
		struct video_scale_info info = {0};
		get_video_info(encoder, &info);

		if (gpu_encode_available(encoder)) {
			start_gpu_encode(encoder);
		} else {
			start_raw_video(encoder->media, &info, encoder->frame_rate_divisor, receive_video, encoder);
		}
	}

	if (encoder->encoder_group) {
		pthread_mutex_lock(&encoder->encoder_group->mutex);
		encoder->encoder_group->num_encoders_started += 1;
		bool ready = encoder->encoder_group->num_encoders_started >= encoder->encoder_group->encoders.num;
		pthread_mutex_unlock(&encoder->encoder_group->mutex);
		if (ready)
			add_ready_encoder_group(encoder);
	}

	set_encoder_active(encoder, true);
}

void obs_encoder_group_actually_destroy(obs_encoder_group_t *group);
static void remove_connection(struct obs_encoder *encoder, bool shutdown)
{
	if (encoder->info.type == OBS_ENCODER_AUDIO) {
		audio_output_disconnect(encoder->media, encoder->mixer_idx, receive_audio, encoder);
	} else {
		if (gpu_encode_available(encoder)) {
			stop_gpu_encode(encoder);
		} else {
			stop_raw_video(encoder->media, receive_video, encoder);
		}
	}

	if (encoder->encoder_group) {
		pthread_mutex_lock(&encoder->encoder_group->mutex);
		if (--encoder->encoder_group->num_encoders_started == 0)
			encoder->encoder_group->start_timestamp = 0;
		pthread_mutex_unlock(&encoder->encoder_group->mutex);
	}

	/* obs_encoder_shutdown locks init_mutex, so don't call it on encode
	 * errors, otherwise you can get a deadlock with outputs when they end
	 * data capture, which will lock init_mutex and the video callback
	 * mutex in the reverse order.  instead, call shutdown before starting
	 * up again */
	if (shutdown)
		obs_encoder_shutdown(encoder);
	encoder->initialized = false;

	set_encoder_active(encoder, false);
}

static inline void free_audio_buffers(struct obs_encoder *encoder)
{
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		deque_free(&encoder->audio_input_buffer[i]);
		bfree(encoder->audio_output_buffer[i]);
		encoder->audio_output_buffer[i] = NULL;
	}
}

void obs_encoder_destroy(obs_encoder_t *encoder)
{
	if (encoder) {
		pthread_mutex_lock(&encoder->outputs_mutex);
		for (size_t i = 0; i < encoder->outputs.num; i++) {
			struct obs_output *output = encoder->outputs.array[i];
			// This happens while the output is still "active", so
			// remove without checking active
			obs_output_remove_encoder_internal(output, encoder);
		}
		da_free(encoder->outputs);
		pthread_mutex_unlock(&encoder->outputs_mutex);

		blog(LOG_DEBUG, "encoder '%s' destroyed", encoder->context.name);

		obs_encoder_set_group(encoder, NULL);

		free_audio_buffers(encoder);

		if (encoder->context.data)
			encoder->info.destroy(encoder->context.data);
		da_free(encoder->callbacks);
		da_free(encoder->roi);
		da_free(encoder->encoder_packet_times);
		pthread_mutex_destroy(&encoder->init_mutex);
		pthread_mutex_destroy(&encoder->callbacks_mutex);
		pthread_mutex_destroy(&encoder->outputs_mutex);
		pthread_mutex_destroy(&encoder->pause.mutex);
		pthread_mutex_destroy(&encoder->roi_mutex);
		obs_context_data_free(&encoder->context);
		if (encoder->owns_info_id)
			bfree((void *)encoder->info.id);
		if (encoder->last_error_message)
			bfree(encoder->last_error_message);
		if (encoder->fps_override)
			video_output_free_frame_rate_divisor(encoder->fps_override);
		bfree(encoder);
	}
}

const char *obs_encoder_get_name(const obs_encoder_t *encoder)
{
	return obs_encoder_valid(encoder, "obs_encoder_get_name") ? encoder->context.name : NULL;
}

void obs_encoder_set_name(obs_encoder_t *encoder, const char *name)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_set_name"))
		return;

	if (name && *name && strcmp(name, encoder->context.name) != 0)
		obs_context_data_setname(&encoder->context, name);
}

static inline obs_data_t *get_defaults(const struct obs_encoder_info *info)
{
	obs_data_t *settings = obs_data_create();
	if (info->get_defaults) {
		info->get_defaults(settings);
	}
	if (info->get_defaults2) {
		info->get_defaults2(settings, info->type_data);
	}
	return settings;
}

obs_data_t *obs_encoder_defaults(const char *id)
{
	const struct obs_encoder_info *info = find_encoder(id);
	return (info) ? get_defaults(info) : NULL;
}

obs_data_t *obs_encoder_get_defaults(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_defaults"))
		return NULL;

	return get_defaults(&encoder->info);
}

/* Maps a user-facing Color Format name (the string values stored in the "color_format" list
 * property) to a video_format. Returns VIDEO_FORMAT_NONE for unknown or empty names. */
static enum video_format color_format_name_to_enum(const char *name)
{
	if (!name || !*name)
		return VIDEO_FORMAT_NONE;

	if (astrcmpi(name, "NV12") == 0)
		return VIDEO_FORMAT_NV12;
	if (astrcmpi(name, "P010") == 0)
		return VIDEO_FORMAT_P010;
	if (astrcmpi(name, "I422") == 0)
		return VIDEO_FORMAT_I422;
	if (astrcmpi(name, "P216") == 0)
		return VIDEO_FORMAT_P216;
	if (astrcmpi(name, "I210") == 0)
		return VIDEO_FORMAT_I210;
	if (astrcmpi(name, "I444") == 0)
		return VIDEO_FORMAT_I444;
	if (astrcmpi(name, "I412") == 0)
		return VIDEO_FORMAT_I412;
	if (astrcmpi(name, "Y410") == 0)
		return VIDEO_FORMAT_Y410;
	if (astrcmpi(name, "BGRA") == 0)
		return VIDEO_FORMAT_BGRA;
	if (astrcmpi(name, "GBR10") == 0)
		return VIDEO_FORMAT_GBR10;
	if (astrcmpi(name, "R10L") == 0)
		return VIDEO_FORMAT_R10L;
	if (astrcmpi(name, "R10P") == 0)
		return VIDEO_FORMAT_R10P;
	if (astrcmpi(name, "YUV420P12") == 0)
		return VIDEO_FORMAT_YUV420P12;
	if (astrcmpi(name, "YUV422P12") == 0)
		return VIDEO_FORMAT_YUV422P12;
	if (astrcmpi(name, "YUV444P12") == 0)
		return VIDEO_FORMAT_YUV444P12;
	if (astrcmpi(name, "GBRP12") == 0)
		return VIDEO_FORMAT_GBRP12;
	if (astrcmpi(name, "GBRP") == 0)
		return VIDEO_FORMAT_GBRP;
	if (astrcmpi(name, "YUV444P10") == 0)
		return VIDEO_FORMAT_YUV444P10;

	return VIDEO_FORMAT_NONE;
}

/* User-facing entries for the generic Color Format dropdown. `value` is the string stored in the
 * encoder settings (parsed back by color_format_name_to_enum); `label` is what the user sees. */
struct color_format_entry {
	enum video_format fmt;
	const char *label;
	const char *value;
};

static const struct color_format_entry color_format_entries[] = {
	{ VIDEO_FORMAT_NV12, "YUV 4:2:0 (8-bit)", "NV12" },
	{ VIDEO_FORMAT_P010, "YUV 4:2:0 (10-bit)", "P010" },
	{ VIDEO_FORMAT_YUV420P12, "YUV 4:2:0 (12-bit)", "YUV420P12" },
	{ VIDEO_FORMAT_I422, "YUV 4:2:2 (8-bit)", "I422" },
	{ VIDEO_FORMAT_P216, "YUV 4:2:2 (10-bit)", "P216" },
	{ VIDEO_FORMAT_I210, "YUV 4:2:2 (10-bit, planar)", "I210" },
	{ VIDEO_FORMAT_YUV422P12, "YUV 4:2:2 (12-bit)", "YUV422P12" },
	{ VIDEO_FORMAT_I444, "YUV 4:4:4 (8-bit)", "I444" },
	{ VIDEO_FORMAT_YUV444P10, "YUV 4:4:4 (10-bit)", "YUV444P10" },
	{ VIDEO_FORMAT_I412, "YUV 4:4:4 (12-bit)", "I412" },
	{ VIDEO_FORMAT_Y410, "YUV 4:4:4 (10-bit, packed)", "Y410" },
	{ VIDEO_FORMAT_YUV444P12, "YUV 4:4:4 (12-bit, planar u16)", "YUV444P12" },
	{ VIDEO_FORMAT_BGRA, "RGB (8-bit)", "BGRA" },
	{ VIDEO_FORMAT_GBRP, "RGB (8-bit, planar)", "GBRP" },
	{ VIDEO_FORMAT_GBR10, "RGB (10-bit)", "GBR10" },
	{ VIDEO_FORMAT_R10P, "RGB (10-bit, planar)", "R10P" },
	{ VIDEO_FORMAT_GBRP12, "RGB (12-bit)", "GBRP12" },
};

#define COLOR_FORMAT_ENTRY_COUNT (sizeof(color_format_entries) / sizeof(color_format_entries[0]))

/* Asks the encoder's get_video_info callback whether it can accept `candidate` as its input format,
 * using the same lossless-equivalence rule the delivery path uses (acquire_encoder_only_mix): a format
 * is accepted if the callback keeps it or remaps it to a lossless equivalent (e.g. I444 -> AYUV). Only
 * called for texture encoders whose get_video_info tolerates a NULL context (true for NVENC/QSV). */
static bool encoder_accepts_format(const struct obs_encoder_info *ei, enum video_format candidate)
{
	struct video_scale_info info = {0};

	if (!ei->get_video_info)
		return false;

	info.format = candidate;
	ei->get_video_info(NULL, &info);

	if (info.format == VIDEO_FORMAT_NONE)
		return false;

	return format_conversion_is_lossless(candidate, info.format);
}

/* Adds the generic Color Format / Color Space / Color Range dropdowns to a video encoder's property
 * tree so they are available for every video encoder in Advanced Output Mode. Encoders that already
 * expose their own controls (e.g. x264/x265) keep theirs: each control is only added when the
 * matching property name is not already present. The Color Format list offers every format the
 * encoder actually accepts (probed via get_video_info for texture encoders). Because the video output
 * format defaults to RGBA16F, all of these are legitimate down-conversions; if a user lowers the base
 * canvas below a selected target's fidelity, delivery is refused at start time with an error. */
static void add_encoder_color_properties(obs_properties_t *props, const struct obs_encoder_info *ei)
{
	obs_property_t *p;
	const bool is_texture = ei && (ei->caps & OBS_ENCODER_CAP_PASS_TEXTURE) != 0;

	if (!props || !ei)
		return;

	if (!obs_properties_get(props, "color_format")) {
		size_t i;
		p = obs_properties_add_list(props, "color_format", "Color Format", OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_STRING);
		for (i = 0; i < COLOR_FORMAT_ENTRY_COUNT; i++) {
			enum video_format fmt = color_format_entries[i].fmt;
			bool supported;

			if (ei->is_color_format_supported)
				supported = ei->is_color_format_supported(ei->type_data, fmt);
			else if (is_texture)
				supported = encoder_accepts_format(ei, fmt);
			else {
				/* Non-texture encoders always accept NV12; 10-bit codecs additionally
				 * accept P010 so HEVC/AV1 can default to YUV 4:2:0 (10-bit). */
				bool ten_bit = ei->codec && (astrcmpi(ei->codec, "hevc") == 0 ||
							     astrcmpi(ei->codec, "av1") == 0);
				supported = (fmt == VIDEO_FORMAT_NV12) || (ten_bit && fmt == VIDEO_FORMAT_P010);
			}

			if (supported)
				obs_property_list_add_string(p, color_format_entries[i].label,
							     color_format_entries[i].value);
		}
	}

	if (!obs_properties_get(props, "color_space")) {
		p = obs_properties_add_list(props, "color_space", "Color Space", OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, "Rec. 709", (long long)VIDEO_CS_709);
		obs_property_list_add_int(p, "Rec. 601", (long long)VIDEO_CS_601);
		obs_property_list_add_int(p, "sRGB", (long long)VIDEO_CS_SRGB);
		obs_property_list_add_int(p, "Rec. 2100 PQ (HDR)", (long long)VIDEO_CS_2100_PQ);
		obs_property_list_add_int(p, "Rec. 2100 HLG (HDR)", (long long)VIDEO_CS_2100_HLG);
	}

	if (!obs_properties_get(props, "color_range")) {
		p = obs_properties_add_list(props, "color_range", "Color Range", OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, "Partial (Limited)", (long long)VIDEO_RANGE_PARTIAL);
		obs_property_list_add_int(p, "Full", (long long)VIDEO_RANGE_FULL);
	}
}

/* Applies the user's per-encoder Color Format / Space / Range choices to the encoder's preferred
 * delivery settings. Only non-default picks override the base (Advanced -> Video) values; leaving a
 * control on "(Default)" keeps the normal base-format delivery path unchanged. Encoders that manage
 * these settings themselves (e.g. x264/x265) re-publish their own resolved values from create()/
 * update(), so this is effectively a no-op for them. */
static void apply_encoder_color_settings(obs_encoder_t *encoder)
{
	obs_data_t *settings;
	const char *fmt_name;
	long long cs;
	long long rg;

	if (!encoder || encoder->info.type != OBS_ENCODER_VIDEO)
		return;

	settings = encoder->context.settings;
	if (!settings)
		return;

	fmt_name = obs_data_get_string(settings, "color_format");
	if (fmt_name && strcmp(fmt_name, "Default") != 0) {
		enum video_format fmt = color_format_name_to_enum(fmt_name);
		if (fmt != VIDEO_FORMAT_NONE)
			obs_encoder_set_preferred_video_format(encoder, fmt);
	}

	cs = obs_data_get_int(settings, "color_space");
	if (cs != (long long)VIDEO_CS_DEFAULT)
		obs_encoder_set_preferred_color_space(encoder, (enum video_colorspace)cs);

	rg = obs_data_get_int(settings, "color_range");
	if (rg != (long long)VIDEO_RANGE_DEFAULT)
		obs_encoder_set_preferred_range(encoder, (enum video_range_type)rg);
}

obs_properties_t *obs_get_encoder_properties(const char *id)
{
	const struct obs_encoder_info *ei = find_encoder(id);
	if (ei && (ei->get_properties || ei->get_properties2)) {
		obs_data_t *defaults = get_defaults(ei);
		obs_properties_t *properties = NULL;

		if (ei->get_properties2) {
			properties = ei->get_properties2(NULL, ei->type_data);
		} else if (ei->get_properties) {
			properties = ei->get_properties(NULL);
		}

		obs_properties_apply_settings(properties, defaults);
		if (ei->type == OBS_ENCODER_VIDEO)
			add_encoder_color_properties(properties, ei);
		obs_data_release(defaults);
		return properties;
	}
	return NULL;
}

obs_properties_t *obs_encoder_properties(const obs_encoder_t *encoder)
{
	obs_properties_t *props = NULL;

	if (!obs_encoder_valid(encoder, "obs_encoder_properties"))
		return NULL;

	if (encoder->orig_info.get_properties2) {
		props = encoder->orig_info.get_properties2(encoder->context.data, encoder->orig_info.type_data);
	} else if (encoder->orig_info.get_properties) {
		props = encoder->orig_info.get_properties(encoder->context.data);
	}

	if (!props)
		return NULL;

	obs_properties_apply_settings(props, encoder->context.settings);
	if (encoder->orig_info.type == OBS_ENCODER_VIDEO)
		add_encoder_color_properties(props, &encoder->orig_info);
	return props;
}

void obs_encoder_update(obs_encoder_t *encoder, obs_data_t *settings)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_update"))
		return;

	obs_data_apply(encoder->context.settings, settings);

	/* Keep the preferred delivery format/space/range in sync with this encoder's Color Format /
	 * Space / Range choices (see apply_encoder_color_settings). Encoders that manage these
	 * themselves re-publish their own values from update() below. */
	apply_encoder_color_settings(encoder);

	// Encoder isn't initialized yet, only apply changes to settings
	if (!encoder->context.data)
		return;

	// Encoder doesn't support updates
	if (!encoder->info.update)
		return;

	// If the encoder is active we defer the update as it may not be
	// reentrant. Setting reconfigure_requested to true makes the changes
	// apply at the next possible moment in the encoder / GPU encoder
	// thread.
	if (encoder_active(encoder)) {
		encoder->reconfigure_requested = true;
	} else {
		encoder->info.update(encoder->context.data, encoder->context.settings);
	}
}

bool obs_encoder_get_extra_data(const obs_encoder_t *encoder, uint8_t **extra_data, size_t *size)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_extra_data"))
		return false;

	if (encoder->info.get_extra_data && encoder->context.data)
		return encoder->info.get_extra_data(encoder->context.data, extra_data, size);

	return false;
}

obs_data_t *obs_encoder_get_settings(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_settings"))
		return NULL;

	obs_data_addref(encoder->context.settings);
	return encoder->context.settings;
}

static inline void reset_audio_buffers(struct obs_encoder *encoder)
{
	free_audio_buffers(encoder);

	for (size_t i = 0; i < encoder->planes; i++)
		encoder->audio_output_buffer[i] = bmalloc(encoder->framesize_bytes);
}

static void intitialize_audio_encoder(struct obs_encoder *encoder)
{
	struct audio_convert_info info = {0};
	get_audio_info(encoder, &info);

	encoder->samplerate = info.samples_per_sec;
	encoder->planes = get_audio_planes(info.format, info.speakers);
	encoder->blocksize = get_audio_size(info.format, info.speakers, 1);
	encoder->framesize = encoder->info.get_frame_size(encoder->context.data);

	encoder->framesize_bytes = encoder->blocksize * encoder->framesize;
	reset_audio_buffers(encoder);
}

static THREAD_LOCAL bool can_reroute = false;

static inline bool obs_encoder_initialize_internal(obs_encoder_t *encoder)
{
	if (!encoder->media) {
		blog(LOG_ERROR, "obs_encoder_initialize_internal: encoder '%s' has no media set",
		     encoder->context.name);
		return false;
	}

	if (encoder_active(encoder))
		return true;
	if (encoder->initialized)
		return true;

	obs_encoder_shutdown(encoder);

	bool pre_bound = false;

	if (encoder->orig_info.create) {
		can_reroute = true;
		encoder->info = encoder->orig_info;
		apply_encoder_color_settings(encoder);
		/* Bind the texture delivery mix before create() so a texture encoder sees an active
		 * shared-texture path at its create() time and takes the GPU fast path, even when the
		 * base canvas is a high-fidelity non-texture format (e.g. RGBA16F multitrack master). */
		pre_bound = maybe_pre_bind_texture_mix(encoder);
		encoder->context.data = encoder->orig_info.create(encoder->context.settings, encoder);
		can_reroute = false;
	}
	if (!encoder->context.data)
		return false;

	/* Bind the delivery mix only after create() so context.data is valid. CPU/raw encoders
	 * (x264/x265/openh264/AV1) publish their preferred YUV delivery format via get_video_info,
	 * which we can only query once the encoder context exists - this lets us bind an optimized
	 * GPU-conversion mix (e.g. NV12/I010) instead of falling back to base-format + CPU swscale.
	 * Texture encoders already pre-bound above are skipped here to avoid a double refcount. */
	if (encoder->orig_info.type == OBS_ENCODER_VIDEO && !pre_bound && !maybe_set_up_gpu_rescale(encoder)) {
		blog(LOG_ERROR, "Encoder '%s': failed to establish video delivery, encoder cannot start",
		     obs_encoder_get_name(encoder));
		encoder->info.destroy(encoder->context.data);
		encoder->context.data = NULL;
		return false;
	}

	if (encoder->orig_info.type == OBS_ENCODER_AUDIO)
		intitialize_audio_encoder(encoder);

	encoder->initialized = true;
	return true;
}

void *obs_encoder_create_rerouted(obs_encoder_t *encoder, const char *reroute_id)
{
	if (!obs_ptr_valid(encoder, "obs_encoder_reroute"))
		return NULL;
	if (!obs_ptr_valid(reroute_id, "obs_encoder_reroute"))
		return NULL;
	if (!can_reroute)
		return NULL;

	const struct obs_encoder_info *ei = find_encoder(reroute_id);
	if (ei) {
		if (ei->type != encoder->orig_info.type || astrcmpi(ei->codec, encoder->orig_info.codec) != 0) {
			return NULL;
		}
		encoder->info = *ei;
		return encoder->info.create(encoder->context.settings, encoder);
	}

	return NULL;
}

bool obs_encoder_initialize(obs_encoder_t *encoder)
{
	bool success;

	if (!encoder)
		return false;

	pthread_mutex_lock(&encoder->init_mutex);
	success = obs_encoder_initialize_internal(encoder);
	pthread_mutex_unlock(&encoder->init_mutex);

	return success;
}

/**
 * free video mix if it's an encoder only video mix
 * see `maybe_set_up_gpu_rescale`
 */
static void maybe_clear_encoder_core_video_mix(obs_encoder_t *encoder)
{
	pthread_mutex_lock(&obs->video.mixes_mutex);
	for (size_t i = 0; i < obs->video.mixes.num; i++) {
		struct obs_core_video_mix *mix = obs->video.mixes.array[i];
		if (mix->video != encoder->media)
			continue;

		if (!mix->encoder_only_mix)
			break;

		encoder_set_video(encoder, obs_get_video());
		mix->encoder_refs -= 1;
		if (mix->encoder_refs == 0) {
			da_erase(obs->video.mixes, i);
			obs_free_video_mix(mix);
		}
	}
	pthread_mutex_unlock(&obs->video.mixes_mutex);
}

void obs_encoder_shutdown(obs_encoder_t *encoder)
{
	pthread_mutex_lock(&encoder->init_mutex);
	if (encoder->context.data) {
		encoder->info.destroy(encoder->context.data);
		encoder->context.data = NULL;
		encoder->first_received = false;
		encoder->offset_usec = 0;
		encoder->start_ts = 0;
		encoder->frame_rate_divisor_counter = 0;
		maybe_clear_encoder_core_video_mix(encoder);

		for (size_t i = 0; i < encoder->paired_encoders.num; i++) {
			obs_weak_encoder_release(encoder->paired_encoders.array[i]);
		}
		da_free(encoder->paired_encoders);
	}
	obs_encoder_set_last_error(encoder, NULL);
	pthread_mutex_unlock(&encoder->init_mutex);
}

static inline size_t get_callback_idx(const struct obs_encoder *encoder, encoded_callback_t new_packet, void *param)
{
	for (size_t i = 0; i < encoder->callbacks.num; i++) {
		struct encoder_callback *cb = encoder->callbacks.array + i;

		if (cb->new_packet == new_packet && cb->param == param)
			return i;
	}

	return DARRAY_INVALID;
}

void pause_reset(struct pause_data *pause)
{
	pthread_mutex_lock(&pause->mutex);
	pause->last_video_ts = 0;
	pause->ts_start = 0;
	pause->ts_end = 0;
	pause->ts_offset = 0;
	pthread_mutex_unlock(&pause->mutex);
}

static inline void obs_encoder_start_internal(obs_encoder_t *encoder, encoded_callback_t new_packet, void *param)
{
	struct encoder_callback cb = {false, new_packet, param};
	bool first = false;

	if (!encoder->context.data || !encoder->media)
		return;

	pthread_mutex_lock(&encoder->callbacks_mutex);

	first = (encoder->callbacks.num == 0);

	size_t idx = get_callback_idx(encoder, new_packet, param);
	if (idx == DARRAY_INVALID)
		da_push_back(encoder->callbacks, &cb);

	pthread_mutex_unlock(&encoder->callbacks_mutex);

	if (first) {
		os_atomic_set_bool(&encoder->paused, false);
		pause_reset(&encoder->pause);

		encoder->cur_pts = 0;
		// A fresh stream starts clean: drop any divisor change staged mid-stream so it is not re-applied on top
		// of the rate already configured for this start.
		encoder->pending_frame_rate_divisor = 0;
		encoder->staged_frame_rate_divisor = 0;
		add_connection(encoder);
	}
}

void obs_encoder_start(obs_encoder_t *encoder, encoded_callback_t new_packet, void *param)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_start"))
		return;
	if (!obs_ptr_valid(new_packet, "obs_encoder_start"))
		return;

	pthread_mutex_lock(&encoder->init_mutex);
	obs_encoder_start_internal(encoder, new_packet, param);
	pthread_mutex_unlock(&encoder->init_mutex);
}

void obs_encoder_stop(obs_encoder_t *encoder, encoded_callback_t new_packet, void *param)
{
	bool last = false;
	size_t idx;

	if (!obs_encoder_valid(encoder, "obs_encoder_stop"))
		return;
	if (!obs_ptr_valid(new_packet, "obs_encoder_stop"))
		return;

	pthread_mutex_lock(&encoder->init_mutex);
	pthread_mutex_lock(&encoder->callbacks_mutex);

	idx = get_callback_idx(encoder, new_packet, param);
	if (idx != DARRAY_INVALID) {
		da_erase(encoder->callbacks, idx);
		last = (encoder->callbacks.num == 0);
	}

	pthread_mutex_unlock(&encoder->callbacks_mutex);

	encoder->encoder_packet_times.num = 0;

	if (last) {
		remove_connection(encoder, true);
		pthread_mutex_unlock(&encoder->init_mutex);

		struct obs_encoder_group *group = encoder->encoder_group;

		/* Destroying the group all the way back here prevents a race
		 * where destruction of the group can prematurely destroy the
		 * encoder within internal functions. This is the point where it
		 * is safe to destroy the group, even if the encoder is then
		 * also destroyed. */
		if (group) {
			pthread_mutex_lock(&group->mutex);
			if (group->destroy_on_stop && group->num_encoders_started == 0)
				obs_encoder_group_actually_destroy(group);
			else
				pthread_mutex_unlock(&group->mutex);
		}

		/* init_mutex already unlocked */
		return;
	}

	pthread_mutex_unlock(&encoder->init_mutex);
}

const char *obs_encoder_get_codec(const obs_encoder_t *encoder)
{
	return obs_encoder_valid(encoder, "obs_encoder_get_codec") ? encoder->info.codec : NULL;
}

const char *obs_get_encoder_codec(const char *id)
{
	struct obs_encoder_info *info = find_encoder(id);
	return info ? info->codec : NULL;
}

enum obs_encoder_type obs_encoder_get_type(const obs_encoder_t *encoder)
{
	return obs_encoder_valid(encoder, "obs_encoder_get_type") ? encoder->info.type : OBS_ENCODER_AUDIO;
}

enum obs_encoder_type obs_get_encoder_type(const char *id)
{
	struct obs_encoder_info *info = find_encoder(id);
	return info ? info->type : OBS_ENCODER_AUDIO;
}

uint32_t obs_encoder_get_encoded_frames(const obs_encoder_t *encoder)
{
	return obs_encoder_valid(encoder, "obs_output_get_encoded_frames") ? encoder->encoded_frames : 0;
}

/**
 * Live-rebind an active encoder to the scaling/conversion mix that matches its
 * current scale type, scaled size and preferred format/colorspace/range.
 *
 * Generalized from the live-resize path in obs_encoder_set_scaled_size(): it
 * detaches the encoder from its current frame delivery source, (re)acquires
 * the encoder-only mix for the new settings, re-subscribes delivery and retires
 * the old mix. When nothing needs to be scaled or converted the encoder is
 * re-pointed at its original source video instead. The relevant encoder fields
 * must already hold the new values when this is called. If the new settings map
 * to the same delivery source nothing is done; if no mix can be acquired it
 * returns false and the delivery is left untouched, so the caller can roll the
 * settings back.
 */
static bool live_rebind_encoder_mix(struct obs_encoder *encoder)
{
	const bool tex_path = gpu_encode_available(encoder); // evaluated against the current source
	struct obs_core_video_mix *old_mix = get_mix_for_video(encoder->media);
	struct obs_core_video_mix *new_mix = NULL;

	enum encoder_mix_acquire result = acquire_encoder_only_mix(encoder, &new_mix);
	if (result == MIX_ACQUIRE_FAILED) {
		blog(LOG_WARNING, "Encoder '%s': live rebind could not be set up, keeping previous delivery",
		     obs_encoder_get_name(encoder));
		return false;
	}

	/* If nothing needs to be scaled or converted, frames are delivered directly from the
	 * original source video (e.g. when the encoder is reverted to the source size). */
	video_t *target = (result == MIX_ACQUIRE_SUCCESS) ? new_mix->video : encoder->source_video;
	if (!target) {
		blog(LOG_WARNING, "Encoder '%s': live rebind could not be set up (no source video), keeping previous delivery",
		     obs_encoder_get_name(encoder));
		return false;
	}

	if (target == encoder->media)
		return true; // delivery already matches the new settings

	/* Detach this encoder from its current frame delivery source. */
	if (tex_path) {
		stop_gpu_encode(encoder);
	} else if (encoder->media) {
		stop_raw_video(encoder->media, receive_video, encoder);
	}

	/* Re-point the encoder at the new source and re-subscribe its delivery. */
	encoder_set_video(encoder, target);

	struct video_scale_info info = {0};
	get_video_info(encoder, &info);

	if (tex_path) {
		start_gpu_encode(encoder);
	} else {
		start_raw_video(target, &info, encoder->frame_rate_divisor, receive_video, encoder);
	}

	/* Re-report the texture-reuse path for the mix now bound to this encoder: the reuse state can
	 * change across a live rescale (e.g. an SDR down-convert becomes Option A), so surface it again. */
	if (result == MIX_ACQUIRE_SUCCESS)
		log_encoder_only_mix_reuse("Live-rescaled encoder mix for", obs_encoder_get_name(encoder),
		                            get_mix_for_video(encoder->source_video), new_mix);

	/* Retire the previous encoder-only mix if we orphaned it. A NULL view marks the mix for
	 * reclamation by the graphics thread on its next tick. */
	if (old_mix && old_mix->encoder_only_mix) {
		old_mix->encoder_refs -= 1;
		if (old_mix->encoder_refs <= 0) {
			pthread_mutex_lock(&obs->video.mixes_mutex);
			old_mix->view = NULL;
			pthread_mutex_unlock(&obs->video.mixes_mutex);
		}
	}

	return true;
}

void obs_encoder_set_scaled_size(obs_encoder_t *encoder, uint32_t width, uint32_t height)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_set_scaled_size"))
		return;
	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_set_scaled_size: "
		     "encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return;
	}

	if (encoder_active(encoder)) {
		/* Live rescale while streaming (multitrack NVENC resolution override):
		 * re-point this encoder at an encoder-only mix that renders the new size, so
		 * frames arrive at the new dimensions from the next tick on. The video
		 * encoder plugin detects the new size on its encode side (obs-nvenc applies
		 * the NVENC session resize in place; other plugins may require a restart). */
		const struct video_output_info *voi = video_output_get_info(encoder->source_video);
		const bool matches_source = voi && voi->width == width && voi->height == height;
		const uint32_t new_width = matches_source ? 0 : width;
		const uint32_t new_height = matches_source ? 0 : height;

		if (new_width == encoder->scaled_width && new_height == encoder->scaled_height)
			return; // no change

		blog(LOG_INFO, "obs_encoder_set_scaled_size: live rescale for '%s' to %ux%u", obs_encoder_get_name(encoder),
		     width, height);

		const uint32_t prev_width = encoder->scaled_width;
		const uint32_t prev_height = encoder->scaled_height;

		encoder->scaled_width = new_width;
		encoder->scaled_height = new_height;

		if (!live_rebind_encoder_mix(encoder)) {
			blog(LOG_WARNING, "obs_encoder_set_scaled_size: live rescale for '%s' could not be set up, reverting",
			     obs_encoder_get_name(encoder));
			encoder->scaled_width = prev_width;
			encoder->scaled_height = prev_height;
		}

		return;
	}

	const struct video_output_info *voi;
	voi = video_output_get_info(encoder->media);
	if (voi && voi->width == width && voi->height == height) {
		blog(LOG_WARNING,
		     "encoder '%s': Scaled resolution "
		     "matches output resolution, scaling "
		     "disabled",
		     obs_encoder_get_name(encoder));
		encoder->scaled_width = encoder->scaled_height = 0;
		return;
	}

	if (encoder->initialized) {
		blog(LOG_WARNING,
		     "encoder '%s': Cannot set the scaled resolution "
		     "after the encoder has been initialized",
		     obs_encoder_get_name(encoder));
		return;
	}

	encoder->scaled_width = width;
	encoder->scaled_height = height;
}

void obs_encoder_set_gpu_scale_type(obs_encoder_t *encoder, enum obs_scale_type gpu_scale_type)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_set_gpu_scale_type"))
		return;
	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_set_gpu_scale_type: "
		     "encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return;
	}

	const enum obs_scale_type prev = encoder->gpu_scale_type;
	encoder->gpu_scale_type = gpu_scale_type;

	if (encoder_active(encoder)) {
		/* Live scale-type change while streaming (multitrack override): re-point the
		 * encoder at a mix that renders with the new scaler. On failure roll back so
		 * settings and delivery stay in sync until the stream restarts. */
		if (!live_rebind_encoder_mix(encoder))
			encoder->gpu_scale_type = prev;
	}
}

bool obs_encoder_set_frame_rate_divisor(obs_encoder_t *encoder, uint32_t frame_rate_divisor)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_set_frame_rate_divisor"))
		return false;

	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_set_frame_rate_divisor: "
		     "encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return false;
	}

	if (encoder_active(encoder)) {
		blog(LOG_WARNING,
		     "encoder '%s': Cannot set frame rate divisor "
		     "while the encoder is active",
		     obs_encoder_get_name(encoder));
		return false;
	}

	if (encoder->initialized) {
		blog(LOG_WARNING,
		     "encoder '%s': Cannot set frame rate divisor "
		     "after the encoder has been initialized",
		     obs_encoder_get_name(encoder));
		return false;
	}

	if (frame_rate_divisor == 0) {
		blog(LOG_WARNING,
		     "encoder '%s': Cannot set frame "
		     "rate divisor to 0",
		     obs_encoder_get_name(encoder));
		return false;
	}

	encoder->frame_rate_divisor = frame_rate_divisor;

	if (encoder->fps_override) {
		video_output_free_frame_rate_divisor(encoder->fps_override);
		encoder->fps_override = NULL;
	}

	if (encoder->media) {
		encoder->fps_override =
			video_output_create_with_frame_rate_divisor(encoder->media, encoder->frame_rate_divisor);
	}

	return true;
}

bool obs_encoder_scaling_enabled(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_scaling_enabled"))
		return false;

	return encoder->scaled_width || encoder->scaled_height;
}

uint32_t obs_encoder_get_width(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_width"))
		return 0;
	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_get_width: "
		     "encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return 0;
	}
	if (!encoder->media)
		return 0;

	return encoder->scaled_width != 0 ? encoder->scaled_width : video_output_get_width(encoder->media);
}

uint32_t obs_encoder_get_height(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_height"))
		return 0;
	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_get_height: "
		     "encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return 0;
	}
	if (!encoder->media)
		return 0;

	return encoder->scaled_height != 0 ? encoder->scaled_height : video_output_get_height(encoder->media);
}

bool obs_encoder_gpu_scaling_enabled(obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_gpu_scaling_enabled"))
		return 0;
	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_gpu_scaling_enabled: "
		     "encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return 0;
	}

	return encoder->gpu_scale_type != OBS_SCALE_DISABLE;
}

enum obs_scale_type obs_encoder_get_scale_type(obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_scale_type"))
		return 0;
	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_get_scale_type: "
		     "encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return 0;
	}

	return encoder->gpu_scale_type;
}

uint32_t obs_encoder_get_frame_rate_divisor(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_set_frame_rate_divisor"))
		return 0;

	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_set_frame_rate_divisor: "
		     "encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return 0;
	}

	return encoder->frame_rate_divisor;
}

double obs_encoder_get_effective_fps(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_effective_fps"))
		return 0.0;
	if (encoder->info.type != OBS_ENCODER_VIDEO)
		return 0.0;

	video_t *media = encoder->media ? encoder->media : obs_get_video();
	if (!media)
		return 0.0;

	const struct video_output_info *voi = video_output_get_info(media);
	if (voi->fps_den == 0)
		return 0.0;

	double base_fps = (double)voi->fps_num / (double)voi->fps_den;
	uint32_t divisor = encoder->frame_rate_divisor > 1 ? encoder->frame_rate_divisor : 1;
	return base_fps / (double)divisor;
}

uint32_t obs_encoder_get_fps_num(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_fps_num"))
		return 0;
	if (encoder->info.type != OBS_ENCODER_VIDEO)
		return 0;

	video_t *media = encoder->media ? encoder->media : obs_get_video();
	if (!media)
		return 0;

	return video_output_get_info(media)->fps_num;
}

uint32_t obs_encoder_get_fps_den(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_fps_den"))
		return 0;
	if (encoder->info.type != OBS_ENCODER_VIDEO)
		return 0;

	video_t *media = encoder->media ? encoder->media : obs_get_video();
	if (!media)
		return 0;

	return video_output_get_info(media)->fps_den;
}

bool obs_encoder_update_frame_rate_divisor(obs_encoder_t *encoder, uint32_t frame_rate_divisor)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_update_frame_rate_divisor"))
		return false;

	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_update_frame_rate_divisor: "
		     "encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return false;
	}

	if (frame_rate_divisor == 0) {
		blog(LOG_WARNING,
		     "obs_encoder_update_frame_rate_divisor: cannot set frame rate divisor to 0");
		return false;
	}

	// Unlike obs_encoder_set_frame_rate_divisor this is intended to be called while the encoder is active, so
	// that per-track frame rates can be changed live (e.g. multitrack video). The divisor fields updated here are
	// read by the encode path without a dedicated lock; only individual 32-bit stores are performed and the
	// fps_override wrapper is rebuilt with the same create/free pair used at create time.
	const bool gpu = gpu_encode_available(encoder);

	if (!gpu && encoder->media) {
		// Raw encoders skip frames using the divisor stored in their video-output connection state, so update
		// that as well (also resets its counter). The new rate applies starting with the next frame.
		video_output_set_frame_rate_divisor(encoder->media, frame_rate_divisor, receive_video, encoder);
	}

	// Used for packet timestamp/timebase scaling on both paths; takes effect for subsequent frames.
	encoder->frame_rate_divisor = frame_rate_divisor;
	// Reset the skip counter so that the new divisor takes effect immediately. Without this,
	// decreasing the divisor (e.g. 12→1) leaves the counter above the new value and the reset
	// condition (counter == divisor) is never met, causing all frames to be skipped.
	encoder->frame_rate_divisor_counter = 0;

	if (encoder->fps_override) {
		video_output_free_frame_rate_divisor(encoder->fps_override);
		encoder->fps_override = NULL;
	}
	if (encoder->media)
		encoder->fps_override = video_output_create_with_frame_rate_divisor(encoder->media, frame_rate_divisor);

	return true;
}

// Records a desired live per-track frame-rate divisor without applying it yet. The value is picked up by the
// encoder plugin (via obs_encoder_get_pending_frame_rate_divisor), which stages the actual change on a shared
// keyframe boundary with obs_encoder_set_frame_rate_divisor_at_pts. A value of 0 clears any pending request.
void obs_encoder_set_pending_frame_rate_divisor(obs_encoder_t *encoder, uint32_t frame_rate_divisor)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_set_pending_frame_rate_divisor"))
		return;
	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_set_pending_frame_rate_divisor: encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return;
	}

	encoder->pending_frame_rate_divisor = frame_rate_divisor;
}

uint32_t obs_encoder_get_pending_frame_rate_divisor(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_pending_frame_rate_divisor"))
		return 0;
	return encoder->pending_frame_rate_divisor;
}

// Stages a live per-track frame-rate divisor change so that it takes effect exactly when the encoder's synthetic
// PTS (cur_pts) reaches target_cur_pts. Used by multitrack to apply an FPS/divisor change on a shared keyframe
// boundary, keeping every track's keyframes aligned (a naive immediate change shifts cur_pts by an arbitrary
// amount because the skip counter is reset without frame-boundary synchronisation). The actual skip-counter reset
// and cur_pts rate switch happen on the video thread in receive_video() once cur_pts >= target_cur_pts.
bool obs_encoder_set_frame_rate_divisor_at_pts(obs_encoder_t *encoder, uint32_t frame_rate_divisor,
		int64_t target_cur_pts)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_set_frame_rate_divisor_at_pts"))
		return false;
	if (encoder->info.type != OBS_ENCODER_VIDEO || frame_rate_divisor == 0) {
		blog(LOG_WARNING,
		     "obs_encoder_set_frame_rate_divisor_at_pts: encoder '%s' is not an active video encoder",
		     obs_encoder_get_name(encoder));
		return false;
	}

	encoder->staged_frame_rate_divisor = frame_rate_divisor;
	encoder->staged_frame_rate_divisor_target = target_cur_pts;
	return true;
}

uint32_t obs_encoder_get_sample_rate(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_sample_rate"))
		return 0;
	if (encoder->info.type != OBS_ENCODER_AUDIO) {
		blog(LOG_WARNING,
		     "obs_encoder_get_sample_rate: "
		     "encoder '%s' is not an audio encoder",
		     obs_encoder_get_name(encoder));
		return 0;
	}
	if (!encoder->media)
		return 0;

	return encoder->samplerate != 0 ? encoder->samplerate : audio_output_get_sample_rate(encoder->media);
}

size_t obs_encoder_get_frame_size(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_frame_size"))
		return 0;
	if (encoder->info.type != OBS_ENCODER_AUDIO) {
		blog(LOG_WARNING,
		     "obs_encoder_get_frame_size: "
		     "encoder '%s' is not an audio encoder",
		     obs_encoder_get_name(encoder));
		return 0;
	}

	return encoder->framesize;
}

size_t obs_encoder_get_mixer_index(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_mixer_index"))
		return 0;

	if (encoder->info.type != OBS_ENCODER_AUDIO) {
		blog(LOG_WARNING,
		     "obs_encoder_get_mixer_index: "
		     "encoder '%s' is not an audio encoder",
		     obs_encoder_get_name(encoder));
		return 0;
	}

	return encoder->mixer_idx;
}

void obs_encoder_set_video(obs_encoder_t *encoder, video_t *video)
{

	if (!obs_encoder_valid(encoder, "obs_encoder_set_video"))
		return;
	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_set_video: "
		     "encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return;
	}
	if (encoder_active(encoder)) {
		blog(LOG_WARNING,
		     "encoder '%s': Cannot apply a new video_t "
		     "object while the encoder is active",
		     obs_encoder_get_name(encoder));
		return;
	}
	if (encoder->initialized) {
		blog(LOG_WARNING,
		     "encoder '%s': Cannot apply a new video_t object "
		     "after the encoder has been initialized",
		     obs_encoder_get_name(encoder));
		return;
	}

	encoder->source_video = video;
	encoder_set_video(encoder, video);
}

static void encoder_set_video(obs_encoder_t *encoder, video_t *video)
{
	const struct video_output_info *voi;

	if (encoder->fps_override) {
		video_output_free_frame_rate_divisor(encoder->fps_override);
		encoder->fps_override = NULL;
	}

	if (video) {
		voi = video_output_get_info(video);
		encoder->media = video;
		encoder->timebase_num = voi->fps_den;
		encoder->timebase_den = voi->fps_num;

		if (encoder->frame_rate_divisor) {
			encoder->fps_override =
				video_output_create_with_frame_rate_divisor(video, encoder->frame_rate_divisor);
		}
	} else {
		encoder->media = NULL;
		encoder->timebase_num = 0;
		encoder->timebase_den = 0;
	}
}

void obs_encoder_set_audio(obs_encoder_t *encoder, audio_t *audio)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_set_audio"))
		return;
	if (encoder->info.type != OBS_ENCODER_AUDIO) {
		blog(LOG_WARNING,
		     "obs_encoder_set_audio: "
		     "encoder '%s' is not an audio encoder",
		     obs_encoder_get_name(encoder));
		return;
	}
	if (encoder_active(encoder)) {
		blog(LOG_WARNING,
		     "encoder '%s': Cannot apply a new audio_t "
		     "object while the encoder is active",
		     obs_encoder_get_name(encoder));
		return;
	}

	if (audio) {
		encoder->media = audio;
		encoder->timebase_num = 1;
		encoder->timebase_den = audio_output_get_sample_rate(audio);
	} else {
		encoder->media = NULL;
		encoder->timebase_num = 0;
		encoder->timebase_den = 0;
	}
}

video_t *obs_encoder_video(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_video"))
		return NULL;
	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_set_video: "
		     "encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return NULL;
	}

	return encoder->fps_override ? encoder->fps_override : encoder->media;
}

video_t *obs_encoder_parent_video(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_parent_video"))
		return NULL;
	if (encoder->info.type != OBS_ENCODER_VIDEO) {
		blog(LOG_WARNING,
		     "obs_encoder_parent_video: "
		     "encoder '%s' is not a video encoder",
		     obs_encoder_get_name(encoder));
		return NULL;
	}

	return encoder->media;
}

audio_t *obs_encoder_audio(const obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_audio"))
		return NULL;
	if (encoder->info.type != OBS_ENCODER_AUDIO) {
		blog(LOG_WARNING,
		     "obs_encoder_set_audio: "
		     "encoder '%s' is not an audio encoder",
		     obs_encoder_get_name(encoder));
		return NULL;
	}

	return encoder->media;
}

bool obs_encoder_active(const obs_encoder_t *encoder)
{
	return obs_encoder_valid(encoder, "obs_encoder_active") ? encoder_active(encoder) : false;
}

static inline bool get_sei(const struct obs_encoder *encoder, uint8_t **sei, size_t *size)
{
	if (encoder->info.get_sei_data)
		return encoder->info.get_sei_data(encoder->context.data, sei, size);
	return false;
}

static void send_first_video_packet(struct obs_encoder *encoder, struct encoder_callback *cb,
				    struct encoder_packet *packet, struct encoder_packet_time *packet_time)
{
	struct encoder_packet first_packet;
	DARRAY(uint8_t) data;
	uint8_t *sei;
	size_t size;

	/* always wait for first keyframe */
	if (!packet->keyframe)
		return;

	da_init(data);

	if (!get_sei(encoder, &sei, &size) || !sei || !size) {
		cb->new_packet(cb->param, packet, packet_time);
		cb->sent_first_packet = true;
		return;
	}

	da_push_back_array(data, sei, size);
	da_push_back_array(data, packet->data, packet->size);

	first_packet = *packet;
	first_packet.data = data.array;
	first_packet.size = data.num;

	cb->new_packet(cb->param, &first_packet, packet_time);
	cb->sent_first_packet = true;

	da_free(data);
}

static const char *send_packet_name = "send_packet";
static inline void send_packet(struct obs_encoder *encoder, struct encoder_callback *cb, struct encoder_packet *packet,
			       struct encoder_packet_time *packet_time)
{
	profile_start(send_packet_name);
	/* include SEI in first video packet */
	if (encoder->info.type == OBS_ENCODER_VIDEO && !cb->sent_first_packet)
		send_first_video_packet(encoder, cb, packet, packet_time);
	else
		cb->new_packet(cb->param, packet, packet_time);
	profile_end(send_packet_name);
}

void full_stop(struct obs_encoder *encoder)
{
	if (encoder) {
		pthread_mutex_lock(&encoder->outputs_mutex);
		for (size_t i = 0; i < encoder->outputs.num; i++) {
			struct obs_output *output = encoder->outputs.array[i];
			obs_output_force_stop(output);

			pthread_mutex_lock(&output->interleaved_mutex);
			output->info.encoded_packet(output->context.data, NULL);
			pthread_mutex_unlock(&output->interleaved_mutex);
		}
		pthread_mutex_unlock(&encoder->outputs_mutex);

		pthread_mutex_lock(&encoder->callbacks_mutex);
		da_free(encoder->callbacks);
		pthread_mutex_unlock(&encoder->callbacks_mutex);

		remove_connection(encoder, false);
	}
}

void send_off_encoder_packet(obs_encoder_t *encoder, bool success, bool received, struct encoder_packet *pkt)
{
	if (!success) {
		blog(LOG_ERROR, "Error encoding with encoder '%s'", encoder->context.name);
		full_stop(encoder);
		return;
	}

	if (received) {
		if (!encoder->first_received) {
			encoder->offset_usec = packet_dts_usec(pkt);
			encoder->first_received = true;
		}

		/* we use system time here to ensure sync with other encoders,
		 * you do not want to use relative timestamps here */
		pkt->dts_usec = encoder->start_ts / 1000 + packet_dts_usec(pkt) - encoder->offset_usec;
		pkt->sys_dts_usec = pkt->dts_usec;

		pthread_mutex_lock(&encoder->pause.mutex);
		pkt->sys_dts_usec += encoder->pause.ts_offset / 1000;
		pthread_mutex_unlock(&encoder->pause.mutex);

		/* Find the encoder packet timing entry in the encoder
		 * timing array with the corresponding PTS value, then remove
		 * the entry from the array to ensure it doesn't continuously fill.
		 */
		struct encoder_packet_time ept_local;
		struct encoder_packet_time *ept = NULL;
		bool found_ept = false;
		if (pkt->type == OBS_ENCODER_VIDEO) {
			for (size_t i = encoder->encoder_packet_times.num; i > 0; i--) {
				ept = &encoder->encoder_packet_times.array[i - 1];
				if (ept->pts == pkt->pts) {
					ept_local = *ept;
					da_erase(encoder->encoder_packet_times, i - 1);
					found_ept = true;
					break;
				}
			}
			if (!found_ept)
				blog(LOG_DEBUG, "%s: Encoder packet timing for PTS %" PRId64 " not found", __FUNCTION__,
				     pkt->pts);
		}

		pthread_mutex_lock(&encoder->callbacks_mutex);

		for (size_t i = encoder->callbacks.num; i > 0; i--) {
			struct encoder_callback *cb;
			cb = encoder->callbacks.array + (i - 1);
			send_packet(encoder, cb, pkt, found_ept ? &ept_local : NULL);
		}

		pthread_mutex_unlock(&encoder->callbacks_mutex);

		// Count number of video frames successfully encoded
		if (pkt->type == OBS_ENCODER_VIDEO)
			encoder->encoded_frames++;
	}
}

static const char *do_encode_name = "do_encode";
bool do_encode(struct obs_encoder *encoder, struct encoder_frame *frame, const uint64_t *frame_cts)
{
	profile_start(do_encode_name);
	if (!encoder->profile_encoder_encode_name)
		encoder->profile_encoder_encode_name =
			profile_store_name(obs_get_profiler_name_store(), "encode(%s)", encoder->context.name);

	struct encoder_packet pkt = {0};
	bool received = false;
	bool success;
	uint64_t fer_ts = 0;

	if (encoder->reconfigure_requested) {
		encoder->reconfigure_requested = false;
		encoder->info.update(encoder->context.data, encoder->context.settings);
	}

	pkt.timebase_num = encoder->timebase_num * encoder->frame_rate_divisor;
	pkt.timebase_den = encoder->timebase_den;
	pkt.encoder = encoder;

	/* Get the frame encode request timestamp. This
	 * needs to be read just before the encode request.
	 */
	fer_ts = os_gettime_ns();

	profile_start(encoder->profile_encoder_encode_name);
	success = encoder->info.encode(encoder->context.data, frame, &pkt, &received);
	profile_end(encoder->profile_encoder_encode_name);

	/* Generate and enqueue the frame timing metrics, namely
	 * the CTS (composition time), FER (frame encode request), FERC
	 * (frame encode request complete) and current PTS. PTS is used to
	 * associate the frame timing data with the encode packet. */
	if (frame_cts) {
		struct encoder_packet_time *ept = da_push_back_new(encoder->encoder_packet_times);
		// Get the frame encode request complete timestamp
		if (success) {
			ept->ferc = os_gettime_ns();
		} else {
			// Encode had error, set ferc to 0
			ept->ferc = 0;
		}
		ept->pts = frame->pts;
		ept->cts = *frame_cts;
		ept->fer = fer_ts;
	}
	send_off_encoder_packet(encoder, success, received, &pkt);

	profile_end(do_encode_name);

	return success;
}

static inline bool video_pause_check_internal(struct pause_data *pause, uint64_t ts)
{
	pause->last_video_ts = ts;
	if (!pause->ts_start) {
		return false;
	}

	if (ts == pause->ts_end) {
		pause->ts_start = 0;
		pause->ts_end = 0;

	} else if (ts >= pause->ts_start) {
		return true;
	}

	return false;
}

bool video_pause_check(struct pause_data *pause, uint64_t timestamp)
{
	bool ignore_frame;

	pthread_mutex_lock(&pause->mutex);
	ignore_frame = video_pause_check_internal(pause, timestamp);
	pthread_mutex_unlock(&pause->mutex);

	return ignore_frame;
}

static const char *receive_video_name = "receive_video";
static void receive_video(void *param, struct video_data *frame)
{
	profile_start(receive_video_name);

	struct obs_encoder *encoder = param;
	struct encoder_frame enc_frame;

	if (encoder->encoder_group && !encoder->start_ts) {
		struct obs_encoder_group *group = encoder->encoder_group;
		bool ready = false;
		pthread_mutex_lock(&group->mutex);
		ready = group->start_timestamp == frame->timestamp;
		pthread_mutex_unlock(&group->mutex);
		if (!ready)
			goto wait_for_audio;
	}

	if (!encoder->first_received && encoder->paired_encoders.num) {
		for (size_t i = 0; i < encoder->paired_encoders.num; i++) {
			obs_encoder_t *paired = obs_weak_encoder_get_encoder(encoder->paired_encoders.array[i]);
			if (!paired)
				continue;

			if (!paired->first_received || paired->first_raw_ts > frame->timestamp) {
				obs_encoder_release(paired);
				goto wait_for_audio;
			}

			obs_encoder_release(paired);
		}
	}

	if (video_pause_check(&encoder->pause, frame->timestamp))
		goto wait_for_audio;

	// Apply a staged live frame-rate divisor change exactly on its target PTS (a shared multitrack keyframe
	// boundary). We are on the video thread and already under input_mutex (receive_video is invoked from
	// video_output_cur_frame), so the skip-counter reset is staged lock-free for this connection and consumed by
	// video_output_cur_frame on the next frame, while the cur_pts advance rate switches to the new divisor from
	// this frame onward. The net effect is that the first post-change frame sits exactly on the target grid point,
	// keeping this track's keyframes aligned with its sibling encoders (see obs_encoder_set_frame_rate_divisor_at_pts).
	if (encoder->staged_frame_rate_divisor &&
	    encoder->cur_pts >= encoder->staged_frame_rate_divisor_target) {
		uint32_t div = encoder->staged_frame_rate_divisor;
		if (!gpu_encode_available(encoder))
			video_output_set_pending_frame_rate_divisor(encoder->media, div, receive_video, encoder);
		encoder->frame_rate_divisor = div;
		encoder->frame_rate_divisor_counter = 0;
		if (encoder->fps_override) {
			video_output_free_frame_rate_divisor(encoder->fps_override);
			encoder->fps_override = NULL;
		}
		if (encoder->media)
			encoder->fps_override = video_output_create_with_frame_rate_divisor(encoder->media, div);
		encoder->staged_frame_rate_divisor = 0;
	}

	memset(&enc_frame, 0, sizeof(struct encoder_frame));

	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		enc_frame.data[i] = frame->data[i];
		enc_frame.linesize[i] = frame->linesize[i];
	}

	if (!encoder->start_ts)
		encoder->start_ts = frame->timestamp;

	enc_frame.frames = 1;
	enc_frame.pts = encoder->cur_pts;

	if (do_encode(encoder, &enc_frame, &frame->timestamp))
		encoder->cur_pts += encoder->timebase_num * encoder->frame_rate_divisor;

wait_for_audio:
	profile_end(receive_video_name);
}

static void clear_audio(struct obs_encoder *encoder)
{
	for (size_t i = 0; i < encoder->planes; i++)
		deque_free(&encoder->audio_input_buffer[i]);
}

static inline void push_back_audio(struct obs_encoder *encoder, struct audio_data *data, size_t size,
				   size_t offset_size)
{
	if (offset_size >= size)
		return;

	size -= offset_size;

	/* push in to the circular buffer */
	for (size_t i = 0; i < encoder->planes; i++)
		deque_push_back(&encoder->audio_input_buffer[i], data->data[i] + offset_size, size);
}

static inline size_t calc_offset_size(struct obs_encoder *encoder, uint64_t v_start_ts, uint64_t a_start_ts)
{
	uint64_t offset = v_start_ts - a_start_ts;
	offset = util_mul_div64(offset, encoder->samplerate, 1000000000ULL);
	return (size_t)offset * encoder->blocksize;
}

static void start_from_buffer(struct obs_encoder *encoder, uint64_t v_start_ts)
{
	size_t size = encoder->audio_input_buffer[0].size;
	struct audio_data audio = {0};
	size_t offset_size = 0;

	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		audio.data[i] = encoder->audio_input_buffer[i].data;
		memset(&encoder->audio_input_buffer[i], 0, sizeof(struct deque));
	}

	if (encoder->first_raw_ts < v_start_ts)
		offset_size = calc_offset_size(encoder, v_start_ts, encoder->first_raw_ts);

	push_back_audio(encoder, &audio, size, offset_size);

	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		bfree(audio.data[i]);
}

static const char *buffer_audio_name = "buffer_audio";
static bool buffer_audio(struct obs_encoder *encoder, struct audio_data *data)
{
	profile_start(buffer_audio_name);

	size_t size = data->frames * encoder->blocksize;
	size_t offset_size = 0;
	bool success = true;

	struct obs_encoder *paired_encoder = NULL;
	/* Audio encoders can only be paired to one video encoder */
	if (encoder->paired_encoders.num) {
		paired_encoder = obs_weak_encoder_get_encoder(encoder->paired_encoders.array[0]);
	}

	if (!encoder->start_ts && paired_encoder) {
		uint64_t end_ts = data->timestamp;
		uint64_t v_start_ts = paired_encoder->start_ts;

		/* no video yet, so don't start audio */
		if (!v_start_ts) {
			success = false;
			goto fail;
		}

		/* audio starting point still not synced with video starting
		 * point, so don't start audio */
		end_ts += util_mul_div64(data->frames, 1000000000ULL, encoder->samplerate);
		if (end_ts <= v_start_ts) {
			success = false;
			goto fail;
		}

		/* ready to start audio, truncate if necessary */
		if (data->timestamp < v_start_ts)
			offset_size = calc_offset_size(encoder, v_start_ts, data->timestamp);
		if (data->timestamp <= v_start_ts)
			clear_audio(encoder);

		encoder->start_ts = v_start_ts;

		/* use currently buffered audio instead */
		if (v_start_ts < data->timestamp) {
			start_from_buffer(encoder, v_start_ts);
		}

	} else if (!encoder->start_ts && !paired_encoder) {
		encoder->start_ts = data->timestamp;
	}

fail:
	push_back_audio(encoder, data, size, offset_size);
	obs_encoder_release(paired_encoder);

	profile_end(buffer_audio_name);
	return success;
}

static bool send_audio_data(struct obs_encoder *encoder)
{
	struct encoder_frame enc_frame;

	memset(&enc_frame, 0, sizeof(struct encoder_frame));

	for (size_t i = 0; i < encoder->planes; i++) {
		deque_pop_front(&encoder->audio_input_buffer[i], encoder->audio_output_buffer[i],
				encoder->framesize_bytes);

		enc_frame.data[i] = encoder->audio_output_buffer[i];
		enc_frame.linesize[i] = (uint32_t)encoder->framesize_bytes;
	}

	enc_frame.frames = (uint32_t)encoder->framesize;
	enc_frame.pts = encoder->cur_pts;

	if (!do_encode(encoder, &enc_frame, NULL))
		return false;

	encoder->cur_pts += encoder->framesize;
	return true;
}

static void pause_audio(struct pause_data *pause, struct audio_data *data, size_t sample_rate)
{
	uint64_t cutoff_frames = pause->ts_start - data->timestamp;
	cutoff_frames = ns_to_audio_frames(sample_rate, cutoff_frames);

	data->frames = (uint32_t)cutoff_frames;
}

static void unpause_audio(struct pause_data *pause, struct audio_data *data, size_t sample_rate)
{
	uint64_t cutoff_frames = pause->ts_end - data->timestamp;
	cutoff_frames = ns_to_audio_frames(sample_rate, cutoff_frames);

	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		if (!data->data[i])
			break;
		data->data[i] += cutoff_frames * sizeof(float);
	}

	data->timestamp = pause->ts_start;
	data->frames = data->frames - (uint32_t)cutoff_frames;
	pause->ts_start = 0;
	pause->ts_end = 0;
}

static inline bool audio_pause_check_internal(struct pause_data *pause, struct audio_data *data, size_t sample_rate)
{
	uint64_t end_ts;

	if (!pause->ts_start) {
		return false;
	}

	end_ts = data->timestamp + audio_frames_to_ns(sample_rate, data->frames);

	if (pause->ts_start >= data->timestamp) {
		if (pause->ts_start <= end_ts) {
			pause_audio(pause, data, sample_rate);
			return !data->frames;
		}

	} else {
		if (pause->ts_end >= data->timestamp && pause->ts_end <= end_ts) {
			unpause_audio(pause, data, sample_rate);
			return !data->frames;
		}

		return true;
	}

	return false;
}

bool audio_pause_check(struct pause_data *pause, struct audio_data *data, size_t sample_rate)
{
	bool ignore_audio;

	pthread_mutex_lock(&pause->mutex);
	ignore_audio = audio_pause_check_internal(pause, data, sample_rate);
	data->timestamp -= pause->ts_offset;
	pthread_mutex_unlock(&pause->mutex);

	return ignore_audio;
}

static const char *receive_audio_name = "receive_audio";
static void receive_audio(void *param, size_t mix_idx, struct audio_data *in)
{
	profile_start(receive_audio_name);

	struct obs_encoder *encoder = param;
	struct audio_data audio = *in;

	if (!encoder->first_received) {
		encoder->first_raw_ts = audio.timestamp;
		encoder->first_received = true;
		clear_audio(encoder);
	}

	if (audio_pause_check(&encoder->pause, &audio, encoder->samplerate))
		goto end;

	if (!buffer_audio(encoder, &audio))
		goto end;

	while (encoder->audio_input_buffer[0].size >= encoder->framesize_bytes) {
		if (!send_audio_data(encoder)) {
			break;
		}
	}

	UNUSED_PARAMETER(mix_idx);

end:
	profile_end(receive_audio_name);
}

void obs_encoder_add_output(struct obs_encoder *encoder, struct obs_output *output)
{
	if (!encoder || !output)
		return;

	pthread_mutex_lock(&encoder->outputs_mutex);
	da_push_back(encoder->outputs, &output);
	pthread_mutex_unlock(&encoder->outputs_mutex);
}

void obs_encoder_remove_output(struct obs_encoder *encoder, struct obs_output *output)
{
	if (!encoder || !output)
		return;

	pthread_mutex_lock(&encoder->outputs_mutex);
	da_erase_item(encoder->outputs, &output);
	pthread_mutex_unlock(&encoder->outputs_mutex);
}

void obs_encoder_packet_create_instance(struct encoder_packet *dst, const struct encoder_packet *src)
{
	long *p_refs;

	*dst = *src;
	p_refs = bmalloc(src->size + sizeof(long));
	dst->data = (void *)(p_refs + 1);
	*p_refs = 1;
	memcpy(dst->data, src->data, src->size);
}

void obs_encoder_packet_ref(struct encoder_packet *dst, struct encoder_packet *src)
{
	if (!src)
		return;

	if (src->data) {
		long *p_refs = ((long *)src->data) - 1;
		os_atomic_inc_long(p_refs);
	}

	*dst = *src;
}

void obs_encoder_packet_release(struct encoder_packet *pkt)
{
	if (!pkt)
		return;

	if (pkt->data) {
		long *p_refs = ((long *)pkt->data) - 1;
		if (os_atomic_dec_long(p_refs) == 0)
			bfree(p_refs);
	}

	memset(pkt, 0, sizeof(struct encoder_packet));
}

void obs_encoder_set_preferred_video_format(obs_encoder_t *encoder, enum video_format format)
{
	if (!encoder || encoder->info.type != OBS_ENCODER_VIDEO)
		return;

	encoder->preferred_format = format;
}

enum video_format obs_encoder_get_preferred_video_format(const obs_encoder_t *encoder)
{
	if (!encoder || encoder->info.type != OBS_ENCODER_VIDEO)
		return VIDEO_FORMAT_NONE;

	return encoder->preferred_format;
}

void obs_encoder_set_preferred_color_space(obs_encoder_t *encoder, enum video_colorspace colorspace)
{
	if (!encoder || encoder->info.type != OBS_ENCODER_VIDEO)
		return;

	encoder->preferred_space = colorspace;
}

enum video_colorspace obs_encoder_get_preferred_color_space(const obs_encoder_t *encoder)
{
	if (!encoder || encoder->info.type != OBS_ENCODER_VIDEO)
		return VIDEO_CS_DEFAULT;

	return encoder->preferred_space;
}

void obs_encoder_set_preferred_range(obs_encoder_t *encoder, enum video_range_type range)
{
	if (!encoder || encoder->info.type != OBS_ENCODER_VIDEO)
		return;

	encoder->preferred_range = range;
}

enum video_range_type obs_encoder_get_preferred_range(const obs_encoder_t *encoder)
{
	if (!encoder || encoder->info.type != OBS_ENCODER_VIDEO)
		return VIDEO_RANGE_DEFAULT;

	return encoder->preferred_range;
}

void obs_encoder_release(obs_encoder_t *encoder)
{
	if (!encoder)
		return;

	obs_weak_encoder_t *control = get_weak(encoder);
	if (obs_ref_release(&control->ref)) {
		// The order of operations is important here since
		// get_context_by_name in obs.c relies on weak refs
		// being alive while the context is listed
		obs_encoder_destroy(encoder);
		obs_weak_encoder_release(control);
	}
}

void obs_weak_encoder_addref(obs_weak_encoder_t *weak)
{
	if (!weak)
		return;

	obs_weak_ref_addref(&weak->ref);
}

void obs_weak_encoder_release(obs_weak_encoder_t *weak)
{
	if (!weak)
		return;

	if (obs_weak_ref_release(&weak->ref))
		bfree(weak);
}

obs_encoder_t *obs_encoder_get_ref(obs_encoder_t *encoder)
{
	if (!encoder)
		return NULL;

	return obs_weak_encoder_get_encoder(get_weak(encoder));
}

obs_weak_encoder_t *obs_encoder_get_weak_encoder(obs_encoder_t *encoder)
{
	if (!encoder)
		return NULL;

	obs_weak_encoder_t *weak = get_weak(encoder);
	obs_weak_encoder_addref(weak);
	return weak;
}

obs_encoder_t *obs_weak_encoder_get_encoder(obs_weak_encoder_t *weak)
{
	if (!weak)
		return NULL;

	if (obs_weak_ref_get_ref(&weak->ref))
		return weak->encoder;

	return NULL;
}

bool obs_weak_encoder_references_encoder(obs_weak_encoder_t *weak, obs_encoder_t *encoder)
{
	return weak && encoder && weak->encoder == encoder;
}

void *obs_encoder_get_type_data(obs_encoder_t *encoder)
{
	return obs_encoder_valid(encoder, "obs_encoder_get_type_data") ? encoder->orig_info.type_data : NULL;
}

const char *obs_encoder_get_id(const obs_encoder_t *encoder)
{
	return obs_encoder_valid(encoder, "obs_encoder_get_id") ? encoder->orig_info.id : NULL;
}

uint32_t obs_get_encoder_caps(const char *encoder_id)
{
	struct obs_encoder_info *info = find_encoder(encoder_id);
	return info ? info->caps : 0;
}

uint32_t obs_encoder_get_caps(const obs_encoder_t *encoder)
{
	return obs_encoder_valid(encoder, "obs_encoder_get_caps") ? encoder->orig_info.caps : 0;
}

bool obs_encoder_paused(const obs_encoder_t *encoder)
{
	return obs_encoder_valid(encoder, "obs_encoder_paused") ? os_atomic_load_bool(&encoder->paused) : false;
}

const char *obs_encoder_get_last_error(obs_encoder_t *encoder)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_get_last_error"))
		return NULL;

	return encoder->last_error_message;
}

void obs_encoder_set_last_error(obs_encoder_t *encoder, const char *message)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_set_last_error"))
		return;

	if (encoder->last_error_message)
		bfree(encoder->last_error_message);

	if (message)
		encoder->last_error_message = bstrdup(message);
	else
		encoder->last_error_message = NULL;
}

uint64_t obs_encoder_get_pause_offset(const obs_encoder_t *encoder)
{
	return encoder ? encoder->pause.ts_offset : 0;
}

bool obs_encoder_has_roi(const obs_encoder_t *encoder)
{
	return encoder->roi.num > 0;
}

bool obs_encoder_add_roi(obs_encoder_t *encoder, const struct obs_encoder_roi *roi)
{
	if (!roi)
		return false;
	if (!(encoder->info.caps & OBS_ENCODER_CAP_ROI))
		return false;
	/* Area smaller than the smallest possible block (16x16) */
	if (roi->bottom - roi->top < 16 || roi->right - roi->left < 16)
		return false;
	/* Other invalid ROIs */
	if (roi->priority < -1.0f || roi->priority > 1.0f)
		return false;

	pthread_mutex_lock(&encoder->roi_mutex);
	da_push_back(encoder->roi, roi);
	encoder->roi_increment++;
	pthread_mutex_unlock(&encoder->roi_mutex);

	return true;
}

void obs_encoder_clear_roi(obs_encoder_t *encoder)
{
	if (!encoder->roi.num)
		return;
	pthread_mutex_lock(&encoder->roi_mutex);
	da_clear(encoder->roi);
	encoder->roi_increment++;
	pthread_mutex_unlock(&encoder->roi_mutex);
}

void obs_encoder_enum_roi(obs_encoder_t *encoder, void (*enum_proc)(void *, struct obs_encoder_roi *), void *param)
{
	float scale_x = 0;
	float scale_y = 0;

	/* Scale ROI passed to callback to output size */
	if (encoder->scaled_height && encoder->scaled_width) {
		const uint32_t width = video_output_get_width(encoder->media);
		const uint32_t height = video_output_get_height(encoder->media);

		if (!width || !height)
			return;

		scale_x = (float)encoder->scaled_width / (float)width;
		scale_y = (float)encoder->scaled_height / (float)height;
	}

	pthread_mutex_lock(&encoder->roi_mutex);

	size_t idx = encoder->roi.num;
	while (idx) {
		struct obs_encoder_roi *roi = &encoder->roi.array[--idx];

		if (scale_x > 0 && scale_y > 0) {
			struct obs_encoder_roi scaled_roi = {
				.top = (uint32_t)((float)roi->top * scale_y),
				.bottom = (uint32_t)((float)roi->bottom * scale_y),
				.left = (uint32_t)((float)roi->left * scale_x),
				.right = (uint32_t)((float)roi->right * scale_x),
				.priority = roi->priority,
			};

			enum_proc(param, &scaled_roi);
		} else {
			enum_proc(param, roi);
		}
	}

	pthread_mutex_unlock(&encoder->roi_mutex);
}

uint32_t obs_encoder_get_roi_increment(const obs_encoder_t *encoder)
{
	return encoder->roi_increment;
}

bool obs_encoder_set_group(obs_encoder_t *encoder, obs_encoder_group_t *group)
{
	if (!obs_encoder_valid(encoder, "obs_encoder_set_group"))
		return false;

	if (obs_encoder_active(encoder)) {
		blog(LOG_ERROR, "obs_encoder_set_group: encoder '%s' is already active", obs_encoder_get_name(encoder));
		return false;
	}

	if (encoder->encoder_group) {
		struct obs_encoder_group *old_group = encoder->encoder_group;
		pthread_mutex_lock(&old_group->mutex);
		if (old_group->num_encoders_started) {
			pthread_mutex_unlock(&old_group->mutex);
			blog(LOG_ERROR, "obs_encoder_set_group: encoder '%s' existing group has started encoders",
			     obs_encoder_get_name(encoder));
			return false;
		}
		da_erase_item(old_group->encoders, &encoder);
		obs_encoder_release(encoder);
		pthread_mutex_unlock(&old_group->mutex);
	}

	if (!group)
		return true;

	pthread_mutex_lock(&group->mutex);

	if (group->num_encoders_started) {
		pthread_mutex_unlock(&group->mutex);
		blog(LOG_ERROR, "obs_encoder_set_group: specified group has started encoders");
		return false;
	}

	obs_encoder_t *ref = obs_encoder_get_ref(encoder);
	if (!ref) {
		pthread_mutex_unlock(&group->mutex);
		return false;
	}
	da_push_back(group->encoders, &ref);
	encoder->encoder_group = group;

	pthread_mutex_unlock(&group->mutex);

	return true;
}

obs_encoder_group_t *obs_encoder_group_create()
{
	struct obs_encoder_group *group = bzalloc(sizeof(struct obs_encoder_group));

	pthread_mutex_init_value(&group->mutex);
	if (pthread_mutex_init(&group->mutex, NULL) != 0) {
		bfree(group);
		return NULL;
	}

	return group;
}

void obs_encoder_group_actually_destroy(obs_encoder_group_t *group)
{
	for (size_t i = 0; i < group->encoders.num; i++) {
		struct obs_encoder *encoder = group->encoders.array[i];
		encoder->encoder_group = NULL;
		obs_encoder_release(encoder);
	}

	da_free(group->encoders);
	pthread_mutex_unlock(&group->mutex);
	pthread_mutex_destroy(&group->mutex);

	bfree(group);
}

void obs_encoder_group_destroy(obs_encoder_group_t *group)
{
	if (!group)
		return;

	pthread_mutex_lock(&group->mutex);

	if (group->num_encoders_started) {
		group->destroy_on_stop = true;
		pthread_mutex_unlock(&group->mutex);
		return;
	}

	obs_encoder_group_actually_destroy(group);
}

bool obs_encoder_video_tex_active(const obs_encoder_t *encoder, enum video_format format)
{
	struct obs_core_video_mix *mix = get_mix_for_video(encoder->media);
	/* The encoder's current media may not be backed by a core mix yet (e.g. a multitrack
	 * canvas queried during encoder create() before any GPU mix is bound). In that case no
	 * shared-texture delivery exists, so report the format as inactive rather than deref NULL. */
	if (!mix)
		return false;
	return mix->encoder_texture_format == format;
}
