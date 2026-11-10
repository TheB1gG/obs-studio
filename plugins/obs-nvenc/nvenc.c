#include "nvenc-internal.h"
#include "nvenc-trace.h"

#include <util/darray.h>
#include <util/dstr.h>
#include <abr-gov.h>

/* ========================================================================= */

#define EXTRA_BUFFERS 5

/* ABR mode QP floor: the rate controller never encodes below this QP, so easy
 * content can't spend the bitrate budget on quality above QP 24 - the budget is
 * left for complex scenes. Applied at session create (and on live switch into
 * ABR); the preset default is restored when switching away from ABR. */
#define ABR_MIN_QP 24

#ifndef _WIN32
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

/* ------------------------------------------------------------------------- */
/* Bitstream Buffer                                                          */

static bool nv_bitstream_init(struct nvenc_data *enc, struct nv_bitstream *bs)
{
	NV_ENC_CREATE_BITSTREAM_BUFFER buf = {NV_ENC_CREATE_BITSTREAM_BUFFER_VER};

	if (NV_FAILED(nv.nvEncCreateBitstreamBuffer(enc->session, &buf))) {
		return false;
	}

	bs->ptr = buf.bitstreamBuffer;
	return true;
}

static void nv_bitstream_free(struct nvenc_data *enc, struct nv_bitstream *bs)
{
	if (bs->ptr) {
		nv.nvEncDestroyBitstreamBuffer(enc->session, bs->ptr);
	}
}

/* ------------------------------------------------------------------------- */
/* Implementation                                                            */

static const char *h264_nvenc_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "NVIDIA NVENC H.264";
}

static const char *h264_nvenc_soft_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "NVIDIA NVENC H.264 (Fallback)";
}

#ifdef ENABLE_HEVC
static const char *hevc_nvenc_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "NVIDIA NVENC HEVC";
}

static const char *hevc_nvenc_soft_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "NVIDIA NVENC HEVC (Fallback)";
}
#endif

static const char *av1_nvenc_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "NVIDIA NVENC AV1";
}

static const char *av1_nvenc_soft_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "NVIDIA NVENC AV1 (Fallback)";
}

static inline int nv_get_cap(struct nvenc_data *enc, NV_ENC_CAPS cap)
{
	if (!enc->session)
		return 0;

	NV_ENC_CAPS_PARAM param = {NV_ENC_CAPS_PARAM_VER};
	int v;
	NVENCSTATUS status;

	param.capsToQuery = cap;
	status = nv.nvEncGetEncodeCaps(enc->session, enc->codec_guid, &param, &v);
	nvenc_trace_cap(enc, cap, v, status);
	return v;
}

/* ------------------------------------------------------------------------- */
/* Live rate-control reconfiguration (CBR / VBR / CQVBR / CQP)               */
/*                                                                           */
/* NV_ENC_RECONFIGURE_ENCODER() re-initializes the encode session from       */
/* `reInitEncodeParams` and, when resetEncoder is set, resets "rate control  */
/* states and other internal encoder states" - only valid together with an   */
/* IDR frame, which we force below. Because this fork always passes its own  */
/* NV_ENC_CONFIG via enc->params.encodeConfig (see initialize_params()),     */
/* writing the desired rateControlMode into enc->config before calling it    */
/* makes NVIDIA restart encoding under that regime from a forced IDR.        */
/* While streaming, the reconfigure is deferred to the next GOP boundary     */
/* (nvenc_encode_base) so sibling encoders of a multitrack output keep      */
/* their keyframes aligned (delay: up to one GOP).                           */
/*                                                                           */
/* NOTE: experimental / for evaluation - NVIDIA does not explicitly          */
/* guarantee RC-regime switches mid-stream, so every switch is logged and    */
/* failures leave the encoder in its previous mode/state.                    */

static const char *nvenc_rc_mode_name(NV_ENC_PARAMS_RC_MODE mode)
{
	switch (mode) {
	case NV_ENC_PARAMS_RC_CBR:
		return "CBR";
	case NV_ENC_PARAMS_RC_VBR:
		return "VBR";
	case NV_ENC_PARAMS_RC_CONSTQP:
		return "CQP";
	}

	return "unknown";
}

/* Applies the staged NV_ENC_CONFIG via NV_ENC_RECONFIGURE_ENCODER with a   */
/* full reset of encoder state and a forced IDR. Must run on the encode     */
/* thread while streaming (between frame submissions), or any time when the */
/* encoder is inactive.                                                      */
static bool apply_nvenc_reconfigure(struct nvenc_data *enc)
{
	NV_ENC_RECONFIGURE_PARAMS params = {0};
	params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
	params.reInitEncodeParams = enc->params; // carries enc->config (encodeConfig)
	params.resetEncoder = 1;                 // resets rate-control state - requires IDR
	params.forceIDR = 1;

	NVENCSTATUS err = nv.nvEncReconfigureEncoder(enc->session, &params);
	nvenc_trace_reconfigure(enc, __FUNCTION__, &params, err);
	if (NV_FAILED(err)) {
		error("reconfigure failed (reset=1 forceIDR=1): encoder keeps previous settings");
		return false; // encoder keeps its previous mode/state
	}

	const uint32_t discarded = enc->buffers_queued;

	/* resetEncoder flushes every in-flight frame inside NVENC. Rewind   */
	/* our submission accounting so get_encoded_packet() never locks (and */
	/* blocks on) bitstreams or DTS entries that will never be filled.    */
	enc->buffers_queued = 0;
	deque_free(&enc->dts_list);
	deque_init(&enc->dts_list);
	enc->cur_bitstream = enc->next_bitstream;

	/* nvEncReconfigureEncoder with resetEncoder=1 invalidates previously
	 * allocated bitstream buffers in the driver (observed as
	 * NV_ENC_ERR_INVALID_PARAM on the next nvEncEncodePicture). Re-create
	 * every ring buffer and the refill buffer so post-reset encode calls
	 * reference valid output destinations. */
	for (uint32_t i = 0; i < enc->bitstreams.num; i++) {
		nv_bitstream_free(enc, &enc->bitstreams.array[i]);
		if (!nv_bitstream_init(enc, &enc->bitstreams.array[i]))
			return false;
	}
	/* Re-arm sequence-parameter capture: the next locked frame is the   */
	/* forced IDR of the reconfigured session, and get_encoded_packet()  */
	/* refreshes enc->header via nvEncGetSequenceParams(), so muxers see */
	/* the new SPS/VPS/PPS and can re-emit an updated codec header.      */
	/* Players only reliably latch a fresh resolution from a dedicated   */
	/* sequence-start packet, not from in-band NALs alone.               */
	enc->first_packet = true;

	/* The bitstream ring (buf_count slots) is sized to hold the full post-reset
	 * pipeline with headroom (see buf_count calculation above). get_encoded_packet()
	 * handles the transient INVALID_PARAM "not ready" responses during the fill
	 * period gracefully. */

	info("reconfigure applied (reset=1 forceIDR=1): %u in-flight frame(s) discarded by reset, "
	     "queued=%u cur_bs=%u next_bs=%u buf_count=%u output_delay=%u first_packet=%d",
	     discarded, (unsigned)enc->buffers_queued, (unsigned)enc->cur_bitstream,
	     (unsigned)enc->next_bitstream, (unsigned)enc->buf_count,
	     (unsigned)enc->output_delay, (int)enc->first_packet);

	return true;
}

/* Applies the staged NV_ENC_CONFIG WITHOUT resetting encoder state and without
 * forcing an IDR: used for pure rate-control parameter updates (ABR quality
 * aim). Mirrors the soft reconfigure the ABR governor does for maxBitRate every
 * second. No in-flight frames are discarded, so no bookkeeping rewind or buffer
 * re-creation is needed. Must run on the encode thread while streaming (between
 * frame submissions), or any time when the encoder is inactive. */
static bool apply_nvenc_reconfigure_soft(struct nvenc_data *enc)
{
	NV_ENC_RECONFIGURE_PARAMS params = {0};
	params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
	params.reInitEncodeParams = enc->params; // carries enc->config (encodeConfig)
	params.resetEncoder = 0;                 // keep rate-control state, no IDR needed
	params.forceIDR = 0;

	NVENCSTATUS err = nv.nvEncReconfigureEncoder(enc->session, &params);
	nvenc_trace_reconfigure(enc, __FUNCTION__, &params, err);
	if (NV_FAILED(err)) {
		error("soft reconfigure failed (reset=0 forceIDR=0): encoder keeps previous settings");
		return false;
	}

	info("soft reconfigure applied (reset=0 forceIDR=0): queued=%u output_delay=%u",
	     (unsigned)enc->buffers_queued, (unsigned)enc->output_delay);
	return true;
}

/* Re-derive the session GOP length (keyint_sec x effective fps, in frames) from the
 * current params frameRate and mirror it into the per-codec IDR period. GopLength is
 * otherwise only set at create time (init_encoder_base), so a live FPS change would
 * leave NVENC emitting IDRs every old_rate*keyint_sec frames - e.g. 240 frames = 16 s
 * of video at 15 fps instead of 60 frames = 4 s - desyncing this track's keyframe
 * cadence from its multitrack siblings. The computation mirrors init_encoder_base,
 * including the "keyint" user-arg override and the 250-frame fallback. */
static void nvenc_refresh_gop(struct nvenc_data *enc)
{
	if (enc->props.keyint_sec <= 0)
		return;

	const uint32_t num = enc->params.frameRateNum;
	const uint32_t den = enc->params.frameRateDen;
	if (num == 0 || den == 0)
		return;

	int keyint = (int)((double)enc->props.keyint_sec * num / den);
	get_user_arg_int(enc, "keyint", &keyint);
	const uint32_t gop = keyint > 0 ? (uint32_t)keyint : 250;

	enc->config.gopLength = gop;
	switch (enc->codec) {
	case CODEC_H264:
		enc->config.encodeCodecConfig.h264Config.idrPeriod = gop;
		break;
#ifdef ENABLE_HEVC
	case CODEC_HEVC:
		enc->config.encodeCodecConfig.hevcConfig.idrPeriod = gop;
		break;
#endif
	case CODEC_AV1:
		enc->config.encodeCodecConfig.av1Config.idrPeriod = gop;
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* ABR (Adaptive Bitrate) governor                                            */
/*                                                                           */
/* ABR mode runs NVENC in QVBR (rateControlMode=VBR, averageBitRate=0,        */
/* targetQuality set) so the driver's sustained-rate cap of                    */
/* min(maxBitRate, 2*averageBitRate) does not apply. The governor below then   */
/* adapts maxBitRate at runtime (NV_ENC_RECONFIGURE_ENCODER, no reset/IDR)     */
/* so the long-term average converges to props.bitrate: complex scenes may     */
/* spend up to the cap, easy scenes stay low, and the average tracks target.   */

static bool nvenc_abr_apply(struct nvenc_data *enc, uint32_t new_max_bps)
{
	NV_ENC_RECONFIGURE_PARAMS params = {0};
	params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
	params.reInitEncodeParams = enc->params; // carries enc->config (encodeConfig)
	params.resetEncoder = 0;                 /* no reset: no IDR churn, no queue rewind */
	params.forceIDR = 0;

	const uint32_t old = enc->config.rcParams.maxBitRate;
	enc->config.rcParams.maxBitRate = new_max_bps;

	NVENCSTATUS err = nv.nvEncReconfigureEncoder(enc->session, &params);
	nvenc_trace_reconfigure(enc, "nvenc_abr_apply", &params, err);
	if (NV_FAILED(err)) {
		enc->config.rcParams.maxBitRate = old; // revert: driver kept previous value
		warn("ABR governor: reconfigure failed, keeping previous max bitrate");
		return false;
	}

	debug("ABR governor: maxBitRate %u -> %u kbps", old / 1000, new_max_bps / 1000);
	return true;
}

static void nvenc_abr_tick(struct nvenc_data *enc, uint32_t frame_size_bytes)
{
	if (!enc->abr_active || !enc->can_change_bitrate)
		return;

	const int64_t target_bps = enc->props.bitrate * 1000;
	if (target_bps <= 0)
		return;

	const struct video_output_info *voi = video_output_get_info(obs_encoder_video(enc->encoder));
	if (voi->fps_num == 0)
		return;

	/* Nominal per-frame duration in seconds (constant-FPS video output). */
	const double dt = (double)voi->fps_den / voi->fps_num;
	if (dt <= 0.0)
		return;

	const int64_t ceiling_bps =
	    (int64_t)((enc->props.max_bitrate > 0 ? enc->props.max_bitrate : enc->props.bitrate) * 1000);

	/* Shared ABR governor (shared/abr-gov): accumulation, cadence (0.5 s), history gate (1 s) and the
	 * control law are identical to x265/AMF. */
	abr_gov_config cfg = abr_gov_default_config();
	cfg.deadband_frac = enc->abr_deadband;

	const int64_t old_bps = enc->abr_state.cur_rate;
	if (abr_gov_tick(&enc->abr_state, &cfg, target_bps, ceiling_bps, dt, frame_size_bytes, NULL)) {
		if (!nvenc_abr_apply(enc, (uint32_t)enc->abr_state.cur_rate))
			enc->abr_state.cur_rate = old_bps; /* driver rejected the change; retry next tick */
	}
}

/* Load the session budget on the first ABR entry of an open (fresh at target, or restored after a kill /
 * short disconnect); on later entries just re-latch the ceiling and keep the cumulative budget. Mirrors
 * amf_abr_configure(). */
static void nvenc_abr_latch(struct nvenc_data *enc, const abr_gov_config *cfg, int64_t target_bps,
                            int64_t ceiling_bps)
{
	if (!enc->abr_session_loaded) {
		abr_gov_session_load(enc->encoder, true, target_bps, ceiling_bps, cfg, &enc->abr_state);
		enc->abr_session_loaded = true;
	} else {
		const int64_t floor_bps = (int64_t)((double)target_bps * cfg->overshoot_floor_frac);
		if (enc->abr_state.cur_rate < floor_bps)
			enc->abr_state.cur_rate = floor_bps;
		if (enc->abr_state.cur_rate > ceiling_bps)
			enc->abr_state.cur_rate = ceiling_bps;
	}
}

/* ------------------------------------------------------------------------- */
/* Live resolution changes                                                     */
/*                                                                           */
/* obs_encoder_set_scaled_size() makes libobs start delivering frames at the  */
/* new size on the very next tick, so this frame's input already has the new  */
/* dimensions. Two handling paths (see nvenc_maybe_resize):                  */
/*                                                                           */
/*  - Encoder group (multitrack): never re-init off-grid. The re-init is     */
/*    staged to the next shared keyframe boundary (nvenc_maybe_apply_timed_  */
/*    reinit) and incoming new-size frames are scaled down to the current    */
/*    session size until then (D3D11 video processor / CPU bilinear scaler), */
/*    so every frame is encoded with continuous DTS and the forced IDR lands */
/*    exactly on-grid.                                                       */
/*  - Outside a group (or no keyframe cadence observed yet, or the backend   */
/*    cannot scale): immediate in-place re-init using resetEncoder=0 +       */
/*    forceIDR=1 (matching NVIDIA's DRC sample):                             */
/*      * The ring buffer continues operating normally (no reset of          */
/*        buffers_queued / cur_bitstream / next_bitstream / dts_list).       */
/*      * In-flight frames continue encoding at the old resolution; the      */
/*        forced IDR marks the start of the new resolution.                  */
/*      * Old input textures are kept alive (in-flight frames reference      */
/*        them) and freed after buf_count frames.                            */

static bool apply_nvenc_resize(struct nvenc_data *enc, uint32_t w, uint32_t h)
{
	if (nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE) != 1) {
		warn("driver does not report support for dynamic resolution change; restart required");
		return false;
	}

	int max_w = nv_get_cap(enc, NV_ENC_CAPS_WIDTH_MAX);
	int max_h = nv_get_cap(enc, NV_ENC_CAPS_HEIGHT_MAX);

	if ((max_w > 0 && (int)w > max_w) || (max_h > 0 && (int)h > max_h)) {
		warn("live resize to %ux%u exceeds encoder limits (%dx%d), restart required", w, h, max_w, max_h);
		return false;
	}

	info("applying live resolution change: %ux%u -> %ux%u (reset=0 forceIDR=1, ring preserved)",
	     enc->cx, enc->cy, w, h);

	/* Update session parameters for the reconfigure. */
	enc->params.encodeWidth = w;
	enc->params.encodeHeight = h;
	enc->params.darWidth = w;
	enc->params.darHeight = h;
	enc->cx = w;
	enc->cy = h;

	/* Soft reconfigure: resetEncoder=0 keeps the output pipeline intact.
	 * forceIDR ensures the decoder receives a fresh SPS/PPS with the new
	 * resolution. The ring buffer (buffers_queued, cur_bitstream,
	 * next_bitstream, dts_list) is NOT reset - NVIDIA's DRC sample
	 * preserves output sequencing across reconfigure. */
	NV_ENC_RECONFIGURE_PARAMS params = {0};
	params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
	params.reInitEncodeParams = enc->params;
	params.resetEncoder = 0;
	params.forceIDR = 1;

	NVENCSTATUS err = nv.nvEncReconfigureEncoder(enc->session, &params);
	nvenc_trace_reconfigure(enc, __FUNCTION__, &params, err);
	if (NV_FAILED(err)) {
		error("resize reconfigure failed (reset=0 forceIDR=1): err=%d", (int)err);
		return false;
	}

	/* Create new-size textures. The old textures remain alive because
	 * in-flight frames (up to output_delay) still reference them via
	 * nvEncMapInputResource. We move them to old_textures and free them
	 * after buf_count frames (guarantees all in-flight frames complete). */
#ifdef _WIN32
	if (!enc->non_texture) {
		/* Save old textures for deferred free. */
		da_move(enc->old_textures, enc->textures);
		enc->textures.num = 0;
		enc->old_tex_frames_remaining = enc->buf_count;

		if (!d3d11_init_textures(enc))
			return false;
	} else
#endif
	{
		cuda_free_surfaces(enc);
		enc->surfaces.num = 0;
		if (!cuda_init_surfaces(enc))
			return false;
	}

	/* The reconfigured session will emit a fresh SPS/PPS with the new
	 * resolution at the forced IDR. The in-flight old-resolution frames in
	 * the ring are emitted normally (they complete the old GOP), followed by
	 * the new-resolution IDR which starts the new GOP. This is valid H.264:
	 * the SPS/PPS change takes effect at the IDR, and all prior P/B frames
	 * belong to the old-resolution sequence. */
	enc->first_packet = true;

	info("resize to %ux%u complete: ring preserved (queued=%u cur_bs=%u next_bs=%u buf_count=%u)",
	     w, h, (unsigned)enc->buffers_queued, (unsigned)enc->cur_bitstream,
	     (unsigned)enc->next_bitstream, (unsigned)enc->buf_count);
	return true;
}

/* Next shared multitrack keyframe boundary strictly ahead of pts, from the measured
 * on-grid keyframe cadence (last_keyframe_pts + gop_pts). Requires gop_pts > 0. */
static int64_t nvenc_next_boundary_pts(struct nvenc_data *enc, int64_t pts)
{
	int64_t target = enc->last_keyframe_pts + enc->gop_pts;
	while (target <= pts)
		target += enc->gop_pts;
	return target;
}

/* Old-resolution buffers for the raw (CPU) path's staged resize: incoming new-size
 * frames are bilinear-scaled into them while waiting for the shared keyframe boundary
 * (mirrors obs-x264). Only allocated when a staged resize is actually staged. */
static void nvenc_free_scale_buffers(struct nvenc_data *enc)
{
	for (int i = 0; i < 3; i++) {
		bfree(enc->scale_buf[i]);
		enc->scale_buf[i] = NULL;
	}
}

static void nvenc_alloc_scale_buffers(struct nvenc_data *enc)
{
	nvenc_free_scale_buffers(enc);
	if (!enc->non_texture)
		return; /* texture backends scale on-GPU (D3D11 video processor) or fall back to immediate */

	const uint32_t w = enc->cx, h = enc->cy;
	switch (enc->in_format) {
	case VIDEO_FORMAT_NV12:
		enc->scale_stride[0] = (int)w;
		enc->scale_buf[0] = bmalloc((size_t)w * h);
		/* UV is indexed as dst_uv[y*ls1 + x*2 + c] with x < w/2: the row stride must be a
		 * full width (w), not w/2, or the last UV rows overflow the buffer. */
		enc->scale_stride[1] = (int)w;
		enc->scale_buf[1] = bmalloc((size_t)w * (h / 2));
		break;
	case VIDEO_FORMAT_P010:
		/* u16 samples: Y plane is 2 bytes/pixel. UV is indexed as ((uint16_t *)dst_uv)[y*(ls1/2) + x*2 + c],
		 * so the UV row stride must be 2*w bytes (a full-width row of u16 pairs). */
		enc->scale_stride[0] = (int)(w * 2);
		enc->scale_buf[0] = bmalloc((size_t)w * h * 2);
		enc->scale_stride[1] = (int)(w * 2);
		enc->scale_buf[1] = bmalloc((size_t)w * h);
		break;
	case VIDEO_FORMAT_I444:
		for (int i = 0; i < 3; i++) {
			enc->scale_stride[i] = (int)w;
			enc->scale_buf[i] = bmalloc((size_t)w * h);
		}
		break;
	default:
		break; /* other raw formats: no staging buffers */
	}
}

/* Whether this backend can scale new-size frames down to the current session size while
 * a resize is staged (needed to keep encoding until the shared keyframe boundary). */
static bool nvenc_staged_resize_supported(struct nvenc_data *enc)
{
	if (enc->non_texture) {
		switch (enc->in_format) {
		case VIDEO_FORMAT_NV12:
		case VIDEO_FORMAT_P010:
		case VIDEO_FORMAT_I444:
			return true; /* CPU bilinear scaler */
		default:
			return false;
		}
	}
#ifdef _WIN32
	return d3d11_staged_resize_supported(enc);
#else
	return false; /* CUDA/OpenGL texture path: no scaling available */
#endif
}

/* Apply the staged multitrack re-init at the shared keyframe boundary: one
 * NV_ENC_RECONFIGURE_ENCODER carrying any pending size change and the current effective
 * FPS (params already updated at staging time). A pure resize keeps the proven DRC path
 * (reset=0, ring preserved); an FPS change (alone or with a resize) resets encoder state
 * so the rate controller re-arms for the new frameRateNum/Den. The forced IDR is the
 * session-start frame and lands exactly on-grid. On failure the pending state is kept so
 * the next frame retries. */
static bool nvenc_apply_timed_reinit(struct nvenc_data *enc)
{
	const bool do_resize = enc->resize_pending;
	const bool do_fps = enc->fps_pending;

	if (do_resize) {
		if (nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE) != 1) {
			warn("driver does not report support for dynamic resolution change; restart required");
			return false;
		}
		int max_w = nv_get_cap(enc, NV_ENC_CAPS_WIDTH_MAX);
		int max_h = nv_get_cap(enc, NV_ENC_CAPS_HEIGHT_MAX);
		if ((max_w > 0 && (int)enc->pending_width > max_w) || (max_h > 0 && (int)enc->pending_height > max_h)) {
			warn("live resize to %ux%u exceeds encoder limits (%dx%d), restart required", enc->pending_width,
			     enc->pending_height, max_w, max_h);
			return false;
		}

		info("timed resize: %ux%u -> %ux%u (session re-init)", enc->cx, enc->cy, enc->pending_width,
		     enc->pending_height);
		enc->params.encodeWidth = enc->pending_width;
		enc->params.encodeHeight = enc->pending_height;
		enc->params.darWidth = enc->pending_width;
		enc->params.darHeight = enc->pending_height;
		enc->cx = enc->pending_width;
		enc->cy = enc->pending_height;
	}

	if (do_fps)
		info("timed FPS re-init: %u/%u (session re-init)", enc->params.frameRateNum, enc->params.frameRateDen);

	NV_ENC_RECONFIGURE_PARAMS params = {0};
	params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
	params.reInitEncodeParams = enc->params; // carries enc->config (encodeConfig)
	params.forceIDR = 1;                     /* session-start IDR, lands on the shared boundary */
	params.resetEncoder = do_fps ? 1 : 0;    /* FPS change needs RC re-arm; pure resize keeps ring */

	NVENCSTATUS err = nv.nvEncReconfigureEncoder(enc->session, &params);
	nvenc_trace_reconfigure(enc, __FUNCTION__, &params, err);
	if (NV_FAILED(err)) {
		error("timed re-init reconfigure failed (reset=%d forceIDR=1): err=%d", do_fps ? 1 : 0, (int)err);
		return false;
	}

	if (do_fps) {
		const uint32_t discarded = enc->buffers_queued;

		/* resetEncoder flushes every in-flight frame inside NVENC. Rewind the submission
		 * accounting so get_encoded_packet() never locks bitstreams or DTS entries that will
		 * never be filled (same rewind as apply_nvenc_reconfigure), and re-create every ring
		 * buffer (the driver invalidates them on reset). */
		enc->buffers_queued = 0;
		deque_free(&enc->dts_list);
		deque_init(&enc->dts_list);
		enc->cur_bitstream = enc->next_bitstream;

		for (uint32_t i = 0; i < enc->bitstreams.num; i++) {
			nv_bitstream_free(enc, &enc->bitstreams.array[i]);
			if (!nv_bitstream_init(enc, &enc->bitstreams.array[i]))
				return false;
		}
		info("timed FPS re-init: %u in-flight frame(s) discarded by reset", discarded);
	}

	if (do_resize) {
		/* Recreate input textures/surfaces at the new size. The old ones stay alive for
		 * in-flight frames and are freed after buf_count frames (see nvenc_encode_base). */
#ifdef _WIN32
		if (!enc->non_texture) {
			da_move(enc->old_textures, enc->textures);
			enc->textures.num = 0;
			enc->old_tex_frames_remaining = enc->buf_count;
			if (!d3d11_init_textures(enc)) {
				return false;
			}
		} else
#endif
		{
			cuda_free_surfaces(enc);
			enc->surfaces.num = 0;
			if (!cuda_init_surfaces(enc))
				return false;
		}
	}

	/* Re-arm sequence-parameter capture: the next locked frame is the forced IDR of the
	 * reconfigured session, so muxers see the new SPS/PPS (fresh resolution and/or rate). */
	enc->first_packet = true;
	enc->frames_since_idr = 0; /* the forced IDR restarts the GOP */
	info("timed re-init applied (resize=%d fps=%d reset=%d forceIDR=1): queued=%u cur_bs=%u next_bs=%u buf_count=%u",
	     do_resize, do_fps, do_fps ? 1 : 0, (unsigned)enc->buffers_queued, (unsigned)enc->cur_bitstream,
	     (unsigned)enc->next_bitstream, (unsigned)enc->buf_count);
	return true;
}

/* After an off-grid (emergency or late) re-init, the session-start IDR has shifted NVENC's
 * internal GOP phase permanently off the shared keyframe grid. Keep the pre-resize on-grid
 * anchor (so future boundary math stays aligned with the sibling tracks) and schedule a forced
 * IDR at the next shared boundary; the driver restarts its idrPeriod from any IDR, so that one
 * on-grid IDR re-anchors the phase. While it is pending, observed IDRs must not move
 * last_keyframe_pts/gop_pts (see get_encoded_packet). */
static void nvenc_schedule_reanchor(struct nvenc_data *enc, int64_t pts)
{
	if (enc->last_keyframe_pts >= 0 && enc->gop_pts > 0) {
		enc->reanchor_idr_pts = nvenc_next_boundary_pts(enc, pts);
		warn("off-grid re-init at pts %lld: keyframes will re-anchor at the next shared boundary (pts %lld)",
		     (long long)pts, (long long)enc->reanchor_idr_pts);
	} else {
		enc->reanchor_idr_pts = 0; /* no cadence observed yet - nothing to re-anchor to */
	}
}

/* Timed multitrack re-init (staged in nvenc_maybe_resize / nvenc_check_fps): at the shared
 * keyframe boundary, re-init the NVENC session carrying any pending size and/or effective-FPS
 * change. The first frame of the fresh session is a forced IDR that lands exactly on-grid, so
 * this track's keyframes stay aligned with its sibling tracks. Returns true if the current
 * frame may be encoded (nothing due, or the re-init just succeeded); false if it must be
 * skipped because the re-init failed (pending state kept for the next frame). */
static bool nvenc_maybe_apply_timed_reinit(struct nvenc_data *enc, int64_t pts)
{
	if (!enc->resize_pending && !enc->fps_pending)
		return true;

	if (pts < enc->reinit_target_pts)
		return true; // boundary not reached yet - keep encoding with the current session

	if (pts > enc->reinit_target_pts) {
		/* The boundary frame was dropped (encoder queue overflow while the encoder was busy).
		 * Re-initialising now would land the forced IDR off-grid, so advance to the next shared
		 * boundary instead; give up after two misses. */
		enc->reinit_misses++;
		if (enc->reinit_misses < 2) {
			enc->reinit_target_pts += enc->gop_pts;
			warn("timed re-init missed keyframe boundary (pts %lld), retrying at pts %lld",
			     (long long)pts, (long long)enc->reinit_target_pts);
		} else {
			warn("timed re-init missed two keyframe boundaries - applying late at pts %lld (off-grid)",
			     (long long)pts);
		}
	}

	if (pts == enc->reinit_target_pts || enc->reinit_misses >= 2) {
		const bool on_grid = (pts == enc->reinit_target_pts);

		bool ok = nvenc_apply_timed_reinit(enc);
		if (!ok)
			return false; // skip this frame - the pending change is retried on the next frame

		nvenc_free_scale_buffers(enc); /* no longer scaling to the old size */
#ifdef _WIN32
		d3d11_free_gpu_scaler(enc);    /* no longer scaling to the old size */
		d3d11_free_cpu_scaler(enc);    /* no longer scaling to the old size */
#endif

		enc->resize_pending = false;
		enc->fps_pending = false;
		/* The full re-init already applied the current RC/bitrate, so drop any staged
		 * RC-only reconfigure to avoid a redundant reset (and extra IDR) right after. */
		enc->reconfig_pending = false;
		if (on_grid) {
			/* The session-start IDR lands exactly on the shared grid and re-anchors it. */
			enc->last_keyframe_pts = pts;
		} else {
			/* Late off-grid apply: keep the on-grid anchor and force an IDR at the next
			 * shared boundary to re-anchor NVENC's internal GOP phase. */
			nvenc_schedule_reanchor(enc, pts);
		}
	}

	return true;
}

bool nvenc_apply_staged_changes_now(struct nvenc_data *enc, int64_t pts)
{
	if (!enc->resize_pending && !enc->fps_pending)
		return true;

	bool ok = nvenc_apply_timed_reinit(enc);
	if (!ok)
		return false; // pending state kept - the staged boundary path retries next frame

	nvenc_free_scale_buffers(enc); /* no longer scaling to the old size */
#ifdef _WIN32
	d3d11_free_gpu_scaler(enc);    /* no longer scaling to the old size */
	d3d11_free_cpu_scaler(enc);    /* no longer scaling to the old size */
#endif

	enc->resize_pending = false;
	enc->fps_pending = false;
	/* The full re-init already applied the current RC/bitrate, so drop any staged
	 * RC-only reconfigure to avoid a redundant reset (and extra IDR) right after. */
	enc->reconfig_pending = false;
	/* Emergency apply lands off-grid: keep the on-grid anchor and force an IDR at the next
	 * shared boundary to re-anchor NVENC's internal GOP phase. */
	nvenc_schedule_reanchor(enc, pts);

	return true;
}

/* Detect and stage live effective-FPS changes (mirrors qsv_check_staged):
 *  (a) A divisor change staged by the frontend via obs_encoder_set_pending_frame_rate_divisor()
 *      (multitrack live FPS change): apply it exactly at the next shared keyframe boundary - libobs
 *      switches the delivery rate there (set_frame_rate_divisor_at_pts) and we re-init the session
 *      there, so the forced IDR lands on-grid. While waiting, frames keep arriving at the OLD rate,
 *      which matches the current session - no transient mismatch.
 *  (b) Any other effective-FPS change (base-rate change, an immediately-applied divisor, or one seen
 *      in nvenc_update via fps_change_staged): the delivery rate has ALREADY switched, so a grouped
 *      encoder stages the re-init at the next shared boundary and everyone else applies it now. */
static void nvenc_check_fps(struct nvenc_data *enc, int64_t pts)
{
	const uint32_t cur_div = obs_encoder_get_frame_rate_divisor(enc->encoder);

	/* (a) Frontend-staged divisor change (multitrack). */
	const uint32_t pending_div = obs_encoder_get_pending_frame_rate_divisor(enc->encoder);
	if (pending_div != 0 && pending_div != cur_div && !enc->fps_pending) {
		const uint32_t fps_num = obs_encoder_get_fps_num(enc->encoder);
		const uint32_t fps_den = obs_encoder_get_fps_den(enc->encoder) * pending_div;
		if (fps_num == 0 || fps_den == 0)
			return;

		info("live FPS change %u/%u -> %u/%u, staging boundary-aligned re-init", enc->params.frameRateNum,
		     enc->params.frameRateDen, fps_num, fps_den);
		enc->params.frameRateNum = fps_num;
		enc->params.frameRateDen = fps_den;
		nvenc_refresh_gop(enc);

		int64_t target = pts + 1; /* no cadence observed yet: apply at the next frame */
		if (enc->last_keyframe_pts >= 0 && enc->gop_pts > 0)
			target = nvenc_next_boundary_pts(enc, pts);
		obs_encoder_set_frame_rate_divisor_at_pts(enc->encoder, pending_div, target);

		if (!enc->resize_pending) { /* a staged resize already owns the boundary */
			enc->reinit_target_pts = target;
			enc->reinit_misses = 0;
		}
		enc->fps_pending = true;
		info("live FPS change staged (will apply at pts %lld, current pts %lld)", (long long)target,
		     (long long)pts);
		obs_encoder_set_pending_frame_rate_divisor(enc->encoder, 0);
		return;
	}

	/* (b) Already-applied effective-FPS change (base-rate change, an immediately-applied divisor, or one
	 * seen in nvenc_update via fps_change_staged): the delivery rate has ALREADY switched, so a grouped
	 * encoder stages the re-init at the next shared boundary and everyone else applies it now. Skipped
	 * while a divisor is still pending - that belongs to path (a) and must be consumed first: rapid
	 * consecutive changes are serialized, the second one staging at the next boundary after the first
	 * lands (mirrors qsv_check_staged). */
	if (pending_div != 0 || enc->fps_pending)
		return;

	const uint32_t fps_num = obs_encoder_get_fps_num(enc->encoder);
	const uint32_t fps_den = obs_encoder_get_fps_den(enc->encoder) * cur_div;
	if (fps_num == 0 || fps_den == 0)
		return;
	if (!enc->fps_change_staged && fps_num == enc->params.frameRateNum && fps_den == enc->params.frameRateDen)
		return;

	if (fps_num != enc->params.frameRateNum || fps_den != enc->params.frameRateDen)
		info("live FPS change %u/%u -> %u/%u while streaming", enc->params.frameRateNum,
		     enc->params.frameRateDen, fps_num, fps_den);
	else
		info("live FPS re-init staged by update() (session params already current)");

	enc->params.frameRateNum = fps_num;
	enc->params.frameRateDen = fps_den;
	nvenc_refresh_gop(enc);
	enc->fps_change_staged = false;

	if (obs_encoder_is_in_group(enc->encoder) && enc->last_keyframe_pts >= 0 && enc->gop_pts > 0) {
		if (!enc->resize_pending) { /* a staged resize already owns the boundary */
			enc->reinit_target_pts = nvenc_next_boundary_pts(enc, pts);
			enc->reinit_misses = 0;
		}
		/* The staged timed re-init re-anchors the grid with its own on-grid IDR, so a
		 * pending off-grid re-anchor becomes moot. */
		enc->reanchor_idr_pts = 0;
		enc->fps_pending = true;
		info("live FPS change staged (will apply at pts %lld, current pts %lld)", (long long)enc->reinit_target_pts,
		     (long long)pts);
	} else {
		if (obs_encoder_is_in_group(enc->encoder))
			info("FPS re-init applied now (no keyframe cadence observed yet - only possible before/at "
			     "stream start, so the session-start IDR still lands on-grid)");
		else
			info("applying live FPS re-init now (reset=1 forceIDR=1)");
		if (!apply_nvenc_reconfigure(enc)) {
			warn("live FPS re-init failed - restart required");
			return;
		}
		/* The re-init already applied the staged RC/bitrate as well. */
		enc->reconfig_pending = false;
		enc->frames_since_idr = 0; /* the forced IDR restarts the GOP */
	}
}

static bool nvenc_maybe_resize(struct nvenc_data *enc, int64_t pts)
{
	uint32_t w = obs_encoder_get_width(enc->encoder);
	uint32_t h = obs_encoder_get_height(enc->encoder);

	if (w == enc->cx && h == enc->cy)
		return true; /* no pending change */

	if (obs_encoder_is_in_group(enc->encoder) && enc->last_keyframe_pts >= 0 && enc->gop_pts > 0 &&
	    /* Once staged, the episode owns the boundary: don't re-probe (the probe has side
	     * effects and a transient failure must not drop us into an immediate re-init). */
	    (enc->resize_pending || nvenc_staged_resize_supported(enc)) &&
	    nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE) == 1 &&
	    /* The raw (CPU) path detects the new size via plane-0 stride, so it can only stage a
	     * resize that changes the width; same-width height-only resizes fall back to immediate. */
	    (w != enc->cx || !enc->non_texture)) {
		if (!enc->resize_pending) {
			int64_t target = enc->fps_pending ? enc->reinit_target_pts : nvenc_next_boundary_pts(enc, pts);

			enc->resize_pending = true;
			enc->pending_width = w;
			enc->pending_height = h;
			if (!enc->fps_pending) { /* a staged FPS change already owns the boundary */
				enc->reinit_target_pts = target;
				enc->reinit_misses = 0;
			}
			/* The staged timed re-init re-anchors the grid with its own on-grid IDR, so a
			 * pending off-grid re-anchor becomes moot. */
			enc->reanchor_idr_pts = 0;
			info("live resize %ux%u -> %ux%u staged (will apply at pts %lld, current pts %lld)",
			     enc->cx, enc->cy, w, h, (long long)target, (long long)pts);
			nvenc_alloc_scale_buffers(enc); /* raw path: old-size buffers for scaling */
		} else if (w != enc->pending_width || h != enc->pending_height) {
			/* A newer size arrived mid-wait: re-target the staged resize. */
			info("live resize re-staged %ux%u -> %ux%u (boundary pts %lld)", enc->pending_width,
			     enc->pending_height, w, h, (long long)enc->reinit_target_pts);
			enc->pending_width = w;
			enc->pending_height = h;
		}
		return true; /* keep encoding (scaled) with the current session until the boundary */
	}

	/* Non-grouped, or no cadence observed yet, or backend cannot scale: immediate in-place re-init. */
	if (!apply_nvenc_resize(enc, w, h)) {
		error("live resize to %ux%u could not be applied - encoder left in previous state until the stream restarts",
		      w, h);
		return false; // caps check: nothing was touched, retried on the next frame
	}
	return true;
}

bool nvenc_prepare_frame(struct nvenc_data *enc, int64_t pts)
{
	/* Timed multitrack re-init (staged resize and/or staged FPS change): at the shared
	 * keyframe boundary, run the deferred session re-init before any input copy of this
	 * frame so its forced IDR lands exactly on-grid. */
	if (!nvenc_maybe_apply_timed_reinit(enc, pts))
		return false; // re-init failed - skip this frame, retry next

	/* Live resolution change: stage a timed resize (grouped) or re-init immediately. */
	if (!nvenc_maybe_resize(enc, pts))
		return false; // immediate resize failed - encoder is broken, skip this frame

	/* Live effective-FPS change: stage a boundary-aligned re-init or apply now. */
	nvenc_check_fps(enc, pts);

	return true;
}

static bool nvenc_update(void *data, obs_data_t *settings)
{
	struct nvenc_data *enc = data;

	int64_t bitrate = obs_data_get_int(settings, "bitrate");
	int64_t max_bitrate = obs_data_get_int(settings, "max_bitrate");
	const char *rc_name = obs_data_get_string(settings, "rate_control");

	NV_ENC_CONFIG *config = &enc->config;

	bool cqp = astrcmpi(rc_name, "CQP") == 0;
	bool vbr = astrcmpi(rc_name, "VBR") == 0;
	bool abr = astrcmpi(rc_name, "ABR") == 0;
	bool cq_vbr = astrcmpi(rc_name, "CQVBR") == 0;
	int64_t cqp_val = obs_data_get_int(settings, "cqp");
	int64_t tq_val = obs_data_get_int(settings, "target_quality");

	/* Lossless encoders use LOSSLESS tuning which is fixed at session init - */
	/* switching away from (or to) it requires a full restart.                */
	if (astrcmpi(enc->props.rate_control, "lossless") == 0 || astrcmpi(rc_name, "lossless") == 0) {
		bool lossless_now = astrcmpi(enc->props.rate_control, "lossless") == 0;
		bool lossless_req = astrcmpi(rc_name, "lossless") == 0;

		if (lossless_now != lossless_req) {
			warn("cannot switch %s rate control to %s while streaming; restart required",
			     lossless_now ? "from LOSSLESS" : "to", rc_name);
			return false;
		}
		return true; // pure lossless: nothing else is live-tunable here
	}

	/* --- resolve desired RC mode (unrecognized values keep the current one) */

	NV_ENC_PARAMS_RC_MODE desired_mode = config->rcParams.rateControlMode;

	if (cqp) {
		desired_mode = NV_ENC_PARAMS_RC_CONSTQP;
	} else if (vbr || cq_vbr || abr) {
		desired_mode = NV_ENC_PARAMS_RC_VBR;
	} else if (astrcmpi(rc_name, "CBR") == 0) {
		desired_mode = NV_ENC_PARAMS_RC_CBR;
	}

	const int64_t cqp_scaled = enc->codec == CODEC_AV1 ? cqp_val * 4 : cqp_val;

	/* CQVBR and ABR both run in VBR mode with averageBitRate = 0; enc->abr_active
	 * distinguishes which one we're currently in. */
	bool qvbr_config = config->rcParams.rateControlMode == NV_ENC_PARAMS_RC_VBR &&
			   config->rcParams.averageBitRate == 0;
	bool cq_state = qvbr_config && !enc->abr_active; /* currently CQVBR (not ABR) */

	/* CQVBR and ABR are both quality-driven VBR (averageBitRate=0): the
	 * "target_quality" setting maps to rcParams.targetQuality in both. */
	bool tq_changed = (cq_vbr || abr) && config->rcParams.targetQuality != (uint8_t)tq_val;

	bool rc_mode_changed =
		desired_mode != config->rcParams.rateControlMode || (cq_vbr != cq_state) || (abr != enc->abr_active) ||
		(desired_mode == NV_ENC_PARAMS_RC_CONSTQP && (int64_t)config->rcParams.constQP.qpIntra != cqp_scaled);

	bool rc_changed = rc_mode_changed || tq_changed;

	bool br_changed = enc->can_change_bitrate &&
			  (enc->props.bitrate != bitrate || enc->props.max_bitrate != max_bitrate);

	/* Detect effective FPS change (e.g. frame rate divisor changed live). NVENC's CBR
	 * rate controller uses frameRateNum/Den to calculate per-frame bit budgets, so it
	 * must be updated when the effective encoding rate changes. */
	uint32_t fps_num = obs_encoder_get_fps_num(enc->encoder);
	uint32_t fps_den = obs_encoder_get_fps_den(enc->encoder) * obs_encoder_get_frame_rate_divisor(enc->encoder);
	bool fps_changed = (enc->params.frameRateNum != fps_num || enc->params.frameRateDen != fps_den);

	/* Build a description of what changed for logging */
	struct dstr change_desc = {0};
	if (fps_changed) {
		dstr_catf(&change_desc, "%gFPS (%u/%u) -> %gFPS (%u/%u)",
			  (double)enc->params.frameRateNum / enc->params.frameRateDen, enc->params.frameRateNum,
			  enc->params.frameRateDen, (double)fps_num / fps_den, fps_num, fps_den);
		enc->params.frameRateNum = fps_num;
		enc->params.frameRateDen = fps_den;
		/* Refresh the GOP length / IDR period for the new effective rate - they are otherwise
		 * frozen at create time and would desync this track's keyframe cadence from its
		 * multitrack siblings. In an encoder group the encode side stages the actual re-init at
		 * the next shared keyframe boundary (nvenc_check_fps consumes fps_change_staged) instead
		 * of riding this track's own - now shorter - GOP counter, which could fire off-grid. */
		nvenc_refresh_gop(enc);
		if (obs_encoder_is_in_group(enc->encoder))
			enc->fps_change_staged = true;
	}
	if (br_changed) {
		if (change_desc.array)
			dstr_cat(&change_desc, ", ");
		dstr_catf(&change_desc, "bitrate %lld -> %lld", (long long)enc->props.bitrate, (long long)bitrate);
	}
	if (rc_changed) {
		if (change_desc.array)
			dstr_cat(&change_desc, ", ");
		dstr_catf(&change_desc, "RC %s", rc_name ? rc_name : "?");
	}
	if (tq_changed) {
		if (change_desc.array)
			dstr_cat(&change_desc, ", ");
		dstr_catf(&change_desc, "quality %u -> %lld", (unsigned)config->rcParams.targetQuality, (long long)tq_val);
	}

	if (!rc_changed && !br_changed && !fps_changed)
		return true; // nothing to do - avoids needless encoder reset + IDR churn

	/* A pure quality-aim change in ABR mode is a rate-control parameter update,
	 * like the maxBitRate updates the ABR governor makes every second: no reset
	 * and no IDR are needed, so it fires on the next frame without keyframe
	 * alignment. CQVBR keeps the staged RC reconfigure path below. */
	if (abr && tq_changed && !rc_mode_changed && !br_changed && !fps_changed) {
		uint8_t old_tq = config->rcParams.targetQuality;
		enc->config.rcParams.targetQuality = (uint8_t)tq_val;
		info("changing ABR quality target from %u to %lld while streaming (soft reconfigure, no IDR)",
		     (unsigned)old_tq, (long long)tq_val);

		if (!obs_encoder_active(enc->encoder)) {
			if (!apply_nvenc_reconfigure_soft(enc)) {
				enc->config.rcParams.targetQuality = old_tq;
				return false;
			}
		} else {
			enc->tq_soft_pending = true;
		}

		enc->props.target_quality = tq_val;
		return true;
	}

	/* --- apply desired state --------------------------------------------- */

	if (rc_changed) {
		bool mode_switch = desired_mode != config->rcParams.rateControlMode || (cq_vbr != cq_state) ||
				   (abr != enc->abr_active);

		if (mode_switch)
			info("switching rate control from %s to %s while streaming (forced IDR)",
			     nvenc_rc_mode_name(config->rcParams.rateControlMode), nvenc_rc_mode_name(desired_mode));
		else if (tq_changed)
			info("changing quality target from %u to %lld while streaming (forced IDR)",
			     (unsigned)config->rcParams.targetQuality, (long long)tq_val);
		else
			info("changing rate-control settings while streaming (forced IDR)");

		config->rcParams.rateControlMode = desired_mode;
		config->rcParams.averageBitRate = (uint32_t)bitrate * 1000;
		config->rcParams.maxBitRate = (vbr || cq_vbr) ? (uint32_t)max_bitrate * 1000 : (uint32_t)bitrate * 1000;
		config->rcParams.vbvBufferSize = (uint32_t)bitrate * 1000;

		if (cqp) {
			config->rcParams.constQP.qpInterP = (uint32_t)cqp_scaled;
			config->rcParams.constQP.qpInterB = (uint32_t)cqp_scaled;
			config->rcParams.constQP.qpIntra = (uint32_t)cqp_scaled;
		} else if (cq_vbr) {
			config->rcParams.targetQuality = (uint8_t)tq_val;
			config->rcParams.averageBitRate = 0;
			config->rcParams.vbvBufferSize = 0;
		} else if (abr) {
			config->rcParams.targetQuality = (uint8_t)tq_val;
			config->rcParams.averageBitRate = 0;
			config->rcParams.vbvBufferSize = 0;
			double db = obs_data_get_double(settings, "deadband");
			if (db < 0.1)
				db = 0.1;
			else if (db > 10.0)
				db = 10.0;
			enc->abr_deadband = db / 100.0;
			const int64_t abr_target_bps = (int64_t)bitrate * 1000;
			const int64_t abr_ceiling_bps = (int64_t)(max_bitrate > 0 ? max_bitrate : bitrate) * 1000;
			abr_gov_config cfg = abr_gov_default_config();
			cfg.deadband_frac = enc->abr_deadband;
			nvenc_abr_latch(enc, &cfg, abr_target_bps, abr_ceiling_bps);
			config->rcParams.maxBitRate = (uint32_t)enc->abr_state.cur_rate;
		}

		/* ABR raises the QP floor to ABR_MIN_QP (see init_encoder_base); restore the
		 * preset default when leaving ABR so other modes keep stock behavior. */
		if (abr) {
			const uint32_t abr_min_qp = enc->codec == CODEC_AV1 ? ABR_MIN_QP * 4 : ABR_MIN_QP;
			config->rcParams.enableMinQP = 1;
			config->rcParams.minQP.qpInterP = abr_min_qp;
			config->rcParams.minQP.qpInterB = abr_min_qp;
			config->rcParams.minQP.qpIntra = abr_min_qp;
		} else {
			config->rcParams.enableMinQP = enc->preset_enable_min_qp;
			config->rcParams.minQP = enc->preset_min_qp;
		}

		enc->abr_active = abr; /* track ABR state for governor + future change detection */

		/* Two-pass state is not safe to carry across an RC-regime change */
		config->rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;

		/* CBR padding - mirror create-time handling (init_encoder_h264) */
		if (enc->codec == CODEC_H264) {
			config->encodeCodecConfig.h264Config.enableFillerDataInsertion =
				desired_mode == NV_ENC_PARAMS_RC_CBR ? 1 : 0;
		}

		enc->can_change_bitrate = cqp ? false : nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE);
	} else if (br_changed) {
		info("changing bitrate from %lld to %lld while streaming", (long long)enc->props.bitrate,
		     (long long)bitrate);

		bool is_vbr = config->rcParams.rateControlMode == NV_ENC_PARAMS_RC_VBR;
		if (!enc->abr_active)
			config->rcParams.averageBitRate = (uint32_t)bitrate * 1000; /* ABR keeps averageBitRate=0 */
		config->rcParams.maxBitRate = is_vbr ? (uint32_t)max_bitrate * 1000 : (uint32_t)bitrate * 1000;
	}

	enc->props.bitrate = bitrate;
	enc->props.max_bitrate = max_bitrate;
	if (cqp) {
		enc->props.cqp = cqp_val;
	} else if (cq_vbr || abr) {
		enc->props.target_quality = tq_val;
	}

	/* While streaming, the reconfigure is deferred to the next GOP      */
	/* boundary. Forcing an IDR mid-GOP shifts this encoder's keyframe  */
	/* phase relative to sibling encoders of a multitrack output (the   */
	/* muxer requires every track's keyframes at identical pts), which  */
	/* permanently desyncs the tracks. At the shared boundary,          */
	/* nvenc_encode_base() fires the same reconfigure so all new GOPs   */
	/* start together again (delay: up to one GOP).                     */
	if (!obs_encoder_active(enc->encoder) || enc->config.gopLength == 0) {
		return apply_nvenc_reconfigure(enc);
	}

	info("staged reconfigure [%s] - applying at next keyframe (%u frames, boundary=%lld counter_now=%lld)",
	     change_desc.array ? change_desc.array : "(none)", (unsigned int)enc->config.gopLength,
	     (long long)((int64_t)enc->config.gopLength - ((int64_t)enc->output_delay - 1)),
	     (long long)enc->frames_since_idr);
	dstr_free(&change_desc);
	enc->reconfig_pending = true;
	return true;
}

static bool init_session(struct nvenc_data *enc)
{
	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
	params.apiVersion = NVENCAPI_VERSION;
#ifdef _WIN32
	if (enc->non_texture) {
		params.device = enc->cu_ctx;
		params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
	} else {
		params.device = enc->device;
		params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
	}
#else
	params.device = enc->cu_ctx;
	params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
#endif

	if (NV_FAILED(nv.nvEncOpenEncodeSessionEx(&params, &enc->session))) {
		return false;
	}

	/* Forensic trace: create the context now that the session exists. */
	enc->trace = nvenc_trace_init(enc);
	return true;
}

static void initialize_params(struct nvenc_data *enc, const GUID *nv_preset, NV_ENC_TUNING_INFO nv_tuning,
			      uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den)
{
	NV_ENC_INITIALIZE_PARAMS *params = &enc->params;
	memset(params, 0, sizeof(*params));
	params->version = NV_ENC_INITIALIZE_PARAMS_VER;
	params->encodeGUID = enc->codec_guid;
	params->presetGUID = *nv_preset;
	params->encodeWidth = width;
	params->encodeHeight = height;
	params->darWidth = width;
	params->darHeight = height;
	params->frameRateNum = fps_num;
	params->frameRateDen = fps_den;
	params->enableEncodeAsync = 0;
	params->enablePTD = 1;
	params->encodeConfig = &enc->config;
	params->tuningInfo = nv_tuning;
	/* Dynamic resize headroom: controlled by max_encode_width/height settings. */
	/* If not specified (0), no headroom is reserved — minimum VRAM usage.      */
	/* If specified, the session allows live resizes up to that size.           */
	const uint32_t cap_w = (uint32_t)nv_get_cap(enc, NV_ENC_CAPS_WIDTH_MAX);
	const uint32_t cap_h = (uint32_t)nv_get_cap(enc, NV_ENC_CAPS_HEIGHT_MAX);

	uint32_t max_w = enc->props.max_encode_width;
	uint32_t max_h = enc->props.max_encode_height;

	if (max_w || max_h) {
		/* User specified headroom — clamp to at least encode size, at most GPU cap. */
		max_w = max(max_w, width);
		max_h = max(max_h, height);
		if (cap_w && cap_h) {
			max_w = min(max_w, cap_w);
			max_h = min(max_h, cap_h);
		}
		params->maxEncodeWidth = max_w;
		params->maxEncodeHeight = max_h;
		info("resize headroom: user-specified max %ux%u (encode %ux%u)", max_w, max_h, width, height);
	} else {
		/* No headroom specified — set to encode size (disables live resize). */
		params->maxEncodeWidth = width;
		params->maxEncodeHeight = height;
		info("resize headroom: disabled (fixed resolution %ux%u, minimum VRAM)", width, height);
	}
#ifdef NVENC_12_1_OR_LATER
	params->splitEncodeMode = (NV_ENC_SPLIT_ENCODE_MODE)enc->props.split_encode;
#endif
}

static inline GUID get_nv_preset(const char *preset2)
{
	if (astrcmpi(preset2, "p1") == 0) {
		return NV_ENC_PRESET_P1_GUID;
	} else if (astrcmpi(preset2, "p2") == 0) {
		return NV_ENC_PRESET_P2_GUID;
	} else if (astrcmpi(preset2, "p3") == 0) {
		return NV_ENC_PRESET_P3_GUID;
	} else if (astrcmpi(preset2, "p4") == 0) {
		return NV_ENC_PRESET_P4_GUID;
	} else if (astrcmpi(preset2, "p6") == 0) {
		return NV_ENC_PRESET_P6_GUID;
	} else if (astrcmpi(preset2, "p7") == 0) {
		return NV_ENC_PRESET_P7_GUID;
	} else {
		return NV_ENC_PRESET_P5_GUID;
	}
}

static inline NV_ENC_TUNING_INFO get_nv_tuning(const char *tuning)
{
	if (astrcmpi(tuning, "ll") == 0) {
		return NV_ENC_TUNING_INFO_LOW_LATENCY;
	} else if (astrcmpi(tuning, "ull") == 0) {
		return NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
#ifdef NVENC_12_2_OR_LATER
	} else if (astrcmpi(tuning, "uhq") == 0) {
		return NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY;
#endif
	} else {
		return NV_ENC_TUNING_INFO_HIGH_QUALITY;
	}
}

static inline NV_ENC_MULTI_PASS get_nv_multipass(const char *multipass)
{
	if (astrcmpi(multipass, "qres") == 0) {
		return NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
	} else if (astrcmpi(multipass, "fullres") == 0) {
		return NV_ENC_TWO_PASS_FULL_RESOLUTION;
	} else {
		return NV_ENC_MULTI_PASS_DISABLED;
	}
}

static bool is_10_bit(const struct nvenc_data *enc)
{
	return enc->non_texture ? enc->in_format == VIDEO_FORMAT_P010
				: (obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_P010) ||
				   obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_Y410) ||
				   obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_GBR10));
}

/* tfLevel for the active codec (0 when the loaded API has no tfLevel field
 * for it). OBS never sets this - the value comes from the NVENC preset. */
static bool is_444(const struct nvenc_data *enc);

static uint32_t nvenc_tf_level(const struct nvenc_data *enc)
{
#ifdef NVENC_12_2_OR_LATER
	if (enc->codec == CODEC_HEVC)
		return enc->config.encodeCodecConfig.hevcConfig.tfLevel;
#endif
#ifdef NVENC_13_0_OR_LATER
	if (enc->codec == CODEC_H264)
		return enc->config.encodeCodecConfig.h264Config.tfLevel;
	if (enc->codec == CODEC_AV1)
		return enc->config.encodeCodecConfig.av1Config.tfLevel;
#endif
	(void)enc;
	return 0;
}

/* Effective input/output bit depth, computed the same way the codec start
 * functions do it (input from the delivery format, output from the profile).
 * Must stay in sync with the profile logic in nvenc_h264/hevc/av1_start -
 * it runs before those functions set config->profileGUID. */
static void nvenc_bit_depths(const struct nvenc_data *enc, int *in_depth, int *out_depth)
{
	*in_depth = is_10_bit(enc) ? 10 : 8;
	*out_depth = *in_depth;

	if (enc->codec == CODEC_H264) {
#ifdef NVENC_13_0_OR_LATER
		/* high10 selected explicitly, or forced for P010 input.
		 * 4:4:4 input takes the HIGH_444 profile (8-bit) instead. */
		*out_depth = !is_444(enc) && (astrcmpi(enc->props.profile, "high10") == 0 || is_10_bit(enc)) ? 10 : 8;
#endif
	} else if (enc->codec == CODEC_HEVC) {
#ifdef NVENC_12_2_OR_LATER
		/* main10 selected explicitly, or forced for P010 input. */
		*out_depth = (astrcmpi(enc->props.profile, "main10") == 0 || is_10_bit(enc)) ? 10 : 8;
#endif
	}
	/* AV1: output always equals input. */
}

static bool is_hdr(const enum video_colorspace space)
{
	return space == VIDEO_CS_2100_HLG || space == VIDEO_CS_2100_PQ;
}

static bool is_444(const struct nvenc_data *enc)
{
	return enc->non_texture ? enc->in_format == VIDEO_FORMAT_I444
				: (obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_GBRA) ||
				   obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_AYUV) ||
				   obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_Y410) ||
				   obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_GBR10));
}

static bool init_encoder_base(struct nvenc_data *enc, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);

	int bitrate = (int)enc->props.bitrate;
	int max_bitrate = (int)enc->props.max_bitrate;
	int rc_lookahead = 0;

	bool cqvbr = astrcmpi(enc->props.rate_control, "CQVBR") == 0;
	bool vbr = cqvbr || astrcmpi(enc->props.rate_control, "VBR") == 0;
	bool abr = astrcmpi(enc->props.rate_control, "ABR") == 0;
	bool lossless = strcmp(enc->props.rate_control, "lossless") == 0;
	bool cqp = strcmp(enc->props.rate_control, "CQP") == 0;

	NVENCSTATUS err;

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	enc->cx = obs_encoder_get_width(enc->encoder);
	enc->cy = obs_encoder_get_height(enc->encoder);

	/* -------------------------- */
	/* get preset                 */

	GUID nv_preset = get_nv_preset(enc->props.preset);
	NV_ENC_TUNING_INFO nv_tuning = get_nv_tuning(enc->props.tune);
	NV_ENC_MULTI_PASS nv_multipass = get_nv_multipass(enc->props.multipass);

	if (lossless) {
		nv_tuning = NV_ENC_TUNING_INFO_LOSSLESS;
		nv_multipass = NV_ENC_MULTI_PASS_DISABLED;
		enc->props.adaptive_quantization = false;
		enc->props.cqp = 0;
		enc->props.rate_control = "Lossless";
	}

	/* -------------------------- */
	/* get preset default config  */

	NV_ENC_PRESET_CONFIG preset_config = {0};
	preset_config.version = NV_ENC_PRESET_CONFIG_VER;
	preset_config.presetCfg.version = NV_ENC_CONFIG_VER;

	err = nv.nvEncGetEncodePresetConfigEx(enc->session, enc->codec_guid, nv_preset, nv_tuning, &preset_config);
	if (nv_failed(enc->encoder, err, __FUNCTION__, "nvEncGetEncodePresetConfig")) {
		return false;
	}

	/* preset_idx = upper 32 bits of the preset GUID (Data1). */
	nvenc_trace_preset(enc, &preset_config, nv_preset.Data1, (uint32_t)nv_tuning, (uint32_t)nv_multipass);

	/* -------------------------- */
	/* main configuration         */

	enc->config = preset_config.presetCfg;
	/* Remember the preset's QP floor so a live switch away from ABR can restore it
	 * (ABR raises rcParams.minQP to ABR_MIN_QP below). */
	enc->preset_min_qp = enc->config.rcParams.minQP;
	enc->preset_enable_min_qp = enc->config.rcParams.enableMinQP != 0;

	int keyint = (int)enc->props.keyint_sec * voi->fps_num / voi->fps_den;
	get_user_arg_int(enc, "keyint", &keyint);

	uint32_t gop_size = keyint > 0 ? keyint : 250;

	NV_ENC_CONFIG *config = &enc->config;

	/* Configure the session from the encoder's actual output size (enc->cx/cy, which
	 * come from obs_encoder_get_width/height), NOT from voi->width/height. At create()
	 * time the delivery mix may not yet be bound to encoder->media - maybe_set_up_gpu_rescale()
	 * in obs-encoder.c runs after create() so CPU/raw encoders can publish their preferred
	 * YUV format via get_video_info(). Until that rebind, voi (obs_encoder_video()) still
	 * reports the full base-canvas size, which would make NVENC emit an SPS at the canvas
	 * resolution while the container/input use the scaled size. enc->cx/cy already hold the
	 * scaled dimensions NVENC will actually encode. */
	initialize_params(enc, &nv_preset, nv_tuning, enc->cx, enc->cy, voi->fps_num, voi->fps_den);

#ifdef NVENC_12_2_OR_LATER
	/* Force at least 4 b-frames when using the UHQ tune */
	if (nv_tuning == NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY && enc->props.bf < 4) {
		warn("Forcing number of b-frames to 4 for UHQ tune.");
		enc->props.bf = 4;
	}
#endif

	config->gopLength = gop_size;
	config->frameIntervalP = gop_size == 1 ? 0 : (int32_t)enc->props.bf + 1;

	/* lookahead */

	const bool use_profile_lookahead = config->rcParams.enableLookahead;
	bool lookahead = nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_LOOKAHEAD) &&
			 (enc->props.lookahead || use_profile_lookahead);

	if (lookahead) {
		rc_lookahead = use_profile_lookahead ? config->rcParams.lookaheadDepth : 8;

		/* Due to the additional calculations required to handle lookahead,
		 * get the user override here (if any). */
		get_user_arg_int(enc, "lookaheadDepth", &rc_lookahead);
	}

	int buf_count = max(4, config->frameIntervalP * 2 * 2);
	int output_delay;
	if (lookahead) {
		/* The ring must hold every in-flight frame plus headroom for the
		 * post-reset pipeline fill. Measured against working/failing configs,
		 * the AV1/uhq post-reset pipeline (lookahead + temporal filtering +
		 * B-frame reordering) can exceed 2x the steady-state depth. Size at
		 * 4x to guarantee the ring never overflows during the fill period. */
		const int pipeline_depth = config->frameIntervalP + rc_lookahead;
		buf_count = max(buf_count, pipeline_depth * 4);

		/* The output gate should open when NVENC has likely produced its first
		 * post-reset output - approximately 2x the nominal pipeline depth plus
		 * a small margin. NOT buf_count-1 (that would delay output by the full
		 * ring size, causing a multi-second silence after reconfigure). */
		output_delay = pipeline_depth * 2 + 16;
	} else {
		output_delay = buf_count - 1;
	}

	buf_count = min(128, buf_count);
	enc->buf_count = buf_count;

	/* Ensure the gate opens before the overflow guard fires. */
	output_delay = min(output_delay, buf_count - 2);
	enc->output_delay = output_delay;

	info("pipeline params: gopLength=%d frameIntervalP=%d bf=%d buf_count=%d output_delay=%d",
	     (int)config->gopLength, (int)config->frameIntervalP, (int)enc->props.bf, enc->buf_count,
	     enc->output_delay);

	if (lookahead) {
		/* Clamp the effective lookahead so that 2x the resulting pipeline
		 * depth fits in the bitstream ring. During the post-reset transition
		 * the pipeline holds extra frames before emitting output, delaying
		 * ring-frame outputs by one full pipeline depth. The ring therefore
		 * needs 2x depth headroom. */
		const int lkd_bound = enc->buf_count / 2 - config->frameIntervalP - 8;
		if (lkd_bound >= 0) {
			config->rcParams.enableLookahead = 1;
			config->rcParams.lookaheadDepth = min(rc_lookahead, lkd_bound);
			config->rcParams.disableIadapt = 0;
			config->rcParams.disableBadapt = 0;
		} else {
			lookahead = false;
		}
	}

	enc->config.rcParams.disableIadapt = enc->props.disable_scenecut;

	/* psycho aq */

	if (enc->props.adaptive_quantization) {
		config->rcParams.enableAQ = 1;
		config->rcParams.aqStrength = 8;
		config->rcParams.enableTemporalAQ = nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ);
	}

	/* -------------------------- */
	/* rate control               */

	enc->can_change_bitrate = nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE);

	config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
	config->rcParams.averageBitRate = bitrate * 1000;
	config->rcParams.maxBitRate = vbr ? max_bitrate * 1000 : bitrate * 1000;
	config->rcParams.vbvBufferSize = bitrate * 1000;

	if (cqp || lossless) {
		int cqp_val = enc->codec == CODEC_AV1 ? (int)enc->props.cqp * 4 : (int)enc->props.cqp;

		config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
		config->rcParams.constQP.qpInterP = cqp_val;
		config->rcParams.constQP.qpInterB = cqp_val;
		config->rcParams.constQP.qpIntra = cqp_val;
		enc->can_change_bitrate = false;

		bitrate = 0;
		max_bitrate = 0;

	} else if (!vbr && !abr) { /* CBR by default (ABR is handled below, not here) */
		config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
	} else if (cqvbr) {
		config->rcParams.targetQuality = (uint8_t)enc->props.target_quality;
		config->rcParams.averageBitRate = 0;
		config->rcParams.vbvBufferSize = 0;
	} else if (abr) {
		/* ABR: QVBR (averageBitRate=0) so the driver's 2x-average cap doesn't apply,
		 * plus a runtime governor that adapts maxBitRate to track the target bitrate.
		 * The quality aim is user-configurable via "target_quality" like CQVBR. */
		config->rcParams.targetQuality = (uint8_t)enc->props.target_quality;
		/* QP floor: easy content stops at ABR_MIN_QP instead of burning the budget on
		 * ultra-low QPs - see ABR_MIN_QP. AV1 QP fields use the 4x user-QP scale,
		 * like cqp does. */
		const uint32_t abr_min_qp = enc->codec == CODEC_AV1 ? ABR_MIN_QP * 4 : ABR_MIN_QP;
		config->rcParams.enableMinQP = 1;
		config->rcParams.minQP.qpInterP = abr_min_qp;
		config->rcParams.minQP.qpInterB = abr_min_qp;
		config->rcParams.minQP.qpIntra = abr_min_qp;
		config->rcParams.averageBitRate = 0;
		config->rcParams.vbvBufferSize = 0;
		double db = obs_data_get_double(settings, "deadband");
		if (db < 0.1)
			db = 0.1; /* avoid a zero band that would reconfig on every tick */
		else if (db > 10.0)
			db = 10.0;
		enc->abr_deadband = db / 100.0;
		/* Open at the governed rate: fresh sessions start at target (headroom is earned, not assumed),
		 * a killed or briefly disconnected previous session restores its cumulative budget. The governor
		 * waits ~1 s of history before deciding, so the opening keyframe can't skew the average. */
		const int64_t abr_target_bps = (int64_t)bitrate * 1000;
		const int64_t abr_ceiling_bps =
		    (int64_t)(enc->props.max_bitrate > 0 ? enc->props.max_bitrate : bitrate) * 1000;
		abr_gov_config cfg = abr_gov_default_config();
		cfg.deadband_frac = enc->abr_deadband;
		nvenc_abr_latch(enc, &cfg, abr_target_bps, abr_ceiling_bps);
		config->rcParams.maxBitRate = (uint32_t)enc->abr_state.cur_rate;
		if (!enc->can_change_bitrate)
			warn("ABR: dynamic bitrate change unsupported - running as uncontrolled QVBR (cap frozen at max)");
	}

	enc->abr_active = abr;

	config->rcParams.multiPass = nv_multipass;
	config->rcParams.qpMapMode = NV_ENC_QP_MAP_DELTA;

	/* -------------------------- */
	/* log settings               */

	struct dstr log = {0};

	dstr_catf(&log, "\tcodec:        %s\n", get_codec_name(enc->codec));
	dstr_catf(&log, "\trate_control: %s\n", enc->props.rate_control);

	if (bitrate && !cqvbr)
		dstr_catf(&log, "\tbitrate:      %d\n", bitrate);
	if (vbr || abr)
		dstr_catf(&log, "\tmax_bitrate:  %d\n", max_bitrate);
	if (config->rcParams.vbvBufferSize)
		dstr_catf(&log, "\tvbv_buffer:   %u kbps\n", config->rcParams.vbvBufferSize / 1000);
	if (cqp)
		dstr_catf(&log, "\tcqp:          %lld\n", (long long)enc->props.cqp);
	if (cqvbr || abr) {
		dstr_catf(&log, "\tcq:           %lld\n", (long long)enc->props.target_quality);
	}
	if (abr)
		dstr_catf(&log, "\tmin_qp:       %u\n", ABR_MIN_QP);

	dstr_catf(&log, "\tkeyint:       %d\n", gop_size);
	dstr_catf(&log, "\tpreset:       %s\n", enc->props.preset);
	dstr_catf(&log, "\ttuning:       %s\n", enc->props.tune);
	dstr_catf(&log, "\tmultipass:    %s\n", enc->props.multipass);
	dstr_catf(&log, "\tprofile:      %s\n", enc->props.profile);

	int in_depth, out_depth;
	nvenc_bit_depths(enc, &in_depth, &out_depth);
	dstr_catf(&log, "\tbit_depth:    in=%d out=%d\n", in_depth, out_depth);

	dstr_catf(&log, "\twidth:        %d\n", enc->cx);
	dstr_catf(&log, "\theight:       %d\n", enc->cy);
	dstr_catf(&log, "\tFPS:          %g (%u/%u)\n", obs_encoder_get_effective_fps(enc->encoder),
		  obs_encoder_get_fps_num(enc->encoder),
		  obs_encoder_get_fps_den(enc->encoder) * obs_encoder_get_frame_rate_divisor(enc->encoder));

	dstr_catf(&log, "\tb-frames:     %lld\n", (long long)enc->props.bf);
	dstr_catf(&log, "\tb-ref-mode:   %lld\n", (long long)enc->props.bframe_ref_mode);

	dstr_catf(&log, "\tlookahead:    %s (%d frames)\n", lookahead ? "true" : "false",
		  config->rcParams.lookaheadDepth);
	dstr_catf(&log, "\taq:           %s\n", enc->props.adaptive_quantization ? "true" : "false");
	dstr_catf(&log, "\ttfLevel:      %u\n", nvenc_tf_level(enc));

	if (enc->props.split_encode) {
		dstr_catf(&log, "\tsplit encode: %lld\n", (long long)enc->props.split_encode);
	}
	if (enc->props.opts.count)
		dstr_catf(&log, "\tuser opts:    %s\n", enc->props.opts_str);

	info("settings:\n%s", log.array);
	dstr_free(&log);

	return true;
}

/* True when this encoder is delivering an identity-RGB texture (GBRA/GBR10), i.e. raw
 * G/B/R samples with no YUV transform. Such encodes can only carry a valid VUI of the
 * identity (RGB) matrix, full range, unspecified primaries/transfer - anything else
 * conflicts with the colr box the muxer writes. */
static bool nvenc_identity_rgb_active(const struct nvenc_data *enc)
{
	return obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_GBRA) ||
	       obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_GBR10);
}

/* Force the active codec's VUI to the identity-RGB description above. NVENC generates its
 * SPS/VUI at create() time - before the delivery mix is bound - so this may need
 * (re)applying once the texture format is actually known, after which the SPS is
 * regenerated via apply_nvenc_reconfigure(). */
static void nvenc_apply_identity_vui(struct nvenc_data *enc)
{
	NV_ENC_CONFIG *config = &enc->config;

	switch (enc->codec) {
	case CODEC_H264: {
		NV_ENC_CONFIG_H264_VUI_PARAMETERS *v = &config->encodeCodecConfig.h264Config.h264VUIParameters;
		v->videoFullRangeFlag = 1;
		v->colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_UNSPECIFIED;
		v->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_UNSPECIFIED;
		v->colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_RGB;
		break;
	}
	case CODEC_HEVC: {
		NV_ENC_CONFIG_HEVC_VUI_PARAMETERS *v = &config->encodeCodecConfig.hevcConfig.hevcVUIParameters;
		v->videoFullRangeFlag = 1;
		v->colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_UNSPECIFIED;
		v->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_UNSPECIFIED;
		v->colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_RGB;
		break;
	}
	default:
		break;
	}
}

static bool init_encoder_h264(struct nvenc_data *enc, obs_data_t *settings)
{
	bool lossless = strcmp(enc->props.rate_control, "lossless") == 0;

	if (!init_encoder_base(enc, settings)) {
		return false;
	}

	NV_ENC_CONFIG *config = &enc->config;
	NV_ENC_CONFIG_H264 *h264_config = &config->encodeCodecConfig.h264Config;
	NV_ENC_CONFIG_H264_VUI_PARAMETERS *vui_params = &h264_config->h264VUIParameters;

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	if (enc->props.repeat_headers) {
		h264_config->repeatSPSPPS = 1;
		h264_config->disableSPSPPS = 0;
		h264_config->outputAUD = 1;
	}

	h264_config->idrPeriod = config->gopLength;

	h264_config->sliceMode = 3;
	h264_config->sliceModeData = 1;

	h264_config->useBFramesAsRef = (NV_ENC_BFRAME_REF_MODE)enc->props.bframe_ref_mode;

	/* Enable CBR padding */
	if (config->rcParams.rateControlMode == NV_ENC_PARAMS_RC_CBR)
		h264_config->enableFillerDataInsertion = 1;

	vui_params->videoSignalTypePresentFlag = 1;
	vui_params->videoFullRangeFlag = (voi->range == VIDEO_RANGE_FULL);
	vui_params->colourDescriptionPresentFlag = 1;

	const bool use_identity = obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_GBRA) ||
				  obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_GBR10);

	switch (voi->colorspace) {
	case VIDEO_CS_601:
		vui_params->colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_SMPTE170M;
		vui_params->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE170M;
		vui_params->colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_SMPTE170M;
		break;
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_709:
		vui_params->colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
		vui_params->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
		vui_params->colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
		break;
	case VIDEO_CS_SRGB:
		vui_params->colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
		vui_params->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
		vui_params->colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
		break;
	default:
		break;
	}

	/* Identity-RGB (GBRA/GBR10): samples are raw G/B/R with no YUV transform, so the only valid VUI is
	 * the identity (RGB) matrix + full range with primaries/transfer unspecified. Mirror the obs-x264
	 * identity-RGB path so both encoders tag RGB identically. */
	if (use_identity)
		nvenc_apply_identity_vui(enc);
	enc->vui_identity_applied = use_identity;

	if (lossless) {
		h264_config->qpPrimeYZeroTransformBypassFlag = 1;
	} else if (strcmp(enc->props.rate_control, "CBR") == 0) { /* CBR */
		h264_config->outputBufferingPeriodSEI = 1;
	}

	h264_config->outputPictureTimingSEI = 1;

	/* -------------------------- */
	/* profile                    */

	if (is_444(enc)) {
		config->profileGUID = NV_ENC_H264_PROFILE_HIGH_444_GUID;
		h264_config->chromaFormatIDC = 3;
#ifdef NVENC_13_0_OR_LATER
	} else if (astrcmpi(enc->props.profile, "high10") == 0) {
		config->profileGUID = NV_ENC_H264_PROFILE_HIGH_10_GUID;
	} else if (is_10_bit(enc)) {
		warn("Forcing high10 for P010");
		config->profileGUID = NV_ENC_H264_PROFILE_HIGH_10_GUID;
#endif
	} else if (astrcmpi(enc->props.profile, "main") == 0) {
		config->profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
	} else if (astrcmpi(enc->props.profile, "baseline") == 0) {
		config->profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
	} else if (!lossless) {
		config->profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
	}

#ifdef NVENC_13_0_OR_LATER
	/* Note: Only supported on Blackwell! */
	h264_config->inputBitDepth = is_10_bit(enc) ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
	h264_config->outputBitDepth = memcmp(&config->profileGUID, &NV_ENC_H264_PROFILE_HIGH_10_GUID, sizeof(GUID)) == 0
					      ? NV_ENC_BIT_DEPTH_10
					      : NV_ENC_BIT_DEPTH_8;
#endif

	if (!apply_user_args(enc)) {
		obs_encoder_set_last_error(enc->encoder, obs_module_text("Opts.Invalid"));
		return false;
	}

	nvenc_trace_pre_init(enc, &enc->config, &enc->params);
	NVENCSTATUS err = nv.nvEncInitializeEncoder(enc->session, &enc->params);
	nvenc_trace_post_init(enc, &enc->config, &enc->params, err);
	if (NV_FAILED(err)) {
		return false;
	}

	return true;
}

static bool init_encoder_hevc(struct nvenc_data *enc, obs_data_t *settings)
{
	if (!init_encoder_base(enc, settings)) {
		return false;
	}

	NV_ENC_CONFIG *config = &enc->config;
	NV_ENC_CONFIG_HEVC *hevc_config = &config->encodeCodecConfig.hevcConfig;
	NV_ENC_CONFIG_HEVC_VUI_PARAMETERS *vui_params = &hevc_config->hevcVUIParameters;

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	if (enc->props.repeat_headers) {
		hevc_config->repeatSPSPPS = 1;
		hevc_config->disableSPSPPS = 0;
		hevc_config->outputAUD = 1;
	}

	hevc_config->idrPeriod = config->gopLength;

	hevc_config->sliceMode = 3;
	hevc_config->sliceModeData = 1;

	hevc_config->useBFramesAsRef = (NV_ENC_BFRAME_REF_MODE)enc->props.bframe_ref_mode;

	/* Enable CBR padding */
	if (config->rcParams.rateControlMode == NV_ENC_PARAMS_RC_CBR)
		hevc_config->enableFillerDataInsertion = 1;

	vui_params->videoSignalTypePresentFlag = 1;
	vui_params->videoFullRangeFlag = (voi->range == VIDEO_RANGE_FULL);
	vui_params->colourDescriptionPresentFlag = 1;

	const bool use_identity = obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_GBRA) ||
				  obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_GBR10);

	switch (voi->colorspace) {
	case VIDEO_CS_601:
		vui_params->colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_SMPTE170M;
		vui_params->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE170M;
		vui_params->colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_SMPTE170M;
		break;
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_709:
		vui_params->colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
		vui_params->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
		vui_params->colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
		break;
	case VIDEO_CS_SRGB:
		vui_params->colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
		vui_params->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
		vui_params->colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
		break;
	case VIDEO_CS_2100_PQ:
		vui_params->colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
		vui_params->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084;
		vui_params->colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
		vui_params->chromaSampleLocationFlag = 1;
		vui_params->chromaSampleLocationTop = 2;
		vui_params->chromaSampleLocationBot = 2;
		break;
	case VIDEO_CS_2100_HLG:
		vui_params->colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
		vui_params->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_ARIB_STD_B67;
		vui_params->colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
		vui_params->chromaSampleLocationFlag = 1;
		vui_params->chromaSampleLocationTop = 2;
		vui_params->chromaSampleLocationBot = 2;
	}

	/* Identity-RGB (GBRA/GBR10): samples are raw G/B/R with no YUV transform, so the only valid VUI is
	 * the identity (RGB) matrix + full range with primaries/transfer unspecified. Mirror the obs-x264
	 * identity-RGB path so both encoders tag RGB identically. */
	if (use_identity)
		nvenc_apply_identity_vui(enc);
	enc->vui_identity_applied = use_identity;

	if (astrcmpi(enc->props.rate_control, "cbr") == 0) {
		hevc_config->outputBufferingPeriodSEI = 1;
	}

	hevc_config->outputPictureTimingSEI = 1;

	/* -------------------------- */
	/* profile                    */

	bool profile_is_10bpc = false;

	if (is_444(enc)) {
		config->profileGUID = NV_ENC_HEVC_PROFILE_FREXT_GUID;
		hevc_config->chromaFormatIDC = 3;
		// FREXT can be 8-bit or 10-bit depending on input
		profile_is_10bpc = is_10_bit(enc);
	} else if (astrcmpi(enc->props.profile, "main10") == 0) {
		config->profileGUID = NV_ENC_HEVC_PROFILE_MAIN10_GUID;
		profile_is_10bpc = true;
	} else if (is_10_bit(enc)) {
		warn("Forcing main10 for P010");
		config->profileGUID = NV_ENC_HEVC_PROFILE_MAIN10_GUID;
		profile_is_10bpc = true;
	} else {
		config->profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
	}

#ifndef NVENC_12_2_OR_LATER
	hevc_config->pixelBitDepthMinus8 = is_10_bit(enc) ? 2 : 0;
#else
	hevc_config->inputBitDepth = is_10_bit(enc) ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
	hevc_config->outputBitDepth = profile_is_10bpc ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
#endif

#ifdef NVENC_13_0_OR_LATER
	if (is_10_bit(enc) && is_hdr(voi->colorspace)) {
		hevc_config->outputMasteringDisplay = 1;
		hevc_config->outputMaxCll = 1;
	}
#endif

	if (!apply_user_args(enc)) {
		obs_encoder_set_last_error(enc->encoder, obs_module_text("Opts.Invalid"));
		return false;
	}

	nvenc_trace_pre_init(enc, &enc->config, &enc->params);
	NVENCSTATUS err = nv.nvEncInitializeEncoder(enc->session, &enc->params);
	nvenc_trace_post_init(enc, &enc->config, &enc->params, err);
	if (NV_FAILED(err)) {
		return false;
	}

	return true;
}

static bool init_encoder_av1(struct nvenc_data *enc, obs_data_t *settings)
{
	if (!init_encoder_base(enc, settings)) {
		return false;
	}

	NV_ENC_CONFIG *config = &enc->config;
	NV_ENC_CONFIG_AV1 *av1_config = &config->encodeCodecConfig.av1Config;

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	av1_config->idrPeriod = config->gopLength;

	av1_config->useBFramesAsRef = (NV_ENC_BFRAME_REF_MODE)enc->props.bframe_ref_mode;

	av1_config->colorRange = (voi->range == VIDEO_RANGE_FULL);

	/* Enable CBR padding */
	if (config->rcParams.rateControlMode == NV_ENC_PARAMS_RC_CBR)
		av1_config->enableBitstreamPadding = 1;

#define PIXELCOUNT_4K (3840 * 2160)

	/* If size is 4K+, set tiles to 2 uniform columns. */
	if ((voi->width * voi->height) >= PIXELCOUNT_4K)
		av1_config->numTileColumns = 2;

	switch (voi->colorspace) {
	case VIDEO_CS_601:
		av1_config->colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_SMPTE170M;
		av1_config->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE170M;
		av1_config->matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_SMPTE170M;
		break;
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_709:
		av1_config->colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
		av1_config->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
		av1_config->matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
		break;
	case VIDEO_CS_SRGB:
		av1_config->colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
		av1_config->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
		av1_config->matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
		break;
	case VIDEO_CS_2100_PQ:
		av1_config->colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
		av1_config->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084;
		av1_config->matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
		break;
	case VIDEO_CS_2100_HLG:
		av1_config->colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
		av1_config->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_ARIB_STD_B67;
		av1_config->matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
	}

	/* -------------------------- */
	/* profile                    */

	config->profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
	av1_config->tier = NV_ENC_TIER_AV1_0;

	av1_config->level = NV_ENC_LEVEL_AV1_AUTOSELECT;
	av1_config->chromaFormatIDC = 1;
#ifndef NVENC_12_2_OR_LATER
	av1_config->pixelBitDepthMinus8 = is_10_bit(enc) ? 2 : 0;
	av1_config->inputPixelBitDepthMinus8 = av1_config->pixelBitDepthMinus8;
#else
	av1_config->inputBitDepth = is_10_bit(enc) ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
	av1_config->outputBitDepth = av1_config->inputBitDepth;
#endif
	av1_config->numFwdRefs = 1;
	av1_config->numBwdRefs = 1;
	av1_config->repeatSeqHdr = 1;

#ifdef NVENC_13_0_OR_LATER
	if (is_10_bit(enc) && is_hdr(voi->colorspace)) {
		av1_config->outputMasteringDisplay = 1;
		av1_config->outputMaxCll = 1;
	}
#endif

	if (!apply_user_args(enc)) {
		obs_encoder_set_last_error(enc->encoder, obs_module_text("Opts.Invalid"));
		return false;
	}

	nvenc_trace_pre_init(enc, &enc->config, &enc->params);
	NVENCSTATUS err = nv.nvEncInitializeEncoder(enc->session, &enc->params);
	nvenc_trace_post_init(enc, &enc->config, &enc->params, err);
	if (NV_FAILED(err)) {
		return false;
	}

	return true;
}

static bool init_bitstreams(struct nvenc_data *enc)
{
	da_reserve(enc->bitstreams, enc->buf_count);
	for (uint32_t i = 0; i < enc->buf_count; i++) {
		struct nv_bitstream bitstream;
		if (!nv_bitstream_init(enc, &bitstream)) {
			return false;
		}

		da_push_back(enc->bitstreams, &bitstream);
	}

	return true;
}

static enum video_format get_preferred_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_P010:
		return VIDEO_FORMAT_P010;
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_I444:
		return VIDEO_FORMAT_I444;
	default:
		return VIDEO_FORMAT_NV12;
	}
}

static void nvenc_destroy(void *data);

static bool init_encoder(struct nvenc_data *enc, enum codec_type codec, obs_data_t *settings, obs_encoder_t *encoder)
{
	UNUSED_PARAMETER(codec);
	UNUSED_PARAMETER(encoder);

	const bool support_10bit = nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_10BIT_ENCODE);
	const bool support_444 = nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_YUV444_ENCODE);

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	enum video_format pref_format = obs_encoder_get_preferred_video_format(enc->encoder);
	if (pref_format == VIDEO_FORMAT_NONE)
		pref_format = voi->format;

	enc->in_format = get_preferred_format(pref_format);

	if (is_444(enc) && !support_444) {
		NV_FAIL(obs_module_text("444Unsupported"));
		return false;
	}

	if (is_10_bit(enc) && !support_10bit) {
		NV_FAIL(obs_module_text("10bitUnsupported"));
		return false;
	}

	switch (voi->format) {
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_P010:
	case VIDEO_FORMAT_R10L:
		break;
	default:
		switch (voi->colorspace) {
		case VIDEO_CS_2100_PQ:
		case VIDEO_CS_2100_HLG:
			NV_FAIL(obs_module_text("8bitUnsupportedHdr"));
			return false;
		default:
			break;
		}
	}

#ifdef NVENC_13_0_OR_LATER
	const bool pq = voi->colorspace == VIDEO_CS_2100_PQ;
	const bool hlg = voi->colorspace == VIDEO_CS_2100_HLG;
	if (pq || hlg) {
		enc->cll = bzalloc(sizeof(CONTENT_LIGHT_LEVEL));
		enc->mdi = bzalloc(sizeof(MASTERING_DISPLAY_INFO));
		const uint16_t hdr_nominal_peak_level = pq ? (uint16_t)obs_get_video_hdr_nominal_peak_level()
							   : (hlg ? 1000 : 0);
		/* Currently these are hardcoded across all encoders. */
		enc->mdi->r.x = 13250;
		enc->mdi->r.y = 34500;
		enc->mdi->g.x = 7500;
		enc->mdi->g.y = 3000;
		enc->mdi->b.x = 34000;
		enc->mdi->b.y = 16000;
		enc->mdi->whitePoint.x = 15635;
		enc->mdi->whitePoint.y = 16450;
		enc->mdi->maxLuma = hdr_nominal_peak_level * 10000;
		enc->mdi->minLuma = 0;

		enc->cll->maxContentLightLevel = hdr_nominal_peak_level;
		enc->cll->maxPicAverageLightLevel = hdr_nominal_peak_level;
	}
#endif

	switch (enc->codec) {
	case CODEC_HEVC:
		return init_encoder_hevc(enc, settings);
	case CODEC_H264:
		return init_encoder_h264(enc, settings);
	case CODEC_AV1:
		return init_encoder_av1(enc, settings);
	}

	return false;
}

static void *nvenc_create_internal(enum codec_type codec, obs_data_t *settings, obs_encoder_t *encoder, bool texture)
{
	struct nvenc_data *enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;
	enc->codec = codec;
	enc->first_packet = true;
	enc->non_texture = !texture;
	enc->last_keyframe_pts = -1; /* no keyframe cadence observed yet */

	nvenc_properties_read(&enc->props, settings);

	NV_ENCODE_API_FUNCTION_LIST init = {NV_ENCODE_API_FUNCTION_LIST_VER};

	switch (enc->codec) {
	case CODEC_H264:
		enc->codec_guid = NV_ENC_CODEC_H264_GUID;
		break;
	case CODEC_HEVC:
		enc->codec_guid = NV_ENC_CODEC_HEVC_GUID;
		break;
	case CODEC_AV1:
		enc->codec_guid = NV_ENC_CODEC_AV1_GUID;
		break;
	}

	if (!init_nvenc(encoder))
		goto fail;

#ifdef _WIN32
	if (texture ? !d3d11_init(enc, settings) : !init_cuda(encoder))
		goto fail;
#else
	if (!init_cuda(encoder))
		goto fail;
#endif

	if (NV_FAILED(nv_create_instance(&init)))
		goto fail;

	if (!cuda_ctx_init(enc, settings, texture))
		goto fail;

	if (!init_session(enc)) {
		goto fail;
	}
	if (!init_encoder(enc, codec, settings, encoder)) {
		goto fail;
	}
	if (!init_bitstreams(enc)) {
		goto fail;
	}

#ifdef _WIN32
	if (texture ? !d3d11_init_textures(enc) : !cuda_init_surfaces(enc))
		goto fail;
#else
	if (!cuda_init_surfaces(enc))
		goto fail;
#endif

	enc->codec = codec;

	return enc;

fail:
	nvenc_destroy(enc);
	return NULL;
}

static void *nvenc_create_base(enum codec_type codec, obs_data_t *settings, obs_encoder_t *encoder, bool texture)
{
	/* This encoder requires shared textures, this cannot be used on a
	 * gpu other than the one OBS is currently running on.
	 *
	 * 2024 Amendment: On Linux when using CUDA<->OpenGL interop we can
	 * in fact use shared textures even when using a different GPU, this
	 * will still copy data through the CPU, but much more efficiently than
	 * our native non-texture encoder. For now allow this via a hidden
	 * option as it may cause issues for people.
	 */
	const int gpu = (int)obs_data_get_int(settings, "device");
	const bool gpu_set = obs_data_has_user_value(settings, "device");
#ifndef _WIN32
	const bool force_tex = obs_data_get_bool(settings, "force_cuda_tex");
#else
	const bool force_tex = false;
#endif

	if (gpu_set && gpu != -1 && texture && !force_tex) {
		blog(LOG_INFO, "[obs-nvenc] different GPU selected by user, falling back to non-texture encoder");
		goto reroute;
	}

	if (obs_encoder_scaling_enabled(encoder)) {
		if (obs_encoder_gpu_scaling_enabled(encoder)) {
			blog(LOG_INFO, "[obs-nvenc] GPU scaling enabled");
		} else if (texture) {
			blog(LOG_INFO, "[obs-nvenc] CPU scaling enabled, falling back to non-texture encoder");
			goto reroute;
		}
	}

	if (texture && !obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_NV12) &&
	    !obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_P010) &&
	    !obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_GBRA) &&
	    !obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_AYUV) &&
	    !obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_Y410) &&
	    !obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_GBR10)) {
		blog(LOG_INFO, "[obs-nvenc] Shared textures not active, falling back to non-texture encoder");
		goto reroute;
	}

	struct nvenc_data *enc = nvenc_create_internal(codec, settings, encoder, texture);

	if (enc) {
		return enc;
	}

reroute:
	if (!texture) {
		blog(LOG_ERROR, "Already in non_texture encoder, can't fall back further!");
		return NULL;
	}

	switch (codec) {
	case CODEC_H264:
		return obs_encoder_create_rerouted(encoder, "obs_nvenc_h264_soft");
	case CODEC_HEVC:
		return obs_encoder_create_rerouted(encoder, "obs_nvenc_hevc_soft");
	case CODEC_AV1:
		return obs_encoder_create_rerouted(encoder, "obs_nvenc_av1_soft");
	}

	return NULL;
}

static void *h264_nvenc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	return nvenc_create_base(CODEC_H264, settings, encoder, true);
}

#ifdef ENABLE_HEVC
static void *hevc_nvenc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	return nvenc_create_base(CODEC_HEVC, settings, encoder, true);
}
#endif

static void *av1_nvenc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	return nvenc_create_base(CODEC_AV1, settings, encoder, true);
}

static void *h264_nvenc_soft_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	return nvenc_create_base(CODEC_H264, settings, encoder, false);
}

#ifdef ENABLE_HEVC
static void *hevc_nvenc_soft_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	return nvenc_create_base(CODEC_HEVC, settings, encoder, false);
}
#endif

static void *av1_nvenc_soft_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	return nvenc_create_base(CODEC_AV1, settings, encoder, false);
}

static bool get_encoded_packet(struct nvenc_data *enc, bool finalize);

static void nvenc_destroy(void *data)
{
	struct nvenc_data *enc = data;

	abr_gov_session_save(enc->encoder, enc->abr_active, &enc->abr_state);

	if (enc->encode_started) {
		NV_ENC_PIC_PARAMS params = {NV_ENC_PIC_PARAMS_VER};
		params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
		nvenc_trace_pic(enc, &params, true);
		nv.nvEncEncodePicture(enc->session, &params);
		get_encoded_packet(enc, true);
	}

	for (size_t i = 0; i < enc->bitstreams.num; i++) {
		nv_bitstream_free(enc, &enc->bitstreams.array[i]);
	}
	if (enc->session)
		nv.nvEncDestroyEncoder(enc->session);

	nvenc_trace_destroy(enc);

#ifdef _WIN32
	d3d11_free_textures(enc);
	/* Free any deferred old-size textures from a live resize. */
	if (enc->old_textures.num > 0) {
		for (size_t i = 0; i < enc->old_textures.num; i++) {
			struct nv_texture *ot = &enc->old_textures.array[i];
			if (ot->res) {
				if (ot->mapped_res)
					nv.nvEncUnmapInputResource(enc->session, ot->mapped_res);
				nv.nvEncUnregisterResource(enc->session, ot->res);
				if (ot->vp_view)
					ot->vp_view->lpVtbl->Release(ot->vp_view);
				ot->tex->lpVtbl->Release(ot->tex);
			}
		}
		da_free(enc->old_textures);
	}
	d3d11_free_gpu_scaler(enc);
	d3d11_free_cpu_scaler(enc);
	d3d11_free(enc);
#else
	cuda_opengl_free(enc);
#endif
	cuda_free_surfaces(enc);
	cuda_ctx_free(enc);

	bfree(enc->header);
	bfree(enc->sei);
	bfree(enc->roi_map);
	nvenc_free_scale_buffers(enc);

#ifdef NVENC_13_0_OR_LATER
	if (enc->mdi)
		bfree(enc->mdi);
	if (enc->cll)
		bfree(enc->cll);
#endif

	deque_free(&enc->dts_list);

	da_free(enc->surfaces);
	da_free(enc->input_textures);
	da_free(enc->bitstreams);
#ifdef _WIN32
	da_free(enc->textures);
#endif
	da_free(enc->packet_data);

	obs_free_options(enc->props.opts);
	obs_data_release(enc->props.data);

	bfree(enc);
}

static bool get_encoded_packet(struct nvenc_data *enc, bool finalize)
{
	void *s = enc->session;

	da_resize(enc->packet_data, 0);

	if (!enc->buffers_queued)
		return true;

	if (!finalize && enc->buffers_queued < enc->output_delay) {
		/* Post-reset fill period: log progress every 10 frames */
		if (enc->first_packet && (enc->buffers_queued % 10 == 0))
			debug("fill period: queued=%u/%u (waiting for output_delay)",
			      (unsigned)enc->buffers_queued, (unsigned)enc->output_delay);
		return true;
	}

	size_t count = finalize ? enc->buffers_queued : 1;

	for (size_t i = 0; i < count; i++) {
		size_t cur_bs_idx = enc->cur_bitstream;
		struct nv_bitstream *bs = &enc->bitstreams.array[cur_bs_idx];
#ifdef _WIN32
		struct nv_texture *nvtex = enc->non_texture ? NULL : &enc->textures.array[cur_bs_idx];
		struct nv_cuda_surface *surf = enc->non_texture ? &enc->surfaces.array[cur_bs_idx] : NULL;
#else
		struct nv_cuda_surface *surf = &enc->surfaces.array[cur_bs_idx];
#endif

		/* ---------------- */

		NV_ENC_LOCK_BITSTREAM lock = {NV_ENC_LOCK_BITSTREAM_VER};
		lock.outputBitstream = bs->ptr;
		lock.doNotWait = true;

		NVENCSTATUS err;

		err = nv.nvEncLockBitstream(s, &lock);
		if (err != NV_ENC_SUCCESS) {
			/* With doNotWait=true, the lock returns immediately if no output
			 * is ready. Two transient errors are expected during the post-reset
			 * fill period:
			 *   INVALID_PARAM - pipeline not yet deep enough to emit output
			 *   LOCK_BUSY     - encoder still processing (synchronous mode)
			 * Both mean "try again on the next submission." Leave the frame
			 * queued (no consume, no DTS pop). Bounded by the ring size: if
			 * the queue fills to buf_count without producing output the
			 * pipeline is genuinely broken and we fall through to fatal. */
			if (!finalize &&
			    (err == NV_ENC_ERR_INVALID_PARAM || err == NV_ENC_ERR_LOCK_BUSY) &&
			    (uint32_t)enc->buffers_queued < enc->buf_count) {
				/* Log transient lock failures during first_packet period */
				if (enc->first_packet && (enc->buffers_queued % 10 == 0))
					debug("lock failure during fill: err=%d (%s) queued=%u/%u bs_idx=%u",
					      (int)err,
					      err == NV_ENC_ERR_LOCK_BUSY ? "LOCK_BUSY" : "INVALID_PARAM",
					      (unsigned)enc->buffers_queued,
					      (unsigned)enc->buf_count, (unsigned)enc->cur_bitstream);
				return true;
			}

			/* During finalize (EOS flush), the driver may have already
			 * released its internal encode resources. OUT_OF_MEMORY means
			 * no more output will be produced - stop flushing gracefully. */
			if (finalize && err == NV_ENC_ERR_OUT_OF_MEMORY) {
				debug("EOS flush: encoder released resources, %u frame(s) not retrieved",
				      (unsigned)enc->buffers_queued);
				return true;
			}

			nv_failed(enc->encoder, err, __FUNCTION__, "nvEncLockBitstream");
			error("nvEncLockBitstream failed (err=%d) after %u queued frame(s) "
			      "(buf_count=%u): the post-reset pipeline is deeper than the "
			      "bitstream ring - lower the lookahead depth to make live "
			      "resize work",
			      (int)err, (unsigned)enc->buffers_queued, (unsigned)enc->buf_count);
			return false;
		}

		if (enc->first_packet) {
			NV_ENC_SEQUENCE_PARAM_PAYLOAD payload = {0};
			uint8_t buf[256];
			uint32_t size = 0;

			payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
			payload.spsppsBuffer = buf;
			payload.inBufferSize = sizeof(buf);
			payload.outSPSPPSPayloadSize = &size;

			NVENCSTATUS sps_err = nv.nvEncGetSequenceParams(s, &payload);
			nvenc_trace_sequence_params(enc, buf, size, sps_err);
			enc->header = bmemdup(buf, size);
			enc->header_size = size;
			enc->first_packet = false;
			info("first packet after reconfigure: pts=%lld size=%u pic_type=%d queued=%u",
			     (long long)lock.outputTimeStamp, lock.bitstreamSizeInBytes,
			     (int)lock.pictureType, (unsigned)enc->buffers_queued);
		}

		da_copy_array(enc->packet_data, lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);

		enc->packet_pts = (int64_t)lock.outputTimeStamp;
		enc->packet_keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR;

		if (enc->packet_keyframe) {
			enc->frames_since_idr = 0; /* resync GOP-boundary counter */
			/* Track the steady-state on-grid keyframe cadence for multitrack timed re-init.
			 * Only update when not mid staged resize/FPS re-init so an off-grid session-start
			 * IDR cannot corrupt the shared grid phase. */
			if (!enc->resize_pending && !enc->fps_pending) {
				const int64_t kpts = (int64_t)lock.outputTimeStamp;
				/* While a re-anchor IDR is pending, ignore the off-grid session-start IDRs
				 * so they cannot shift the shared grid anchor; tracking resumes at the
				 * on-grid re-anchor IDR itself. */
				if (!(enc->reanchor_idr_pts != 0 && kpts != enc->reanchor_idr_pts)) {
					if (enc->last_keyframe_pts >= 0) {
						int64_t delta = kpts - enc->last_keyframe_pts;
						if (delta > 0)
							enc->gop_pts = delta;
					}
					enc->last_keyframe_pts = kpts;
				}
				if (enc->reanchor_idr_pts != 0 && kpts == enc->reanchor_idr_pts) {
					info("off-grid re-init re-anchored at shared keyframe boundary pts=%lld",
					     (long long)kpts);
					enc->reanchor_idr_pts = 0;
				}
			}
			if (enc->reconfig_pending)
				info("observed keyframe pts=%lld while RC change staged",
				     (long long)lock.outputTimeStamp);
		}

		err = nv.nvEncUnlockBitstream(s, bs->ptr);
		if (NV_FAILED(err)) {
			error("nvEncUnlockBitstream failed (err=%d)", (int)err);
			return false;
		}

		/* ---------------- */
#ifdef _WIN32
		if (nvtex && nvtex->mapped_res) {
			NVENCSTATUS err;
			err = nv.nvEncUnmapInputResource(s, nvtex->mapped_res);
			if (nv_failed(enc->encoder, err, __FUNCTION__, "unmap")) {
				return false;
			}
			nvtex->mapped_res = NULL;
		}
#endif
		/* ---------------- */

		if (surf && surf->mapped_res) {
			NVENCSTATUS err;
			err = nv.nvEncUnmapInputResource(s, surf->mapped_res);
			if (nv_failed(enc->encoder, err, __FUNCTION__, "unmap")) {
				return false;
			}
			surf->mapped_res = NULL;
		}

		/* ---------------- */

		if (++enc->cur_bitstream == enc->buf_count)
			enc->cur_bitstream = 0;

		enc->buffers_queued--;
	}

	return true;
}

struct roi_params {
	uint32_t mb_width;
	uint32_t mb_height;
	uint32_t mb_size;
	bool av1;
	int8_t *map;
};

static void roi_cb(void *param, struct obs_encoder_roi *roi)
{
	const struct roi_params *rp = param;

	int8_t qp_val;
	/* AV1 has a larger QP range than HEVC/H.264 */
	if (rp->av1) {
		qp_val = (int8_t)(-128.0f * roi->priority);
	} else {
		qp_val = (int8_t)(-51.0f * roi->priority);
	}

	const uint32_t roi_left = roi->left / rp->mb_size;
	const uint32_t roi_top = roi->top / rp->mb_size;
	const uint32_t roi_right = (roi->right - 1) / rp->mb_size;
	const uint32_t roi_bottom = (roi->bottom - 1) / rp->mb_size;

	for (uint32_t mb_y = 0; mb_y < rp->mb_height; mb_y++) {
		if (mb_y < roi_top || mb_y > roi_bottom)
			continue;

		for (uint32_t mb_x = 0; mb_x < rp->mb_width; mb_x++) {
			if (mb_x < roi_left || mb_x > roi_right)
				continue;

			rp->map[mb_y * rp->mb_width + mb_x] = qp_val;
		}
	}
}

static void add_roi(struct nvenc_data *enc, NV_ENC_PIC_PARAMS *params)
{
	const uint32_t increment = obs_encoder_get_roi_increment(enc->encoder);

	if (enc->roi_map && enc->roi_increment == increment) {
		params->qpDeltaMap = enc->roi_map;
		params->qpDeltaMapSize = (uint32_t)enc->roi_map_size;
		return;
	}

	uint32_t mb_size = 0;
	switch (enc->codec) {
	case CODEC_H264:
		/* H.264 is always 16x16 */
		mb_size = 16;
		break;
	case CODEC_HEVC:
		/* HEVC can be 16x16, 32x32, or 64x64, but NVENC is always 32x32 */
		mb_size = 32;
		break;
	case CODEC_AV1:
		/* AV1 can be 64x64 or 128x128, but NVENC is always 64x64 */
		mb_size = 64;
		break;
	}

	const uint32_t mb_width = (enc->cx + mb_size - 1) / mb_size;
	const uint32_t mb_height = (enc->cy + mb_size - 1) / mb_size;
	const size_t map_size = mb_width * mb_height * sizeof(int8_t);

	if (map_size != enc->roi_map_size) {
		enc->roi_map = brealloc(enc->roi_map, map_size);
		enc->roi_map_size = map_size;
	}

	memset(enc->roi_map, 0, enc->roi_map_size);

	struct roi_params par = {
		.mb_width = mb_width,
		.mb_height = mb_height,
		.mb_size = mb_size,
		.av1 = enc->codec == CODEC_AV1,
		.map = enc->roi_map,
	};

	obs_encoder_enum_roi(enc->encoder, roi_cb, &par);

	enc->roi_increment = increment;
	params->qpDeltaMap = enc->roi_map;
	params->qpDeltaMapSize = (uint32_t)map_size;
}

bool nvenc_encode_base(struct nvenc_data *enc, struct nv_bitstream *bs, void *pic, int64_t pts,
		       struct encoder_packet *packet, bool *received_packet)
{
	/* NVENC bakes its SPS/VUI at create() time, before the delivery mix is bound to
	 * encoder->media, so the identity-RGB VUI override in init_encoder_* can miss RGB
	 * encodes (obs_encoder_video_tex_active() not yet true). By the first encode the
	 * texture format is known; if identity-RGB applies now but the SPS was generated
	 * without it, regenerate the SPS via reconfigure so the VUI matches the colr box
	 * the muxer writes (avoids conflicting MediaInfo *_Original tags). */
	if (!enc->vui_identity_checked) {
		enc->vui_identity_checked = true;
		if (!enc->non_texture && (enc->codec == CODEC_H264 || enc->codec == CODEC_HEVC)) {
			const bool identity_now = nvenc_identity_rgb_active(enc);
			if (identity_now && !enc->vui_identity_applied) {
				info("nvenc: identity-RGB active but SPS lacks RGB VUI; reconfiguring to "
				     "regenerate SPS with identity-RGB VUI");
				nvenc_apply_identity_vui(enc);
				if (apply_nvenc_reconfigure(enc))
					enc->vui_identity_applied = true;
			}
		}
	}


	/* Soft RC-parameter change (ABR quality aim): no reset, no IDR, so it can
	 * fire immediately on any frame - no keyframe alignment needed. */
	if (enc->tq_soft_pending) {
		info("applying soft quality reconfigure (reset=0 forceIDR=0) pts=%lld", (long long)pts);
		if (!apply_nvenc_reconfigure_soft(enc))
			warn("soft quality reconfigure failed; keeping previous target quality");
		enc->tq_soft_pending = false;
	}

	/* Count submissions since last observed IDR. get_encoded_packet() */
	/* only starts returning packets once buffers_queued reaches       */
	/* output_delay, so in steady state exactly output_delay - 1      */
	/* frames are in flight between a frame's submission and its      */
	/* packet observation (first lock happens on the same call that    */
	/* bumps the queue to output_delay). This counter therefore        */
	/* reaches gopLength - (output_delay - 1) precisely on the         */
	/* submission that will naturally be an IDR; firing there forces   */
	/* the new GOP to start exactly at the shared keyframe pts of     */
	/* sibling encoders (the multitrack muxer requires identical pts  */
	/* across all tracks).                                            */
	++enc->frames_since_idr;

	const int64_t pipeline_delay = (int64_t)enc->output_delay - 1; /* see get_encoded_packet() gate */
	const int64_t keyframe_boundary = (int64_t)enc->config.gopLength - pipeline_delay;

	/* Staged RC change firing at the shared keyframe boundary.
	 * Do NOT fire during the post-reset fill period (first_packet still
	 * true): the ring is filling and no output has been produced yet.
	 * A second reset would discard the in-flight frames and restart
	 * the fill, causing an infinite loop of resets with no output. */
	bool fire_now = false;

	if (!enc->first_packet) {
		if (enc->reconfig_pending && enc->frames_since_idr >= keyframe_boundary)
			fire_now = true;
	}

	if (fire_now) {
		info("applying staged encoder reconfigure at shared keyframe boundary "
		     "(rc=%d counter=%lld gopLength=%d output_delay=%d forced_frame_pts=%lld)",
		     enc->reconfig_pending, (long long)enc->frames_since_idr,
		     (int)enc->config.gopLength, enc->output_delay, (long long)pts);
		bool ok = apply_nvenc_reconfigure(enc);

		/* One reset+IDR at a shared tick re-anchors the GOP phase. */
		enc->reconfig_pending = false;
		if (ok) {
			enc->frames_since_idr = 0;
			/* The full re-init already carried any staged FPS params (updated at staging time),
			 * so drop the timed FPS re-init to avoid a redundant reset right after. */
			if (enc->fps_pending)
				info("staged RC reconfigure also applied the staged FPS change; dropping timed FPS re-init");
			enc->fps_pending = false;
		}
	}

	NV_ENC_PIC_PARAMS params = {0};
	params.version = NV_ENC_PIC_PARAMS_VER;
	params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	params.inputBuffer = pic;
	params.inputTimeStamp = (uint64_t)pts;
	params.inputWidth = enc->cx;
	params.inputHeight = enc->cy;
	params.inputPitch = enc->cx;
	params.outputBitstream = bs->ptr;
	params.frameIdx = (uint32_t)pts;

	if (enc->non_texture) {
		params.bufferFmt = enc->surface_format;
	} else {
		params.bufferFmt = obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_P010)
					   ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT
					   : NV_ENC_BUFFER_FORMAT_NV12;
	}

#ifdef NVENC_13_0_OR_LATER
	if (enc->cll) {
		if (enc->codec == CODEC_AV1)
			params.codecPicParams.av1PicParams.pMaxCll = enc->cll;
		else if (enc->codec == CODEC_HEVC)
			params.codecPicParams.hevcPicParams.pMaxCll = enc->cll;
	}
	if (enc->mdi) {
		if (enc->codec == CODEC_AV1)
			params.codecPicParams.av1PicParams.pMasteringDisplay = enc->mdi;
		else if (enc->codec == CODEC_HEVC)
			params.codecPicParams.hevcPicParams.pMasteringDisplay = enc->mdi;
	}
#endif

	/* Add ROI map if enabled */
	if (obs_encoder_has_roi(enc->encoder))
		add_roi(enc, &params);

	/* A consumer attached while this encoder was already encoding - a recording on the shared
	 * encoder, a fan-out destination started mid-session: libobs raised a keyframe request
	 * because that consumer's packet gate (send_first_video_packet) drops every packet until
	 * it has seen a keyframe, so without a fresh IDR it receives a whole GOP of undecodable
	 * stream while the consumers already attached keep playing.  forceIDR restarts the
	 * driver's idrPeriod from this frame, so a grouped encoder must not take one off-grid. */
	if (obs_encoder_keyframe_requested(enc->encoder)) {
		if (obs_encoder_is_in_group(enc->encoder)) {
			info("keyframe request dropped: grouped encoder keeps its shared keyframe grid");
		} else {
			params.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
			info("forcing IDR for a consumer that attached mid-session (pts=%lld)", (long long)pts);
		}
	}

	/* Forced IDR at a shared boundary to re-anchor an off-grid session (see
	 * nvenc_schedule_reanchor): the driver restarts its idrPeriod from any IDR, so one
	 * on-grid IDR restores keyframe alignment with the sibling tracks. */
	if (enc->reanchor_idr_pts != 0) {
		if (pts == enc->reanchor_idr_pts) {
			params.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
			info("forcing IDR at shared keyframe boundary pts=%lld to re-anchor the off-grid GOP",
			     (long long)pts);
		} else if (pts > enc->reanchor_idr_pts) {
			/* Boundary frame was dropped - advance to the next shared boundary. */
			warn("re-anchor boundary pts %lld missed (pts %lld), retrying at pts %lld",
			     (long long)enc->reanchor_idr_pts, (long long)pts,
			     (long long)(enc->reanchor_idr_pts += enc->gop_pts));
		}
	}

	nvenc_trace_pic(enc, &params, false);

	/* Overflow guard: if the ring is nearly full (all but one slot in-flight
	 * with no output yet), skip the submission to avoid wrapping next_bitstream
	 * onto cur_bitstream. Still attempt a read so NVENC can drain its pipeline
	 * and free a slot. This should not happen with the enlarged ring
	 * (buf_count sized at 4x pipeline depth). */
	bool submitted = true;
	if ((uint32_t)enc->buffers_queued >= enc->buf_count - 1) {
		static int64_t last_overflow_warn = 0;
		if (pts - last_overflow_warn > 2000000000LL) { /* ~2s at 1GHz timescale */
			warn("bitstream ring nearly full (%u/%u) with no output ready - "
			     "dropping frames until pipeline drains (pts=%lld)",
			     (unsigned)enc->buffers_queued, (unsigned)enc->buf_count, (long long)pts);
			last_overflow_warn = pts;
		}
		submitted = false;
		/* The backend already pushed this frame's PTS onto dts_list before
		 * calling us. Since we're not submitting, remove it to keep the
		 * list in sync with the ring. */
		int64_t dummy;
		if (enc->dts_list.size > 0)
			deque_pop_back(&enc->dts_list, &dummy, sizeof(dummy));
	}

	if (submitted) {
		NVENCSTATUS err = nv.nvEncEncodePicture(enc->session, &params);
		if (err != NV_ENC_SUCCESS && err != NV_ENC_ERR_NEED_MORE_INPUT) {
			nv_failed(enc->encoder, err, __FUNCTION__, "nvEncEncodePicture");
			return false;
		}

		enc->encode_started = true;
		enc->buffers_queued++;

		if (++enc->next_bitstream == enc->buf_count) {
			enc->next_bitstream = 0;
		}

#ifdef _WIN32
		/* Deferred free of old-size textures after a live resize.
		 * In-flight frames referenced the old textures via
		 * nvEncMapInputResource; after buf_count new frames are
		 * submitted, all old in-flight frames have completed. */
		if (enc->old_tex_frames_remaining > 0) {
			enc->old_tex_frames_remaining--;
			if (enc->old_tex_frames_remaining == 0 && enc->old_textures.num > 0) {
				debug("freeing %u old-size texture(s) after live resize",
				      (unsigned)enc->old_textures.num);
				for (size_t i = 0; i < enc->old_textures.num; i++) {
					struct nv_texture *ot = &enc->old_textures.array[i];
					if (ot->res) {
						if (ot->mapped_res)
							nv.nvEncUnmapInputResource(enc->session, ot->mapped_res);
						nv.nvEncUnregisterResource(enc->session, ot->res);
						if (ot->vp_view)
							ot->vp_view->lpVtbl->Release(ot->vp_view);
						ot->tex->lpVtbl->Release(ot->tex);
					}
				}
				da_free(enc->old_textures);
			}
		}
#endif
	}

	/* ------------------------------------ */
	/* check for encoded packet and parse   */

	if (!get_encoded_packet(enc, false)) {
		return false;
	}

	/* ------------------------------------ */
	/* output encoded packet                */

	if (enc->packet_data.num) {
		int64_t dts;
		deque_pop_front(&enc->dts_list, &dts, sizeof(dts));

		/* subtract bframe delay from dts for H.264/HEVC */
		if (enc->codec != CODEC_AV1)
			dts -= enc->props.bf * packet->timebase_num;

		*received_packet = true;
		packet->data = enc->packet_data.array;
		packet->size = enc->packet_data.num;
		packet->type = OBS_ENCODER_VIDEO;
		packet->pts = enc->packet_pts;
		packet->dts = dts;
		packet->keyframe = enc->packet_keyframe;

		/* ABR governor: feed the actual encoded size so it can adapt maxBitRate. */
		nvenc_abr_tick(enc, (uint32_t)enc->packet_data.num);
	} else {
		*received_packet = false;
	}

	return true;
}

static void nvenc_soft_video_info(void *data, struct video_scale_info *info)
{
	struct nvenc_data *enc = data;
	info->format = enc->in_format;
}

static void nvenc_tex_video_info(void *data, struct video_scale_info *info)
{
	UNUSED_PARAMETER(data);
	/* Override to GBRA for BGRA to enable RGB texture encoding without colour conversion. */
	if (info->format == VIDEO_FORMAT_BGRA) {
		info->format = VIDEO_FORMAT_GBRA;
		info->range = VIDEO_RANGE_FULL;
		info->colorspace = VIDEO_CS_SRGB;
	} else if (info->format == VIDEO_FORMAT_I444) {
		info->format = VIDEO_FORMAT_AYUV;
	} else if (info->format == VIDEO_FORMAT_Y410) {
		info->format = VIDEO_FORMAT_Y410;
	} else if (info->format == VIDEO_FORMAT_GBR10 || info->format == VIDEO_FORMAT_R10L) {
		info->format = VIDEO_FORMAT_GBR10;
		info->range = VIDEO_RANGE_FULL;
		/* Use SRGB for SDR */
		if (info->colorspace != VIDEO_CS_2100_PQ && info->colorspace != VIDEO_CS_2100_HLG)
			info->colorspace = VIDEO_CS_SRGB;
	} else {
		info->format = get_preferred_format(info->format);
	}
}

static bool nvenc_extra_data(void *data, uint8_t **header, size_t *size)
{
	struct nvenc_data *enc = data;

	if (!enc->header) {
		return false;
	}

	*header = enc->header;
	*size = enc->header_size;
	return true;
}

static bool nvenc_sei_data(void *data, uint8_t **sei, size_t *size)
{
	struct nvenc_data *enc = data;

	if (!enc->sei) {
		return false;
	}

	*sei = enc->sei;
	*size = enc->sei_size;
	return true;
}

/* NVENC AV1 hardware encode only supports YUV 4:2:0 (8-bit NV12 / 10-bit P010); it cannot take
 * 4:2:2, 4:4:4 or RGB inputs. Declare this so the shared libobs injection offers only those two
 * formats for AV1 instead of probing every texture-encodable format. */
static bool nvenc_av1_is_color_format_supported(void *type_data, enum video_format format)
{
	UNUSED_PARAMETER(type_data);
	return format == VIDEO_FORMAT_NV12 || format == VIDEO_FORMAT_P010;
}

/* NVENC H.264 hardware encode is 8-bit only; it cannot perform a 10/16-bit encode, so never offer
 * those formats. The texture probe would otherwise list P010 (get_preferred_format(P010) == P010 is
 * an identity "lossless" repack), which then fails at start time with "Cannot perform 10-bit encode".
 * Offer the same 8-bit set the probe reports. */
static bool nvenc_h264_is_color_format_supported(void *type_data, enum video_format format)
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

struct obs_encoder_info h264_nvenc_info = {
	.id = "obs_nvenc_h264_tex",
	.codec = "h264",
	.type = OBS_ENCODER_VIDEO,
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_ROI,
	.get_name = h264_nvenc_get_name,
	.create = h264_nvenc_create,
	.destroy = nvenc_destroy,
	.update = nvenc_update,
#ifdef _WIN32
	.encode_texture2 = d3d11_encode,
#else
	.encode_texture2 = cuda_opengl_encode,
#endif
	.get_defaults = h264_nvenc_defaults,
	.get_properties = h264_nvenc_properties,
	.get_extra_data = nvenc_extra_data,
	.get_sei_data = nvenc_sei_data,
	.get_video_info = nvenc_tex_video_info,
	.is_color_format_supported = nvenc_h264_is_color_format_supported,
};

#ifdef ENABLE_HEVC
struct obs_encoder_info hevc_nvenc_info = {
	.id = "obs_nvenc_hevc_tex",
	.codec = "hevc",
	.type = OBS_ENCODER_VIDEO,
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_ROI,
	.get_name = hevc_nvenc_get_name,
	.create = hevc_nvenc_create,
	.destroy = nvenc_destroy,
	.update = nvenc_update,
#ifdef _WIN32
	.encode_texture2 = d3d11_encode,
#else
	.encode_texture2 = cuda_opengl_encode,
#endif
	.get_defaults = hevc_nvenc_defaults,
	.get_properties = hevc_nvenc_properties,
	.get_extra_data = nvenc_extra_data,
	.get_sei_data = nvenc_sei_data,
	.get_video_info = nvenc_tex_video_info,
};
#endif

struct obs_encoder_info av1_nvenc_info = {
	.id = "obs_nvenc_av1_tex",
	.codec = "av1",
	.type = OBS_ENCODER_VIDEO,
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_ROI,
	.get_name = av1_nvenc_get_name,
	.create = av1_nvenc_create,
	.destroy = nvenc_destroy,
	.update = nvenc_update,
#ifdef _WIN32
	.encode_texture2 = d3d11_encode,
#else
	.encode_texture2 = cuda_opengl_encode,
#endif
	.get_defaults = av1_nvenc_defaults,
	.get_properties = av1_nvenc_properties,
	.get_extra_data = nvenc_extra_data,
	.get_video_info = nvenc_tex_video_info,
	.is_color_format_supported = nvenc_av1_is_color_format_supported,
};

struct obs_encoder_info h264_nvenc_soft_info = {
	.id = "obs_nvenc_h264_soft",
	.codec = "h264",
	.type = OBS_ENCODER_VIDEO,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_ROI | OBS_ENCODER_CAP_INTERNAL,
	.get_name = h264_nvenc_soft_get_name,
	.create = h264_nvenc_soft_create,
	.destroy = nvenc_destroy,
	.update = nvenc_update,
	.encode = cuda_encode,
	.get_defaults = h264_nvenc_defaults,
	.get_properties = h264_nvenc_properties,
	.get_extra_data = nvenc_extra_data,
	.get_sei_data = nvenc_sei_data,
	.get_video_info = nvenc_soft_video_info,
};

#ifdef ENABLE_HEVC
struct obs_encoder_info hevc_nvenc_soft_info = {
	.id = "obs_nvenc_hevc_soft",
	.codec = "hevc",
	.type = OBS_ENCODER_VIDEO,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_ROI | OBS_ENCODER_CAP_INTERNAL,
	.get_name = hevc_nvenc_soft_get_name,
	.create = hevc_nvenc_soft_create,
	.destroy = nvenc_destroy,
	.update = nvenc_update,
	.encode = cuda_encode,
	.get_defaults = hevc_nvenc_defaults,
	.get_properties = hevc_nvenc_properties,
	.get_extra_data = nvenc_extra_data,
	.get_sei_data = nvenc_sei_data,
	.get_video_info = nvenc_soft_video_info,
};
#endif

struct obs_encoder_info av1_nvenc_soft_info = {
	.id = "obs_nvenc_av1_soft",
	.codec = "av1",
	.type = OBS_ENCODER_VIDEO,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_ROI | OBS_ENCODER_CAP_INTERNAL,
	.get_name = av1_nvenc_soft_get_name,
	.create = av1_nvenc_soft_create,
	.destroy = nvenc_destroy,
	.update = nvenc_update,
	.encode = cuda_encode,
	.get_defaults = av1_nvenc_defaults,
	.get_properties = av1_nvenc_properties,
	.get_extra_data = nvenc_extra_data,
	.get_video_info = nvenc_soft_video_info,
	.is_color_format_supported = nvenc_av1_is_color_format_supported,
};

void register_encoders(void)
{
	obs_register_encoder(&h264_nvenc_info);
	obs_register_encoder(&h264_nvenc_soft_info);
#ifdef ENABLE_HEVC
	obs_register_encoder(&hevc_nvenc_info);
	obs_register_encoder(&hevc_nvenc_soft_info);
#endif
	if (is_codec_supported(CODEC_AV1)) {
		obs_register_encoder(&av1_nvenc_info);
		obs_register_encoder(&av1_nvenc_soft_info);
	}
}
