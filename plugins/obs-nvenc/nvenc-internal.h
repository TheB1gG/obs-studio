#pragma once

#include "cuda-helpers.h"
#include "nvenc-helpers.h"

#include <util/deque.h>
#include <opts-parser.h>
#include <abr-gov.h>

#ifdef _WIN32
#define INITGUID
#include <dxgi.h>
#include <d3d11.h>
#include <d3d11_1.h>
#else
#include <glad/glad.h>
#endif

#define do_log(level, format, ...) \
	blog(level, "[obs-nvenc: '%s'] " format, obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR, format, ##__VA_ARGS__)
#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

#define error_hr(msg) error("%s: %s: 0x%08lX", __FUNCTION__, msg, (uint32_t)hr);

#define NV_FAIL(format, ...) nv_fail(enc->encoder, format, ##__VA_ARGS__)
#define NV_FAILED(x) nv_failed(enc->encoder, x, __FUNCTION__, #x)

#ifdef _WIN32
/* CPU readback scaler for staged resize when the D3D11 video processor is unavailable:
 * new-size shared input -> own two-plane texture (full CopyResource, on the SHARED private
 * device enc->priv_dev) -> both planes dumped into single-plane textures by a compute
 * shader through format-override UAVs -> staging textures (Map) -> CPU bilinear scale ->
 * old-size staging textures -> CS plane copies back into the shared two-plane writeback
 * texture -> CopyResource into the registered session-size input. NV12/P010 only.
 * All CPU-mappable work (staging textures, Map, the private writeback texture) runs on the
 * SHARED private device: in some processes the NVENC device's d3d11 runtime state is
 * corrupted (CreateComputeShader faults inside d3d11.dll and afterwards validation-heavy
 * calls like Map / CreateVideoProcessor* return E_INVALIDARG non-deterministically), while
 * plain texture creation and CopyResource on it keep working - so the readback/writeback
 * round trip is done on a private device and only a final CopyResource touches the NVENC
 * device. Plane-level CopySubresourceRegion is deliberately NOT used: on some drivers it
 * silently no-ops for planar subresources (verified 2026-10-23, NVIDIA 617.42/RTX 4090),
 * which would decode as a solid green frame (zero chroma). */
struct d3d11_cpu_scaler {
	ID3D11Texture2D *in_def; /* NEW size two-plane DEFAULT + UNORDERED_ACCESS (on enc->priv_dev): full CopyResource target for the shared input */
	ID3D11UnorderedAccessView *in_uav[2]; /* override views on in_def planes (R8/R8G8 or R16/R16G16) */
	ID3D11Texture2D *y_dump_def; /* NEW size single-plane DEFAULT + UNORDERED_ACCESS: CS dump target for the Y plane */
	ID3D11UnorderedAccessView *y_dump_uav;
	ID3D11Texture2D *uv_dump_def; /* NEW/2 single-plane DEFAULT + UNORDERED_ACCESS: CS dump target for the UV plane */
	ID3D11UnorderedAccessView *uv_dump_uav;
	ID3D11Texture2D *y_rb_tex; /* NEW size Y plane, SINGLE-PLANE STAGING + READ (R8/R16, on enc->priv_dev) */
	ID3D11Texture2D *uv_rb_tex; /* NEW/2 UV plane, SINGLE-PLANE STAGING + READ (R8G8/R16G16) */
	ID3D11Texture2D *y_wb_tex; /* OLD size Y plane, SINGLE-PLANE STAGING + WRITE (R8/R16, on enc->priv_dev) */
	ID3D11Texture2D *uv_wb_tex; /* OLD/2 UV plane, SINGLE-PLANE STAGING + WRITE (R8G8/R16G16) */
	ID3D11Texture2D *y_wb_def; /* OLD size single-plane DEFAULT + UNORDERED_ACCESS: CS dump source for the Y writeback */
	ID3D11UnorderedAccessView *y_wb_def_uav;
	ID3D11Texture2D *uv_wb_def; /* OLD/2 single-plane DEFAULT + UNORDERED_ACCESS: CS dump source for the UV writeback */
	ID3D11UnorderedAccessView *uv_wb_def_uav;
	ID3D11Texture2D *writeback_tex; /* OLD (session) size two-plane, SHARED KEYED MUTEX (on enc->priv_dev) */
	ID3D11UnorderedAccessView *wb_y_uav; /* override view on writeback_tex plane 0 (R8/R16) */
	ID3D11UnorderedAccessView *wb_uv_uav; /* override view on writeback_tex plane 1 (R8G8/R16G16) */
	ID3D11Texture2D *wb_opened; /* writeback_tex opened on the NVENC device: CopyResource source for nvtex */
	IDXGIKeyedMutex *wb_km; /* writeback_tex keyed mutex, private-device side (writer lock) */
	IDXGIKeyedMutex *wb_km_enc; /* same mutex via wb_opened, NVENC device side (reader lock) */
	ID3D11ComputeShader *cs_copy; /* plane-copy shader: the embedded scaler with an identity-size cbuffer (exact pixel copy) */
	ID3D11Buffer *cb_in; /* identity cbuffer {new_w,new_h,new_w/2,new_h/2, x4} for the readback dump */
	ID3D11Buffer *cb_out; /* identity cbuffer at the OLD (session) size for the writeback dump */
	bool cs_faulted; /* CreateComputeShader faulted inside d3d11.dll/driver - never retry */
	uint8_t *buf[2]; /* scaled old-size planes (Y, then interleaved UV) */
	int stride[2];   /* bytes per row in buf[] */
	uint32_t new_w;  /* size the readback objects were created for */
	uint32_t new_h;
	uint32_t old_w;  /* session size the writeback texture / buffers were created for */
	uint32_t old_h;
	bool ready;
};

/* GPU compute-shader scaler for staged resize when the D3D11 video processor is
 * unavailable: new-size shared input -> own intermediate texture (on the SHARED private
 * device enc->priv_dev) -> bilinear CS via format-override UAVs (Load-based - planar
 * SRVs/RTVs and cross-plane copies are not usable on some drivers) -> old-size
 * intermediate texture, opened on the NVENC device under a keyed mutex -> registered
 * session-size input texture. The private device is used because in some processes
 * CreateComputeShader faults inside d3d11.dll on the NVENC device itself (see the
 * shader_faulted latch); only the final CopyResource touches the NVENC device.
 * NV12/P010 only; primary fallback ahead of the CPU readback path. */
struct d3d11_gpu_scaler {
	ID3D11ComputeShader *cs;
	ID3D11Texture2D *src_tex; /* NEW size, DEFAULT + UNORDERED_ACCESS (on enc->priv_dev): receives CopyResource from the shared input */
	ID3D11UnorderedAccessView *src_uav[2]; /* override views on src_tex planes (R8/R8G8 or R16/R16G16) */
	ID3D11Texture2D *dst_tex; /* OLD (session) size, DEFAULT + UNORDERED_ACCESS + SHARED KEYED MUTEX (on enc->priv_dev) */
	ID3D11UnorderedAccessView *dst_uav[2];
	ID3D11Texture2D *wb_opened; /* dst_tex opened on the NVENC device: CopyResource source for nvtex */
	IDXGIKeyedMutex *wb_km; /* dst_tex keyed mutex, private-device side (writer lock) */
	IDXGIKeyedMutex *wb_km_enc; /* same mutex via wb_opened, NVENC device side (reader lock) */
	ID3D11Buffer *cb; /* {src_w, src_h, src_w/2, src_h/2, dst_w, dst_h, dst_w/2, dst_h/2} */
	uint32_t src_w; /* size the src texture / CB were created for */
	uint32_t src_h;
	uint32_t dst_w; /* session size the dst texture / CB were created for */
	uint32_t dst_h;
	bool ready;
	bool shader_faulted; /* CreateComputeShader faulted inside d3d11.dll/driver - never retry, use VP/CPU fallback */
};
#endif

/* ------------------------------------------------------------------------- */
/* Main Implementation Structure                                             */

struct nvenc_properties {
	int64_t bitrate;
	int64_t max_bitrate;
	int64_t keyint_sec;
	int64_t cqp;
	int64_t device;
	int64_t bf;
	int64_t bframe_ref_mode;
	int64_t split_encode;
	int64_t target_quality;

	const char *rate_control;
	const char *preset;
	const char *profile;
	const char *tune;
	const char *multipass;
	const char *opts_str;

	bool adaptive_quantization;
	bool lookahead;
	bool disable_scenecut;
	bool repeat_headers;
	bool force_cuda_tex;

	uint32_t max_encode_width;  /* 0 = not specified (no headroom) */
	uint32_t max_encode_height; /* 0 = not specified (no headroom) */

	struct obs_options opts;
	obs_data_t *data;
};

struct nvenc_data {
	obs_encoder_t *encoder;
	enum codec_type codec;
	GUID codec_guid;

	void *session;
	NV_ENC_INITIALIZE_PARAMS params;
	NV_ENC_CONFIG config;
	uint32_t buf_count;
	int output_delay;
	int buffers_queued;
	size_t next_bitstream;
	size_t cur_bitstream;
	bool encode_started;
	bool first_packet;
	bool can_change_bitrate;
	bool non_texture;

	/* Forensic trace context (nvenc-trace.c), NULL when tracing is off. */
	void *trace;

	/* NVENC bakes its SPS/VUI at create() time, before the delivery mix is bound to
	 * encoder->media, so the identity-RGB VUI override in init_encoder_* may miss RGB
	 * encodes. vui_identity_applied records what the SPS was generated with; the
	 * first-encode path re-checks and, if RGB turned out active, regenerates the SPS
	 * via reconfigure (see nvenc_encode_base). */
	bool vui_identity_applied;
	bool vui_identity_checked;

	DARRAY(struct handle_tex) input_textures;
	DARRAY(struct nv_bitstream) bitstreams;
	DARRAY(struct nv_cuda_surface) surfaces;
	NV_ENC_BUFFER_FORMAT surface_format;
	struct deque dts_list;

	DARRAY(uint8_t) packet_data;
	int64_t packet_pts;
	bool packet_keyframe;

	/* Live RC/bitrate changes are staged here and applied at the next */
	/* GOP boundary so sibling encoders of a multitrack output stay   */
	/* keyframe-aligned (see nvenc_update / nvenc_encode_base).       */
	bool reconfig_pending;
	int64_t frames_since_idr; /* submitted frames since last observed IDR packet */

	/* Staged multitrack re-init at the shared keyframe boundary (mirrors the QSV/x264/
	 * x265 pattern): resize_pending / fps_pending are staged by nvenc_maybe_resize /
	 * nvenc_check_fps and applied together by nvenc_maybe_apply_timed_reinit(), so the
	 * session-start IDR lands exactly on the shared keyframe grid. While a resize is
	 * staged, incoming new-size frames are scaled down to the current session size
	 * (backend-specific: D3D11 video processor / CPU bilinear scaler) until then. */
	bool resize_pending;      /* live size change staged at reinit_target_pts */
	bool fps_pending;         /* live effective-FPS change staged at reinit_target_pts */
	bool fps_change_staged;   /* FPS change seen in nvenc_update, awaiting boundary staging */
	uint32_t pending_width;   /* staged resize target size (0 = none) */
	uint32_t pending_height;
	int64_t reinit_target_pts; /* shared keyframe boundary where the staged re-init fires */
	int64_t reinit_misses;    /* boundary misses tolerated before applying late */
	int64_t last_keyframe_pts; /* most recent steady-state on-grid keyframe pts (-1 = none yet) */
	int64_t gop_pts;          /* measured keyframe cadence in pts units (0 = unknown) */
	/* After an off-grid (emergency/late) re-init the session-start IDR has shifted NVENC's
	 * internal GOP phase off the shared grid: schedule a forced IDR at this on-grid boundary
	 * to re-anchor it (0 = none pending). While set, observed IDRs must not move
	 * last_keyframe_pts/gop_pts (see get_encoded_packet / nvenc_encode_base). */
	int64_t reanchor_idr_pts;

	/* Old-resolution buffers used to scale incoming new-size frames while a staged
	 * resize waits for the shared keyframe boundary (raw/CPU path only - mirrors
	 * obs-x264). Sized per input format: NV12/P010 use planes 0+1, I444 uses 0..2.
	 * NULL when unused. */
	uint8_t *scale_buf[3];
	int scale_stride[3];

#ifdef _WIN32
	DARRAY(struct nv_texture) textures;
	/* Old-size textures kept alive after a live resize (resetEncoder=0 path).
	 * In-flight frames still reference these; freed after buf_count frames. */
	DARRAY(struct nv_texture) old_textures;
	uint32_t old_tex_frames_remaining;
	ID3D11Device *device;
	ID3D11DeviceContext *context;

	/* Private D3D11 device on the same adapter as enc->device. The staged-resize GPU
	 * compute-shader scaler and CPU readback scaler run on it because in some processes
	 * (OBS with other D3D11 devices present) the encoder's own d3d11 runtime state is
	 * corrupted: validation-heavy calls (CreateComputeShader, Map on planar plane 1,
	 * CreateVideoProcessor*) fail or fault there, while plain texture creation and
	 * CopyResource keep working. Lazily created when a staged resize needs a scaler. */
	ID3D11Device *priv_dev;
	ID3D11DeviceContext *priv_ctx;

	/* D3D11 video processor used to scale new-size frames down to the current session
	 * size while a staged resize waits for the shared keyframe boundary (the texture
	 * path cannot CPU-scale). Lazily created on first use; NULL when the driver does
	 * not support it for the active format. */
	ID3D11VideoDevice *video_device;
	ID3D11VideoContext *video_context;
	ID3D11VideoProcessorEnumerator *vp_enumerator;
	ID3D11VideoProcessor *vp_processor;
	uint32_t vp_dst_w; /* session size the processor was created for */
	uint32_t vp_dst_h;

	/* Consecutive staged-scaling frame failures (D3D11 texture path): after two in a row,
	 * no scaler can handle this driver, so the pending re-init is applied immediately
	 * (off-grid) instead of erroring the stream. */
	int scale_fail_count;

	/* CPU readback scaler - staged-resize fallback when the video processor is unavailable:
	 * each plane of the new-size shared input is copied into its own single-plane CPU-mappable
	 * staging texture, bilinear-scaled to the current session size on the CPU
	 * (nvenc_scale_nv12/p010), written into an old-size staging texture and copied into the
	 * registered input texture. NV12/P010 only. */
	struct d3d11_cpu_scaler cpu_scaler;

	/* GPU compute-shader scaler - staged-resize fallback ahead of the CPU readback path:
	 * stays fully on the GPU (no PCIe round-trip, no Map sync stall), running on the shared
	 * private device enc->priv_dev. Lazily created when the video processor is unavailable;
	 * NV12/P010 only. */
	struct d3d11_gpu_scaler gpu_scaler;
#endif

	/* ABR quality-aim changes are applied without reset/IDR (the same soft
	 * mechanism the ABR governor uses for maxBitRate), so they can fire on any
	 * frame - no keyframe alignment needed. Staged to the encode thread like
	 * reconfig_pending (see nvenc_update / nvenc_encode_base). */
	bool tq_soft_pending;

	/* ABR governor (ABR mode only): runs NVENC in QVBR (averageBitRate=0) so the
	 * driver's sustained-rate cap of min(maxBitRate, 2*averageBitRate) doesn't
	 * apply, then adapts maxBitRate at runtime so the long-term average tracks
	 * props.bitrate. The control law, tick cadence and stream-session budget live in
	 * the shared abr-gov module; abr_state is the per-open latch (cur_rate = applied
	 * maxBitRate in bps) and survives stop/start via abr_gov_session_load/save. */
	bool          abr_active;  /* ABR mode selected (gates the governor) */
	double        abr_deadband; /* settings "deadband" as a fraction (0.005 = +/-0.5%) */
	abr_gov_state   abr_state;   /* session budget + current governed maxBitRate (bps) */
	bool          abr_session_loaded; /* true once the session budget has been loaded this open (re-latch thereafter) */
	NV_ENC_QP preset_min_qp;        /* rcParams.minQP from the preset config (restored on live switch away from ABR) */
	bool      preset_enable_min_qp; /* rcParams.enableMinQP from the preset config */

	uint32_t cx;
	uint32_t cy;
	enum video_format in_format;

	uint8_t *header;
	size_t header_size;

	uint8_t *sei;
	size_t sei_size;

	int8_t *roi_map;
	size_t roi_map_size;
	uint32_t roi_increment;

#ifdef NVENC_13_0_OR_LATER
	CONTENT_LIGHT_LEVEL *cll;
	MASTERING_DISPLAY_INFO *mdi;
#endif

	struct nvenc_properties props;

	CUcontext cu_ctx;
};

/* ------------------------------------------------------------------------- */
/* Resource data structures                                                  */

/* Input texture handles */
struct handle_tex {
#ifdef _WIN32
	uint32_t handle;
	ID3D11Texture2D *tex;
	IDXGIKeyedMutex *km;
	/* Cached video-processor input view for staged-resize scaling (lazy). */
	ID3D11VideoProcessorInputView *vp_view;
#else
	GLuint tex_id;
	/* CUDA mappings */
	CUgraphicsResource res_y;
	CUgraphicsResource res_uv;
#endif
};

/* Bitstream buffer */
struct nv_bitstream {
	void *ptr;
};

/** Mapped resources **/
/* CUDA Arrays */
struct nv_cuda_surface {
	CUarray tex;
	NV_ENC_REGISTERED_PTR res;
	NV_ENC_INPUT_PTR *mapped_res;
};

#ifdef _WIN32
/* DX11 textures */
struct nv_texture {
	void *res;
	ID3D11Texture2D *tex;
	void *mapped_res;
	/* Cached video-processor output view for staged-resize scaling (lazy). */
	ID3D11VideoProcessorOutputView *vp_view;
};
#endif

/* ------------------------------------------------------------------------- */
/* Shared functions                                                          */

bool nvenc_encode_base(struct nvenc_data *enc, struct nv_bitstream *bs, void *pic, int64_t pts,
		       struct encoder_packet *packet, bool *received_packet);

/* Per-frame live-change handling for the multitrack encode path. Applies a staged
 * boundary re-init when due (so its session-start IDR lands on-grid), stages or
 * immediately applies a live resolution change, and stages/applies a live effective-
 * FPS change. Called from every backend encode entry before any input resource copy
 * of this frame. Returns false when the current frame must be skipped because a
 * re-init failed (pending state kept for the next frame). */
bool nvenc_prepare_frame(struct nvenc_data *enc, int64_t pts);

/* Bilinear scalers used while a staged multitrack resize waits for the shared keyframe
 * boundary: scale a new-size frame into buffers of the current session (old) size.
 * Defined in nvenc-cuda.c; shared by the raw/CPU path and the D3D11 CPU readback path. */
void nvenc_scale_nv12(const uint8_t *src_y, const uint8_t *src_uv, int src_w, int src_h, int src_ls0, int src_ls1,
		      uint8_t *dst_y, uint8_t *dst_uv, int dst_w, int dst_h, int dst_ls0, int dst_ls1);
void nvenc_scale_p010(const uint8_t *src_y, const uint8_t *src_uv, int src_w, int src_h, int src_ls0, int src_ls1,
		      uint8_t *dst_y, uint8_t *dst_uv, int dst_w, int dst_h, int dst_ls0, int dst_ls1);

/* ------------------------------------------------------------------------- */
/* Backend-specific functions                                                */

#ifdef _WIN32
/** D3D11 **/
bool d3d11_init(struct nvenc_data *enc, obs_data_t *settings);
void d3d11_free(struct nvenc_data *enc);

bool d3d11_init_textures(struct nvenc_data *enc);
void d3d11_free_textures(struct nvenc_data *enc);

/* Whether a staged (timed) resize can scale new-size frames down to the current
 * session size on this backend/format, lazily creating the D3D11 video processor.
 * Returns false when the driver does not support it (live resize then falls back
 * to an immediate in-place re-init). */
bool d3d11_staged_resize_supported(struct nvenc_data *enc);

/* Scale a new-size input texture into the current session-size staging texture.
 * Uses the D3D11 video processor when available, otherwise the CPU readback scaler
 * (NV12/P010). Runs while holding the input keyed mutex on the NVENC device. The GPU
 * compute-shader scaler is deliberately not routed through here: it reads the shared
 * input on its own private device and must hold that device's AcquireSync itself
 * (probe T22), so d3d11_encode dispatches to it directly. */
bool d3d11_scale_to_session_size(struct nvenc_data *enc, struct handle_tex *ht, struct nv_texture *nvtex);

/* Tear down the CPU readback scaler (staged-resize fallback when no video processor). */
void d3d11_free_cpu_scaler(struct nvenc_data *enc);

/* Tear down the GPU compute-shader scaler (staged-resize fallback ahead of the CPU path). */
void d3d11_free_gpu_scaler(struct nvenc_data *enc);

bool d3d11_encode(void *data, struct encoder_texture *texture, int64_t pts, uint64_t lock_key, uint64_t *next_key,
		  struct encoder_packet *packet, bool *received_packet);

/* Emergency path (D3D11 texture backend): when no staged-resize scaler can scale the
 * new-size frames on this driver, apply the pending staged re-init immediately at the
 * current frame instead of waiting for the shared keyframe boundary. The forced IDR
 * lands off-grid - a deliberate trade-off so the stream keeps running with continuous
 * DTS instead of erroring out. On failure the pending state is kept (retry next frame). */
bool nvenc_apply_staged_changes_now(struct nvenc_data *enc, int64_t pts);
#endif

/** CUDA **/
bool cuda_ctx_init(struct nvenc_data *enc, obs_data_t *settings, bool texture);
void cuda_ctx_free(struct nvenc_data *enc);

bool cuda_init_surfaces(struct nvenc_data *enc);
void cuda_free_surfaces(struct nvenc_data *enc);

bool cuda_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received_packet);

#ifndef _WIN32
/** CUDA OpenGL **/
void cuda_opengl_free(struct nvenc_data *enc);
bool cuda_opengl_encode(void *data, struct encoder_texture *tex, int64_t pts, uint64_t lock_key, uint64_t *next_key,
			struct encoder_packet *packet, bool *received_packet);
#endif

/* ------------------------------------------------------------------------- */
/* Properties crap                                                           */

void nvenc_properties_read(struct nvenc_properties *enc, obs_data_t *settings);

void h264_nvenc_defaults(obs_data_t *settings);
void hevc_nvenc_defaults(obs_data_t *settings);
void av1_nvenc_defaults(obs_data_t *settings);

obs_properties_t *h264_nvenc_properties(void *);
obs_properties_t *hevc_nvenc_properties(void *);
obs_properties_t *av1_nvenc_properties(void *);

/* Custom argument parsing */
bool apply_user_args(struct nvenc_data *enc);
bool get_user_arg_int(struct nvenc_data *enc, const char *name, int *val);
