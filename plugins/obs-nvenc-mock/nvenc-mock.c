/******************************************************************************
    Mock NVENC Encoder Plugin - VRAM Measurement Tool

    Registers encoders with the same IDs and capabilities as the real obs-nvenc
    plugin (OBS_ENCODER_CAP_PASS_TEXTURE) but allocates ZERO GPU resources:
      - No D3D11 textures
      - No NVENC sessions
      - No CUDA contexts
      - No bitstream buffers

    This allows isolating and measuring the libobs-side VRAM usage:
      - Canvas mixes (RGBA16F render/output textures)
      - Encoder-only mixes (per-encoder render/output/convert textures)
      - GPU encode pools (NUM_ENCODE_TEXTURES shared textures per mix)
      - Mix reuse paths (Option A/B)

    The encode_texture2 callback accepts the shared texture handle and returns
    a tiny fake packet each frame, keeping the full output pipeline alive.
******************************************************************************/

#include <obs.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <stdio.h>

#define MOCK_PACKET_SIZE 128 /* Small dummy payload per "encoded" frame */

/* ------------------------------------------------------------------------- */
/* Encoder state                                                             */

struct mock_enc {
	obs_encoder_t *encoder;
	uint32_t cx;
	uint32_t cy;
	uint64_t frame_count;
	bool first_packet;
	int64_t bitrate_kbps;
};

/* ------------------------------------------------------------------------- */
/* Common callbacks                                                          */

static const char *mock_get_name_h264(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "Mock NVENC H.264 (VRAM Test)";
}

static const char *mock_get_name_hevc(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "Mock NVENC HEVC (VRAM Test)";
}

static const char *mock_get_name_av1(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "Mock NVENC AV1 (VRAM Test)";
}

static void mock_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", 6000);
	obs_data_set_default_int(settings, "max_bitrate", 6000);
	obs_data_set_default_string(settings, "preset", "p1");
	obs_data_set_default_string(settings, "profile", "auto");
	obs_data_set_default_string(settings, "tune", "");
	obs_data_set_default_int(settings, "keyint_sec", 2);
	obs_data_set_default_int(settings, "bframes", 3);
}

static obs_properties_t *mock_properties(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_properties_create();
}

/* ------------------------------------------------------------------------- */
/* Create / Destroy                                                          */

static void *mock_create_h264(obs_data_t *settings, obs_encoder_t *encoder)
{
	struct mock_enc *enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;
	enc->first_packet = true;
	enc->bitrate_kbps = (int64_t)obs_data_get_int(settings, "bitrate");

	video_t *video = obs_encoder_video(encoder);
	if (video) {
		enc->cx = video_output_get_width(video);
		enc->cy = video_output_get_height(video);
	}

	blog(LOG_INFO, "[mock-nvenc] Created H.264 encoder '%s' at %ux%u - NO GPU resources allocated",
	     obs_encoder_get_name(encoder), enc->cx, enc->cy);
	return enc;
}

static void *mock_create_hevc(obs_data_t *settings, obs_encoder_t *encoder)
{
	struct mock_enc *enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;
	enc->first_packet = true;
	enc->bitrate_kbps = (int64_t)obs_data_get_int(settings, "bitrate");

	video_t *video = obs_encoder_video(encoder);
	if (video) {
		enc->cx = video_output_get_width(video);
		enc->cy = video_output_get_height(video);
	}

	blog(LOG_INFO, "[mock-nvenc] Created HEVC encoder '%s' at %ux%u - NO GPU resources allocated",
	     obs_encoder_get_name(encoder), enc->cx, enc->cy);
	return enc;
}

static void *mock_create_av1(obs_data_t *settings, obs_encoder_t *encoder)
{
	struct mock_enc *enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;
	enc->first_packet = true;
	enc->bitrate_kbps = (int64_t)obs_data_get_int(settings, "bitrate");

	video_t *video = obs_encoder_video(encoder);
	if (video) {
		enc->cx = video_output_get_width(video);
		enc->cy = video_output_get_height(video);
	}

	blog(LOG_INFO, "[mock-nvenc] Created AV1 encoder '%s' at %ux%u - NO GPU resources allocated",
	     obs_encoder_get_name(encoder), enc->cx, enc->cy);
	return enc;
}

static void mock_destroy(void *data)
{
	struct mock_enc *enc = data;
	if (enc) {
		blog(LOG_INFO, "[mock-nvenc] Destroyed encoder '%s' after %llu frames",
		     obs_encoder_get_name(enc->encoder), (unsigned long long)enc->frame_count);
		bfree(enc);
	}
}

static bool mock_update(void *data, obs_data_t *settings)
{
	struct mock_enc *enc = data;
	if (enc && settings) {
		enc->bitrate_kbps = (int64_t)obs_data_get_int(settings, "bitrate");
	}
	return true;
}

/* ------------------------------------------------------------------------- */
/* encode_texture2 - The key function                                        */
/*                                                                           */
/* Called by libobs for each frame on the GPU encode path.                   */
/* The real NVENC would:                                                     */
/*   1. Look up the shared texture by handle                                */
/*   2. Copy it into one of its buf_count input textures                    */
/*   3. Submit to NVENC hardware                                            */
/*   4. Lock/retrieve the bitstream                                         */
/*                                                                           */
/* The mock does NONE of this - returns a tiny fake packet.                  */
/* No D3D11 resources are created or touched.                                */

static bool mock_encode_texture(void *data, struct encoder_texture *texture, int64_t pts, uint64_t lock_key,
				uint64_t *next_key, struct encoder_packet *packet, bool *received_packet)
{
	struct mock_enc *enc = data;
	UNUSED_PARAMETER(texture);

	if (next_key)
		*next_key = lock_key + 1;

	enc->frame_count++;

	uint8_t fake_data[MOCK_PACKET_SIZE];
	memset(fake_data, 0, sizeof(fake_data));

	bool is_keyframe = (enc->frame_count % 120 == 1);

	packet->data = fake_data;
	packet->size = MOCK_PACKET_SIZE;
	packet->pts = pts;
	packet->dts = pts;
	packet->keyframe = is_keyframe;
	packet->type = OBS_ENCODER_VIDEO;

	*received_packet = true;
	return true;
}

/* ------------------------------------------------------------------------- */
/* get_video_info - Tell libobs what texture format we accept                */
/* Must match real NVENC so encoder-only mix creation produces same mixes.   */

static void mock_get_video_info(void *data, struct video_scale_info *info)
{
	UNUSED_PARAMETER(data);

	if (info->format == VIDEO_FORMAT_P010 || info->format == VIDEO_FORMAT_I010) {
		info->format = VIDEO_FORMAT_P010;
	} else if (info->format == VIDEO_FORMAT_BGRA || info->format == VIDEO_FORMAT_RGBA ||
		   info->format == VIDEO_FORMAT_BGRX || info->format == VIDEO_FORMAT_I444) {
		info->format = VIDEO_FORMAT_I444;
	} else if (info->format == VIDEO_FORMAT_GBRA || info->format == VIDEO_FORMAT_AYUV) {
		info->format = VIDEO_FORMAT_AYUV;
	} else if (info->format == VIDEO_FORMAT_Y410) {
		info->format = VIDEO_FORMAT_Y410;
	} else if (info->format == VIDEO_FORMAT_GBR10 || info->format == VIDEO_FORMAT_R10L) {
		info->format = VIDEO_FORMAT_GBR10;
		info->range = VIDEO_RANGE_FULL;
		if (info->colorspace != VIDEO_CS_2100_PQ && info->colorspace != VIDEO_CS_2100_HLG)
			info->colorspace = VIDEO_CS_SRGB;
	} else {
		info->format = VIDEO_FORMAT_NV12;
	}
}

/* ------------------------------------------------------------------------- */
/* is_color_format_supported                                                 */

static bool mock_h264_fmt(void *type_data, enum video_format format)
{
	UNUSED_PARAMETER(type_data);
	switch (format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_BGRA:
		return true;
	default:
		return false;
	}
}

static bool mock_hevc_fmt(void *type_data, enum video_format format)
{
	UNUSED_PARAMETER(type_data);
	switch (format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_P010:
	case VIDEO_FORMAT_BGRA:
		return true;
	default:
		return false;
	}
}

static bool mock_av1_fmt(void *type_data, enum video_format format)
{
	UNUSED_PARAMETER(type_data);
	return format == VIDEO_FORMAT_NV12 || format == VIDEO_FORMAT_P010;
}

/* ------------------------------------------------------------------------- */
/* Encoder info registrations                                                */
/* IDs match the real obs-nvenc plugin so frontend selects them unchanged.   */

struct obs_encoder_info mock_h264_info = {
	.id = "obs_nvenc_h264_tex",
	.codec = "h264",
	.type = OBS_ENCODER_VIDEO,
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_DYN_BITRATE,
	.get_name = mock_get_name_h264,
	.create = mock_create_h264,
	.destroy = mock_destroy,
	.update = mock_update,
	.encode_texture2 = mock_encode_texture,
	.get_defaults = mock_defaults,
	.get_properties = mock_properties,
	.get_video_info = mock_get_video_info,
	.is_color_format_supported = mock_h264_fmt,
};

struct obs_encoder_info mock_hevc_info = {
	.id = "obs_nvenc_hevc_tex",
	.codec = "hevc",
	.type = OBS_ENCODER_VIDEO,
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_DYN_BITRATE,
	.get_name = mock_get_name_hevc,
	.create = mock_create_hevc,
	.destroy = mock_destroy,
	.update = mock_update,
	.encode_texture2 = mock_encode_texture,
	.get_defaults = mock_defaults,
	.get_properties = mock_properties,
	.get_video_info = mock_get_video_info,
	.is_color_format_supported = mock_hevc_fmt,
};

struct obs_encoder_info mock_av1_info = {
	.id = "obs_nvenc_av1_tex",
	.codec = "av1",
	.type = OBS_ENCODER_VIDEO,
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_DYN_BITRATE,
	.get_name = mock_get_name_av1,
	.create = mock_create_av1,
	.destroy = mock_destroy,
	.update = mock_update,
	.encode_texture2 = mock_encode_texture,
	.get_defaults = mock_defaults,
	.get_properties = mock_properties,
	.get_video_info = mock_get_video_info,
	.is_color_format_supported = mock_av1_fmt,
};

/* "soft" (non-texture) IDs for compat layer reroute fallback */
struct obs_encoder_info mock_h264_soft_info = {
	.id = "obs_nvenc_h264_soft",
	.codec = "h264",
	.type = OBS_ENCODER_VIDEO,
	.caps = 0,
	.get_name = mock_get_name_h264,
	.create = mock_create_h264,
	.destroy = mock_destroy,
	.update = mock_update,
	.get_defaults = mock_defaults,
	.get_properties = mock_properties,
};

struct obs_encoder_info mock_hevc_soft_info = {
	.id = "obs_nvenc_hevc_soft",
	.codec = "hevc",
	.type = OBS_ENCODER_VIDEO,
	.caps = 0,
	.get_name = mock_get_name_hevc,
	.create = mock_create_hevc,
	.destroy = mock_destroy,
	.update = mock_update,
	.get_defaults = mock_defaults,
	.get_properties = mock_properties,
};

struct obs_encoder_info mock_av1_soft_info = {
	.id = "obs_nvenc_av1_soft",
	.codec = "av1",
	.type = OBS_ENCODER_VIDEO,
	.caps = 0,
	.get_name = mock_get_name_av1,
	.create = mock_create_av1,
	.destroy = mock_destroy,
	.update = mock_update,
	.get_defaults = mock_defaults,
	.get_properties = mock_properties,
};

/* ------------------------------------------------------------------------- */
/* Module lifecycle                                                          */

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-nvenc-mock", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Mock NVENC Encoder (VRAM Measurement - Zero GPU Allocations)";
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[mock-nvenc] === MOCK NVENC LOADED ===");
	blog(LOG_INFO, "[mock-nvenc] All encoders allocate ZERO GPU resources.");
	blog(LOG_INFO, "[mock-nvenc] Use this to measure libobs-side VRAM (mixes, pools, convert textures).");
	blog(LOG_INFO, "[mock-nvenc] Registered: obs_nvenc_h264_tex, obs_nvenc_hevc_tex, obs_nvenc_av1_tex");

	obs_register_encoder(&mock_h264_info);
	obs_register_encoder(&mock_hevc_info);
	obs_register_encoder(&mock_av1_info);
	obs_register_encoder(&mock_h264_soft_info);
	obs_register_encoder(&mock_hevc_soft_info);
	obs_register_encoder(&mock_av1_soft_info);

	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[mock-nvenc] === MOCK NVENC UNLOADED ===");
}