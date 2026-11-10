/*

This file is provided under a dual BSD/GPLv2 license.  When using or
redistributing this file, you may do so under either license.

GPL LICENSE SUMMARY

Copyright(c) Oct. 2015 Intel Corporation.

This program is free software; you can redistribute it and/or modify
it under the terms of version 2 of the GNU General Public License as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

Contact Information:

Seung-Woo Kim, seung-woo.kim@intel.com
705 5th Ave S #500, Seattle, WA 98104

BSD LICENSE

Copyright(c) <date> Intel Corporation.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in
the documentation and/or other materials provided with the
distribution.

* Neither the name of Intel Corporation nor the names of its
contributors may be used to endorse or promote products derived
from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <inttypes.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/base.h>
#include <obs-module.h>
#include <obs-hevc.h>
#include <obs-avc.h>
#include <abr-gov.h>

#include "QSV_Encoder.h"
#include "common_utils.h"

#define do_log(level, format, ...) \
	blog(level, "[qsv encoder: '%s'] " format, obs_encoder_get_name(obsqsv->encoder), ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR, format, ##__VA_ARGS__)
#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

#ifndef GS_INVALID_HANDLE
#define GS_INVALID_HANDLE (uint32_t) - 1
#endif

/* Number of emitted packets after a live re-init for which the DTS regression check
 * is suppressed (see struct obs_qsv::dts_suppress). Covers MFX's initial B-frame
 * window, whose DecodeTimeStamps are not yet monotonic. */
#define QSV_REINIT_DTS_SUPPRESS 16

/* ------------------------------------------------------------------------- */

/* One frame deep-copied while a background timed re-init is in progress (see
 * obs_qsv::reinit_in_progress). Buffered so it can be re-fed through the normal encode path
 * after the reset instead of being dropped - which would leave a DTS gap at the resize point. */
struct qsv_pending_frame {
	uint8_t *y;  /* owned copy of plane 0 (NV12/P010) */
	uint8_t *uv; /* owned copy of plane 1 */
	uint32_t ls0;
	uint32_t ls1;
	int64_t pts;
};

struct obs_qsv {
	obs_encoder_t *encoder;

	enum qsv_codec codec;

	qsv_param_t params;
	qsv_t *context;

	DARRAY(uint8_t) packet_data;

	uint8_t *extra_data;
	uint8_t *sei;

	size_t extra_data_size;
	size_t sei_size;

	os_performance_token_t *performance_token;

	uint32_t roi_increment;

	/* Live RC/bitrate changes are staged here and applied at the next */
	/* GOP boundary so sibling encoders of a multitrack output stay   */
	/* keyframe-aligned (see qsv_check_staged).                       */
	bool reconfig_pending;
	/* ABR live TargetKbps changes are staged here and applied at the next keyframe boundary (qsv_check_staged),
	 * where the reset starts a new sequence (StartNewSequence=ON) so the driver's BRC/VBV state re-anchors to
	 * the new target immediately - a non-IDR reset would leave HEVC drifting toward it for ~20 s. Firing on the
	 * boundary makes the inserted IDR coincide with the natural GOP keyframe, keeping multitrack siblings aligned.
	 * Set by the governor tick or a manual ABR settings change; one re-anchor per boundary (latest value wins). */
	bool abr_reconfig_pending;
	bool fps_reinit_pending; /* effective-FPS change needs a full Close+Init: staged at the next shared
	                             * keyframe boundary for grouped encoders, immediate otherwise (qsv_check_staged) */
	int64_t frames_since_idr; /* submitted frames since last observed IDR packet */

	/* Live RESOLUTION changes: single-track encoders re-init the MFX      */
	/* session immediately (forced mid-GOP IDR); align_pending then counts*/
	/* down submissions and forces one more keyframe exactly on the next  */
	/* GOP boundary so the track re-locks onto its keyframe pts grid.     */
	/* Multitrack (encoder group) members instead stage a TIMED resize    */
	/* (resize_pending, mirrors obs_x264_maybe_resize): an off-grid       */
	/* session re-init would emit its session-start IDR at the resize      */
	/* point and re-anchor this track's GOP phase there, desyncing every  */
	/* subsequent keyframe from its siblings on the shared grid (libobs   */
	/* reports "Missing keyframe" for the whole group). The re-init is     */
	/* therefore deferred to the next shared keyframe boundary so that     */
	/* IDR lands exactly on-grid; while waiting, new-size frames are       */
	/* scaled down to the old resolution (scale_buf) and encoded by the    */
	/* current session. No off-grid keyframe is ever emitted.             */
	bool align_pending;
	int64_t align_remaining; /* encode() calls until realignment fires (incl. current) */

	bool resize_pending;      /* timed resize staged: scale new-size frames to the old size while waiting */
	uint32_t pending_width;   /* target size for the staged re-init         */
	uint32_t pending_height;
	bool fps_pending;         /* timed full re-init staged for an effective-FPS change (shares the boundary below) */
	int64_t reinit_target_pts;/* shared keyframe boundary to re-init on (staged resize and/or FPS change) */
	uint32_t reinit_misses;   /* boundaries missed due to dropped frames    */
	int64_t last_keyframe_pts;/* most recent steady-state on-grid keyframe pts (-1 = none yet) */
	int64_t gop_pts;          /* measured keyframe cadence in pts units (0 = unknown) */
	uint8_t *scale_buf[2];    /* NV12 scale-to-old-size buffers for the wait window */
	uint32_t scale_stride[2];
	enum video_format delivery_format; /* format libobs delivers (set in obs_qsv_video_info) */

	/* Background timed re-init (CPU-frame path only): the ~400 ms MFX Close+Init of a timed
	 * resize/FPS change runs on a worker thread so it never blocks the encode (mix) thread -
	 * blocking would stall the whole mix pipeline and every frame of the window is lost
	 * (missed ticks are never rendered: a visible freeze). While the worker runs, incoming
	 * frames are deep-copied into pending_frames; once the reset completes they are re-fed
	 * through the normal encode path one per call (oldest first), so no frame is dropped and
	 * DTS stays continuous across the boundary. Note: with libobs' 1-frame-in/1-packet-out
	 * contract this track then runs ~one reset duration behind real time until a restart -
	 * catching up faster would require dropping frames, i.e. the freeze this prevents. */
	bool reinit_in_progress;
	pthread_t reinit_thread; /* valid while reinit_in_progress */
	os_event_t *reinit_done;  /* manual-reset event, valid while reinit_in_progress */
	int64_t reinit_boundary_pts; /* pts of the frame that triggered the re-init (grid re-anchor) */
	DARRAY(struct qsv_pending_frame) pending_frames;

	/* Current encoded resolution (for live resize detection).        */
	uint32_t cx;
	uint32_t cy;

	/* Pipeline delay estimate: frames in flight between submission   */
	/* and output observation. Derived from async depth at init.      */
	int output_delay;

	/* The MFX GopPicSize is fixed for the life of the encoder session
	 * (set once at create, never changed by a live reconfigure). Store it
	 * here so the IDR-boundary timing in qsv_check_staged always matches
	 * the encoder's actual GOP. Using the *current* effective FPS instead
	 * would make gop_length larger than the real GOP after an FPS increase,
	 * so frames_since_idr would never reach the boundary and the staged
	 * reconfigure would silently never fire (leaving MFX at the old, lower
	 * frame rate while receiving more frames -> inflated bitrate). */
	int64_t gop_frames;

	/* Output timestamps stay on the OBS timeline across a live reconfigure.
	 * MFX echoes back the per-frame TimeStamp we submit (encoder->cur_pts) as its
	 * output PresentationTimeStamp and continues its DecodeTimeStamp base, so no
	 * correction is normally needed: ts_offset stays 0 and pkt->pts keeps matching
	 * encoder->cur_pts (libobs's frame-timing association). The one anomaly the
	 * offset guards against is a genuine DTS regression outside the post-reinit
	 * window; if that ever happens, ts_offset is grown to bridge it (last_dts/
	 * have_last_dts feed the check in qsv_fix_timestamps). */
	int64_t ts_offset;
	int64_t last_dts;
	bool have_last_dts;

	/* >0: number of emitted packets left to pass through WITHOUT running the DTS
	 * regression check. Set on every live re-init (qsv_reset_ts_state): MFX emits
	 * its initial B-frame window with non-monotonic DecodeTimeStamps, which would
	 * otherwise trip the regression check and shift ts_offset off 0 (desyncing PTS
	 * from cur_pts). See qsv_fix_timestamps. */
	int64_t dts_suppress;

	/* ABR (QVBR) governor state. The shared governor (shared/abr-gov) steers TargetKbps around the
	 * user's "bitrate" so the long-term average converges to it; MaxKbps is a fixed ceiling. */
	bool abr_active;        /* true when rate_control == ABR (QVBR base mode) */
	double abr_deadband;    /* deadband as a fraction of target, e.g. 0.005 = +/-0.5% */
	int abr_target_kbps;     /* user's "bitrate" setting (the convergence target) */
	int abr_ceiling_kbps;    /* fixed ceiling = max(max_bitrate, bitrate) */
	bool abr_session_loaded; /* per-open: session budget latched this open */
	abr_gov_state abr_state;  /* cumulative bits/time + governed cur_rate (bits/sec) */
};

/* Forward declaration: also called from obs_qsv_destroy, which precedes the definition. */
static void free_scale_buffers(struct obs_qsv *obsqsv);
static void qsv_pending_free(struct obs_qsv *obsqsv);

/* ------------------------------------------------------------------------- */

static pthread_mutex_t g_QsvLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned short g_verMajor;
static unsigned short g_verMinor;

static const char *obs_qsv_getname_v1(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "QuickSync H.264 (v1 deprecated)";
}

static const char *obs_qsv_getname(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "QuickSync H.264";
}

static const char *obs_qsv_getname_av1(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "QuickSync AV1";
}

static const char *obs_qsv_getname_hevc(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "QuickSync HEVC";
}

static const char *obs_qsv_getname_vp9(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "QuickSync VP9";
}

static void clear_data(struct obs_qsv *obsqsv)
{
	if (obsqsv->context) {
		pthread_mutex_lock(&g_QsvLock);
		qsv_encoder_close(obsqsv->context);
		obsqsv->context = NULL;
		pthread_mutex_unlock(&g_QsvLock);

		// bfree(obsqsv->sei);
		bfree(obsqsv->extra_data);

		// obsqsv->sei = NULL;
		obsqsv->extra_data = NULL;
	}
}

static void obs_qsv_destroy(void *data)
{
	struct obs_qsv *obsqsv = (struct obs_qsv *)data;

	if (obsqsv) {
		os_end_high_performance(obsqsv->performance_token);
		clear_data(obsqsv); /* blocks on g_QsvLock until a running re-init worker finishes */
		abr_gov_session_save(obsqsv->encoder, obsqsv->abr_active, &obsqsv->abr_state);
		if (obsqsv->reinit_in_progress) {
			pthread_join(obsqsv->reinit_thread, NULL);
			os_event_destroy(obsqsv->reinit_done);
			obsqsv->reinit_done = NULL;
			obsqsv->reinit_in_progress = false;
		}
		free_scale_buffers(obsqsv);
		qsv_pending_free(obsqsv);
		da_free(obsqsv->packet_data);
		bfree(obsqsv);
	}
}

static void obs_qsv_defaults(obs_data_t *settings, int ver, enum qsv_codec codec)
{
	obs_data_set_default_string(settings, "target_usage", "TU4");
	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_int(settings, "max_bitrate", 3000);
	obs_data_set_default_int(settings, "qvbr_quality", 25);
	obs_data_set_default_double(settings, "deadband", 0.5);
	obs_data_set_default_string(settings, "abr_strategy", "target");
	obs_data_set_default_bool(settings, "abr_low_latency", false);
	obs_data_set_default_string(settings, "profile", codec == QSV_CODEC_AVC ? "high" :
	                                      (codec == QSV_CODEC_VP9 ? "profile0" : "main"));

	/* CBR is the recommended live-streaming mode for VP9 (bitrate-constrained);
	 * other codecs default to ICQ. */
	obs_data_set_default_string(settings, "rate_control", codec == QSV_CODEC_VP9 ? "CBR" : "ICQ");

	obs_data_set_default_int(settings, "__ver", ver);

	obs_data_set_default_int(settings, "cqp", 23);
	obs_data_set_default_int(settings, "qpi", 23);
	obs_data_set_default_int(settings, "qpp", 23);
	obs_data_set_default_int(settings, "qpb", 23);
	obs_data_set_default_int(settings, "icq_quality", 17);

	/* VP9 uses an explicit live-streaming GOP (~2s) rather than the recording-
	 * oriented auto/0 default, for deterministic keyframe/bitrate behavior. */
	obs_data_set_default_int(settings, "keyint_sec", codec == QSV_CODEC_VP9 ? 2 : 0);
	obs_data_set_default_string(settings, "latency", "normal");
	/* VP9 has no traditional B-frame pipeline (GopRefDist=1); force 0. */
	obs_data_set_default_int(settings, "bframes", codec == QSV_CODEC_VP9 ? 0 : 3);
	obs_data_set_default_bool(settings, "repeat_headers", false);

	/* Per-encoder color defaults: H.264 is 8-bit (NV12), HEVC/AV1 default to
	 * 10-bit (P010). VP9 starts on the 8-bit NV12 / profile 0 path. */
	obs_data_set_default_string(settings, "color_format", codec == QSV_CODEC_AVC ? "NV12" :
	                                      (codec == QSV_CODEC_VP9 ? "NV12" : "P010"));
	obs_data_set_default_int(settings, "color_space", VIDEO_CS_709);
	obs_data_set_default_int(settings, "color_range", VIDEO_RANGE_PARTIAL);
}

static void obs_qsv_defaults_h264_v1(obs_data_t *settings)
{
	obs_qsv_defaults(settings, 1, QSV_CODEC_AVC);
}

static void obs_qsv_defaults_h264_v2(obs_data_t *settings)
{
	obs_qsv_defaults(settings, 2, QSV_CODEC_AVC);
}

static void obs_qsv_defaults_av1(obs_data_t *settings)
{
	obs_qsv_defaults(settings, 2, QSV_CODEC_AV1);
}

/* Aggregate VP9 capabilities across all Intel adapters: a profile is "available"
 * if ANY adapter's runtime probe confirmed it (spec §36-§37). This mirrors how
 * encoder registration is gated and drives the dynamic profile UI + validation. */
static void vp9_aggregate_caps(struct vp9_caps *out)
{
	memset(out, 0, sizeof(*out));
	for (size_t i = 0; i < adapter_count; i++) {
		if (!adapters[i].is_intel)
			continue;
		out->profile0_nv12_8bit |= adapters[i].vp9.profile0_nv12_8bit;
		out->profile1_ayuv_8bit |= adapters[i].vp9.profile1_ayuv_8bit;
		out->profile2_p010_10bit |= adapters[i].vp9.profile2_p010_10bit;
		out->profile3_y410_10bit |= adapters[i].vp9.profile3_y410_10bit;
	}
	out->encoder_supported = out->profile0_nv12_8bit || out->profile1_ayuv_8bit ||
	                         out->profile2_p010_10bit || out->profile3_y410_10bit;
}

static void obs_qsv_defaults_hevc(obs_data_t *settings)
{
	obs_qsv_defaults(settings, 2, QSV_CODEC_HEVC);
}

static void obs_qsv_defaults_vp9(obs_data_t *settings)
{
	obs_qsv_defaults(settings, 2, QSV_CODEC_VP9);

	/* Default the standard "Color Format" dropdown (injected by libobs) to the first
	 * runtime-supported VP9 profile so the initial selection is always valid on this
	 * hardware. Each OBS color format maps 1:1 to a QSV VP9 profile: NV12->P0, P010->P2,
	 * I444->P1 (delivered as AYUV), Y410->P3 (spec §23). */
	struct vp9_caps caps;
	vp9_aggregate_caps(&caps);
	const char *default_fmt = "NV12";
	if (!caps.profile0_nv12_8bit) {
		if (caps.profile2_p010_10bit)
			default_fmt = "P010";
		else if (caps.profile1_ayuv_8bit)
			default_fmt = "I444";
		else if (caps.profile3_y410_10bit)
			default_fmt = "Y410";
	}
	obs_data_set_default_string(settings, "color_format", default_fmt);
}

static inline void add_strings(obs_property_t *list, const char *const *strings)
{
	while (*strings) {
		obs_property_list_add_string(list, *strings, *strings);
		strings++;
	}
}

static inline void add_translated_strings(obs_property_t *list, const char *const *keys, const char *const *strings)
{
	while (*keys && *strings) {
		obs_property_list_add_string(list, obs_module_text(*keys), *strings);
		keys++;
		strings++;
	}
}

#define TEXT_SPEED obs_module_text("TargetUsage")
#define TEXT_TARGET_BITRATE obs_module_text("Bitrate")
#define TEXT_MAX_BITRATE obs_module_text("MaxBitrate")
#define TEXT_PROFILE obs_module_text("Profile")
#define TEXT_LATENCY obs_module_text("Latency")
#define TEXT_RATE_CONTROL obs_module_text("RateControl")
#define TEXT_ICQ_QUALITY obs_module_text("ICQQuality")
#define TEXT_QVBR_QUALITY obs_module_text("QVBRQuality")
#define TEXT_DEADBAND obs_module_text("Deadband")
#define TEXT_ABR_STRATEGY obs_module_text("ABRStrategy")
#define TEXT_ABR_LOW_LATENCY obs_module_text("ABRLowLatency")
#define TEXT_KEYINT_SEC obs_module_text("KeyframeIntervalSec")
#define TEXT_BFRAMES obs_module_text("BFrames")

static bool update_latency(obs_data_t *settings)
{
	bool update = false;
	int async_depth = 4;
	if (obs_data_item_byname(settings, "async_depth") != NULL) {
		async_depth = (int)obs_data_get_int(settings, "async_depth");
		obs_data_erase(settings, "async_depth");
		update = true;
	}

	int la_depth = 15;
	if (obs_data_item_byname(settings, "la_depth") != NULL) {
		la_depth = (int)obs_data_get_int(settings, "la_depth");
		obs_data_erase(settings, "la_depth");
		update = true;
	}

	const char *rate_control = obs_data_get_string(settings, "rate_control");

	bool lookahead = false;
	if (astrcmpi(rate_control, "LA_CBR") == 0) {
		obs_data_set_string(settings, "rate_control", "CBR");
		lookahead = true;
	} else if (astrcmpi(rate_control, "LA_VBR") == 0) {
		obs_data_set_string(settings, "rate_control", "VBR");
		lookahead = true;
	} else if (astrcmpi(rate_control, "LA_ICQ") == 0) {
		obs_data_set_string(settings, "rate_control", "ICQ");
		lookahead = true;
	}

	if (update) {
		if (lookahead) {
			if (la_depth == 0 || la_depth >= 15)
				obs_data_set_string(settings, "latency", "normal");
			else
				obs_data_set_string(settings, "latency", "low");
		} else {
			if (async_depth != 1)
				obs_data_set_string(settings, "latency", "normal");
			else
				obs_data_set_string(settings, "latency", "ultra-low");
		}
	}

	return true;
}

static bool update_ratecontrol(obs_data_t *settings)
{
	const char *rate_control = obs_data_get_string(settings, "rate_control");

	if (astrcmpi(rate_control, "VCM") == 0) {
		obs_data_set_string(settings, "rate_control", "CBR");
	} else if (astrcmpi(rate_control, "AVBR") == 0) {
		obs_data_set_string(settings, "rate_control", "VBR");
	}

	return true;
}

static void update_targetusage(obs_data_t *settings)
{
	const char *target_usage = obs_data_get_string(settings, "target_usage");

	if (astrcmpi(target_usage, "veryslow") == 0 || astrcmpi(target_usage, "quality") == 0)
		obs_data_set_string(settings, "target_usage", "TU1");
	else if (astrcmpi(target_usage, "slower") == 0)
		obs_data_set_string(settings, "target_usage", "TU2");
	else if (astrcmpi(target_usage, "slow") == 0)
		obs_data_set_string(settings, "target_usage", "TU3");
	else if (astrcmpi(target_usage, "medium") == 0 || astrcmpi(target_usage, "balanced") == 0)
		obs_data_set_string(settings, "target_usage", "TU4");
	else if (astrcmpi(target_usage, "fast") == 0)
		obs_data_set_string(settings, "target_usage", "TU5");
	else if (astrcmpi(target_usage, "faster") == 0)
		obs_data_set_string(settings, "target_usage", "TU6");
	else if (astrcmpi(target_usage, "veryfast") == 0 || astrcmpi(target_usage, "speed") == 0)
		obs_data_set_string(settings, "target_usage", "TU7");
}

static bool rate_control_modified(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	const char *rate_control = obs_data_get_string(settings, "rate_control");

	bool bVisible = astrcmpi(rate_control, "VBR") == 0 || astrcmpi(rate_control, "ABR") == 0;
	p = obs_properties_get(ppts, "max_bitrate");
	obs_property_set_visible(p, bVisible);

	bVisible = astrcmpi(rate_control, "CQP") == 0 || astrcmpi(rate_control, "ICQ") == 0;
	p = obs_properties_get(ppts, "bitrate");
	obs_property_set_visible(p, !bVisible);

	bVisible = astrcmpi(rate_control, "CQP") == 0;
	p = obs_properties_get(ppts, "qpi");
	if (p)
		obs_property_set_visible(p, bVisible);
	p = obs_properties_get(ppts, "qpb");
	if (p)
		obs_property_set_visible(p, bVisible);
	p = obs_properties_get(ppts, "qpp");
	if (p)
		obs_property_set_visible(p, bVisible);
	p = obs_properties_get(ppts, "cqp");
	if (p)
		obs_property_set_visible(p, bVisible);

	bVisible = astrcmpi(rate_control, "ICQ") == 0;
	p = obs_properties_get(ppts, "icq_quality");
	obs_property_set_visible(p, bVisible);

	bVisible = astrcmpi(rate_control, "ABR") == 0;
	p = obs_properties_get(ppts, "qvbr_quality");
	if (p)
		obs_property_set_visible(p, bVisible);
	p = obs_properties_get(ppts, "deadband");
	if (p)
		obs_property_set_visible(p, bVisible);
	p = obs_properties_get(ppts, "abr_strategy");
	if (p)
		obs_property_set_visible(p, bVisible);
	p = obs_properties_get(ppts, "abr_low_latency");
	if (p)
		obs_property_set_visible(p, bVisible);

	update_latency(settings);
	update_targetusage(settings);
	update_ratecontrol(settings);

	return true;
}

static inline void add_rate_controls(obs_property_t *list, const struct qsv_rate_control_info *rc)
{
	enum qsv_cpu_platform plat = qsv_get_cpu_platform();
	while (rc->name) {
		if (!rc->haswell_or_greater || (plat >= QSV_CPU_PLATFORM_HSW || plat == QSV_CPU_PLATFORM_UNKNOWN))
			obs_property_list_add_string(list, rc->name, rc->name);
		rc++;
	}
}

static obs_properties_t *obs_qsv_props(enum qsv_codec codec, void *unused, int ver)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop;

	prop = obs_properties_add_list(props, "rate_control", TEXT_RATE_CONTROL, OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);

	/* VP9 exposes only CBR/VBR/CQP (no ICQ/LA lookahead modes). */
	if (codec == QSV_CODEC_VP9)
		add_rate_controls(prop, qsv_ratecontrols_vp9);
	else
		add_rate_controls(prop, qsv_ratecontrols);

	/* ABR (quality-anchored VBR with a live governor) is offered for H.264/HEVC first; VP9/AV1 follow
	 * once proven on those hardware paths. */
	if (codec == QSV_CODEC_AVC || codec == QSV_CODEC_HEVC)
		obs_property_list_add_string(prop, "ABR", "ABR");

	obs_property_set_modified_callback(prop, rate_control_modified);

	prop = obs_properties_add_int(props, "bitrate", TEXT_TARGET_BITRATE, 50, 10000000, 50);
	obs_property_int_set_suffix(prop, " Kbps");

	prop = obs_properties_add_int(props, "max_bitrate", TEXT_MAX_BITRATE, 50, 10000000, 50);
	obs_property_int_set_suffix(prop, " Kbps");

	if (ver >= 2) {
		obs_properties_add_int(props, "cqp", "CQP", 1, codec == QSV_CODEC_AV1 ? 63 : 51, 1);
	} else {
		obs_properties_add_int(props, "qpi", "QPI", 1, 51, 1);
		obs_properties_add_int(props, "qpp", "QPP", 1, 51, 1);
		obs_properties_add_int(props, "qpb", "QPB", 1, 51, 1);
	}

	obs_properties_add_int(props, "icq_quality", TEXT_ICQ_QUALITY, 1, 51, 1);
	obs_properties_add_int(props, "qvbr_quality", TEXT_QVBR_QUALITY, 1, 51, 1);

	prop = obs_properties_add_float(props, "deadband", TEXT_DEADBAND, 0.1, 10.0, 0.1);
	obs_property_float_set_suffix(prop, "%");

	/* ABR comparison controls: which rate parameter the governor steers, and a low-latency (no B-frame)
	 * architecture. Both are ABR-only and default to the validated baseline behavior. */
	prop = obs_properties_add_list(props, "abr_strategy", TEXT_ABR_STRATEGY, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "target", "Target (QVBR)");
	obs_property_list_add_string(prop, "ceiling", "Ceiling (MaxKbps)");
	obs_property_set_long_description(prop, obs_module_text("ABRStrategy.ToolTip"));

	obs_properties_add_bool(props, "abr_low_latency", TEXT_ABR_LOW_LATENCY);
	obs_property_set_long_description(obs_properties_get(props, "abr_low_latency"),
					  obs_module_text("ABRLowLatency.ToolTip"));

	prop = obs_properties_add_list(props, "target_usage", TEXT_SPEED, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	add_translated_strings(prop, qsv_usage_translation_keys, qsv_usage_names);

	if (codec != QSV_CODEC_VP9) {
		/* VP9 exposes no explicit profile dropdown: the standard "Color Format"
		 * dropdown (injected by libobs from is_color_format_supported) selects the
		 * profile implicitly - NV12->P0, P010->P2, I444->P1, Y410->P3 (spec §23-§25). */
		prop = obs_properties_add_list(props, "profile", TEXT_PROFILE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

		if (codec == QSV_CODEC_AVC)
			add_strings(prop, qsv_profile_names);
		else if (codec == QSV_CODEC_AV1)
			add_strings(prop, qsv_profile_names_av1);
		else if (codec == QSV_CODEC_HEVC)
			add_strings(prop, qsv_profile_names_hevc);
	}

	prop = obs_properties_add_int(props, "keyint_sec", TEXT_KEYINT_SEC, 0, 20, 1);
	obs_property_int_set_suffix(prop, " s");

	prop = obs_properties_add_list(props, "latency", TEXT_LATENCY, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	add_strings(prop, qsv_latency_names);
	obs_property_set_long_description(prop, obs_module_text("Latency.ToolTip"));

	/* VP9 has no B-frame pipeline (GopRefDist=1), so the B-frames control is
	 * not exposed for it. */
	if (codec != QSV_CODEC_VP9)
		obs_properties_add_int(props, "bframes", TEXT_BFRAMES, 0, 3, 1);

	return props;
}

static obs_properties_t *obs_qsv_props_h264(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_qsv_props(QSV_CODEC_AVC, unused, 1);
}

static obs_properties_t *obs_qsv_props_h264_v2(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_qsv_props(QSV_CODEC_AVC, unused, 2);
}

static obs_properties_t *obs_qsv_props_av1(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_qsv_props(QSV_CODEC_AV1, unused, 2);
}

static obs_properties_t *obs_qsv_props_hevc(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_qsv_props(QSV_CODEC_HEVC, unused, 2);
}

static obs_properties_t *obs_qsv_props_vp9(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_qsv_props(QSV_CODEC_VP9, unused, 2);
}

static void update_params(struct obs_qsv *obsqsv, obs_data_t *settings)
{
	video_t *video = obs_encoder_video(obsqsv->encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	update_latency(settings);
	update_targetusage(settings);

	const char *target_usage = obs_data_get_string(settings, "target_usage");
	const char *profile = obs_data_get_string(settings, "profile");
	const char *rate_control = obs_data_get_string(settings, "rate_control");
	const char *latency = obs_data_get_string(settings, "latency");
	int target_bitrate = (int)obs_data_get_int(settings, "bitrate");
	int max_bitrate = (int)obs_data_get_int(settings, "max_bitrate");
	int qpi = (int)obs_data_get_int(settings, "qpi");
	int qpp = (int)obs_data_get_int(settings, "qpp");
	int qpb = (int)obs_data_get_int(settings, "qpb");
	int cqp = (int)obs_data_get_int(settings, "cqp");
	int ver = (int)obs_data_get_int(settings, "__ver");
	int icq_quality = (int)obs_data_get_int(settings, "icq_quality");
	int qvbr_quality = (int)obs_data_get_int(settings, "qvbr_quality");
	double deadband = obs_data_get_double(settings, "deadband");
	const char *abr_strategy = obs_data_get_string(settings, "abr_strategy");
	bool abr_low_latency = obs_data_get_bool(settings, "abr_low_latency");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	bool cbr_override = obs_data_get_bool(settings, "cbr");
	int bFrames = (int)obs_data_get_int(settings, "bframes");
	bool repeat_headers = obs_data_get_bool(settings, "repeat_headers");
	const char *codec = "";

	if (obs_data_has_user_value(settings, "bf"))
		bFrames = (int)obs_data_get_int(settings, "bf");

	enum qsv_cpu_platform plat = qsv_get_cpu_platform();
	if (plat == QSV_CPU_PLATFORM_IVB || plat == QSV_CPU_PLATFORM_SNB)
		bFrames = 0;

	int width = (int)obs_encoder_get_width(obsqsv->encoder);
	int height = (int)obs_encoder_get_height(obsqsv->encoder);
	if (astrcmpi(target_usage, "TU1") == 0)
		obsqsv->params.nTargetUsage = MFX_TARGETUSAGE_BEST_QUALITY;
	else if (astrcmpi(target_usage, "TU4") == 0)
		obsqsv->params.nTargetUsage = MFX_TARGETUSAGE_BALANCED;
	else if (astrcmpi(target_usage, "TU7") == 0)
		obsqsv->params.nTargetUsage = MFX_TARGETUSAGE_BEST_SPEED;
	else if (astrcmpi(target_usage, "TU2") == 0)
		obsqsv->params.nTargetUsage = MFX_TARGETUSAGE_2;
	else if (astrcmpi(target_usage, "TU3") == 0)
		obsqsv->params.nTargetUsage = MFX_TARGETUSAGE_3;
	else if (astrcmpi(target_usage, "TU5") == 0)
		obsqsv->params.nTargetUsage = MFX_TARGETUSAGE_5;
	else if (astrcmpi(target_usage, "TU6") == 0)
		obsqsv->params.nTargetUsage = MFX_TARGETUSAGE_6;

	if (obsqsv->codec == QSV_CODEC_AVC) {
		codec = "H.264";
		if (astrcmpi(profile, "baseline") == 0)
			obsqsv->params.nCodecProfile = MFX_PROFILE_AVC_BASELINE;
		else if (astrcmpi(profile, "main") == 0)
			obsqsv->params.nCodecProfile = MFX_PROFILE_AVC_MAIN;
		else if (astrcmpi(profile, "high") == 0)
			obsqsv->params.nCodecProfile = MFX_PROFILE_AVC_HIGH;

	} else if (obsqsv->codec == QSV_CODEC_HEVC) {
		codec = "HEVC";

		if (obsqsv->params.video_fmt_ayuv || obsqsv->params.video_fmt_y410) {
			obsqsv->params.nCodecProfile = MFX_PROFILE_HEVC_REXT;
		} else if (astrcmpi(profile, "main") == 0) {
			obsqsv->params.nCodecProfile = MFX_PROFILE_HEVC_MAIN;
			if (obs_encoder_video_tex_active(obsqsv->encoder, VIDEO_FORMAT_P010)) {
				blog(LOG_WARNING, "[qsv encoder] Forcing main10 for P010");
				obsqsv->params.nCodecProfile = MFX_PROFILE_HEVC_MAIN10;
			}

		} else if (astrcmpi(profile, "main10") == 0) {
			obsqsv->params.nCodecProfile = MFX_PROFILE_HEVC_MAIN10;
		}

	} else if (obsqsv->codec == QSV_CODEC_AV1) {
		codec = "AV1";
		obsqsv->params.nCodecProfile = MFX_PROFILE_AV1_MAIN;
	} else if (obsqsv->codec == QSV_CODEC_VP9) {
		codec = "VP9";
		/* Deterministic profile<->format mapping (spec §25): the VP9 profile
		 * follows the input surface format. 4:4:4 profiles are only reached when
		 * capability detection confirmed them and the create path allowed the
		 * matching AYUV/Y410 input. */
		if (obsqsv->params.video_fmt_p010)
			obsqsv->params.nCodecProfile = MFX_PROFILE_VP9_2;
		else if (obsqsv->params.video_fmt_y410)
			obsqsv->params.nCodecProfile = MFX_PROFILE_VP9_3;
		else if (obsqsv->params.video_fmt_ayuv)
			obsqsv->params.nCodecProfile = MFX_PROFILE_VP9_1;
		else
			obsqsv->params.nCodecProfile = MFX_PROFILE_VP9_0;
	}

	/* The settings dump below prints the raw "profile" OBS string, which is not used for VP9
	 * (its profile is derived from the input color format above). Show the derived profile so
	 * the log matches the actual MFX session (CodecProfile). */
	const char *profile_log = profile ? profile : "";
	if (obsqsv->codec == QSV_CODEC_VP9) {
		switch (obsqsv->params.nCodecProfile) {
		case MFX_PROFILE_VP9_1:
			profile_log = "VP9 Profile 1 (4:4:4 / AYUV)";
			break;
		case MFX_PROFILE_VP9_2:
			profile_log = "VP9 Profile 2 (10-bit / P010)";
			break;
		case MFX_PROFILE_VP9_3:
			profile_log = "VP9 Profile 3 (4:4:4 / Y410)";
			break;
		default:
			profile_log = "VP9 Profile 0 (4:2:0 / NV12)";
			break;
		}
	}

	obsqsv->params.VideoFormat = 5;
	obsqsv->params.VideoFullRange = voi->range == VIDEO_RANGE_FULL;

	const bool is_gbr = obs_encoder_video_tex_active(obsqsv->encoder, VIDEO_FORMAT_GBRA) ||
			    obs_encoder_video_tex_active(obsqsv->encoder, VIDEO_FORMAT_GBR10);

	/* Raw-RGB delivery (HEVC Main 4:4:4, MatrixCoefficients=0): RGB sources are remapped to
	 * Y410+SRGB+FULL by obs_qsv_video_info_hevc_tex and the Y410_RGB conversion repacks the
	 * display-encoded channels into the coded samples in GBR order (the component order the
	 * "RGB"/Identity matrix actually uses) without color conversion. The (Y410, SRGB, FULL)
	 * triple is exactly the marker obs.c uses to select that conversion, so these VUI tags
	 * always describe what the shader actually did. With an Identity matrix the decoder must
	 * interpret the samples as RGB, so per spec the remaining VUI color fields are unspecified
	 * (2) and the range is full. */
	const bool rgb_identity = (voi->format == VIDEO_FORMAT_Y410 && voi->colorspace == VIDEO_CS_SRGB &&
				   voi->range == VIDEO_RANGE_FULL);
	if (rgb_identity)
		obsqsv->params.VideoFullRange = true;

	switch (voi->colorspace) {
	case VIDEO_CS_601:
		obsqsv->params.ColourPrimaries = 6;
		obsqsv->params.TransferCharacteristics = 6;
		obsqsv->params.MatrixCoefficients = 6;
		obsqsv->params.ChromaSampleLocTypeTopField = 0;
		obsqsv->params.ChromaSampleLocTypeBottomField = 0;
		break;
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_709:
		obsqsv->params.ColourPrimaries = 1;
		obsqsv->params.TransferCharacteristics = 1;
		obsqsv->params.MatrixCoefficients = 1;
		obsqsv->params.ChromaSampleLocTypeTopField = 0;
		obsqsv->params.ChromaSampleLocTypeBottomField = 0;
		break;
	case VIDEO_CS_SRGB:
		if (rgb_identity) {
			obsqsv->params.ColourPrimaries = 2; /* unspecified */
			obsqsv->params.TransferCharacteristics = 2; /* unspecified */
			obsqsv->params.MatrixCoefficients = 0; /* Identity (RGB) */
		} else {
			obsqsv->params.ColourPrimaries = 1;
			obsqsv->params.TransferCharacteristics = 13;
			obsqsv->params.MatrixCoefficients = is_gbr ? 0 : 1;
		}
		obsqsv->params.ChromaSampleLocTypeTopField = 0;
		obsqsv->params.ChromaSampleLocTypeBottomField = 0;
		break;
	case VIDEO_CS_2100_PQ:
		obsqsv->params.ColourPrimaries = 9;
		obsqsv->params.TransferCharacteristics = 16;
		obsqsv->params.MatrixCoefficients = 9;
		obsqsv->params.ChromaSampleLocTypeTopField = 2;
		obsqsv->params.ChromaSampleLocTypeBottomField = 2;
		break;
	case VIDEO_CS_2100_HLG:
		obsqsv->params.ColourPrimaries = 9;
		obsqsv->params.TransferCharacteristics = 18;
		obsqsv->params.MatrixCoefficients = 9;
		obsqsv->params.ChromaSampleLocTypeTopField = 2;
		obsqsv->params.ChromaSampleLocTypeBottomField = 2;
	}

	const bool pq = voi->colorspace == VIDEO_CS_2100_PQ;
	const bool hlg = voi->colorspace == VIDEO_CS_2100_HLG;
	if (pq || hlg) {
		const int hdr_nominal_peak_level = pq ? (int)obs_get_video_hdr_nominal_peak_level() : (hlg ? 1000 : 0);

		obsqsv->params.DisplayPrimariesX[0] = 13250;
		obsqsv->params.DisplayPrimariesX[1] = 7500;
		obsqsv->params.DisplayPrimariesX[2] = 34000;
		obsqsv->params.DisplayPrimariesY[0] = 34500;
		obsqsv->params.DisplayPrimariesY[1] = 3000;
		obsqsv->params.DisplayPrimariesY[2] = 16000;
		obsqsv->params.WhitePointX = 15635;
		obsqsv->params.WhitePointY = 16450;
		obsqsv->params.MaxDisplayMasteringLuminance = hdr_nominal_peak_level * 10000;
		obsqsv->params.MinDisplayMasteringLuminance = 0;

		obsqsv->params.MaxContentLightLevel = hdr_nominal_peak_level;
		obsqsv->params.MaxPicAverageLightLevel = hdr_nominal_peak_level;
	}

	/* internal convenience parameter, overrides rate control param
	 * XXX: Deprecated */
	if (cbr_override) {
		warn("\"cbr\" setting has been deprecated for all encoders!  "
		     "Please set \"rate_control\" to \"CBR\" instead.  "
		     "Forcing CBR mode.  "
		     "(Note to all: this is why you shouldn't use strings for "
		     "common settings)");
		rate_control = "CBR";
	}

	if (astrcmpi(rate_control, "CBR") == 0)
		obsqsv->params.nRateControl = MFX_RATECONTROL_CBR;
	else if (astrcmpi(rate_control, "VBR") == 0)
		obsqsv->params.nRateControl = MFX_RATECONTROL_VBR;
	else if (astrcmpi(rate_control, "CQP") == 0)
		obsqsv->params.nRateControl = MFX_RATECONTROL_CQP;
	else if (astrcmpi(rate_control, "ICQ") == 0)
		obsqsv->params.nRateControl = MFX_RATECONTROL_ICQ;
	else if (astrcmpi(rate_control, "ABR") == 0)
		obsqsv->params.nRateControl = MFX_RATECONTROL_QVBR;

	obsqsv->params.nLADEPTH = (mfxU16)0;
	if (astrcmpi(latency, "ultra-low") == 0) {
		obsqsv->params.nAsyncDepth = 1;
	} else if (astrcmpi(latency, "low") == 0) {
		obsqsv->params.nAsyncDepth = 4;
		if (obsqsv->params.nRateControl == MFX_RATECONTROL_CBR ||
		    obsqsv->params.nRateControl == MFX_RATECONTROL_VBR)
			obsqsv->params.nLADEPTH = 30;
	} else if (astrcmpi(latency, "normal") == 0) {
		obsqsv->params.nAsyncDepth = 4;
		if (obsqsv->params.nRateControl == MFX_RATECONTROL_CBR ||
		    obsqsv->params.nRateControl == MFX_RATECONTROL_VBR)
			obsqsv->params.nLADEPTH = 60;
	}

	if (obsqsv->params.nLADEPTH > 0) {
		if (obsqsv->params.nLADEPTH > 100)
			obsqsv->params.nLADEPTH = 100;
		else if (obsqsv->params.nLADEPTH < 10)
			obsqsv->params.nLADEPTH = 10;
	}

	/* VP9: the Intel low-power VP9 hardware path does not support the generic
	 * QSV lookahead BRC (MFX_RATECONTROL_LA / LA_ICQ), and uses a no-B-frame
	 * pipeline (GopRefDist=1) with native LAST/GOLDEN/ALTREF references.
	 * Decouple the OBS latency setting from hidden VP9 lookahead, and force
	 * B-frames to 0 so the generic H.264/HEVC defaults (bframes=3, LA depth
	 * 30/60) do not leak into the VP9 configuration. AsyncDepth still controls
	 * the encoder pipeline latency (1 for ultra-low, 4 for low/normal). */
	if (obsqsv->codec == QSV_CODEC_VP9) {
		obsqsv->params.nLADEPTH = 0;
		bFrames = 0;
	}

	if (ver == 1) {
		obsqsv->params.nQPI = (mfxU16)qpi;
		obsqsv->params.nQPP = (mfxU16)qpp;
		obsqsv->params.nQPB = (mfxU16)qpb;
	} else {
		int actual_cqp = cqp;
		if (obsqsv->codec == QSV_CODEC_AV1)
			actual_cqp *= 4;
		obsqsv->params.nQPI = actual_cqp;
		obsqsv->params.nQPP = actual_cqp;
		obsqsv->params.nQPB = actual_cqp;
	}
	obsqsv->params.nTargetBitRate = (mfxU16)target_bitrate;
	obsqsv->params.nMaxBitRate = (mfxU16)max_bitrate;
	obsqsv->params.nWidth = (mfxU16)width;
	obsqsv->params.nHeight = (mfxU16)height;
	/* Pass the effective FPS to QSV: base rate with the frame-rate divisor
	 * folded into the denominator (e.g. 60/1 with divisor 2 -> 60/2 = 30fps),
	 * exactly like nvenc. We take the BASE rate from obs_encoder_get_fps_* (which
	 * read encoder->media) and apply the divisor ourselves - we must NOT use
	 * voi->fps_den here, because obs_encoder_video() already returns an
	 * fps_override whose fps_den carries the divisor, which would fold it twice.
	 * QSV's BRC derives per-frame bit budgets from FrameRateExtN/Den, so this
	 * keeps the bitrate calculation correct when the effective encoding rate
	 * differs from the base canvas rate. */
	obsqsv->params.nFpsNum = (mfxU16)obs_encoder_get_fps_num(obsqsv->encoder);
	obsqsv->params.nFpsDen =
		(mfxU16)(obs_encoder_get_fps_den(obsqsv->encoder) * obs_encoder_get_frame_rate_divisor(obsqsv->encoder));
	obsqsv->params.nbFrames = (mfxU16)bFrames;
	obsqsv->params.nKeyIntSec = (mfxU16)keyint_sec;
	obsqsv->params.nICQQuality = (mfxU16)icq_quality;
	obsqsv->params.bRepeatHeaders = repeat_headers;

	/* ABR (QVBR): quality-anchored VBR with a live governor. The governor steers one rate parameter around
	 * the user's "bitrate" so the long-term average converges to it - TargetKbps in target mode, MaxKbps in
	 * ceiling mode; QVBRQuality is set once here and a live reconfig only ever touches the steered param. */
	obsqsv->abr_active = (obsqsv->params.nRateControl == MFX_RATECONTROL_QVBR);
	if (obsqsv->abr_active) {
		obsqsv->abr_target_kbps = target_bitrate;
		/* The ceiling must never be below the target: it is both the governor's absolute cap (it may
		 * raise Target up to here) and the basis for the VBV buffer sizing, so a max_bitrate set below
		 * the target would clamp the stream at its own average. Fall back to the target when unset or
		 * too low. */
		obsqsv->abr_ceiling_kbps = (max_bitrate > target_bitrate ? max_bitrate : target_bitrate);
		obsqsv->abr_deadband = deadband / 100.0;
		obsqsv->params.nQVBRQuality = (mfxU16)qvbr_quality;
		obsqsv->params.nABRCeilingKbps = (mfxU16)obsqsv->abr_ceiling_kbps;

		/* ABR comparison strategy: which rate parameter the governor steers. "target" (default) is the
		 * validated baseline; "ceiling" is the Agent-1 model that moves MaxKbps instead of TargetKbps. */
		obsqsv->params.nABRStrategy = (astrcmpi(abr_strategy, "ceiling") == 0) ? QSV_ABR_STRATEGY_CEILING
		                                                                        : QSV_ABR_STRATEGY_TARGET;

		if (obsqsv->params.nABRStrategy == QSV_ABR_STRATEGY_CEILING) {
			/* Ceiling steering: TargetKbps is a fixed base anchor; the governor moves MaxKbps. */
			obsqsv->params.nTargetBitRate = (mfxU16)target_bitrate;
			obsqsv->params.nMaxBitRate = (mfxU16)obsqsv->abr_ceiling_kbps; /* latch sets governed rate */
		} else {
			/* Target steering: MaxKbps is the fixed session ceiling. */
			obsqsv->params.nMaxBitRate = (mfxU16)obsqsv->abr_ceiling_kbps;
		}

		/* Low-latency comparison mode (Agent-2 architecture): drop B-frames and the async depth so the
		 * encoder has no reorder buffer to flush, making a live reset cheap. LowDelayBRC is set in Init. */
		obsqsv->params.bABRLowLatency = abr_low_latency;
		if (abr_low_latency) {
			obsqsv->params.nbFrames = 0;
			obsqsv->params.nAsyncDepth = 1;
		}
	} else {
		obsqsv->abr_session_loaded = false; /* a non-ABR open resets the per-open latch flag */
	}

	info("settings:\n"
	     "\tcodec:          %s\n"
	     "\trate_control:   %s",
	     codec, rate_control);

	if (obsqsv->params.nRateControl != MFX_RATECONTROL_ICQ && obsqsv->params.nRateControl != MFX_RATECONTROL_CQP)
		blog(LOG_INFO, "\ttarget_bitrate: %d", (int)obsqsv->params.nTargetBitRate);

	if (obsqsv->params.nRateControl == MFX_RATECONTROL_VBR)
		blog(LOG_INFO, "\tmax_bitrate:    %d", (int)obsqsv->params.nMaxBitRate);

	if (obsqsv->params.nRateControl == MFX_RATECONTROL_QVBR)
		blog(LOG_INFO, "\tQVBR Quality:   %d (ABR, ceiling %d kbps, strategy=%s, low-latency=%s)",
		     (int)obsqsv->params.nQVBRQuality, (int)obsqsv->abr_ceiling_kbps,
		     obsqsv->params.nABRStrategy == QSV_ABR_STRATEGY_CEILING ? "ceiling(MaxKbps)" : "target",
		     obsqsv->params.bABRLowLatency ? "on" : "off");

	if (obsqsv->params.nRateControl == MFX_RATECONTROL_ICQ)
		blog(LOG_INFO, "\tICQ Quality:    %d", (int)obsqsv->params.nICQQuality);

	if (obsqsv->params.nLADEPTH)
		blog(LOG_INFO, "\tLookahead Depth:%d", (int)obsqsv->params.nLADEPTH);

	if (obsqsv->params.nRateControl == MFX_RATECONTROL_CQP)
		blog(LOG_INFO,
		     "\tqpi:            %d\n"
		     "\tqpb:            %d\n"
		     "\tqpp:            %d",
		     qpi, qpb, qpp);

	blog(LOG_INFO,
	     "\ttarget_usage:   %s\n"
	     "\tprofile:        %s\n"
	     "\tkeyint:         %d\n"
	     "\tlatency:        %s\n"
	     "\tb-frames:       %d\n"
	     "\twidth:          %d\n"
	     "\theight:         %d\n"
	     "\tFPS:            %g (%u/%u)",
	     target_usage, profile_log, keyint_sec, latency, bFrames, width, height,
	     obs_encoder_get_effective_fps(obsqsv->encoder), obs_encoder_get_fps_num(obsqsv->encoder),
	     obs_encoder_get_fps_den(obsqsv->encoder) * obs_encoder_get_frame_rate_divisor(obsqsv->encoder));

	blog(LOG_DEBUG, "debug info:");
}

/* ------------------------------------------------------------------------- */
/* ABR (QVBR) governor glue                                                    */
/*                                                                             */
/* ABR runs QSV in MFX_RATECONTROL_QVBR (quality-anchored VBR). The shared      */
/* governor (shared/abr-gov) computes a governed rate from the session-         */
/* cumulative average; we apply it as a live TargetKbps re-anchor, staged to the */
/* next keyframe boundary (qsv_check_staged). The reset starts a new sequence    */
/* (StartNewSequence=ON, one IDR) so the driver's BRC/VBV state re-anchors to the*/
/* new target immediately - a non-IDR reset would leave HEVC drifting toward it  */
/* for ~20 s. Firing on the boundary keeps multitrack keyframes aligned. MaxKbps */
/* stays the fixed ceiling.                                                      */
/* ------------------------------------------------------------------------- */

/* QSV-specific anti-churn gate. A re-anchor is a full pipeline flush + StartNewSequence=ON reset (tens of ms),
 * far costlier than the cheap per-tick parameter update the other encoders apply, so batch it: only stage a
 * keyframe-boundary re-anchor when the governed rate has moved at least this fraction from the currently-applied
 * value. Smaller moves are absorbed until they accumulate past the threshold (the boundary gate in
 * qsv_check_staged then limits frequency to at most one re-anchor per GOP). Kept below the overshoot floor's
 * 2.5% drop so a full correction always passes the gate. */
#define QSV_ABR_MIN_DELTA_FRAC 0.01

/* Convert the governor's current rate (bits/sec) into the kbps value this ABR strategy applies. In ceiling
 * mode the result is clamped to [base target, hard ceiling] so MaxKbps never drops below the fixed TargetKbps
 * anchor (an invalid QVBR config) nor above the session ceiling. */
static mfxU16 qsv_abr_governed_kbps(struct obs_qsv *obsqsv, bool ceiling)
{
	int64_t r = obsqsv->abr_state.cur_rate;

	if (ceiling) {
		const int64_t lo = (int64_t)obsqsv->abr_target_kbps * 1000;
		const int64_t hi = (int64_t)obsqsv->abr_ceiling_kbps * 1000;
		if (r < lo)
			r = lo;
		if (r > hi)
			r = hi;
	}

	return (mfxU16)(r / 1000);
}

static void qsv_abr_latch(struct obs_qsv *obsqsv)
{
	if (!obsqsv->abr_active)
		return;

	const int64_t target_bps = (int64_t)obsqsv->abr_target_kbps * 1000;
	const int64_t ceiling_bps = (int64_t)obsqsv->abr_ceiling_kbps * 1000;
	if (target_bps <= 0)
		return;

	abr_gov_config cfg = abr_gov_default_config();
	cfg.deadband_frac = obsqsv->abr_deadband;

	if (!obsqsv->abr_session_loaded) {
		abr_gov_session_load(obsqsv->encoder, true, target_bps, ceiling_bps, &cfg, &obsqsv->abr_state);
		obsqsv->abr_session_loaded = true;
	} else {
		const int64_t floor_bps = (int64_t)((double)target_bps * cfg.overshoot_floor_frac);
		if (obsqsv->abr_state.cur_rate < floor_bps)
			obsqsv->abr_state.cur_rate = floor_bps;
		if (obsqsv->abr_state.cur_rate > ceiling_bps)
			obsqsv->abr_state.cur_rate = ceiling_bps;
	}

	/* Anchor the governed rate onto the parameter this strategy steers, so a restored budget continues
	 * smoothly. Target mode moves TargetKbps; ceiling mode moves MaxKbps (kept >= the base target). */
	if (obsqsv->params.nABRStrategy == QSV_ABR_STRATEGY_CEILING)
		obsqsv->params.nMaxBitRate = qsv_abr_governed_kbps(obsqsv, true);
	else
		obsqsv->params.nTargetBitRate = qsv_abr_governed_kbps(obsqsv, false);
}

static void qsv_abr_tick(struct obs_qsv *obsqsv, uint32_t frame_bytes)
{
	if (!obsqsv->abr_active || !obsqsv->abr_session_loaded)
		return;

	const int64_t target_bps = (int64_t)obsqsv->abr_target_kbps * 1000;
	if (target_bps <= 0)
		return;

	/* Nominal per-frame duration from the effective FPS (the frame-rate divisor is already folded into
	 * nFpsDen, see update_params). */
	if (obsqsv->params.nFpsNum == 0 || obsqsv->params.nFpsDen == 0)
		return;
	const double dt = (double)obsqsv->params.nFpsDen / obsqsv->params.nFpsNum;
	if (dt <= 0.0)
		return;

	const int64_t ceiling_bps = (int64_t)obsqsv->abr_ceiling_kbps * 1000;

	abr_gov_config cfg = abr_gov_default_config();
	cfg.deadband_frac = obsqsv->abr_deadband;

	if (abr_gov_tick(&obsqsv->abr_state, &cfg, target_bps, ceiling_bps, dt, frame_bytes, NULL)) {
		/* cur_rate moved. The shared governor is a continuous law (restored for parity with the other encoders),
		 * so it nudges cur_rate on most ticks; QSV cannot afford to re-anchor on every one of those (a flush +
		 * StartNewSequence=ON reset costs tens of ms). Stage a keyframe-boundary re-anchor only when the governed
		 * rate has moved at least QSV_ABR_MIN_DELTA_FRAC from what is currently applied - smaller moves accumulate
		 * until they cross the threshold. Target mode steers TargetKbps; ceiling mode steers MaxKbps (clamped to stay
		 * >= the base target). The reset then fires at the next keyframe boundary (qsv_check_staged), at most once
		 * per GOP, so it re-anchors the BRC on-grid. */
		const bool ceiling = (obsqsv->params.nABRStrategy == QSV_ABR_STRATEGY_CEILING);
		const mfxU16 new_kbps = qsv_abr_governed_kbps(obsqsv, ceiling);
		const mfxU16 old_kbps = ceiling ? obsqsv->params.nMaxBitRate : obsqsv->params.nTargetBitRate;
		if (old_kbps > 0) {
			const int64_t diff = (new_kbps > old_kbps) ? (int64_t)(new_kbps - old_kbps)
			                                         : (int64_t)(old_kbps - new_kbps);
			if ((double)diff / (double)old_kbps >= QSV_ABR_MIN_DELTA_FRAC) {
				if (ceiling) {
					obsqsv->params.nMaxBitRate = new_kbps;
					debug("ABR governor (ceiling): MaxKbps %lld -> %lld kbps (re-anchor at next keyframe)",
					      (long long)old_kbps, (long long)new_kbps);
				} else {
					obsqsv->params.nTargetBitRate = new_kbps;
					debug("ABR governor: target %lld -> %lld kbps (re-anchor at next keyframe)",
					      (long long)old_kbps, (long long)new_kbps);
				}
				obsqsv->abr_reconfig_pending = true;
			}
		}
	}
}

static bool update_settings(struct obs_qsv *obsqsv, obs_data_t *settings)
{
	update_params(obsqsv, settings);

	/* ABR: (re-)latch the governor's session budget on every open/update and anchor the effective
	 * target at the governed rate (fresh start == user target; a restored budget continues smoothly). */
	qsv_abr_latch(obsqsv);
	return true;
}

static void load_hevc_headers(struct obs_qsv *obsqsv)
{
	DARRAY(uint8_t) header;
	DARRAY(uint8_t) sei;

	da_init(header);
	da_init(sei);

	uint8_t *pVPS, *pSPS, *pPPS;
	uint16_t nVPS, nSPS, nPPS;
	qsv_hevc_encoder_headers(obsqsv->context, &pVPS, &pSPS, &pPPS, &nVPS, &nSPS, &nPPS);
	da_push_back_array(header, pVPS, nVPS);
	da_push_back_array(header, pSPS, nSPS);
	da_push_back_array(header, pPPS, nPPS);

	obsqsv->extra_data = header.array;
	obsqsv->extra_data_size = header.num;
	obsqsv->sei = sei.array;
	obsqsv->sei_size = sei.num;
}

static void load_headers(struct obs_qsv *obsqsv)
{
	// VP9 has no SPS/PPS/VPS parameter-set model. Do NOT call the H.264/HEVC
	// header loader or manufacture parameter sets; the raw VP9 bitstream is
	// passed through directly (no IVF headers for streaming output).
	if (obsqsv->codec == QSV_CODEC_VP9) {
		obsqsv->extra_data = NULL;
		obsqsv->extra_data_size = 0;
		obsqsv->sei = NULL;
		obsqsv->sei_size = 0;
		return;
	}

	DARRAY(uint8_t) header;
	static uint8_t sei = 0;

	// Not sure if SEI is needed.
	// Just filling in empty meaningless SEI message.
	// Seems to work fine.
	// DARRAY(uint8_t) sei;

	da_init(header);
	// da_init(sei);

	uint8_t *pSPS, *pPPS;
	uint16_t nSPS, nPPS;

	qsv_encoder_headers(obsqsv->context, &pSPS, &pPPS, &nSPS, &nPPS);
	da_push_back_array(header, pSPS, nSPS);

	// AV1 does not need PPS
	if (obsqsv->codec != QSV_CODEC_AV1)
		da_push_back_array(header, pPPS, nPPS);

	obsqsv->extra_data = header.array;
	obsqsv->extra_data_size = header.num;
	obsqsv->sei = &sei;
	obsqsv->sei_size = 1;
}

/* Reset the output-timestamp correction state after any live re-init (RC Reset,
 * Close+Init FPS re-init, or resize). MFX keeps echoing the per-frame TimeStamp we
 * submit (encoder->cur_pts) as its output PresentationTimeStamp and continues its
 * DecodeTimeStamp base from where it left off, so no PTS offset is needed (ts_offset
 * stays 0). We only drop the DTS anchor and arm the post-reinit suppression window so
 * MFX's initial out-of-order B-frame DTS can't shift the offset (qsv_fix_timestamps). */
static void qsv_reset_ts_state(struct obs_qsv *obsqsv)
{
	obsqsv->ts_offset = 0;
	obsqsv->have_last_dts = false;
	obsqsv->dts_suppress = QSV_REINIT_DTS_SUPPRESS;
}

/* Mirror the MFX GopPicSize that InitParams()/UpdateParams() compute for the session
 * (nKeyIntSec * effective fps, in frames). Used to time staged reconfigure boundaries. */
static void qsv_refresh_gop_frames(struct obs_qsv *obsqsv)
{
	obsqsv->gop_frames = obsqsv->params.nKeyIntSec ?
			(int64_t)((mfxU16)(obsqsv->params.nKeyIntSec * obsqsv->params.nFpsNum / (float)obsqsv->params.nFpsDen)) : 0;
}

/* Live RC reconfig: mfxVideoEncoder::Reset() resets state; next frame is IDR. */
static bool qsv_apply_reconfigure(struct obs_qsv *obsqsv)
{
	if (!qsv_encoder_reconfig(obsqsv->context, &obsqsv->params)) {
		warn("live reconfigure failed");
		return false;
	}
	obsqsv->frames_since_idr = 0;
	if (obsqsv->codec == QSV_CODEC_HEVC)
		load_hevc_headers(obsqsv);
	else
		load_headers(obsqsv);
	qsv_reset_ts_state(obsqsv);
	return true;
}

/* ABR live RC re-anchor: changes TargetKbps via a StartNewSequence=ON Reset (ReconfigureEncoderSoft), which
 * first does a full pipeline flush (safe mid-stream) and then re-anchors the driver's BRC to the new target.
 * Because ON emits an IDR on the next frame, this restarts the GOP counter and reloads SPS/PPS + re-syncs
 * timestamps exactly like qsv_apply_reconfigure. The caller (qsv_check_staged) fires it at a keyframe
 * boundary so that IDR lands on-grid and multitrack siblings stay aligned. The underlying call logs failures. */
static bool qsv_apply_reconfigure_soft(struct obs_qsv *obsqsv)
{
	if (!qsv_encoder_reconfig_soft(obsqsv->context, &obsqsv->params)) {
		warn("ABR re-anchor failed");
		return false;
	}
	obsqsv->frames_since_idr = 0;
	if (obsqsv->codec == QSV_CODEC_HEVC)
		load_hevc_headers(obsqsv);
	else
		load_headers(obsqsv);
	qsv_reset_ts_state(obsqsv);
	return true;
}

/* Full-session re-init (Close+Init) for a live effective-FPS change. Unlike qsv_apply_reconfigure
 * (Reset only), this reapplies FrameInfo so the new frame rate actually takes effect, and it is used
 * when the effective FPS changes because libobs has already switched the frame-delivery cadence -
 * Reset() alone cannot change FrameRateExtN/Den in place. */
static bool qsv_apply_reconfigure_full(struct obs_qsv *obsqsv)
{
	if (!qsv_encoder_reconfig_full(obsqsv->context, &obsqsv->params)) {
		warn("live FPS re-init failed");
		return false;
	}
	/* The fresh session's MFX GopPicSize now follows the (possibly new) effective fps, so refresh the
	 * local mirror used to time staged reconfigure boundaries (qsv_check_staged). */
	qsv_refresh_gop_frames(obsqsv);
	obsqsv->frames_since_idr = 0;
	if (obsqsv->codec == QSV_CODEC_HEVC)
		load_hevc_headers(obsqsv);
	else
		load_headers(obsqsv);
	qsv_reset_ts_state(obsqsv);
	return true;
}

/* Nearest-neighbor NV12 scaler for the multitrack timed-resize transition period. Scales a
 * frame from the NEW resolution (the encoder's current width/height) down to the OLD one
 * (cx/cy, the size of the live MFX session) into pre-allocated buffers. Only used for the
 * short window between resize detection and the keyframe-boundary re-init, so quality is
 * not a concern (mirrors scale_frame_to_old_size in obs-x264.c). */
static void free_scale_buffers(struct obs_qsv *obsqsv)
{
	bfree(obsqsv->scale_buf[0]);
	bfree(obsqsv->scale_buf[1]);
	obsqsv->scale_buf[0] = NULL;
	obsqsv->scale_buf[1] = NULL;
	obsqsv->scale_stride[0] = 0;
	obsqsv->scale_stride[1] = 0;
}

static bool alloc_scale_buffers(struct obs_qsv *obsqsv)
{
	free_scale_buffers(obsqsv);
	const uint32_t w = obsqsv->cx;
	const uint32_t h = obsqsv->cy;
	/* P010 (AV1 10-bit) carries u16 samples: plane rows are 2 bytes/pixel. */
	const int bpp = (obsqsv->delivery_format == VIDEO_FORMAT_P010) ? 2 : 1;

	obsqsv->scale_stride[0] = ((w * bpp) + 15u) & ~15u;
	obsqsv->scale_buf[0] = bzalloc((size_t)obsqsv->scale_stride[0] * h);
	obsqsv->scale_stride[1] = obsqsv->scale_stride[0];
	obsqsv->scale_buf[1] = bzalloc((size_t)obsqsv->scale_stride[1] * (h / 2));
	return obsqsv->scale_buf[0] != NULL && obsqsv->scale_buf[1] != NULL;
}

static void qsv_scale_nv12_to_old_size(struct obs_qsv *obsqsv, const struct encoder_frame *frame)
{
	const uint32_t src_w = obs_encoder_get_width(obsqsv->encoder);
	const uint32_t src_h = obs_encoder_get_height(obsqsv->encoder);
	const uint32_t dst_w = obsqsv->cx;
	const uint32_t dst_h = obsqsv->cy;

	if (src_w == dst_w && src_h == dst_h)
		return; /* no scaling needed */

	/* Plane 0: Y (full res) */
	for (uint32_t y = 0; y < dst_h; y++) {
		const uint8_t *srow = frame->data[0] + (size_t)((y * src_h) / dst_h) * frame->linesize[0];
		uint8_t *drow = obsqsv->scale_buf[0] + (size_t)y * obsqsv->scale_stride[0];
		for (uint32_t x = 0; x < dst_w; x++)
			drow[x] = srow[(x * src_w) / dst_w];
	}

	/* Plane 1: interleaved UV (half res) */
	const uint32_t src_uv_w = src_w / 2, src_uv_h = src_h / 2;
	const uint32_t dst_uv_w = dst_w / 2, dst_uv_h = dst_h / 2;
	for (uint32_t y = 0; y < dst_uv_h; y++) {
		const uint8_t *srow = frame->data[1] + (size_t)((y * src_uv_h) / dst_uv_h) * frame->linesize[1];
		uint8_t *drow = obsqsv->scale_buf[1] + (size_t)y * obsqsv->scale_stride[1];
		for (uint32_t x = 0; x < dst_uv_w; x++) {
			const uint32_t sx = (x * src_uv_w) / dst_uv_w;
			drow[x * 2]     = srow[sx * 2];     /* U */
			drow[x * 2 + 1] = srow[sx * 2 + 1]; /* V */
		}
	}
}

/* P010 (u16) variant of the above for the AV1 10-bit delivery format. Same NV12-style layout
 * (Y plane + interleaved UV at half res), just 16-bit samples. */
static void qsv_scale_p010_to_old_size(struct obs_qsv *obsqsv, const struct encoder_frame *frame)
{
	const uint32_t src_w = obs_encoder_get_width(obsqsv->encoder);
	const uint32_t src_h = obs_encoder_get_height(obsqsv->encoder);
	const uint32_t dst_w = obsqsv->cx;
	const uint32_t dst_h = obsqsv->cy;

	if (src_w == dst_w && src_h == dst_h)
		return; /* no scaling needed */

	/* Plane 0: Y (full res, u16 samples) */
	for (uint32_t y = 0; y < dst_h; y++) {
		const uint16_t *srow = (const uint16_t *)(frame->data[0] + (size_t)((y * src_h) / dst_h) * frame->linesize[0]);
		uint16_t *drow = (uint16_t *)(obsqsv->scale_buf[0] + (size_t)y * obsqsv->scale_stride[0]);
		for (uint32_t x = 0; x < dst_w; x++)
			drow[x] = srow[(x * src_w) / dst_w];
	}

	/* Plane 1: interleaved UV (half res, u16 pairs) */
	const uint32_t src_uv_w = src_w / 2, src_uv_h = src_h / 2;
	const uint32_t dst_uv_w = dst_w / 2, dst_uv_h = dst_h / 2;
	for (uint32_t y = 0; y < dst_uv_h; y++) {
		const uint16_t *srow = (const uint16_t *)(frame->data[1] + (size_t)((y * src_uv_h) / dst_uv_h) * frame->linesize[1]);
		uint16_t *drow = (uint16_t *)(obsqsv->scale_buf[1] + (size_t)y * obsqsv->scale_stride[1]);
		for (uint32_t x = 0; x < dst_uv_w; x++) {
			const uint32_t sx = (x * src_uv_w) / dst_uv_w;
			drow[x * 2]     = srow[sx * 2];     /* U */
			drow[x * 2 + 1] = srow[sx * 2 + 1]; /* V */
		}
	}
}

static bool qsv_apply_resize(struct obs_qsv *obsqsv, uint32_t w, uint32_t h)
{
	info("live resize: %ux%u -> %ux%u (session re-init)", obsqsv->cx, obsqsv->cy, w, h);
	obsqsv->params.nWidth = (mfxU16)w;
	obsqsv->params.nHeight = (mfxU16)h;

	if (!qsv_encoder_reset_full(obsqsv->context, &obsqsv->params, obsqsv->codec)) {
		warn("live resize failed, restart required");
		return false; /* cx/cy unchanged - the pending resize is retried on the next frame */
	}
	obsqsv->cx = w;
	obsqsv->cy = h;
	/* The fresh session restarts its GOP at this point. */
	obsqsv->frames_since_idr = 0;
	if (obsqsv->codec == QSV_CODEC_HEVC)
		load_hevc_headers(obsqsv);
	else
		load_headers(obsqsv);
	info("resize reconfigure accepted");
	qsv_reset_ts_state(obsqsv);
	return true;
}

/* Next shared multitrack keyframe boundary strictly ahead of frame_pts, from the measured
 * on-grid keyframe cadence (last_keyframe_pts + gop_pts). Requires gop_pts > 0. */
static int64_t qsv_next_boundary_pts(const struct obs_qsv *obsqsv, int64_t frame_pts)
{
	int64_t target = obsqsv->last_keyframe_pts + obsqsv->gop_pts;
	while (target <= frame_pts)
		target += obsqsv->gop_pts;
	return target;
}

/* Apply a staged full re-init at the shared keyframe boundary: carries any pending size
 * change and the current effective FPS (params already updated at staging time) in one
 * Close+Init so the session-start IDR lands exactly on-grid. On failure the pending state
 * is kept so the next frame retries. */
static bool qsv_apply_timed_reinit(struct obs_qsv *obsqsv)
{
	if (obsqsv->resize_pending) {
		info("live resize: %ux%u -> %ux%u (session re-init)", obsqsv->cx, obsqsv->cy, obsqsv->pending_width,
		     obsqsv->pending_height);
		obsqsv->params.nWidth = (mfxU16)obsqsv->pending_width;
		obsqsv->params.nHeight = (mfxU16)obsqsv->pending_height;
	}

	bool ok;
	if (obsqsv->fps_pending) {
		/* UpdateParams() syncs FrameRateExtN/Den and the fps-derived GopPicSize into the MFX params
		 * before the Close+Init, so both take effect in the fresh session. */
		info("live FPS re-init: %u/%u (session re-init)", obsqsv->params.nFpsNum, obsqsv->params.nFpsDen);
		ok = qsv_encoder_reconfig_full(obsqsv->context, &obsqsv->params);
	} else {
		ok = qsv_encoder_reset_full(obsqsv->context, &obsqsv->params, obsqsv->codec);
	}

	if (!ok) {
		warn("timed re-init failed");
		return false;
	}

	if (obsqsv->resize_pending) {
		obsqsv->cx = obsqsv->pending_width;
		obsqsv->cy = obsqsv->pending_height;
		info("resize reconfigure accepted");
	} else {
		info("FPS re-init accepted");
	}

	/* The fresh session restarts its GOP at this point. */
	obsqsv->frames_since_idr = 0;
	if (obsqsv->fps_pending)
		qsv_refresh_gop_frames(obsqsv);
	if (obsqsv->codec == QSV_CODEC_HEVC)
		load_hevc_headers(obsqsv);
	else
		load_headers(obsqsv);
	qsv_reset_ts_state(obsqsv);
	return true;
}

/* Timed multitrack re-init (staged in qsv_maybe_resize / qsv_check_staged): at the shared
 * keyframe boundary, re-init the MFX session carrying any pending size and/or effective-FPS
 * change. The first frame of the fresh session is a session-start IDR that lands exactly
 * on-grid, so this track's keyframes stay aligned with its sibling tracks. Returns true if
 * the current frame may be encoded (nothing due, or the re-init just succeeded); false if
 * it must be skipped because the re-init failed (pending state kept for the next frame). */
static bool qsv_maybe_apply_timed_reinit(struct obs_qsv *obsqsv, int64_t frame_pts)
{
	if (frame_pts < 0 || (!obsqsv->resize_pending && !obsqsv->fps_pending))
		return true;

	if (frame_pts < obsqsv->reinit_target_pts)
		return true; // boundary not reached yet - keep encoding with the current session

	if (frame_pts > obsqsv->reinit_target_pts) {
		/* The boundary frame was dropped (encoder queue overflow while the encoder was busy).
		 * Re-initialising now would land the session-start IDR off-grid, so advance to the next
		 * shared boundary instead; give up after two misses. */
		obsqsv->reinit_misses++;
		if (obsqsv->reinit_misses < 2) {
			obsqsv->reinit_target_pts += obsqsv->gop_pts;
			warn("timed re-init missed keyframe boundary (pts %lld), retrying at pts %lld",
			     (long long)frame_pts, (long long)obsqsv->reinit_target_pts);
		} else {
			warn("timed re-init missed two keyframe boundaries - applying late at pts %lld (off-grid)",
			     (long long)frame_pts);
		}
	}

	if (frame_pts == obsqsv->reinit_target_pts || obsqsv->reinit_misses >= 2) {
		if (obsqsv->resize_pending)
			info("timed resize: applying %ux%u -> %ux%u at pts %lld (target %lld)",
			     obsqsv->cx, obsqsv->cy, obsqsv->pending_width, obsqsv->pending_height,
			     (long long)frame_pts, (long long)obsqsv->reinit_target_pts);
		if (obsqsv->fps_pending)
			info("timed FPS re-init: applying at pts %lld (target %lld)", (long long)frame_pts,
			     (long long)obsqsv->reinit_target_pts);

		bool ok = qsv_apply_timed_reinit(obsqsv);
		free_scale_buffers(obsqsv);
		if (!ok) {
			error("timed re-init failed - restart required");
			return false; // skip this frame - the pending change is retried on the next frame
		}
		obsqsv->resize_pending = false;
		obsqsv->fps_pending = false;
		/* The full re-init already applied the current RC/bitrate, so drop any staged
		 * RC-only reconfigure to avoid a redundant Reset (and extra IDR) right after. */
		obsqsv->reconfig_pending = false;
		/* The session-start IDR re-anchors the grid; update tracking. */
		obsqsv->last_keyframe_pts = frame_pts;
	}

	return true;
}

/* Common completion bookkeeping for a timed re-init (synchronous path or background worker):
 * frees the staging scale buffers and, on success, clears the pending flags and re-anchors the
 * keyframe grid tracking at the boundary pts. On failure the pending state is kept so the next
 * frame retries (matches the synchronous path's semantics). */
static void qsv_timed_reinit_done(struct obs_qsv *obsqsv, bool ok, int64_t boundary_pts)
{
	free_scale_buffers(obsqsv);
	if (!ok) {
		error("timed re-init failed - restart required");
		return;
	}
	obsqsv->resize_pending = false;
	obsqsv->fps_pending = false;
	/* The full re-init already applied the current RC/bitrate, so drop any staged RC-only
	 * reconfigure to avoid a redundant Reset (and extra IDR) right after. */
	obsqsv->reconfig_pending = false;
	/* The session-start IDR re-anchors the grid; update tracking. */
	obsqsv->last_keyframe_pts = boundary_pts;
}

/* Background worker for qsv_maybe_apply_timed_reinit_bg: runs the ~400 ms MFX Close+Init off
 * the encode thread. Takes g_QsvLock so it is mutually exclusive with every other MFX access
 * (encode calls, reconfig, close); the mix thread never waits on this worker, so no deadlock. */
static void *qsv_reinit_worker(void *data)
{
	struct obs_qsv *obsqsv = data;

	pthread_mutex_lock(&g_QsvLock);
	bool ok = qsv_apply_timed_reinit(obsqsv);
	qsv_timed_reinit_done(obsqsv, ok, obsqsv->reinit_boundary_pts);
	pthread_mutex_unlock(&g_QsvLock);

	os_event_signal(obsqsv->reinit_done);
	return NULL;
}

/* CPU-frame path variant of qsv_maybe_apply_timed_reinit: at the shared keyframe boundary,
 * start the re-init on a background worker instead of blocking the encode thread (see
 * reinit_in_progress). Returns true if the current frame may be encoded normally; false if it
 * must be queued by the caller because a re-init is starting or already in progress. */
static bool qsv_maybe_apply_timed_reinit_bg(struct obs_qsv *obsqsv, int64_t frame_pts)
{
	if (frame_pts < 0 || (!obsqsv->resize_pending && !obsqsv->fps_pending))
		return true;

	if (frame_pts < obsqsv->reinit_target_pts)
		return true; // boundary not reached yet - keep encoding with the current session

	if (frame_pts > obsqsv->reinit_target_pts) {
		/* The boundary frame was dropped (encoder queue overflow while the encoder was busy).
		 * Re-initialising now would land the session-start IDR off-grid, so advance to the next
		 * shared boundary instead; give up after two misses. */
		obsqsv->reinit_misses++;
		if (obsqsv->reinit_misses < 2) {
			obsqsv->reinit_target_pts += obsqsv->gop_pts;
			warn("timed re-init missed keyframe boundary (pts %lld), retrying at pts %lld",
			     (long long)frame_pts, (long long)obsqsv->reinit_target_pts);
		} else {
			warn("timed re-init missed two keyframe boundaries - applying late at pts %lld (off-grid)",
			     (long long)frame_pts);
		}
	}

	if (frame_pts == obsqsv->reinit_target_pts || obsqsv->reinit_misses >= 2) {
		if (obsqsv->resize_pending)
			info("timed resize: applying %ux%u -> %ux%u at pts %lld (target %lld, background)",
			     obsqsv->cx, obsqsv->cy, obsqsv->pending_width, obsqsv->pending_height,
			     (long long)frame_pts, (long long)obsqsv->reinit_target_pts);
		if (obsqsv->fps_pending)
			info("timed FPS re-init: applying at pts %lld (target %lld, background)",
			     (long long)frame_pts, (long long)obsqsv->reinit_target_pts);

		obsqsv->reinit_boundary_pts = frame_pts;
		os_event_init(&obsqsv->reinit_done, OS_EVENT_TYPE_MANUAL);
		if (pthread_create(&obsqsv->reinit_thread, NULL, qsv_reinit_worker, obsqsv) != 0) {
			/* Thread spawn failed: fall back to the synchronous re-init on this thread. */
			os_event_destroy(obsqsv->reinit_done);
			obsqsv->reinit_done = NULL;
			pthread_mutex_lock(&g_QsvLock);
			bool ok = qsv_apply_timed_reinit(obsqsv);
			qsv_timed_reinit_done(obsqsv, ok, frame_pts);
			pthread_mutex_unlock(&g_QsvLock);
			return ok; // false -> skip this frame (pending state kept for retry)
		}
		obsqsv->reinit_in_progress = true;
		return false; // caller queues this frame - it becomes the first frame of the new session
	}

	return true;
}

/* Deep-copy one incoming frame into the re-init queue (see reinit_in_progress). NV12/P010 are
 * both two-plane 4:2:0, so plane sizes follow from linesize and the current target height. */
static void qsv_pending_enqueue(struct obs_qsv *obsqsv, const struct encoder_frame *frame)
{
	if (obsqsv->pending_frames.num >= 128) {
		warn("background re-init queue full - dropping frame pts %lld", (long long)frame->pts);
		return;
	}

	const uint32_t h = obs_encoder_get_height(obsqsv->encoder);
	const size_t y_size  = (size_t)frame->linesize[0] * h;
	const size_t uv_size = (size_t)frame->linesize[1] * (h / 2);

	struct qsv_pending_frame *pf = da_push_back_new(obsqsv->pending_frames);
	pf->y  = bmalloc(y_size);
	pf->uv = bmalloc(uv_size);
	memcpy(pf->y, frame->data[0], y_size);
	memcpy(pf->uv, frame->data[1], uv_size);
	pf->ls0 = frame->linesize[0];
	pf->ls1 = frame->linesize[1];
	pf->pts = frame->pts;
}

/* Pop the oldest queued frame (FIFO - DTS order) and transfer ownership of its copies out. */
static void qsv_pending_dequeue(struct obs_qsv *obsqsv, struct qsv_pending_frame *out)
{
	*out = obsqsv->pending_frames.array[0];
	/* Shift the remaining entries one slot toward the front. The dequeued head is now owned by
	 * the caller (who releases its copies); every other entry stays live in the queue and must
	 * NOT be freed here - that would double-free / use-after-free the still-queued frames. */
	for (size_t i = 1; i < obsqsv->pending_frames.num; i++)
		obsqsv->pending_frames.array[i - 1] = obsqsv->pending_frames.array[i];
	da_resize(obsqsv->pending_frames, obsqsv->pending_frames.num - 1);
}

/* Free any frames still buffered in the re-init queue (called on destroy). */
static void qsv_pending_free(struct obs_qsv *obsqsv)
{
	for (size_t i = 0; i < obsqsv->pending_frames.num; i++) {
		bfree(obsqsv->pending_frames.array[i].y);
		bfree(obsqsv->pending_frames.array[i].uv);
	}
	da_free(obsqsv->pending_frames);
}

static bool qsv_maybe_resize(struct obs_qsv *obsqsv, int64_t frame_pts, uint32_t frame_linesize_y,
			     enum video_format frame_format)
{
	uint32_t w = obs_encoder_get_width(obsqsv->encoder);
	uint32_t h = obs_encoder_get_height(obsqsv->encoder);
	if (w == obsqsv->cx && h == obsqsv->cy)
		return true; // no resize needed - safe to encode

	/* The CPU scaler covers the frame delivery formats NV12 and P010 (AV1 10-bit). */
	const bool scalable = (frame_format == VIDEO_FORMAT_NV12 || frame_format == VIDEO_FORMAT_P010);

	/* Race guard: obs_encoder_set_scaled_size updates scaled_width/height
	 * immediately, but the GPU mix rebind takes effect on the next tick.
	 * If the incoming frame's Y-plane stride is smaller than the target
	 * width, the frame is still at the old size — defer to next tick.
	 * Compare in pixels: P010 plane-0 strides are 2 bytes/pixel. */
	const uint32_t ls0_px = (frame_format == VIDEO_FORMAT_P010) ? frame_linesize_y / 2 : frame_linesize_y;
	if (ls0_px < w)
		return true; // not yet at new size, will retry next frame

	/* Multitrack (encoder group): never re-init off-grid. Group members keep keyframes
	 * aligned on a shared GOP grid anchored at stream start; an MFX session re-init
	 * mid-GOP emits its session-start IDR at the resize point and re-anchors this track's
	 * GOP phase there, so every subsequent keyframe would sit off-grid and libobs would
	 * report "Missing keyframe" for the whole group until the stream restarts. Instead,
	 * stage a TIMED resize (mirrors obs_x264_maybe_resize): defer the re-init to the next
	 * shared keyframe boundary so that session-start IDR lands exactly on-grid, and keep
	 * encoding new-size frames scaled down to the old resolution until then. No off-grid
	 * keyframe is ever emitted. */
	if (obs_encoder_is_in_group(obsqsv->encoder) && scalable &&
	    obsqsv->last_keyframe_pts >= 0 && obsqsv->gop_pts > 0) {
		if (!obsqsv->resize_pending) {
			/* If a staged FPS re-init already holds the shared boundary, keep it so both
			 * changes are carried by the same Close+Init. */
			int64_t target = obsqsv->fps_pending ? obsqsv->reinit_target_pts :
			                  qsv_next_boundary_pts(obsqsv, frame_pts);

			if (alloc_scale_buffers(obsqsv)) {
				obsqsv->resize_pending    = true;
				obsqsv->pending_width     = w;
				obsqsv->pending_height    = h;
				if (!obsqsv->fps_pending) {
					obsqsv->reinit_target_pts = target;
					obsqsv->reinit_misses     = 0;
				}
				/* A timed resize is starting: any frames still queued from an earlier
				 * background re-init are at the OLD size and cannot be encoded by the
				 * upcoming session - drop them (one-time gap; the first resize of a session
				 * always starts with an empty queue and stays clean). */
				if (obsqsv->pending_frames.num > 0) {
					warn("dropping %zu queued frame(s) of the previous size for a new timed resize",
					     obsqsv->pending_frames.num);
					qsv_pending_free(obsqsv);
				}
				info("live resize %ux%u -> %ux%u staged (will apply at pts %lld, current pts %lld)",
				     obsqsv->cx, obsqsv->cy, w, h, (long long)target, (long long)frame_pts);
			} else {
				warn("live resize staging failed to allocate scale buffers - falling back to immediate re-init");
			}
		} else {
			/* A newer size arrived mid-wait: re-target the staged re-init. */
			obsqsv->pending_width  = w;
			obsqsv->pending_height = h;
		}

		if (obsqsv->resize_pending)
			return true; // keep encoding (scaled) with the current session
	}

	if (!qsv_apply_resize(obsqsv, w, h)) {
		error("live resize to %ux%u failed - restart required", w, h);
		return false; // resize FAILED - encoder is broken
	}

	/* Multitrack fallback (resize within the first GOP, before any keyframe cadence was
	 * observed, or a delivery format the CPU scaler cannot handle): the re-init landed off-grid.
	 * Never force a realignment IDR here - that would desync this track from its siblings; accept
	 * the one-time grid shift instead (the session-start IDR is the only keyframe emitted). */
	if (obs_encoder_is_in_group(obsqsv->encoder)) {
		info("resize applied off-grid (no timed staging possible); skipping IDR realignment (multitrack)");
		return true;
	}

	video_t *video = obs_encoder_video(obsqsv->encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	int64_t g = (int64_t)obsqsv->params.nKeyIntSec;
	if (g > 0)
		g *= voi->fps_num / voi->fps_den;

	if (!obs_encoder_active(obsqsv->encoder) || g <= 0 || obsqsv->output_delay < 2) {
		info("resize applied off-grid; alignment not staged");
		return true;
	}

	const int64_t s = g - ((int64_t)obsqsv->output_delay - 1);
	const int64_t p = obsqsv->frames_since_idr + 1;
	int64_t delta;
	if (p < s)      delta = s - p;
	else if (p > s) delta = g - (p - s);
	else            delta = 0;

	if (delta == 0) {
		info("resize on keyframe boundary - alignment preserved");
		obsqsv->reconfig_pending = false;
	} else {
		obsqsv->align_pending   = true;
		obsqsv->align_remaining = delta + 1;
		info("staged resize realignment (%lld frames)", (long long)delta);
	}
	return true;
}

static bool qsv_check_staged(struct obs_qsv *obsqsv, int64_t frame_pts, uint32_t frame_linesize_y,
			     enum video_format frame_format)
{
	++obsqsv->frames_since_idr;
	if (!qsv_maybe_resize(obsqsv, frame_pts, frame_linesize_y, frame_format))
		return false; // resize failed - encoder is broken, skip this frame

	/* Detect a live effective-FPS change (base rate or frame-rate divisor). This runs per-frame on the
	 * encode thread so it catches divisor changes made via obs_encoder_update_frame_rate_divisor(), which
	 * do NOT go through update() and would otherwise never be detected. */
	const uint32_t pending_div = obs_encoder_get_pending_frame_rate_divisor(obsqsv->encoder);

	/* (a) A divisor change staged by the frontend via obs_encoder_set_pending_frame_rate_divisor()
	 * (multitrack live FPS change): apply it exactly on the next shared keyframe boundary. libobs switches
	 * the delivery rate there (set_frame_rate_divisor_at_pts) and we re-init the MFX session there, so the
	 * session-start IDR lands on-grid. While waiting, frames keep arriving at the OLD rate, which matches
	 * the current MFX session - no transient mismatch (mirrors obs_x264_check_fps). */
	if (pending_div != 0 && pending_div != obs_encoder_get_frame_rate_divisor(obsqsv->encoder) &&
	    !obsqsv->fps_pending && !obsqsv->fps_reinit_pending) {
		const uint32_t fps_num = obs_encoder_get_fps_num(obsqsv->encoder);
		const uint32_t fps_den = obs_encoder_get_fps_den(obsqsv->encoder) * pending_div;

		if (fps_num != 0 && fps_den != 0) {
			info("live FPS change %u/%u -> %u/%u, staging boundary-aligned re-init", obsqsv->params.nFpsNum,
			     obsqsv->params.nFpsDen, fps_num, fps_den);
			obsqsv->params.nFpsNum = (mfxU16)fps_num;
			obsqsv->params.nFpsDen = (mfxU16)fps_den;

			int64_t target;
			if (obs_encoder_active(obsqsv->encoder) && obsqsv->last_keyframe_pts >= 0 &&
			    obsqsv->gop_pts > 0) {
				target = qsv_next_boundary_pts(obsqsv, frame_pts);
			} else {
				/* No on-grid cadence measured yet (change within the first GOP): best effort at
				 * the next frame. */
				info("no keyframe cadence observed yet; applying FPS change at the next frame");
				target = frame_pts + 1;
			}

			obs_encoder_set_frame_rate_divisor_at_pts(obsqsv->encoder, pending_div, target);
			if (!obsqsv->resize_pending) {
				obsqsv->reinit_target_pts = target;
				obsqsv->reinit_misses     = 0;
			}
			obsqsv->fps_pending = true;
			info("live FPS change staged (will apply at pts %lld, current pts %lld)", (long long)target,
			     (long long)frame_pts);
			obs_encoder_set_pending_frame_rate_divisor(obsqsv->encoder, 0);
		}
	}

	/* (b) Any other effective-FPS change (base-rate change, or a divisor set immediately via
	 * obs_encoder_update_frame_rate_divisor): the delivery rate has ALREADY switched. A grouped encoder
	 * still stages the full re-init at the next shared boundary; others apply it now. */
	if (!obsqsv->fps_pending && !obsqsv->fps_reinit_pending && pending_div == 0) {
		const uint32_t fps_num = obs_encoder_get_fps_num(obsqsv->encoder);
		const uint32_t fps_den = obs_encoder_get_fps_den(obsqsv->encoder) *
		                          obs_encoder_get_frame_rate_divisor(obsqsv->encoder);
		if (fps_num != 0 && fps_den != 0 &&
		    ((mfxU16)fps_num != obsqsv->params.nFpsNum || (mfxU16)fps_den != obsqsv->params.nFpsDen)) {
			info("live FPS change %u/%u -> %u/%u while streaming", obsqsv->params.nFpsNum,
			     obsqsv->params.nFpsDen, fps_num, fps_den);
			obsqsv->params.nFpsNum = (mfxU16)fps_num;
			obsqsv->params.nFpsDen = (mfxU16)fps_den;
			obsqsv->fps_reinit_pending = true;
		}
	}

	if (obsqsv->fps_reinit_pending) {
		if (!obsqsv->fps_pending) {
			if (obs_encoder_is_in_group(obsqsv->encoder) && obsqsv->last_keyframe_pts >= 0 &&
			    obsqsv->gop_pts > 0) {
				/* Grouped: an immediate Close+Init would emit its session-start IDR off-grid and
				 * re-anchor this track's GOP phase there, desyncing every subsequent keyframe from
				 * its siblings. Defer the re-init to the next shared keyframe boundary (same
				 * mechanism as the timed resize); never force a realignment IDR. */
				if (!obsqsv->resize_pending) {
					obsqsv->reinit_target_pts = qsv_next_boundary_pts(obsqsv, frame_pts);
					obsqsv->reinit_misses     = 0;
				}
				obsqsv->fps_pending = true;
				info("live FPS change staged (will apply at pts %lld, current pts %lld)",
				     (long long)obsqsv->reinit_target_pts, (long long)frame_pts);
			} else {
				if (obs_encoder_is_in_group(obsqsv->encoder))
					info("FPS re-init applied off-grid (no timed staging possible); skipping IDR realignment (multitrack)");
				else
					info("applying live FPS re-init now");
				bool ok = qsv_apply_reconfigure_full(obsqsv);
				if (!ok)
					warn("live FPS re-init failed - restart required");
			}
		}
		obsqsv->fps_reinit_pending = false;
		/* The full re-init already applied the current RC/bitrate, so drop any staged
		 * RC-only reconfigure to avoid a redundant Reset (and extra IDR) right after. */
		obsqsv->reconfig_pending = false;
	}

	/* Use the MFX GopPicSize mirrored in gop_frames (keyint_sec * effective fps, as set for the
	 * live session; qsv_refresh_gop_frames() keeps it in sync after a live FPS re-init). Timing the
	 * boundary off this value matches the encoder's actual GOP, so the staged Reset lands exactly on
	 * MFX's own IDR. */
	const int64_t gop_length = obsqsv->gop_frames;
	const int64_t pipeline_delay = (int64_t)obsqsv->output_delay - 1;
	const int64_t keyframe_boundary = gop_length - pipeline_delay;

	/* ABR re-anchor fire point, tuned separately from the manual-reconfig boundary above.
	 *
	 * The StartNewSequence=ON reset emits its IDR a few frames after Reset(); fired at
	 * keyframe_boundary that lands ~6 frames AFTER the natural grid IDR in the live QSV pipeline,
	 * and the flush re-delivers the still-in-flight natural IDR on top of it => double IDR. Firing
	 * that many frames EARLIER keeps the natural IDR out of the flush and makes the ON-reset's IDR
	 * land exactly on the grid slot (re-anchoring the GOP counter, so no competing natural IDR):
	 * a single on-grid IDR. Empirically tuned for the live pipeline (see abr-findings §37); re-measure
	 * if gop/fps/output_delay change. */
	const int64_t abr_early_fire = 6;
	const int64_t abr_keyframe_boundary = keyframe_boundary - abr_early_fire;

	if (obsqsv->align_pending) {
		if (--obsqsv->align_remaining == 0) {
			info("resize realignment: forcing IDR at keyframe boundary (counter=%lld gop=%lld)",
			     (long long)obsqsv->frames_since_idr, (long long)gop_length);
			qsv_encoder_force_idr(obsqsv->context);
			obsqsv->align_pending = false;
			obsqsv->frames_since_idr = 0;
		}
	} else if (obsqsv->reconfig_pending && gop_length > 0 && obsqsv->frames_since_idr >= keyframe_boundary) {
		info("firing staged reconfigure at keyframe boundary (counter=%lld gop=%lld)",
		     (long long)obsqsv->frames_since_idr, (long long)gop_length);
		bool ok = qsv_apply_reconfigure(obsqsv);
		obsqsv->reconfig_pending = false;
		if (!ok)
			warn("staged reconfigure failed - restart required");
	} else if (obsqsv->abr_reconfig_pending &&
	           (gop_length <= 0 || obsqsv->frames_since_idr >= abr_keyframe_boundary)) {
		/* ABR re-anchor: fire the StartNewSequence=ON reset here so its IDR lands on the natural GOP
		 * keyframe slot (multitrack-safe); see the abr_keyframe_boundary note above. With no GOP there
		 * is no grid to align to, so apply immediately. */
		info("firing ABR re-anchor at keyframe boundary (counter=%lld gop=%lld abr_boundary=%lld)",
		     (long long)obsqsv->frames_since_idr, (long long)gop_length, (long long)abr_keyframe_boundary);
		bool ok = qsv_apply_reconfigure_soft(obsqsv);
		obsqsv->abr_reconfig_pending = false;
		if (!ok)
			warn("ABR re-anchor failed - keeping previous target");
	}
	return true; // safe to proceed with encoding
}

static bool obs_qsv_update(void *data, obs_data_t *settings)
{
	struct obs_qsv *obsqsv = data;

	/* Capture the effective FPS currently active in the encoder BEFORE
	 * update_settings() overwrites params. This is what was last applied to
	 * the MFX session, so we can tell whether a live FPS change (base frame
	 * rate or frame-rate divisor) needs pushing - at an IDR boundary, exactly
	 * like nvenc. */
	mfxU16 old_fps_num = obsqsv->params.nFpsNum;
	mfxU16 old_fps_den = obsqsv->params.nFpsDen;
	/* Capture the RC/bitrate currently active BEFORE update_settings() overwrites params: it
	 * unconditionally writes the new bitrate into params, so reading params afterwards would only
	 * ever compare a value against itself. These are what was last applied to the MFX session. */
	mfxU16 old_target = obsqsv->params.nTargetBitRate;
	mfxU16 old_max    = obsqsv->params.nMaxBitRate;

	update_settings(obsqsv, settings);

	/* Detect an effective-FPS change. QSV's BRC derives per-frame bit budgets
	 * from FrameRateExtN/Den (which now carry the frame-rate divisor folded into
	 * the denominator - see update_params), so the encoder must be reconfigured
	 * whenever the effective rate changes to keep the bitrate calculation correct. */
	bool fps_changed = (obsqsv->params.nFpsNum != old_fps_num || obsqsv->params.nFpsDen != old_fps_den);
	if (fps_changed) {
		info("changing effective FPS from %u/%u to %u/%u while streaming",
		     old_fps_num, old_fps_den, obsqsv->params.nFpsNum, obsqsv->params.nFpsDen);
	}

	const char *rc_name = obs_data_get_string(settings, "rate_control");

	bool rc_changed = false;
	bool br_changed = false;

	/* Compare the freshly-updated params against the values captured above (what was last applied to the
	 * MFX session). CBR tracks TargetKbps only; VBR tracks both TargetKbps and MaxKbps. Quality-based modes
	 * (CQP/ICQ) always need a reconfigure on any update, matching prior behavior. */
	switch (obsqsv->params.nRateControl) {
	case MFX_RATECONTROL_CBR:
		br_changed = (obsqsv->params.nTargetBitRate != old_target);
		break;
	case MFX_RATECONTROL_VBR:
		br_changed = (obsqsv->params.nTargetBitRate != old_target) ||
		             (obsqsv->params.nMaxBitRate != old_max);
		break;
	case MFX_RATECONTROL_QVBR:
		/* ABR: the governor owns TargetKbps; a user settings change re-anchors it, so track like VBR. */
		br_changed = (obsqsv->params.nTargetBitRate != old_target) ||
		             (obsqsv->params.nMaxBitRate != old_max);
		break;
	case MFX_RATECONTROL_ICQ:
	case MFX_RATECONTROL_CQP:
		rc_changed = true;
		break;
	default:
		break;
	}

	if (astrcmpi(rc_name, "CBR") == 0 && obsqsv->params.nRateControl != MFX_RATECONTROL_CBR)
		rc_changed = true;
	else if (astrcmpi(rc_name, "VBR") == 0 && obsqsv->params.nRateControl != MFX_RATECONTROL_VBR)
		rc_changed = true;
	else if (astrcmpi(rc_name, "ICQ") == 0 && obsqsv->params.nRateControl != MFX_RATECONTROL_ICQ)
		rc_changed = true;
	else if (astrcmpi(rc_name, "CQP") == 0 && obsqsv->params.nRateControl != MFX_RATECONTROL_CQP)
		rc_changed = true;

	/* A live effective-FPS change needs a full session re-init: QSV's BRC derives per-frame bit
	 * budgets from FrameRateExtN/Den, and Reset() alone cannot change FrameInfo. qsv_check_staged
	 * applies it - staged at the next shared keyframe boundary for grouped encoders (an off-grid
	 * session-start IDR would desync the whole group), immediately otherwise. The full re-init also
	 * carries any concurrent RC/bitrate change, so nothing else needs staging here. */
	if (fps_changed) {
		obsqsv->fps_reinit_pending = true;
		return true;
	}

	if (!rc_changed && !br_changed)
		return true;

	/* ABR (QVBR): the governor owns TargetKbps and its changes are applied with a StartNewSequence=ON
	 * re-anchor. A manual settings change has already re-anchored the governor in qsv_abr_latch
	 * (update_settings), so this just pushes the new target to the driver: immediately when not streaming,
	 * otherwise staged to the next keyframe boundary (qsv_check_staged) so the inserted IDR lands on-grid. */
	if (obsqsv->params.nRateControl == MFX_RATECONTROL_QVBR) {
		if (!obs_encoder_active(obsqsv->encoder)) {
			info("applying ABR re-anchor immediately (not streaming)");
			return qsv_apply_reconfigure_soft(obsqsv);
		}
		obsqsv->abr_reconfig_pending = true;
		return true;
	}

	video_t *video = obs_encoder_video(obsqsv->encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	int64_t gop_length = (int64_t)obsqsv->params.nKeyIntSec;
	if (gop_length > 0)
		gop_length *= voi->fps_num / voi->fps_den;

	if (!obs_encoder_active(obsqsv->encoder) || gop_length <= 0) {
		info("applying reconfigure immediately (not streaming or no GOP)");
		return qsv_apply_reconfigure(obsqsv);
	}

	info("staged encoder change - applying at next keyframe (gop=%lld counter=%lld)",
	     (long long)gop_length, (long long)obsqsv->frames_since_idr);
	obsqsv->reconfig_pending = true;
	return true;
}

static void *obs_qsv_create(enum qsv_codec codec, obs_data_t *settings, obs_encoder_t *encoder, bool useTexAlloc)
{
	struct obs_qsv *obsqsv = bzalloc(sizeof(struct obs_qsv));
	obsqsv->encoder = encoder;
	obsqsv->codec = codec;

	struct vp9_caps vp9_caps = {0};
	if (codec == QSV_CODEC_VP9)
		vp9_aggregate_caps(&vp9_caps);

	video_t *video = obs_encoder_video(encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	switch (voi->format) {
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_P010:
		if (codec == QSV_CODEC_AVC) {
			const char *const text = obs_module_text("10bitUnsupportedAvc");
			obs_encoder_set_last_error(encoder, text);
			error("%s", text);
			bfree(obsqsv);
			return NULL;
		}
		obsqsv->params.video_fmt_p010 = true;
		break;
	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416: {
		const char *const text = obs_module_text("16bitUnsupported");
		obs_encoder_set_last_error(encoder, text);
		error("%s", text);
		bfree(obsqsv);
		return NULL;
	}
	case VIDEO_FORMAT_GBRA:
		/* Safety net only: on Windows obs_qsv_video_info_hevc_tex remaps BGRA to Y410, so a
		 * GBRA mix can no longer reach here for HEVC. Kept so any future raw-RGB delivery is
		 * rejected explicitly instead of mis-encoding RGB bytes as YUV. */
		if (codec != QSV_CODEC_HEVC || !useTexAlloc) {
			const char *const text = obs_module_text("444Unsupported");
			obs_encoder_set_last_error(encoder, text);
			error("%s", text);
			bfree(obsqsv);
			return NULL;
		}
		obsqsv->params.video_fmt_ayuv = true;
		break;
	case VIDEO_FORMAT_AYUV:
		/* 8-bit 4:4:4 -> VP9 Profile 1, allowed on both the texture and non-texture paths when
		 * runtime capability detection confirmed P1 (spec §27). */
		if ((codec == QSV_CODEC_HEVC) ||
		    (codec == QSV_CODEC_VP9 && vp9_caps.profile1_ayuv_8bit)) {
			obsqsv->params.video_fmt_ayuv = true;
		} else {
			const char *const text = obs_module_text("444Unsupported");
			obs_encoder_set_last_error(encoder, text);
			error("%s", text);
			bfree(obsqsv);
			return NULL;
		}
		break;
	case VIDEO_FORMAT_GBR10:
		/* Safety net only: on Windows obs_qsv_video_info_hevc_tex remaps GBR10/R10L to Y410,
		 * so a GBR10 mix can no longer reach here for HEVC (see the GBRA case above). */
		if (codec != QSV_CODEC_HEVC || !useTexAlloc) {
			const char *const text = obs_module_text("444Unsupported");
			obs_encoder_set_last_error(encoder, text);
			error("%s", text);
			bfree(obsqsv);
			return NULL;
		}
		obsqsv->params.video_fmt_y410 = true;
		break;
	case VIDEO_FORMAT_Y410:
		/* 10-bit 4:4:4 -> VP9 Profile 3, allowed on both the texture and non-texture paths when
		 * runtime capability detection confirmed P3 (spec §27). */
		if ((codec == QSV_CODEC_HEVC) ||
		    (codec == QSV_CODEC_VP9 && vp9_caps.profile3_y410_10bit)) {
			obsqsv->params.video_fmt_y410 = true;
		} else {
			const char *const text = obs_module_text("444Unsupported");
			obs_encoder_set_last_error(encoder, text);
			error("%s", text);
			bfree(obsqsv);
			return NULL;
		}
		break;
	default:
		switch (voi->colorspace) {
		case VIDEO_CS_2100_PQ:
		case VIDEO_CS_2100_HLG: {
			const char *const text = obs_module_text("8bitUnsupportedHdr");
			obs_encoder_set_last_error(encoder, text);
			error("%s", text);
			bfree(obsqsv);
			return NULL;
		}
		}
	}

	if (codec == QSV_CODEC_VP9) {
		// Validate the effective (format-derived) profile against runtime
		// capability detection before opening the encoder (spec §26, §30).
		bool supported;
		if (obsqsv->params.video_fmt_p010)
			supported = vp9_caps.profile2_p010_10bit;
		else if (obsqsv->params.video_fmt_y410)
			supported = vp9_caps.profile3_y410_10bit;
		else if (obsqsv->params.video_fmt_ayuv)
			supported = vp9_caps.profile1_ayuv_8bit;
		else
			supported = vp9_caps.profile0_nv12_8bit;

		if (!supported) {
			const char *const text = obs_module_text("VP9ProfileUnsupported");
			obs_encoder_set_last_error(encoder, text);
			error("%s", text);
			bfree(obsqsv);
			return NULL;
		}
	}

	if (update_settings(obsqsv, settings)) {
		pthread_mutex_lock(&g_QsvLock);
		obsqsv->context = qsv_encoder_open(&obsqsv->params, codec, useTexAlloc);
		pthread_mutex_unlock(&g_QsvLock);

		if (obsqsv->context == NULL)
			warn("qsv failed to load");
		else if (obsqsv->codec == QSV_CODEC_HEVC)
			load_hevc_headers(obsqsv);
		else
			load_headers(obsqsv);
	} else {
		warn("bad settings specified");
	}

	/* Mirror the GopPicSize that InitParams() computes for the MFX session (nKeyIntSec * effective
	 * fps, in frames). qsv_refresh_gop_frames() keeps it in sync after a live FPS re-init as well. */
	if (obsqsv->context != NULL)
		qsv_refresh_gop_frames(obsqsv);

	qsv_encoder_version(&g_verMajor, &g_verMinor);

	blog(LOG_DEBUG,
	     "\tmajor:          %d\n"
	     "\tminor:          %d",
	     g_verMajor, g_verMinor);

	if (!obsqsv->context) {
		bfree(obsqsv);
		return NULL;
	}

	/* Initialize live reconfig/resize tracking state */
	obsqsv->cx = obs_encoder_get_width(encoder);
	obsqsv->cy = obs_encoder_get_height(encoder);
	obsqsv->output_delay = (int)obsqsv->params.nAsyncDepth;
	obsqsv->last_keyframe_pts = -1; /* no keyframe cadence observed yet */

	obsqsv->performance_token = os_request_high_performance("qsv encoding");

	return obsqsv;
}

static void *obs_qsv_create_h264(obs_data_t *settings, obs_encoder_t *encoder)
{
	return obs_qsv_create(QSV_CODEC_AVC, settings, encoder, false);
}

static void *obs_qsv_create_av1(obs_data_t *settings, obs_encoder_t *encoder)
{
	return obs_qsv_create(QSV_CODEC_AV1, settings, encoder, false);
}

static void *obs_qsv_create_hevc(obs_data_t *settings, obs_encoder_t *encoder)
{
	return obs_qsv_create(QSV_CODEC_HEVC, settings, encoder, false);
}

static void *obs_qsv_create_vp9(obs_data_t *settings, obs_encoder_t *encoder)
{
	return obs_qsv_create(QSV_CODEC_VP9, settings, encoder, false);
}

static void *obs_qsv_create_tex(enum qsv_codec codec, obs_data_t *settings, obs_encoder_t *encoder,
				const char *fallback_id)
{
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);

	if (!adapters[ovi.adapter].is_intel) {
		blog(LOG_INFO, ">>> app not on intel GPU, fall back to old qsv encoder");
		return obs_encoder_create_rerouted(encoder, (const char *)fallback_id);
	}

	if (codec == QSV_CODEC_AV1 && !adapters[ovi.adapter].supports_av1) {
		blog(LOG_INFO, ">>> cap on different device, fall back to non-texture sharing AV1 qsv encoder");
		return obs_encoder_create_rerouted(encoder, (const char *)fallback_id);
	}

	if (codec == QSV_CODEC_VP9 && !adapters[ovi.adapter].supports_vp9) {
		blog(LOG_INFO, ">>> cap on different device, fall back to non-texture sharing VP9 qsv encoder");
		return obs_encoder_create_rerouted(encoder, (const char *)fallback_id);
	}

	bool gpu_texture_active = obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_NV12);

	if (codec != QSV_CODEC_AVC)
		gpu_texture_active = gpu_texture_active || obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_P010);
	if (codec == QSV_CODEC_HEVC) {
		gpu_texture_active = gpu_texture_active || obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_GBRA) ||
				     obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_AYUV) ||
				     obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_Y410) ||
				     obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_GBR10);
	}
	if (codec == QSV_CODEC_VP9) {
		gpu_texture_active = gpu_texture_active || obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_AYUV) ||
				     obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_Y410);
	}


	if (!gpu_texture_active) {
		blog(LOG_INFO, ">>> gpu tex not active, fall back to old qsv encoder");
		return obs_encoder_create_rerouted(encoder, (const char *)fallback_id);
	}

	if (obs_encoder_scaling_enabled(encoder)) {
		if (!obs_encoder_gpu_scaling_enabled(encoder)) {
			blog(LOG_INFO, ">>> encoder CPU scaling active, fall back to old qsv encoder");
			return obs_encoder_create_rerouted(encoder, (const char *)fallback_id);
		}
		blog(LOG_INFO, ">>> encoder GPU scaling active");
	}

	blog(LOG_INFO, ">>> new qsv encoder");
	return obs_qsv_create(codec, settings, encoder, true);
}

static void *obs_qsv_create_tex_h264(obs_data_t *settings, obs_encoder_t *encoder)
{
	return obs_qsv_create_tex(QSV_CODEC_AVC, settings, encoder, "obs_qsv11_soft");
}

static void *obs_qsv_create_tex_h264_v2(obs_data_t *settings, obs_encoder_t *encoder)
{
	return obs_qsv_create_tex(QSV_CODEC_AVC, settings, encoder, "obs_qsv11_soft_v2");
}

static void *obs_qsv_create_tex_av1(obs_data_t *settings, obs_encoder_t *encoder)
{
	return obs_qsv_create_tex(QSV_CODEC_AV1, settings, encoder, "obs_qsv11_av1_soft");
}

static void *obs_qsv_create_tex_hevc(obs_data_t *settings, obs_encoder_t *encoder)
{
	return obs_qsv_create_tex(QSV_CODEC_HEVC, settings, encoder, "obs_qsv11_hevc_soft");
}

static void *obs_qsv_create_tex_vp9(obs_data_t *settings, obs_encoder_t *encoder)
{
	return obs_qsv_create_tex(QSV_CODEC_VP9, settings, encoder, "obs_qsv11_vp9_soft");
}

static bool obs_qsv_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct obs_qsv *obsqsv = data;

	if (!obsqsv->context)
		return false;

	*extra_data = obsqsv->extra_data;
	*size = obsqsv->extra_data_size;
	return true;
}

static bool obs_qsv_sei(void *data, uint8_t **sei, size_t *size)
{
	struct obs_qsv *obsqsv = data;

	if (!obsqsv->context)
		return false;

	*sei = obsqsv->sei;
	*size = obsqsv->sei_size;
	return true;
}

static inline bool valid_format(enum video_format format)
{
	return format == VIDEO_FORMAT_NV12;
}

static inline bool valid_av1_format(enum video_format format)
{
	return format == VIDEO_FORMAT_NV12 || format == VIDEO_FORMAT_P010;
}

static inline void cap_resolution(struct video_scale_info *info)
{
	enum qsv_cpu_platform qsv_platform = qsv_get_cpu_platform();

	if (adapters[adapter_index].is_dgpu == true)
		qsv_platform = QSV_CPU_PLATFORM_UNKNOWN;

	if (qsv_platform <= QSV_CPU_PLATFORM_IVB && qsv_platform != QSV_CPU_PLATFORM_UNKNOWN) {
		if (info->width > 1920) {
			info->width = 1920;
		}

		if (info->height > 1200) {
			info->height = 1200;
		}
	}
}

static void obs_qsv_video_info(void *data, struct video_scale_info *info)
{
	struct obs_qsv *obsqsv = data;
	enum video_format pref_format = VIDEO_FORMAT_NONE;

	if (obsqsv) {
		pref_format = obs_encoder_get_preferred_video_format(obsqsv->encoder);
	}

	if (!valid_format(pref_format)) {
		pref_format = valid_format(info->format) ? info->format : VIDEO_FORMAT_NV12;
	}

	info->format = pref_format;
	cap_resolution(info);

	/* Remember the format libobs delivers to this encoder: the multitrack timed-resize
	 * CPU scaler covers NV12 and P010 (see qsv_maybe_resize). */
	if (obsqsv)
		obsqsv->delivery_format = info->format;
}

static void obs_qsv_video_plus_hdr_info(void *data, struct video_scale_info *info)
{
	struct obs_qsv *obsqsv = data;
	enum video_format pref_format = VIDEO_FORMAT_NONE;

	if (obsqsv) {
		pref_format = obs_encoder_get_preferred_video_format(obsqsv->encoder);
	}

	if (!valid_av1_format(pref_format)) {
		pref_format = valid_av1_format(info->format) ? info->format : VIDEO_FORMAT_NV12;
	}

	info->format = pref_format;
	cap_resolution(info);

	/* Remember the format libobs delivers to this encoder: the multitrack timed-resize
	 * CPU scaler covers NV12 and P010 (see qsv_maybe_resize). */
	if (obsqsv)
		obsqsv->delivery_format = info->format;
}

static void obs_qsv_video_info_hevc_tex(void *data, struct video_scale_info *info)
{
#ifdef _WIN32
	/* Deliver every 4:4:4 and RGB input as Y410 (packed R10G10B10A2):
	 *  - YUV sources (I444/Y410): the Y410_SRGB/PQ/HLG conversion shaders perform a real
	 *    in-shader RGB->YUV transform on the linear canvas; the VUI tags describe the
	 *    converted YUV data.
	 *  - RGB sources (BGRA/GBR10/R10L): tagged SRGB+FULL so libobs selects the Y410_RGB
	 *    conversion, which repacks the channels raw into the coded samples in GBR order
	 *    (the component order the Identity matrix actually uses; no colour conversion),
	 *    and update_params writes MatrixCoefficients=0 (Identity) + full range - HEVC Main
	 *    4:4:4 carrying genuine RGB exactly as the spec intends.
	 *
	 * Previously I444 was remapped to AYUV and BGRA/GBR10 were repacked raw into AYUV/Y410
	 * slots without colour conversion. That stored the channels in the wrong coded positions
	 * (scrambled colours) and the AYUV layout is broken on the dGPU fallback path: libobs has
	 * no GPU-conversion/staging/readback support for AYUV mixes, so the raw connection
	 * delivered a constant stale buffer and recordings came out solid green. Y410 is the only
	 * 4:4:4 layout with full support across conversion, staging, readback and the MFX session
	 * on both the iGPU texture path and the dGPU fallback (obs_qsv_create accepts it regardless
	 * of useTexAlloc). 8-bit content maps losslessly into the 10-bit container (Main 4:4:4 10). */
	if (info->format == VIDEO_FORMAT_BGRA) {
		info->format = VIDEO_FORMAT_Y410;
		info->range = VIDEO_RANGE_FULL;
		info->colorspace = VIDEO_CS_SRGB;
		return;
	} else if (info->format == VIDEO_FORMAT_I444 || info->format == VIDEO_FORMAT_Y410) {
		info->format = VIDEO_FORMAT_Y410;
		return;
	} else if (info->format == VIDEO_FORMAT_GBR10 || info->format == VIDEO_FORMAT_R10L) {
		info->format = VIDEO_FORMAT_Y410;
		info->range = VIDEO_RANGE_FULL;
		/* Use SRGB for SDR */
		if (info->colorspace != VIDEO_CS_2100_PQ && info->colorspace != VIDEO_CS_2100_HLG)
			info->colorspace = VIDEO_CS_SRGB;
		return;
	}
#endif
	obs_qsv_video_plus_hdr_info(data, info);
}

/* Declares which entries of the standard "Color Format" dropdown the QSV VP9 encoder can
 * actually encode, based on runtime-detected per-profile capabilities. libobs builds that
 * dropdown from this set (add_encoder_color_properties), so only formats whose profile was
 * confirmed by the adapter probe are offered. I444 is the OBS-facing name for 8-bit 4:4:4;
 * obs_qsv_video_info_vp9_tex remaps it to AYUV, the QSV FourCC the encoder consumes. */
static bool vp9_is_color_format_supported(void *type_data, enum video_format format)
{
	UNUSED_PARAMETER(type_data);
	struct vp9_caps caps;
	vp9_aggregate_caps(&caps);
	switch (format) {
	case VIDEO_FORMAT_NV12:
		return caps.profile0_nv12_8bit;
	case VIDEO_FORMAT_P010:
		return caps.profile2_p010_10bit;
	case VIDEO_FORMAT_I444:
		return caps.profile1_ayuv_8bit;
	case VIDEO_FORMAT_Y410:
		return caps.profile3_y410_10bit;
	default:
		return false;
	}
}

static bool vp9_soft_is_color_format_supported(void *type_data, enum video_format format)
{
	/* The non-texture ("soft") fallback encodes the same formats as the texture path (it just
	 * receives AYUV/Y410 via system memory instead of a shared texture), so offer the identical,
	 * capability-gated set. */
	return vp9_is_color_format_supported(type_data, format);
}

/* VP9 video info. Remaps the OBS-facing 8-bit 4:4:4 color format (I444) to the QSV FourCC layout
 * the encoder consumes (AYUV) so libobs delivers the matching texture and the create path sees the
 * correct voi->format (spec §27). Y410 is already the QSV layout. This mirrors the HEVC tex info:
 * 4:4:4 is delivered regardless of which GPU OBS renders on, because the non-texture ("soft")
 * fallback also encodes AYUV/Y410 from system memory (see obs_qsv_create). */
static void obs_qsv_video_info_vp9_tex(void *data, struct video_scale_info *info)
{
#ifdef _WIN32
	if (info->format == VIDEO_FORMAT_I444 || info->format == VIDEO_FORMAT_Y410) {
		if (info->format == VIDEO_FORMAT_I444)
			info->format = VIDEO_FORMAT_AYUV;
		/* Y410 is already the QSV layout */
		return;
	}
#endif
	obs_qsv_video_plus_hdr_info(data, info);
}

static mfxU64 ts_obs_to_mfx(int64_t ts, const struct video_output_info *voi)
{
	return ts * 90000 / voi->fps_num;
}

static int64_t ts_mfx_to_obs(mfxI64 ts, const struct video_output_info *voi)
{
	/* Exact inverse of ts_obs_to_mfx(): convert MFX's 90 kHz ticks back to the OBS
	 * timebase (1 tick per 1/fps_num second). The previous form rounded the result to
	 * a multiple of fps_den, which is only lossless when cur_pts already lies on that
	 * grid - after a live FPS change it does not (at 60/4 an off-grid cur_pts such as
	 * 6375 round-tripped to 6376), so no emitted packet matched encoder->cur_pts and
	 * libobs lost the frame-timing association for every frame after the change. */
	if (ts < 0)
		return (ts * voi->fps_num - 45000) / 90000;
	else
		return (ts * voi->fps_num + 45000) / 90000;
}

/* Keep output timestamps on the OBS timeline across a live reconfigure.
 * MFX echoes back the per-frame TimeStamp we submit (encoder->cur_pts) as its output
 * PresentationTimeStamp and continues its DecodeTimeStamp base, so pkt->pts already
 * equals encoder->cur_pts and no PTS offset is needed (ts_offset stays 0). The one
 * anomaly: right after a re-init MFX emits its initial B-frame window with non-
 * monotonic DecodeTimeStamps, which must not trip the DTS-regression check below (it
 * would shift ts_offset and desync PTS from cur_pts) - so the check is suppressed for
 * the first packets after a re-init (dts_suppress, set by qsv_reset_ts_state). After
 * that window the check resumes as a safety net against a genuine DTS base restart. */
static void qsv_fix_timestamps(struct obs_qsv *obsqsv, struct encoder_packet *packet)
{
	int64_t dts = packet->dts + obsqsv->ts_offset;
	int64_t pts = packet->pts + obsqsv->ts_offset;

	if (obsqsv->dts_suppress > 0) {
		/* Initial B-frame window after a re-init: pass through untouched. */
		obsqsv->dts_suppress--;
	} else if (obsqsv->have_last_dts && dts <= obsqsv->last_dts) {
		/* Regression outside the post-reinit window: grow the offset so this
		 * frame continues from where the previous one left off (+1 tick). */
		int64_t shift = obsqsv->last_dts - dts + 1;
		obsqsv->ts_offset += shift;
		dts += shift;
		pts += shift;
		blog(LOG_DEBUG, "[qsv] DTS regression at raw_dts=%lld: +%lld -> ts_offset=%lld (pts shifted to %lld)",
		     (long long)packet->dts, (long long)shift, (long long)obsqsv->ts_offset, (long long)pts);
	}

	if (pts < dts)
		pts = dts;

	packet->dts = dts;
	packet->pts = pts;
	obsqsv->last_dts = dts;
	obsqsv->have_last_dts = true;
}

static void parse_packet(struct obs_qsv *obsqsv, struct encoder_packet *packet, mfxBitstream *pBS,
			 const struct video_output_info *voi, bool *received_packet)
{
	uint8_t *start, *end;
	int type;

	if (pBS == NULL || pBS->DataLength == 0) {
		*received_packet = false;
		return;
	}

	da_resize(obsqsv->packet_data, 0);
	da_push_back_array(obsqsv->packet_data, &pBS->Data[pBS->DataOffset], pBS->DataLength);

	packet->data = obsqsv->packet_data.array;
	packet->size = obsqsv->packet_data.num;
	packet->type = OBS_ENCODER_VIDEO;
	packet->pts = ts_mfx_to_obs((mfxI64)pBS->TimeStamp, voi);
	packet->keyframe = (pBS->FrameType & MFX_FRAMETYPE_IDR);

	uint16_t frameType = pBS->FrameType;
	uint8_t priority;

	if (frameType & MFX_FRAMETYPE_I)
		priority = OBS_NAL_PRIORITY_HIGHEST;
	else if ((frameType & MFX_FRAMETYPE_P) || (frameType & MFX_FRAMETYPE_REF))
		priority = OBS_NAL_PRIORITY_HIGH;
	else
		priority = 0;

	packet->priority = priority;

	/* ------------------------------------ */

	start = obsqsv->packet_data.array;
	end = start + obsqsv->packet_data.num;

	start = (uint8_t *)obs_avc_find_startcode(start, end);
	while (true) {
		while (start < end && !*(start++))
			;

		if (start == end)
			break;

		type = start[0] & 0x1F;
		if (type == OBS_NAL_SLICE_IDR || type == OBS_NAL_SLICE) {
			start[0] &= ~(3 << 5);
			start[0] |= priority << 5; //0 for non-ref frames and not equal to 0 for ref frames
		}

		start = (uint8_t *)obs_avc_find_startcode(start, end);
	}

	/* ------------------------------------ */

	packet->dts = ts_mfx_to_obs(pBS->DecodeTimeStamp, voi);
	qsv_fix_timestamps(obsqsv, packet);

#if 0
	bool iFrame = pBS->FrameType & MFX_FRAMETYPE_I;
	bool bFrame = pBS->FrameType & MFX_FRAMETYPE_B;
	bool pFrame = pBS->FrameType & MFX_FRAMETYPE_P;
	int iType = iFrame ? 0 : (bFrame ? 1 : (pFrame ? 2 : -1));

	info("parse packet:\n"
		"\tFrameType: %d\n"
		"\tpts:       %d\n"
		"\tdts:       %d",
		iType, packet->pts, packet->dts);
#endif

	*received_packet = true;
	if (pBS->FrameType & MFX_FRAMETYPE_IDR) {
		obsqsv->frames_since_idr = 0;
		/* Track the steady-state on-grid keyframe cadence for multitrack timed re-init.
		 * Only update when not mid-resize/FPS-reinit/realignment so an off-grid session-start IDR
		 * cannot corrupt the shared grid phase. */
		if (!obsqsv->resize_pending && !obsqsv->fps_pending && !obsqsv->align_pending) {
			const int64_t kpts = packet->pts;
			if (obsqsv->last_keyframe_pts >= 0) {
				int64_t delta = kpts - obsqsv->last_keyframe_pts;
				if (delta > 0)
					obsqsv->gop_pts = delta;
			}
			obsqsv->last_keyframe_pts = kpts;
		}
	}
	pBS->DataLength = 0;
}

static void parse_packet_av1(struct obs_qsv *obsqsv, struct encoder_packet *packet, mfxBitstream *pBS,
			     const struct video_output_info *voi, bool *received_packet)
{
	if (pBS == NULL || pBS->DataLength == 0) {
		*received_packet = false;
		return;
	}

	da_resize(obsqsv->packet_data, 0);
	da_push_back_array(obsqsv->packet_data, &pBS->Data[pBS->DataOffset], pBS->DataLength);

	packet->data = obsqsv->packet_data.array;
	packet->size = obsqsv->packet_data.num;
	packet->type = OBS_ENCODER_VIDEO;
	packet->pts = ts_mfx_to_obs((mfxI64)pBS->TimeStamp, voi);
	packet->keyframe = (pBS->FrameType & MFX_FRAMETYPE_IDR);

	uint16_t frameType = pBS->FrameType;
	uint8_t priority;

	if (frameType & MFX_FRAMETYPE_I)
		priority = OBS_NAL_PRIORITY_HIGHEST;
	else if ((frameType & MFX_FRAMETYPE_P) || (frameType & MFX_FRAMETYPE_REF))
		priority = OBS_NAL_PRIORITY_HIGH;
	else
		priority = OBS_NAL_PRIORITY_DISPOSABLE;

	packet->priority = priority;

	packet->dts = ts_mfx_to_obs(pBS->DecodeTimeStamp, voi);
	qsv_fix_timestamps(obsqsv, packet);

#if 0
	info("parse packet:\n"
		"\tFrameType: %d\n"
		"\tpts:       %d\n"
		"\tdts:       %d",
		iType, packet->pts, packet->dts);
#endif

	*received_packet = true;
	if (pBS->FrameType & MFX_FRAMETYPE_IDR) {
		obsqsv->frames_since_idr = 0;
		/* Track the steady-state on-grid keyframe cadence for multitrack timed re-init. */
		if (!obsqsv->resize_pending && !obsqsv->fps_pending && !obsqsv->align_pending) {
			const int64_t kpts = packet->pts;
			if (obsqsv->last_keyframe_pts >= 0) {
				int64_t delta = kpts - obsqsv->last_keyframe_pts;
				if (delta > 0)
					obsqsv->gop_pts = delta;
			}
			obsqsv->last_keyframe_pts = kpts;
		}
	}
	pBS->DataLength = 0;
}

static void parse_packet_vp9(struct obs_qsv *obsqsv, struct encoder_packet *packet, mfxBitstream *pBS,
			     const struct video_output_info *voi, bool *received_packet)
{
	if (pBS == NULL || pBS->DataLength == 0) {
		*received_packet = false;
		return;
	}

	da_resize(obsqsv->packet_data, 0);
	da_push_back_array(obsqsv->packet_data, &pBS->Data[pBS->DataOffset], pBS->DataLength);

	packet->data = obsqsv->packet_data.array;
	packet->size = obsqsv->packet_data.num;
	packet->type = OBS_ENCODER_VIDEO;
	packet->pts = ts_mfx_to_obs((mfxI64)pBS->TimeStamp, voi);
	packet->keyframe = (pBS->FrameType & MFX_FRAMETYPE_IDR);

	uint16_t frameType = pBS->FrameType;
	uint8_t priority = OBS_NAL_PRIORITY_DISPOSABLE;

	if (frameType & MFX_FRAMETYPE_I)
		priority = OBS_NAL_PRIORITY_HIGHEST;
	else if ((frameType & MFX_FRAMETYPE_P) || (frameType & MFX_FRAMETYPE_REF))
		priority = OBS_NAL_PRIORITY_HIGH;

	packet->priority = priority;

	/* VPL's low-power VP9 path does not populate DecodeTimeStamp (it stays 0),
	 * which would make every packet report DTS=0 and corrupt the decode/presentation
	 * timeline in qsv_fix_timestamps. With B-frames disabled in our VP9 config,
	 * decode order == presentation order, so fall back to TimeStamp for DTS when
	 * DecodeTimeStamp is absent. */
	mfxI64 dts_ts = pBS->DecodeTimeStamp;
	if (dts_ts == 0 && pBS->TimeStamp != 0)
		dts_ts = pBS->TimeStamp;
	packet->dts = ts_mfx_to_obs(dts_ts, voi);

	static bool vp9_ts_logged = false;
	if (!vp9_ts_logged) {
		vp9_ts_logged = true;
		blog(LOG_DEBUG, "[qsv vp9] first packet raw TS: TimeStamp=%lld DecodeTimeStamp=%lld -> pts=%lld dts=%lld",
		     (long long)pBS->TimeStamp, (long long)pBS->DecodeTimeStamp, (long long)packet->pts, (long long)packet->dts);
	}

	qsv_fix_timestamps(obsqsv, packet);

	*received_packet = true;
	if (pBS->FrameType & MFX_FRAMETYPE_IDR) {
		obsqsv->frames_since_idr = 0;
		/* Track the steady-state on-grid keyframe cadence for multitrack timed re-init. */
		if (!obsqsv->resize_pending && !obsqsv->fps_pending && !obsqsv->align_pending) {
			const int64_t kpts = packet->pts;
			if (obsqsv->last_keyframe_pts >= 0) {
				int64_t delta = kpts - obsqsv->last_keyframe_pts;
				if (delta > 0)
					obsqsv->gop_pts = delta;
			}
			obsqsv->last_keyframe_pts = kpts;
		}
	}
	pBS->DataLength = 0;
}

static void parse_packet_hevc(struct obs_qsv *obsqsv, struct encoder_packet *packet, mfxBitstream *pBS,
			      const struct video_output_info *voi, bool *received_packet)
{
	if (pBS == NULL || pBS->DataLength == 0) {
		*received_packet = false;
		return;
	}

	da_resize(obsqsv->packet_data, 0);
	da_push_back_array(obsqsv->packet_data, &pBS->Data[pBS->DataOffset], pBS->DataLength);

	packet->data = obsqsv->packet_data.array;
	packet->size = obsqsv->packet_data.num;
	packet->type = OBS_ENCODER_VIDEO;
	packet->pts = ts_mfx_to_obs((mfxI64)pBS->TimeStamp, voi);
	packet->keyframe = (pBS->FrameType & MFX_FRAMETYPE_IDR);

	uint16_t frameType = pBS->FrameType;
	uint8_t priority = OBS_NAL_PRIORITY_DISPOSABLE;

	if (frameType & MFX_FRAMETYPE_I)
		priority = OBS_NAL_PRIORITY_HIGHEST;
	else if ((frameType & MFX_FRAMETYPE_P) || (frameType & MFX_FRAMETYPE_REF))
		priority = OBS_NAL_PRIORITY_HIGH;

	packet->priority = priority;

	/* ------------------------------------ */

	packet->dts = ts_mfx_to_obs(pBS->DecodeTimeStamp, voi);
	qsv_fix_timestamps(obsqsv, packet);

#if 0
	bool iFrame = pBS->FrameType & MFX_FRAMETYPE_I;
	bool bFrame = pBS->FrameType & MFX_FRAMETYPE_B;
	bool pFrame = pBS->FrameType & MFX_FRAMETYPE_P;

	int iType = iFrame ? 0 : (bFrame ? 1 : (pFrame ? 2 : -1));

	info("parse packet:\n"
		"\tFrameType: %d\n"
		"\tpts:       %d\n"
		"\tdts:       %d",
		iType, packet->pts, packet->dts);
#endif
	*received_packet = true;
	if (pBS->FrameType & MFX_FRAMETYPE_IDR) {
		obsqsv->frames_since_idr = 0;
		/* Track the steady-state on-grid keyframe cadence for multitrack timed re-init. */
		if (!obsqsv->resize_pending && !obsqsv->fps_pending && !obsqsv->align_pending) {
			const int64_t kpts = packet->pts;
			if (obsqsv->last_keyframe_pts >= 0) {
				int64_t delta = kpts - obsqsv->last_keyframe_pts;
				if (delta > 0)
					obsqsv->gop_pts = delta;
			}
			obsqsv->last_keyframe_pts = kpts;
		}
	}
	pBS->DataLength = 0;
}

static void roi_cb(void *param, struct obs_encoder_roi *roi)
{
	struct darray *da = param;
	darray_push_back(sizeof(struct obs_encoder_roi), da, roi);
}

static void obs_qsv_setup_rois(struct obs_qsv *obsqsv)
{
	const uint32_t increment = obs_encoder_get_roi_increment(obsqsv->encoder);
	if (obsqsv->roi_increment == increment)
		return;

	qsv_encoder_clear_roi(obsqsv->context);
	/* Because we pass-through the ROIs more or less directly we need to
	 * pass them in reverse order, so make a temporary copy and then use
	 * that instead. */
	DARRAY(struct obs_encoder_roi) rois;
	da_init(rois);

	obs_encoder_enum_roi(obsqsv->encoder, roi_cb, &rois);

	size_t idx = rois.num;
	while (idx)
		qsv_encoder_add_roi(obsqsv->context, &rois.array[--idx]);

	da_free(rois);

	obsqsv->roi_increment = increment;
}

static bool obs_qsv_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
			   bool *received_packet)
{
	struct obs_qsv *obsqsv = data;

	if (!frame || !packet || !received_packet)
		return false;

	video_t *video = obs_encoder_video(obsqsv->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	mfxBitstream *pBS = NULL;

	int ret;

	/* --- Background timed re-init (see reinit_in_progress). Queue handling below is
	 * lock-free: while the worker holds g_QsvLock for the ~400 ms MFX Close+Init, encode calls
	 * must not block on it - they only deep-copy frames into pending_frames and return. --- */

	/* (a) At the shared keyframe boundary, start the re-init on a background worker instead of
	 * blocking this thread (see qsv_maybe_apply_timed_reinit_bg). The queued frame becomes the
	 * first frame of the fresh session, so its session-start IDR lands exactly on-grid. */
	if (!obsqsv->reinit_in_progress && !qsv_maybe_apply_timed_reinit_bg(obsqsv, frame->pts)) {
		qsv_pending_enqueue(obsqsv, frame);
		*received_packet = false;
		return true; // queued - re-fed after the background reset completes
	}

	/* (b) While the worker runs: queue this frame and emit nothing. */
	if (obsqsv->reinit_in_progress) {
		qsv_pending_enqueue(obsqsv, frame);
		if (os_event_try(obsqsv->reinit_done)) {
			pthread_join(obsqsv->reinit_thread, NULL); /* worker already finished */
			os_event_destroy(obsqsv->reinit_done);
			obsqsv->reinit_done = NULL;
			obsqsv->reinit_in_progress = false;
			info("background re-init complete - draining %zu queued frame(s)", obsqsv->pending_frames.num);
		}
		if (obsqsv->reinit_in_progress) {
			*received_packet = false;
			return true; // worker still running - this frame is queued
		}
	}

	/* (c) While queued frames remain, re-feed the oldest one (DTS order) through the normal
	 * path and keep queueing incoming frames, so no frame is dropped and DTS stays continuous
	 * across the boundary. (See the reinit_in_progress comment for the 1-in/1-out trade-off.) */
	struct encoder_frame queued;
	const struct encoder_frame *enc = frame;
	if (obsqsv->pending_frames.num > 0) {
		qsv_pending_enqueue(obsqsv, frame);
		struct qsv_pending_frame pf;
		qsv_pending_dequeue(obsqsv, &pf);
		memset(&queued, 0, sizeof(queued));
		queued.data[0]     = pf.y;
		queued.data[1]     = pf.uv;
		queued.linesize[0] = pf.ls0;
		queued.linesize[1] = pf.ls1;
		queued.pts          = pf.pts;
		enc = &queued;
	}

	pthread_mutex_lock(&g_QsvLock);

	if (!qsv_check_staged(obsqsv, enc->pts, enc->linesize[0], obsqsv->delivery_format)) {
		if (enc != frame) {
			bfree(queued.data[0]); /* dequeued frame is dropped - release its copies */
			bfree(queued.data[1]);
		}
		pthread_mutex_unlock(&g_QsvLock);
		*received_packet = false;
		return true; // skip this frame - encoder is transitioning
	}

	if (obs_encoder_keyframe_requested(obsqsv->encoder)) {
		if (obs_encoder_is_in_group(obsqsv->encoder)) {
			info("keyframe request dropped: grouped encoder keeps its shared keyframe grid");
		} else {
			info("forcing IDR for a consumer that attached mid-session (pts %lld)", (long long)enc->pts);
			qsv_encoder_force_idr(obsqsv->context);
		}
	}

	if (obs_encoder_has_roi(obsqsv->encoder))
		obs_qsv_setup_rois(obsqsv);

	/* While a timed resize is pending, the MFX session is still at the OLD size: scale
	 * the incoming new-size frame down so it can be encoded by the current session with
	 * continuous DTS (mirrors obs_x264_encode's timed-resize transition). Queued frames
	 * are already at session size and must not be scaled - the scaler reads the frame at
	 * the encoder's target dimensions, which would be out of bounds for them. */
	uint8_t *enc_y = enc->data[0];
	uint8_t *enc_uv = enc->data[1];
	uint32_t enc_ls0 = enc->linesize[0];
	uint32_t enc_ls1 = enc->linesize[1];
	if (obsqsv->resize_pending && obsqsv->scale_buf[0] && enc == frame) {
		if (obsqsv->delivery_format == VIDEO_FORMAT_P010)
			qsv_scale_p010_to_old_size(obsqsv, enc);
		else
			qsv_scale_nv12_to_old_size(obsqsv, enc);
		enc_y   = obsqsv->scale_buf[0];
		enc_uv  = obsqsv->scale_buf[1];
		enc_ls0 = obsqsv->scale_stride[0];
		enc_ls1 = obsqsv->scale_stride[1];
	}

	ret = qsv_encoder_encode(obsqsv->context, ts_obs_to_mfx(enc->pts, voi), enc_y, enc_uv, enc_ls0, enc_ls1, &pBS);

	if (ret < 0) {
		warn("encode failed: mfxStatus=%d", ret);
		if (enc != frame) {
			bfree(queued.data[0]); /* dequeued frame was not encoded - release its copies */
			bfree(queued.data[1]);
		}
		pthread_mutex_unlock(&g_QsvLock);
		return false;
	}

	if (obsqsv->codec == QSV_CODEC_AVC)
		parse_packet(obsqsv, packet, pBS, voi, received_packet);
	else if (obsqsv->codec == QSV_CODEC_AV1)
		parse_packet_av1(obsqsv, packet, pBS, voi, received_packet);
	else if (obsqsv->codec == QSV_CODEC_HEVC)
		parse_packet_hevc(obsqsv, packet, pBS, voi, received_packet);
	else if (obsqsv->codec == QSV_CODEC_VP9)
		parse_packet_vp9(obsqsv, packet, pBS, voi, received_packet);

	if (*received_packet)
		qsv_abr_tick(obsqsv, (uint32_t)obsqsv->packet_data.num);

	if (enc != frame) {
		bfree(queued.data[0]); /* re-fed frame was encoded - release its copies */
		bfree(queued.data[1]);
	}

	pthread_mutex_unlock(&g_QsvLock);

	return true;
}

static bool obs_qsv_encode_tex(void *data, struct encoder_texture *tex, int64_t pts, uint64_t lock_key,
			       uint64_t *next_key, struct encoder_packet *packet, bool *received_packet)
{
	struct obs_qsv *obsqsv = data;

#ifdef _WIN32
	if (!tex || tex->handle == GS_INVALID_HANDLE) {
#else
	if (!tex || !tex->tex[0] || !tex->tex[1]) {
#endif
		warn("Encode failed: bad texture handle");
		*next_key = lock_key;
		return false;
	}

	if (!packet || !received_packet)
		return false;

	pthread_mutex_lock(&g_QsvLock);

	video_t *video = obs_encoder_video(obsqsv->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	mfxBitstream *pBS = NULL;

	int ret;

	mfxU64 qsvPTS = ts_obs_to_mfx(pts, voi);

	/* Timed multitrack FPS re-init (staged in qsv_check_staged): a texture cannot be CPU-scaled to
	 * the old size, so only effective-FPS changes can be staged on this path - no scaling is needed,
	 * the Close+Init simply lands on the shared keyframe boundary. */
	if (!qsv_maybe_apply_timed_reinit(obsqsv, pts)) {
		pthread_mutex_unlock(&g_QsvLock);
		*next_key = lock_key;
		*received_packet = false;
		return true; // skip this frame - encoder is transitioning
	}

	/* Texture path: dimensions are self-describing in the D3D11 texture,
	 * so no linesize guard needed (pass UINT32_MAX to skip the check). The
	 * timed multitrack resize is not available here (a texture cannot be
	 * CPU-scaled to the old size), so a grouped texture encoder re-inits
	 * immediately like before. */
	if (!qsv_check_staged(obsqsv, pts, UINT32_MAX, VIDEO_FORMAT_NONE)) {
		pthread_mutex_unlock(&g_QsvLock);
		*next_key = lock_key;
		*received_packet = false;
		return true; // skip this frame - encoder is transitioning
	}

	if (obs_encoder_keyframe_requested(obsqsv->encoder)) {
		if (obs_encoder_is_in_group(obsqsv->encoder)) {
			info("keyframe request dropped: grouped encoder keeps its shared keyframe grid");
		} else {
			info("forcing IDR for a consumer that attached mid-session (pts %lld)", (long long)pts);
			qsv_encoder_force_idr(obsqsv->context);
		}
	}

	if (obs_encoder_has_roi(obsqsv->encoder))
		obs_qsv_setup_rois(obsqsv);

	ret = qsv_encoder_encode_tex(obsqsv->context, qsvPTS, (void *)tex, lock_key, next_key, &pBS);

	if (ret < 0) {
		warn("encode failed: mfxStatus=%d", ret);
		pthread_mutex_unlock(&g_QsvLock);
		return false;
	}

	if (obsqsv->codec == QSV_CODEC_AVC)
		parse_packet(obsqsv, packet, pBS, voi, received_packet);
	else if (obsqsv->codec == QSV_CODEC_AV1)
		parse_packet_av1(obsqsv, packet, pBS, voi, received_packet);
	else if (obsqsv->codec == QSV_CODEC_HEVC)
		parse_packet_hevc(obsqsv, packet, pBS, voi, received_packet);
	else if (obsqsv->codec == QSV_CODEC_VP9)
		parse_packet_vp9(obsqsv, packet, pBS, voi, received_packet);

	if (*received_packet)
		qsv_abr_tick(obsqsv, (uint32_t)obsqsv->packet_data.num);

	pthread_mutex_unlock(&g_QsvLock);

	return true;
}

struct obs_encoder_info obs_qsv_encoder_tex = {
	.id = "obs_qsv11",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.get_name = obs_qsv_getname_v1,
	.create = obs_qsv_create_tex_h264,
	.destroy = obs_qsv_destroy,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_DEPRECATED,
	.encode_texture2 = obs_qsv_encode_tex,
	.update = obs_qsv_update,
	.get_properties = obs_qsv_props_h264,
	.get_defaults = obs_qsv_defaults_h264_v1,
	.get_extra_data = obs_qsv_extra_data,
	.get_sei_data = obs_qsv_sei,
	.get_video_info = obs_qsv_video_info,
};

struct obs_encoder_info obs_qsv_encoder = {
	.id = "obs_qsv11_soft",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.get_name = obs_qsv_getname_v1,
	.create = obs_qsv_create_h264,
	.destroy = obs_qsv_destroy,
	.encode = obs_qsv_encode,
	.update = obs_qsv_update,
	.get_properties = obs_qsv_props_h264,
	.get_defaults = obs_qsv_defaults_h264_v1,
	.get_extra_data = obs_qsv_extra_data,
	.get_sei_data = obs_qsv_sei,
	.get_video_info = obs_qsv_video_info,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_INTERNAL | OBS_ENCODER_CAP_DEPRECATED,
};

struct obs_encoder_info obs_qsv_encoder_tex_v2 = {
	.id = "obs_qsv11_v2",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.get_name = obs_qsv_getname,
	.create = obs_qsv_create_tex_h264_v2,
	.destroy = obs_qsv_destroy,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_ROI,
	.encode_texture2 = obs_qsv_encode_tex,
	.update = obs_qsv_update,
	.get_properties = obs_qsv_props_h264_v2,
	.get_defaults = obs_qsv_defaults_h264_v2,
	.get_extra_data = obs_qsv_extra_data,
	.get_sei_data = obs_qsv_sei,
	.get_video_info = obs_qsv_video_info,
};

struct obs_encoder_info obs_qsv_encoder_v2 = {
	.id = "obs_qsv11_soft_v2",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.get_name = obs_qsv_getname,
	.create = obs_qsv_create_h264,
	.destroy = obs_qsv_destroy,
	.encode = obs_qsv_encode,
	.update = obs_qsv_update,
	.get_properties = obs_qsv_props_h264_v2,
	.get_defaults = obs_qsv_defaults_h264_v2,
	.get_extra_data = obs_qsv_extra_data,
	.get_sei_data = obs_qsv_sei,
	.get_video_info = obs_qsv_video_info,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_INTERNAL | OBS_ENCODER_CAP_ROI,
};

struct obs_encoder_info obs_qsv_av1_encoder_tex = {
	.id = "obs_qsv11_av1",
	.type = OBS_ENCODER_VIDEO,
	.codec = "av1",
	.get_name = obs_qsv_getname_av1,
	.create = obs_qsv_create_tex_av1,
	.destroy = obs_qsv_destroy,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_ROI,
	.encode_texture2 = obs_qsv_encode_tex,
	.update = obs_qsv_update,
	.get_properties = obs_qsv_props_av1,
	.get_defaults = obs_qsv_defaults_av1,
	.get_extra_data = obs_qsv_extra_data,
	.get_video_info = obs_qsv_video_plus_hdr_info,
};

struct obs_encoder_info obs_qsv_av1_encoder = {
	.id = "obs_qsv11_av1_soft",
	.type = OBS_ENCODER_VIDEO,
	.codec = "av1",
	.get_name = obs_qsv_getname_av1,
	.create = obs_qsv_create_av1,
	.destroy = obs_qsv_destroy,
	.encode = obs_qsv_encode,
	.update = obs_qsv_update,
	.get_properties = obs_qsv_props_av1,
	.get_defaults = obs_qsv_defaults_av1,
	.get_extra_data = obs_qsv_extra_data,
	.get_video_info = obs_qsv_video_plus_hdr_info,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_INTERNAL | OBS_ENCODER_CAP_ROI,
};

struct obs_encoder_info obs_qsv_hevc_encoder_tex = {
	.id = "obs_qsv11_hevc",
	.type = OBS_ENCODER_VIDEO,
	.codec = "hevc",
	.get_name = obs_qsv_getname_hevc,
	.create = obs_qsv_create_tex_hevc,
	.destroy = obs_qsv_destroy,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_ROI,
	.encode_texture2 = obs_qsv_encode_tex,
	.update = obs_qsv_update,
	.get_properties = obs_qsv_props_hevc,
	.get_defaults = obs_qsv_defaults_hevc,
	.get_extra_data = obs_qsv_extra_data,
	.get_video_info = obs_qsv_video_info_hevc_tex,
};

struct obs_encoder_info obs_qsv_hevc_encoder = {
	.id = "obs_qsv11_hevc_soft",
	.type = OBS_ENCODER_VIDEO,
	.codec = "hevc",
	.get_name = obs_qsv_getname_hevc,
	.create = obs_qsv_create_hevc,
	.destroy = obs_qsv_destroy,
	.encode = obs_qsv_encode,
	.update = obs_qsv_update,
	.get_properties = obs_qsv_props_hevc,
	.get_defaults = obs_qsv_defaults_hevc,
	.get_extra_data = obs_qsv_extra_data,
	/* Negotiate the same formats as the texture path. The QSV session's FourCC is derived from
	 * voi->format at create() time (e.g. Y410 when the user picks 10-bit 4:4:4), so the raw
	 * delivery format must match it exactly. The generic obs_qsv_video_plus_hdr_info only accepts
	 * NV12/P010 and would hand 8-bit NV12 bytes to a Y410 session, which LoadY410() then misreads
	 * as packed 10-bit 4:4:4 words -> hue-shifted output. With matching formats the raw connection
	 * reuses the pre-bound GPU-conversion mix (RGBA16F -> XV30LE shader + staging surface) with no
	 * per-frame CPU swscale stage. */
	.get_video_info = obs_qsv_video_info_hevc_tex,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_INTERNAL | OBS_ENCODER_CAP_ROI,
};

struct obs_encoder_info obs_qsv_vp9_encoder_tex = {
	.id = "obs_qsv11_vp9",
	.type = OBS_ENCODER_VIDEO,
	.codec = "vp9",
	.get_name = obs_qsv_getname_vp9,
	.create = obs_qsv_create_tex_vp9,
	.destroy = obs_qsv_destroy,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_PASS_TEXTURE,
	.encode_texture2 = obs_qsv_encode_tex,
	.update = obs_qsv_update,
	.get_properties = obs_qsv_props_vp9,
	.get_defaults = obs_qsv_defaults_vp9,
	.get_extra_data = obs_qsv_extra_data,
	.get_video_info = obs_qsv_video_info_vp9_tex,
	.is_color_format_supported = vp9_is_color_format_supported,
};

struct obs_encoder_info obs_qsv_vp9_encoder = {
	.id = "obs_qsv11_vp9_soft",
	.type = OBS_ENCODER_VIDEO,
	.codec = "vp9",
	.get_name = obs_qsv_getname_vp9,
	.create = obs_qsv_create_vp9,
	.destroy = obs_qsv_destroy,
	.encode = obs_qsv_encode,
	.update = obs_qsv_update,
	.get_properties = obs_qsv_props_vp9,
	.get_defaults = obs_qsv_defaults_vp9,
	.get_extra_data = obs_qsv_extra_data,
	.get_video_info = obs_qsv_video_plus_hdr_info,
	.is_color_format_supported = vp9_soft_is_color_format_supported,
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_INTERNAL,
};
