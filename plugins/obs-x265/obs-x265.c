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
#include <abr-gov.h>
#include <util/threading.h>

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

/* One encoded access unit captured while flushing a context before re-opening it (see
 * obs_x265_flush_pending). Buffered so the in-flight frames can be re-emitted, in DTS order,
 * across subsequent encode calls instead of being dropped by encoder_close(). */
struct obs_x265_queued_pkt {
	uint8_t *data; /* owned copy of the concatenated NALs */
	size_t   size;
	int64_t  pts;
	int64_t  dts;
	bool     keyframe;
};

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

	/* ABR runtime governor (active only for rate_control=ABR with max_bitrate > 0, latched at
	 * open by x265_abr_prepare_open - see the governor section above). The whole control law,
	 * tick cadence and stream-session budget live in the shared abr-gov module; abr_state is the
	 * per-open latch of that state (cur_rate in bits/sec at the shared boundary) and survives
	 * stop/start via abr_gov_session_load/save. */
	bool abr_active;
	int64_t abr_target_kbps; /* settings "bitrate" (kbps) */
	int64_t abr_max_kbps;    /* settings "max_bitrate" (kbps), governor ceiling */
	double  abr_deadband;    /* settings "deadband" as a fraction (0.005 = +/-0.5%) */
	abr_gov_state abr_state; /* session budget + current governed rate (bps) */

	/* Live resolution change (multitrack) support - mirrors obs-x264/obs-nvenc. The size the
	 * x265 context was last opened with, plus the plane-0 stride of frames at that size
	 * (race guard: the mix rebind takes effect on the next tick). A resize is applied by
	 * re-opening the context immediately when it is detected: in-flight frames are flushed
	 * and re-emitted (see flush_queue) so no frame is dropped and DTS stays continuous. The
	 * re-open emits an off-grid IDR; a second re-open on the next shared keyframe boundary
	 * (align_pending) re-locks this track onto its siblings' keyframe grid. */
	uint32_t inited_width;
	uint32_t inited_height;
	uint32_t inited_ls0;
	int      resize_stall;

	/* In-flight frames captured by flushing the old context at a re-open (live resize /
	 * realignment / FPS change), re-emitted in DTS order across subsequent encode calls so no
	 * frames are dropped and no DTS gap appears. Empty in steady state. */
	DARRAY(struct obs_x265_queued_pkt) flush_queue;

	/* Multitrack keyframe-grid realignment after a live resize (PTS-based, mirrors obs-x264).
	 * The off-grid resize IDR re-anchors this track mid-GOP; we stage a second context re-open
	 * exactly on the next shared boundary. We target it by PTS (not frame counting): 
	 * last_keyframe_pts/gop_pts track the steady-state on-grid keyframe cadence, and target_pts
	 * is the next shared boundary to re-open on. x265 cannot reliably force an IDR mid-GOP either,
	 * so the realignment re-opens (a freshly opened context always emits a real IDR first). */
	bool     align_pending;
	int64_t  target_pts;        /* next shared keyframe pts to re-open on (0 = not staged) */
	int64_t  last_keyframe_pts; /* most recent steady-state on-grid keyframe pts (-1 = none yet) */
	int64_t  gop_pts;           /* measured keyframe interval in pts units */

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
/* ABR runtime governor                                                      */
/*                                                                           */
/* With rate_control=ABR and max_bitrate > 0, ABR is upgraded in place: the  */
/* context opens with a CRF+VBV base (constant quality point + VBV ceiling)  */
/* and x265_abr_tick() steers rc.vbvMaxBitrate/rc.vbvBufferSize at runtime   */
/* within [target, max] so the stream's cumulative average converges toward  */
/* the target: complex content spends up to the ceiling, easy content stays  */
/* low. This requires live RC reconfiguration in libx265 (the obs-deps       */
/* x265-live-rc-reconfig patch keeps Encoder::m_param the single live param  */
/* block, so mid-stream reconfigs reach RateControl).                        */
/*                                                                           */
/* The cumulative bits/time counters are a stream-session quantity, not an   */
/* encoder-context quantity: libobs destroys the plugin context on every     */
/* stop (obs_encoder_stop -> info.destroy) and recreates it on the next      */
/* start, so the budget lives in a plugin-static registry keyed by the       */
/* stable obs_encoder_t. Whether the previous session's budget is kept or    */
/* reset is decided once at open time from how the previous stop ended (the   */
/* stop reason libobs stamps onto the encoder, see obs-output.c):            */
/*                                                                           */
/*   graceful stop / encode error / first start  -> fresh budget             */
/*   ungraceful kill                             -> keep                     */
/*   disconnect within 60 s                      -> keep                     */
/*   disconnect longer than 60 s                 -> fresh                    */
/* ------------------------------------------------------------------------- */

/* +/-0.5% band around target before the governor corrects (was +/-3%). Tighter, so the cumulative
 * average is held much closer to target - the ceiling reacts as soon as it drifts more than 0.5%. */
#define X265_ABR_DEADBAND_FRAC 0.005

/* Decide the session budget for this open (fresh or restored) and latch the per-context state.
 * Must run before update_settings() in create(), so the context opens at the correct governed
 * ceiling. No-op unless rate_control=ABR with max_bitrate > 0. */
static void x265_abr_prepare_open(struct obs_x265 *obsx265, obs_data_t *settings)
{
	obsx265->abr_active = false;

	const char *rc_str = obs_data_get_string(settings, "rate_control");
	int target = (int)obs_data_get_int(settings, "bitrate");
	int maxbr = (int)obs_data_get_int(settings, "max_bitrate");

	if (strcmp(rc_str, "ABR") != 0 || target <= 0 || maxbr <= 0)
		return;

	if (maxbr < target)
		maxbr = target; /* a ceiling below the target gives no headroom; clamp it */

	/* The decision law, tick cadence and session persistence live in the shared abr-gov module.
	 * The governor works in bits/sec at the shared boundary; x265's settings/params are kbps, so
	 * convert here. Fresh sessions start at target (headroom is earned); a killed or briefly
	 * disconnected previous session restores its cumulative budget. */
	abr_gov_config cfg = abr_gov_default_config();
	abr_gov_session_load(obsx265->encoder, true, (int64_t)target * 1000, (int64_t)maxbr * 1000, &cfg,
	                     &obsx265->abr_state);

	info("x265 ABR governor: session budget ready, target %d kbps, ceiling %d kbps, governed rate -> %lld kbps",
	     target, maxbr, (long long)(obsx265->abr_state.cur_rate / 1000));

	obsx265->abr_target_kbps = target;
	obsx265->abr_max_kbps = maxbr;
	obsx265->abr_deadband = X265_ABR_DEADBAND_FRAC; /* fallback; update_settings latches the setting */
	obsx265->abr_active = true;
}

/* Write the session budget back to the shared registry (called from destroy()). */
static void x265_abr_save_session(struct obs_x265 *obsx265)
{
	if (!obsx265->abr_active || !obsx265->encoder)
		return;

	abr_gov_session_save(obsx265->encoder, true, &obsx265->abr_state);
}

/* Re-latch target/ceiling on a mid-stream settings update (the update() path). The budget
 * itself is never reset here: it is a whole-stream quantity. */
static void x265_abr_re_latch(struct obs_x265 *obsx265, int target_kbps, int max_kbps)
{
	if (!obsx265->abr_active)
		return;

	if (max_kbps < target_kbps)
		max_kbps = target_kbps;
	obsx265->abr_target_kbps = target_kbps;
	obsx265->abr_max_kbps = max_kbps;

	const abr_gov_config cfg = abr_gov_default_config();
	const int64_t floor_bps = (int64_t)((double)target_kbps * 1000.0 * cfg.overshoot_floor_frac);
	const int64_t ceiling_bps = (int64_t)max_kbps * 1000;
	if (obsx265->abr_state.cur_rate < floor_bps)
		obsx265->abr_state.cur_rate = floor_bps;
	if (obsx265->abr_state.cur_rate > ceiling_bps)
		obsx265->abr_state.cur_rate = ceiling_bps;
}

/* One governor tick: feed a coded packet and, if the cumulative average has drifted outside
 * the deadband, steer the governed ceiling. Accumulation, cadence (0.5 s), the history gate and the
 * decision law all live in the shared abr-gov module; x265 only computes dt, converts kbps<->bps at
 * the boundary, and applies the result via encoder_reconfig. */
static void x265_abr_tick(struct obs_x265 *obsx265, size_t packet_size)
{
	if (!obsx265->abr_active || !obsx265->context)
		return;

	double eff_fps = obs_encoder_get_effective_fps(obsx265->encoder);
	if (eff_fps <= 0.0)
		return;
	const double dt = 1.0 / eff_fps;

	const int64_t target_bps = obsx265->abr_target_kbps * 1000;
	const int64_t ceiling_bps = obsx265->abr_max_kbps * 1000;

	abr_gov_config cfg = abr_gov_default_config();
	cfg.deadband_frac = obsx265->abr_deadband;

	const int64_t old_rate_bps = obsx265->abr_state.cur_rate;
	double cum_avg_bps = 0.0;
	const bool changed =
	    abr_gov_tick(&obsx265->abr_state, &cfg, target_bps, ceiling_bps, dt, (uint32_t)packet_size, &cum_avg_bps);

	if (cum_avg_bps > 0.0) {
		blog(LOG_DEBUG, "x265 ABR governor: cumulative avg %.0f kbps (target %lld, governed ceiling %lld, t=%.1fs)",
		     cum_avg_bps / 1000.0, (long long)obsx265->abr_target_kbps,
		     (long long)(obsx265->abr_state.cur_rate / 1000), obsx265->abr_state.total_time);
	}

	if (!changed)
		return;

	const int new_kbps = (int)(obsx265->abr_state.cur_rate / 1000);

	/* Apply: full copy of the current params with only the VBV ceiling/buffer changed. x265 flags an RC
	 * reconfig and RateControl::reconfigureRC() re-derives its VBV state from these values at the next frame
	 * boundary.
	 *
	 * HARD DEPENDENCY: this only works because the obs-deps "x265-live-rc-reconfig" patch is present in
	 * libx265 - it keeps Encoder::m_param the single live param block so mid-stream reconfigs actually reach
	 * RateControl. Against a stock/unpatched libx265 those reconfigs never reach RateControl, so the governed
	 * ceiling would not take effect and the stream simply runs at its open-time VBV ceiling (the governor is a
	 * no-op). The failure path below (revert + retry next tick) keeps any rejection graceful. */
	x265_param newp = obsx265->params;
	newp.rc.vbvMaxBitrate = new_kbps;
	newp.rc.vbvBufferSize = new_kbps;

	int ret = obsx265->api->encoder_reconfig(obsx265->context, &newp);
	if (ret < 0) {
		warn("x265 ABR governor: reconfig to %d kbps failed (%d), keeping %lld kbps", new_kbps, ret,
		     (long long)(old_rate_bps / 1000));
		obsx265->abr_state.cur_rate = old_rate_bps; /* codec kept the previous ceiling; retry next tick */
		return;
	}

	/* Keep the stored params in sync so a later user-triggered reconfig does not snap back to the open-time
	 * ceiling. ret == 1 means a same-class reconfig is still pending - the value lands at the next frame
	 * boundary and the next tick resends if it has moved on. */
	obsx265->params.rc.vbvMaxBitrate = new_kbps;
	obsx265->params.rc.vbvBufferSize = new_kbps;

	blog(LOG_DEBUG, "x265 ABR governor: ceiling %lld -> %d kbps (cumulative avg %.0f kbps, reconfig ret=%d)",
	     (long long)(old_rate_bps / 1000), new_kbps, cum_avg_bps / 1000.0, ret);
}

/* End of x265_abr_tick */



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

/* Copy one flushed access unit into the re-emit queue. */
static void obs_x265_flush_enqueue(struct obs_x265 *obsx265, x265_nal *nals, int nal_count, const x265_picture *pic_out)
{
	size_t total = 0;
	for (int i = 0; i < nal_count; i++)
		total += nals[i].sizeBytes;

	struct obs_x265_queued_pkt *pkt = da_push_back_new(obsx265->flush_queue);
	pkt->data = bmalloc(total);
	uint8_t *dst = pkt->data;
	for (int i = 0; i < nal_count; i++) {
		memcpy(dst, nals[i].payload, nals[i].sizeBytes);
		dst += nals[i].sizeBytes;
	}
	pkt->size     = total;
	pkt->pts      = pic_out->pts;
	pkt->dts      = pic_out->dts;
	pkt->keyframe = IS_X265_TYPE_I(pic_out->sliceType);
}

/* Free any access units still buffered in the re-emit queue (called on stop/destroy). */
static void obs_x265_flush_queue_free(struct obs_x265 *obsx265)
{
	for (size_t i = 0; i < obsx265->flush_queue.num; i++)
		bfree(obsx265->flush_queue.array[i].data);
	da_resize(obsx265->flush_queue, 0);
}

/* Drain every in-flight frame the current context still holds into the re-emit queue so it can be
 * re-emitted (with correct timestamps) after the context is re-opened. A plain encoder_close()
 * discards these frames and mis-stamps the tail, producing a DTS gap and undecodable output at a
 * live resize; flushing finalizes their decode/display order correctly. */
static void obs_x265_flush_pending(struct obs_x265 *obsx265)
{
	x265_nal *nals;
	uint32_t nal_count = 0;
	x265_picture pic_out;

	for (int guard = 0; guard < 4096; guard++) {
		memset(&pic_out, 0, sizeof(pic_out));
		if (obsx265->api->encoder_encode(obsx265->context, &nals, &nal_count, NULL, &pic_out) < 0 || nal_count <= 0)
			break;
		obs_x265_flush_enqueue(obsx265, nals, nal_count, &pic_out);
	}
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
	obs_x265_flush_queue_free(obsx265);
}

static void obs_x265_destroy(void *data)
{
	struct obs_x265 *obsx265 = data;

	if (obsx265) {
		x265_abr_save_session(obsx265);
		os_end_high_performance(obsx265->performance_token);
		clear_data(obsx265);
		da_free(obsx265->packet_data);
		bfree(obsx265);
	}
}

static void obs_x265_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", 2500);
	/* Max bitrate (ABR only): VBV peak cap above the target bitrate. 0 = keep capping at the
	 * target bitrate (historical ABR behavior). */
	obs_data_set_default_int(settings, "max_bitrate", 0);
	/* ABR governor tolerance: how far (in %) the cumulative average may drift from target before
	 * the governor corrects it. Only used when the governor is active (ABR + max_bitrate > 0). */
	obs_data_set_default_double(settings, "deadband", X265_ABR_DEADBAND_FRAC * 100.0);
	/* QCompress (classic ABR only): quantizer curve compression, valid range 0.5-1.0; 0.6 is
	 * the encoder default. Shown for classic ABR by rate_control_modified / max_bitrate_modified. */
	obs_data_set_default_double(settings, "qcomp", 0.6);
	obs_data_set_default_bool(settings, "use_bufsize", false);
	obs_data_set_default_int(settings, "buffer_size", 2500);
	obs_data_set_default_int(settings, "keyint_sec", 0);
	obs_data_set_default_int(settings, "crf", 17);
	obs_data_set_default_string(settings, "rate_control", "CRF");

	obs_data_set_default_string(settings, "preset", "veryfast");
	obs_data_set_default_string(settings, "profile", "");
	obs_data_set_default_string(settings, "tune", "");
	obs_data_set_default_string(settings, "x265opts", "");
	/* Repeat VPS/SPS/PPS before every IDR (keyframe). This is required for live resolution
	 * changes (multitrack): on re-open x265 emits a fresh IDR at the new size, and strict
	 * players (VLC / MPC-HC) latch the new parameter sets only when they are present inline
	 * in that IDR's access unit - a mid-stream HVCC sequence-start tag alone is not always
	 * honoured. The overhead is a few hundred bytes per keyframe. x264 repeats SPS/PPS inline
	 * by default, which is why H.264 live resizes play back cleanly while HEVC does not. */
	obs_data_set_default_bool(settings, "repeat_headers", true);

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
#define TEXT_MAX_BITRATE obs_module_text("MaxBitrate")
#define TEXT_DEADBAND obs_module_text("Deadband")
#define TEXT_QCOMP obs_module_text("QCompress")
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
	bool rc_quality = strcmp(rc, "CRF") == 0 || strcmp(rc, "CQP") == 0;

	p = obs_properties_get(ppts, "buffer_size");
	obs_property_set_visible(p, use_bufsize && !rc_quality);
	return true;
}

/* Toggling the max bitrate on/off switches ABR between the governor (CRF+VBV) and classic
 * ABR, which changes whether CRF or QCompress is the applicable quality knob. Return true
 * only when a visibility actually changed: returning true makes the properties view tear
 * down and rebuild every widget (losing keyboard focus), so plain value edits must not
 * trigger it. */
static bool max_bitrate_modified(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	if (strcmp(rc, "ABR") != 0)
		return false;

	bool gov = obs_data_get_int(settings, "max_bitrate") > 0;
	bool changed = false;

	p = obs_properties_get(ppts, "crf");
	if (obs_property_visible(p) != gov) {
		obs_property_set_visible(p, gov);
		changed = true;
	}
	p = obs_properties_get(ppts, "qcomp");
	if (obs_property_visible(p) != !gov) {
		obs_property_set_visible(p, !gov);
		changed = true;
	}
	p = obs_properties_get(ppts, "deadband");
	if (obs_property_visible(p) != gov) {
		obs_property_set_visible(p, gov);
		changed = true;
	}
	return changed;
}

static bool rate_control_modified(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool use_bufsize = obs_data_get_bool(settings, "use_bufsize");
	bool abr = strcmp(rc, "CBR") == 0 || strcmp(rc, "ABR") == 0;
	bool rc_quality = strcmp(rc, "CRF") == 0 || strcmp(rc, "CQP") == 0;
	/* Governor (ABR + max bitrate > 0): CRF+VBV base, so the quality knob is CRF and
	 * QCompress (an ABR-curve parameter) does not apply. Classic ABR keeps QCompress. */
	bool gov = strcmp(rc, "ABR") == 0 && obs_data_get_int(settings, "max_bitrate") > 0;

	p = obs_properties_get(ppts, "crf");
	obs_property_set_visible(p, !abr || gov);

	p = obs_properties_get(ppts, "bitrate");
	obs_property_set_visible(p, !rc_quality);
	p = obs_properties_get(ppts, "max_bitrate");
	obs_property_set_visible(p, strcmp(rc, "ABR") == 0);
	p = obs_properties_get(ppts, "deadband");
	obs_property_set_visible(p, gov);
	p = obs_properties_get(ppts, "qcomp");
	obs_property_set_visible(p, strcmp(rc, "ABR") == 0 && !gov);
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

	/* Max bitrate (ABR only): VBV peak cap above the target bitrate; shown for ABR by
	 * rate_control_modified. 0 = keep capping at the target bitrate. */
	p = obs_properties_add_int(props, "max_bitrate", TEXT_MAX_BITRATE, 0, 10000000, 50);
	obs_property_int_set_suffix(p, " Kbps");
	obs_property_set_long_description(p,
			"Peak bitrate ceiling for ABR (0 = off, classic ABR). When set, ABR runs with a CRF "
			"quality base and a runtime governor that raises the ceiling above the target whenever "
			"the stream's cumulative average is below target, and pulls it back when above: complex "
			"scenes spend up to the ceiling, easy scenes stay low, and the long-term average converges "
			"toward the bitrate. The CRF field sets the base quality point of this mode.");
	obs_property_set_modified_callback(p, max_bitrate_modified);
	obs_property_set_visible(p, false);

	/* Deadband / bitrate window (governor only): how far the cumulative average may drift from
	 * the target before the governor corrects it. Shown when ABR + max_bitrate > 0 by
	 * rate_control_modified / max_bitrate_modified. Stored as a percent; the tick uses it /100. */
	p = obs_properties_add_float_slider(props, "deadband", TEXT_DEADBAND, 0.1, 10.0, 0.1);
	obs_property_set_long_description(p,
			"Governor tolerance: how far (in percent) the stream's cumulative average bitrate may "
			"drift from the target before the governor pulls the ceiling down (above) or raises it "
			"(below). Smaller = tighter control and a whole-file average held closer to the target, "
			"at the cost of more frequent reconfigs; larger = looser, fewer adjustments.");
	obs_property_set_visible(p, false);

	/* QCompress (classic ABR only): quantizer curve compression; hidden when the governor
	 * (max bitrate > 0) is active because that mode runs CRF+VBV. */
	p = obs_properties_add_float_slider(props, "qcomp", TEXT_QCOMP, 0.5, 1.0, 0.05);
	obs_property_set_long_description(p,
					"The valid range is 0.5 to 1.0. The closer you get to 1.0, the more the encoder favors a constant "
					"quantization level. This can lead to greater bitrate variations between simple and complex scenes.");
	obs_property_set_visible(p, false);

	p = obs_properties_add_bool(props, "use_bufsize", TEXT_CUSTOM_BUF);
	obs_property_set_modified_callback(p, use_bufsize_modified);
	obs_properties_add_int(props, "buffer_size", TEXT_BUF_SIZE, 0, 10000000, 1);

	p = obs_properties_add_int(props, "crf", TEXT_CRF, 0, 51, 1);
	obs_property_set_long_description(p,
			"Constant rate factor (quality): lower values give better quality. In ABR mode with a max "
			"bitrate set, this is the base quality point of the governed encoder - easy scenes encode near "
			"it at low bitrate, complex scenes are clamped by the runtime ceiling.");

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
	int max_bitrate = (int)obs_data_get_int(settings, "max_bitrate");
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
	if (strcmp(rate_control, "ABR") == 0) {
		rc = RATE_CONTROL_ABR;
		/* crf is the governor's quality base (CRF+VBV); classic ABR ignores it. */
		if (!obsx265->abr_active)
			crf = 0;

	} else if (strcmp(rate_control, "VBR") == 0) {
		rc = RATE_CONTROL_VBR;

	} else if (strcmp(rate_control, "CRF") == 0) {
		rc = RATE_CONTROL_CRF;
		bitrate = 0;
		buffer_size = 0;

	} else if (strcmp(rate_control, "CQP") == 0) {
		rc = RATE_CONTROL_CQP;
		bitrate = 0;
		buffer_size = 0;

	} else { /* CBR */
		rc = RATE_CONTROL_CBR;
		crf = 0;
	}

	if (keyint_sec)
		obsx265->params.keyframeMax = keyint_sec * voi->fps_num / voi->fps_den;

	/* ABR VBV wiring. With the runtime governor active (ABR + max_bitrate > 0, latched at open
	 * in x265_abr_prepare_open), the VBV ceiling is governed at runtime and must not be
	 * re-derived from settings here; mid-stream target/ceiling changes are re-latched instead.
	 * Without it, an explicit max bitrate raises the fixed VBV peak cap above the target; 0
	 * keeps the historical behavior of capping at the target. Other modes are unaffected. */
	if (rc == RATE_CONTROL_ABR && max_bitrate > 0 && obsx265->abr_active) {
		x265_abr_re_latch(obsx265, bitrate, max_bitrate);
		double deadband = obs_data_get_double(settings, "deadband");
		if (deadband < 0.1)
			deadband = 0.1; /* avoid a zero band that would reconfig on every tick */
		else if (deadband > 10.0)
			deadband = 10.0;
		obsx265->abr_deadband = deadband / 100.0;
		obsx265->params.rc.vbvMaxBitrate = (int)(obsx265->abr_state.cur_rate / 1000);
		obsx265->params.rc.vbvBufferSize = (int)(obsx265->abr_state.cur_rate / 1000);
	} else {
		int vbv_max = (rc == RATE_CONTROL_ABR && max_bitrate > 0) ? max_bitrate : bitrate;

		if (!use_bufsize)
			buffer_size = vbv_max;

		obsx265->params.rc.vbvMaxBitrate = vbv_max;
		obsx265->params.rc.vbvBufferSize = buffer_size;
	}
	obsx265->params.rc.bitrate = bitrate;
	/* QCompress: quantizer curve compression for ABR (ignored by the other rate-control modes). */
	obsx265->params.rc.qCompress = obs_data_get_double(settings, "qcomp");
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

	/* Strict CBR: filler padding that keeps the output exactly at the target rate - the analog
	 * of x264's b_filler for CBR. Only valid with the CBR configuration below (x265 logs a
	 * warning and ignores it otherwise); set explicitly on every update so a live switch out of
	 * CBR clears it instead of leaving it stale in the reconfig'd params. */
	obsx265->params.rc.bStrictCbr = (rc == RATE_CONTROL_CBR);

	/* Rate control - mirror obs-x264: CBR/ABR use the ABR method, VBR/CRF use the
	 * constant-ratefactor method (VBR keeps a VBV cap, CRF has none), and CQP uses
	 * constant QP. */
	if (rc == RATE_CONTROL_CBR || rc == RATE_CONTROL_ABR) {
		obsx265->params.rc.rateControlMode = X265_RC_ABR;

		if (rc == RATE_CONTROL_CBR) {
			/* CBR: pin the VBV max rate and buffer to the target bitrate. That combination is
			 * what makes x265 run its internal CBR path (ABR + VBV with maxrate <= bitrate);
			 * together with bStrictCbr above it gives strict constant-rate output. */
			obsx265->params.rc.vbvMaxBitrate = bitrate;
			obsx265->params.rc.vbvBufferSize = bitrate;
		} else if (obsx265->abr_active) {
			/* Governor base: constant-ratefactor with a runtime-steered VBV ceiling (see
			 * x265_abr_tick). The CRF setting is the quality base point: easy content encodes
			 * near that QP at low bitrate, complex content is clamped by the governed ceiling. */
			obsx265->params.rc.rateControlMode = X265_RC_CRF;
			obsx265->params.rc.rfConstant = (float)crf;
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
		     "\tmax bitrate:  %d\n"
		     "\tbuffer size:  %d\n"
		     "\tcrf:          %d\n"
		     "\tFPS:          %g (%u/%u)\n"
		     "\twidth:        %d\n"
		     "\theight:       %d\n"
		     "\tkeyint:       %d\n"
		     "\tcolor format: %d\n"
		     "\tbit depth:    %u\n",
		     rate_control, bitrate, obsx265->abr_active ? max_bitrate : obsx265->params.rc.vbvMaxBitrate, buffer_size, crf,
		     obs_encoder_get_effective_fps(obsx265->encoder),
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

	/* ABR governor: decide the session budget (fresh vs restored from a previous session of
	 * this encoder) before the params are built, so the context opens at the correct governed
	 * ceiling. No-op unless rate_control=ABR with max_bitrate > 0. */
	x265_abr_prepare_open(obsx265, settings);

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

			/* Record the size the context was opened with so a later live resolution
			 * change can be detected against it (see obs_x265_maybe_resize). */
			obsx265->inited_width  = (uint32_t)obsx265->params.sourceWidth;
			obsx265->inited_height = (uint32_t)obsx265->params.sourceHeight;

			/* No keyframe cadence observed yet (see the realignment tracking in
			 * obs_x265_encode). */
			obsx265->last_keyframe_pts = -1;
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
 * steady-state frames cost one pass and no allocation. width/height MUST match the actual
 * dimensions of the frame being de-interleaved (during the one-tick mix rebind race after a
 * live resize that is the OLD session size, not the encoder's current target size). */
static uint8_t *deinterleave_gbra(struct obs_x265 *obsx265, struct encoder_frame *frame, uint32_t width,
				  uint32_t height, int *stride_out)
{
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
		 * YUV 4:4:4 with an identity matrix (see deinterleave_gbra). During the one-tick mix
		 * rebind race after a live resize the frame still carries the OLD size; detect it via
		 * the packed stride so the de-interleave reads the correct dimensions. */
		uint32_t width, height;
		if ((uint32_t)frame->linesize[0] == obsx265->inited_width * 4u) {
			width = obsx265->inited_width;
			height = obsx265->inited_height;
		} else {
			width = obs_encoder_get_width(obsx265->encoder);
			height = obs_encoder_get_height(obsx265->encoder);
		}
		const size_t plane_size = (size_t)width * height;
		int stride;
		uint8_t *base = deinterleave_gbra(obsx265, frame, width, height, &stride);

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
/* Close and re-open the x265 context with the current params. A freshly opened context emits
 * VPS/SPS/PPS + IDR as its first frame, so any live change that requires a re-init (FPS, size)
 * lands exactly on an IDR. Returns false if the re-open failed (context left NULL). */
static bool obs_x265_reopen_context(struct obs_x265 *obsx265)
{
	info("x265 reopen: requesting %dx%d (keyframeMax=%d, fps=%u/%u)",
	     obsx265->params.sourceWidth, obsx265->params.sourceHeight,
	     obsx265->params.keyframeMax, obsx265->params.fpsNum, obsx265->params.fpsDenom);

	/* Flush in-flight frames into the re-emit queue BEFORE closing so they are re-emitted (with
	 * correct timestamps) after the re-open instead of being dropped by encoder_close(), which
	 * would leave a DTS gap and mis-stamped tail frames that make continuous decoding fail. */
	if (obsx265->context) {
		obs_x265_flush_pending(obsx265);
		info("x265 reopen: flushed %zu in-flight frame(s) into re-emit queue", obsx265->flush_queue.num);
	}

	obsx265->api->encoder_close(obsx265->context);
	obsx265->context = NULL;
	bfree(obsx265->extra_data);
	obsx265->extra_data = NULL;
	bfree(obsx265->sei);
	obsx265->sei = NULL;

	obsx265->context = obsx265->api->encoder_open(&obsx265->params);
	if (!obsx265->context)
		return false;
	load_headers(obsx265);

	/* DIAG (mid-stream resize): read back the size x265 actually adopted so a re-open
	 * that silently keeps the old resolution is visible in the log. */
	x265_param probe;
	memset(&probe, 0, sizeof(probe));
	obsx265->api->encoder_parameters(obsx265->context, &probe);
	info("x265 reopen: context adopted %dx%d (extra_data=%zu bytes)",
	     probe.sourceWidth, probe.sourceHeight, obsx265->extra_data_size);

	return true;
}

static bool obs_x265_reinit_fps(struct obs_x265 *obsx265)
{
	/* params.fpsNum/fpsDenom and keyframeMax are already updated by the caller. */
	if (!obs_x265_reopen_context(obsx265)) {
		warn("failed to re-initialise x265 after live FPS change");
		return false;
	}
	return true;
}

/* Compute the target PTS for the next shared multitrack keyframe grid boundary after a live
 * re-init (resize or FPS change). Must be called AFTER the context re-open. Sets target_pts
 * on success and returns 1, or returns 0 if no realignment is needed. */
static int obs_x265_compute_realign_keyint(struct obs_x265 *obsx265, struct encoder_frame *frame)
{
	if (!obs_encoder_active(obsx265->encoder)) {
		info("realignment skipped (not streaming)");
		return 0;
	}

	if (obsx265->last_keyframe_pts < 0 || obsx265->gop_pts <= 0) {
		info("realignment skipped (no keyframe cadence observed yet)");
		return 0;
	}

	/* last_keyframe_pts is the most recent on-grid keyframe before this frame, so the next shared
	 * boundary is one GOP later. This frame's pts sits between them, so the target is strictly ahead
	 * of us (defensively bump it forward if a deep pipeline has already passed it). */
	int64_t target = obsx265->last_keyframe_pts + obsx265->gop_pts;
	while (target <= frame->pts)
		target += obsx265->gop_pts;

	obsx265->target_pts = target;
	return 1;
}

/* Mark that realignment is pending (call after the re-open has been applied). */
static void obs_x265_commit_realign(struct obs_x265 *obsx265, const char *reason)
{
	obsx265->align_pending = true;
	info("staged realignment after %s (target pts %lld, gop_pts %lld)",
	     reason, (long long)obsx265->target_pts, (long long)obsx265->gop_pts);
}

static void obs_x265_check_fps(struct obs_x265 *obsx265, struct encoder_frame *frame)
{
	/* Mid-realignment (a resize or FPS re-open is already staged): don't re-detect, the inited_* values are
	 * refreshed when the staged re-open fires in obs_x265_encode. */
	if (obsx265->align_pending)
		return;

	/* Live per-track FPS change (multitrack): the frontend stages a divisor change via
	 * obs_encoder_set_pending_frame_rate_divisor(). We apply it exactly on the next shared keyframe boundary so
	 * this track's keyframes stay aligned with its siblings. An immediate change would reset the skip counter
	 * without frame-boundary synchronisation and shift the synthetic PTS phase by an arbitrary amount, which no
	 * later re-open can recover (the sub-rate stream only ever delivers frames at that one offset). */
	uint32_t pending_div = obs_encoder_get_pending_frame_rate_divisor(obsx265->encoder);
	if (pending_div != 0 && pending_div != obs_encoder_get_frame_rate_divisor(obsx265->encoder)) {
		uint32_t num = obs_encoder_get_fps_num(obsx265->encoder);
		uint32_t den = obs_encoder_get_fps_den(obsx265->encoder) * pending_div;

		if (num == 0 || den == 0)
			return;

		info("live FPS change %u/%u -> %u/%u, staging boundary-aligned re-init", obsx265->inited_fps_num,
		     obsx265->inited_fps_den, num, den);

		/* Keep the keyframe interval (in seconds) constant across the rate change. When the user set an
		 * explicit interval in seconds, recompute it for the new rate directly; otherwise scale the current
		 * frame count from the fps the context was opened with. */
		if (obsx265->keyint_sec > 0) {
			obsx265->params.keyframeMax = obsx265->keyint_sec * (int)num / (int)den;
		} else if (obsx265->params.keyframeMax > 0 && obsx265->inited_fps_num != 0) {
			int64_t knew = (int64_t)obsx265->params.keyframeMax * obsx265->inited_fps_den * num /
			               ((int64_t)obsx265->inited_fps_num * den);
			if (knew > 0)
				obsx265->params.keyframeMax = (int)knew;
		}

		obsx265->params.fpsNum   = num;
		obsx265->params.fpsDenom = den;

		if (obs_encoder_active(obsx265->encoder) && obsx265->last_keyframe_pts >= 0 && obsx265->gop_pts > 0) {
			/* Defer the divisor change to the next shared keyframe boundary and re-open there. The encode
			 * path fires both at target_pts: libobs switches the delivery rate exactly on that PTS and the
			 * context re-open emits an IDR there, so the new GOP phase starts exactly on the shared grid. */
			int64_t target = obsx265->last_keyframe_pts + obsx265->gop_pts;
			while (target <= frame->pts)
				target += obsx265->gop_pts;

			obs_encoder_set_frame_rate_divisor_at_pts(obsx265->encoder, pending_div, target);
			obsx265->target_pts    = target;
			obsx265->align_pending = true;
			info("staged FPS divisor change at shared keyframe boundary (target pts %lld, gop_pts %lld)",
			     (long long)target, (long long)obsx265->gop_pts);
		} else {
			/* No on-grid cadence measured yet (change within the first GOP): apply at the next frame as a best effort.
			 * Use the same staged mechanism (no lock) - we must NOT call obs_encoder_update_frame_rate_divisor here,
			 * because check_fps runs on the video thread already holding the input lock and that setter would deadlock. */
			info("no keyframe cadence observed yet; applying FPS divisor change at the next frame");
			int64_t target = frame->pts + 1;
			obs_encoder_set_frame_rate_divisor_at_pts(obsx265->encoder, pending_div, target);
			obsx265->target_pts    = target;
			obsx265->align_pending = true;
		}

		obs_encoder_set_pending_frame_rate_divisor(obsx265->encoder, 0);
		return;
	}

	/* Fallback: detect any other effective-FPS change (e.g. a base-rate change) against the fps the context was
	 * actually opened with, not params.fpsNum (a deferred update() may have advanced params.fpsNum via reconfig
	 * without rebuilding the rate control, which a params-based comparison would miss). */
	uint32_t num = obs_encoder_get_fps_num(obsx265->encoder);
	uint32_t den = obs_encoder_get_fps_den(obsx265->encoder) *
	                obs_encoder_get_frame_rate_divisor(obsx265->encoder);

	if (num == 0 || den == 0)
		return;
	if (num == obsx265->inited_fps_num && den == obsx265->inited_fps_den)
		return;

	info("live FPS change %u/%u -> %u/%u, re-initialising x265", obsx265->inited_fps_num,
	     obsx265->inited_fps_den, num, den);

	if (obsx265->keyint_sec > 0) {
		obsx265->params.keyframeMax = obsx265->keyint_sec * (int)num / (int)den;
	} else if (obsx265->params.keyframeMax > 0 && obsx265->inited_fps_num != 0) {
		int64_t knew = (int64_t)obsx265->params.keyframeMax * obsx265->inited_fps_den * num /
		               ((int64_t)obsx265->inited_fps_num * den);
		if (knew > 0)
			obsx265->params.keyframeMax = (int)knew;
	}

	obsx265->params.fpsNum   = num;
	obsx265->params.fpsDenom = den;

	if (obs_x265_reinit_fps(obsx265)) {
		obsx265->inited_fps_num = num;
		obsx265->inited_fps_den = den;
		int realign_keyint = obs_x265_compute_realign_keyint(obsx265, frame);
		if (realign_keyint > 0) {
			obs_x265_commit_realign(obsx265, "FPS change");
		}
	}
}

/* Detect a live resolution change (multitrack) and re-open the context immediately. The
 * previous staged approach scaled new-size frames back to the old resolution on the CPU while
 * waiting for a keyframe boundary, but that cost more per frame than the delivery budget
 * allows - the bounded mix cache overflowed and every frame of the wait window was dropped
 * (a visible freeze of up to one GOP). Re-opening instead takes only ~30-80 ms: in-flight
 * frames are flushed and re-emitted by obs_x265_reopen_context, so no frame is lost and DTS
 * stays continuous. The fresh context emits an off-grid IDR, so a second re-open is staged on
 * the next shared keyframe boundary (align_pending) to re-lock this track onto its siblings'
 * keyframe grid. */
static void obs_x265_maybe_resize(struct obs_x265 *obsx265, struct encoder_frame *frame)
{
	const uint32_t target_w = obs_encoder_get_width(obsx265->encoder);
	const uint32_t target_h = obs_encoder_get_height(obsx265->encoder);

	if (target_w == obsx265->inited_width && target_h == obsx265->inited_height) {
		obsx265->inited_ls0 = frame->linesize[0]; /* keep in sync with current context size */
		obsx265->resize_stall = 0;
		return;
	}

	/* Race guard: the mix rebind takes effect on the next tick, so for up to one frame
	 * after the scaled size is updated the delivered frame still carries the OLD size.
	 * Re-opening to the new size and feeding it an old-size frame would read out of bounds,
	 * so defer until the plane-0 stride actually changes (width change) or we've clearly passed
	 * the one-tick window (a same-width height change leaves the stride unchanged). Until then
	 * encode the frame into the current context as-is. */
	const bool arrived = (frame->linesize[0] != obsx265->inited_ls0);
	if (!arrived && ++obsx265->resize_stall < 2)
		return;

	info("live resize %ux%u -> %ux%u (immediate re-open)", obsx265->inited_width, obsx265->inited_height,
	     target_w, target_h);
	obsx265->params.sourceWidth  = (int)target_w;
	obsx265->params.sourceHeight = (int)target_h;
	if (!obs_x265_reopen_context(obsx265)) {
		err("live resize to %ux%u failed - restart required", target_w, target_h);
		return;
	}
	obsx265->inited_width  = target_w;
	obsx265->inited_height = target_h;
	obsx265->inited_ls0    = frame->linesize[0];
	obsx265->resize_stall  = 0;
	int realign_keyint = obs_x265_compute_realign_keyint(obsx265, frame);
	if (realign_keyint > 0)
		obs_x265_commit_realign(obsx265, "resize");
}

static bool obs_x265_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
			     bool *received_packet)
{
	struct obs_x265 *obsx265 = data;
	x265_nal *nals;
	uint32_t nal_count = 0;
	int ret;
	x265_picture pic, pic_out;

	if (!frame || !packet || !received_packet)
		return false;

	/* A failed live-FPS re-init leaves the context NULL; do not hammer a broken
	 * encoder (and avoid retrying encoder_open every frame). */
	if (!obsx265->context)
		return false;

	obs_x265_check_fps(obsx265, frame);
	if (!obsx265->context)
		return false;

	/* Live resolution change (multitrack): detect and re-open immediately. */
	obs_x265_maybe_resize(obsx265, frame);
	if (!obsx265->context)
		return false;

	/* Keyframe-grid realignment: re-open the context exactly on the next shared keyframe
	 * boundary so this track re-locks onto its sibling encoders' keyframe pts grid. A freshly
	 * opened x265 context always emits a real IDR as its first frame. */
	if (obsx265->align_pending && frame->pts >= obsx265->target_pts) {
		obsx265->align_pending = false;
		info("realignment: re-opening context at shared keyframe boundary (pts %lld, target %lld)",
		     (long long)frame->pts, (long long)obsx265->target_pts);
		if (!obs_x265_reopen_context(obsx265)) {
			err("realignment re-open failed - restart required");
			return false;
		}
		obsx265->inited_fps_num = (uint32_t)obsx265->params.fpsNum;
		obsx265->inited_fps_den = (uint32_t)obsx265->params.fpsDenom;
		/* The re-open emitted an IDR at the shared boundary; update grid tracking. */
		obsx265->last_keyframe_pts = frame->pts;
	}

	/* A consumer attached while this encoder was already encoding, so libobs raised a
	 * keyframe request for it (obs_encoder_request_keyframe): its packet gate drops every
	 * packet until it sees a keyframe, and without help that is a whole GOP of dead air for
	 * the new output while the consumers already attached keep streaming.  x265 has no
	 * per-picture IDR request, so re-open the context - a freshly opened one emits an IDR
	 * with the parameter sets as its first frame (the mechanism the keyframe-grid
	 * realignment above relies on), and the pts stay continuous across the re-open.
	 */
	if (obs_encoder_keyframe_requested(obsx265->encoder)) {
		if (obs_encoder_is_in_group(obsx265->encoder)) {
			info("keyframe request dropped: grouped encoder keeps its shared keyframe grid");
		} else {
			info("keyframe request: re-opening the encoder so a late consumer starts at an IDR");
			if (!obs_x265_reopen_context(obsx265)) {
				err("keyframe-request re-open failed - restart required");
				return false;
			}
			obsx265->inited_fps_num = (uint32_t)obsx265->params.fpsNum;
			obsx265->inited_fps_den = (uint32_t)obsx265->params.fpsDenom;
			obsx265->last_keyframe_pts = frame->pts;
		}
	}

	init_pic_data(obsx265, &pic, frame);

	if (obs_encoder_has_roi(obsx265->encoder))
		add_roi(obsx265, &pic);

	ret = obsx265->api->encoder_encode(obsx265->context, &nals, &nal_count, &pic, &pic_out);
	if (ret < 0) {
		warn("encode failed");
		return false;
	}

	/* Re-emit queue drain: if a re-open just flushed in-flight frames into the queue, this call
	 * emits the next queued packet (lowest DTS) and defers the freshly encoded output to the back
	 * of the queue. The pre-reopen frames therefore drain out before the post-reopen ones, preserving
	 * decode order with no DTS gap and no dropped frames. In steady state the queue is empty and this
	 * branch is skipped. */
	if (obsx265->flush_queue.num > 0) {
		if (nal_count > 0)
			obs_x265_flush_enqueue(obsx265, nals, nal_count, &pic_out);

		struct obs_x265_queued_pkt front = obsx265->flush_queue.array[0];
		const size_t remaining = obsx265->flush_queue.num - 1;
		if (remaining)
			memmove(&obsx265->flush_queue.array[0], &obsx265->flush_queue.array[1], remaining * sizeof(front));
		da_resize(obsx265->flush_queue, remaining);

		packet->data     = front.data; /* ownership transfers to the caller */
		packet->size     = front.size;
		packet->type     = OBS_ENCODER_VIDEO;
		packet->pts      = front.pts;
		packet->dts      = front.dts;
		packet->keyframe = front.keyframe;
		*received_packet = true;

		return true;
	}

	*received_packet = (nal_count > 0);
	parse_packet(obsx265, packet, nals, nal_count, &pic_out);

	/* ABR governor: feed the coded size so it can steer the VBV ceiling. */
	if (*received_packet)
		x265_abr_tick(obsx265, packet->size);

	/* Track the steady-state on-grid keyframe cadence for future realignment. Only update
	 * when not mid-realignment so the off-grid resize IDR does not corrupt the grid phase. */
	if (nal_count > 0 && IS_X265_TYPE_I(pic_out.sliceType) && !obsx265->align_pending) {
		const int64_t kpts = pic_out.pts;
		if (obsx265->last_keyframe_pts >= 0)
			obsx265->gop_pts = kpts - obsx265->last_keyframe_pts;
		obsx265->last_keyframe_pts = kpts;
	}

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

