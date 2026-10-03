/*
 * obs-nvenc forensic trace
 *
 * Compile-time gated (CMake option OBS_NVENC_DEBUG_TRACE -> NVENC_DEBUG_TRACE).
 * When the macro is not defined every function below compiles to a static
 * inline no-op, so normal builds have zero overhead and no extra symbols.
 *
 * Runtime enablement (only in debug-trace builds):
 *   OBS_NVENC_TRACE=1|2|3      verbosity level (unset/0 = disabled)
 *   OBS_NVENC_TRACE_DIR=path   output directory (default: <config path>/nvenc-trace)
 *   OBS_NVENC_TRACE_PAYLOADS=1 also hex-dump QP delta / ROI map payloads
 *
 * The trace keeps three concepts strictly separate:
 *   02_PRESET_RETURNED_BY_NVENC  - bytes returned by NvEncGetEncodePresetConfigEx()
 *   03_FINAL_CONFIG_SENT_TO_NVENC - bytes OBS passes to NvEncInitializeEncoder()
 *   07_MEMORY_AFTER_INITIALIZE_CALL - process memory after the call returned.
 *                                     This is NOT a readback of driver/firmware
 *                                     state; no public API exposes it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <ffnvcodec/nvEncodeAPI.h>

/* Field inventory of NV_ENC_CONFIG for the forensic trace (nvenc-trace.c).
 * Version-gated additions keep the list valid on older SDK headers. */
#define NVENC_CONFIG_FIELDS(F) \
	F(version) \
	F(gopLength) \
	F(frameIntervalP) \
	F(monoChromeEncoding) \
	F(frameFieldMode) \
	F(mvPrecision) \
	/* rate control */ \
	F(rcParams.version) \
	F(rcParams.rateControlMode) \
	F(rcParams.constQP.qpInterP) \
	F(rcParams.constQP.qpInterB) \
	F(rcParams.constQP.qpIntra) \
	F(rcParams.averageBitRate) \
	F(rcParams.maxBitRate) \
	F(rcParams.vbvBufferSize) \
	F(rcParams.vbvInitialDelay) \
	F(rcParams.enableMinQP) \
	F(rcParams.minQP.qpInterP) \
	F(rcParams.minQP.qpInterB) \
	F(rcParams.minQP.qpIntra) \
	F(rcParams.enableMaxQP) \
	F(rcParams.maxQP.qpInterP) \
	F(rcParams.maxQP.qpInterB) \
	F(rcParams.maxQP.qpIntra) \
	F(rcParams.enableInitialRCQP) \
	F(rcParams.initialRCQP.qpInterP) \
	F(rcParams.initialRCQP.qpInterB) \
	F(rcParams.initialRCQP.qpIntra) \
	F(rcParams.targetQuality) \
	F(rcParams.targetQualityLSB) \
	F(rcParams.lookaheadDepth) \
	F(rcParams.enableAQ) \
	F(rcParams.aqStrength) \
	F(rcParams.enableLookahead) \
	F(rcParams.enableTemporalAQ) \
	F(rcParams.zeroReorderDelay) \
	F(rcParams.multiPass) \
	F(rcParams.qpMapMode) \
	/* H264 */ \
	F(encodeCodecConfig.h264Config.level) \
	F(encodeCodecConfig.h264Config.idrPeriod) \
	F(encodeCodecConfig.h264Config.intraRefreshPeriod) \
	F(encodeCodecConfig.h264Config.maxNumRefFrames) \
	F(encodeCodecConfig.h264Config.sliceMode) \
	F(encodeCodecConfig.h264Config.sliceModeData) \
	F(encodeCodecConfig.h264Config.ltrNumFrames) \
	F(encodeCodecConfig.h264Config.ltrTrustMode) \
	F(encodeCodecConfig.h264Config.chromaFormatIDC) \
	F(encodeCodecConfig.h264Config.maxTemporalLayers) \
	F(encodeCodecConfig.h264Config.useBFramesAsRef) \
	F(encodeCodecConfig.h264Config.numRefL0) \
	F(encodeCodecConfig.h264Config.numRefL1) \
	F(encodeCodecConfig.h264Config.h264VUIParameters.videoFullRangeFlag) \
	F(encodeCodecConfig.h264Config.h264VUIParameters.colourPrimaries) \
	F(encodeCodecConfig.h264Config.h264VUIParameters.transferCharacteristics) \
	F(encodeCodecConfig.h264Config.h264VUIParameters.colourMatrix) \
	/* HEVC */ \
	F(encodeCodecConfig.hevcConfig.level) \
	F(encodeCodecConfig.hevcConfig.tier) \
	F(encodeCodecConfig.hevcConfig.minCUSize) \
	F(encodeCodecConfig.hevcConfig.maxCUSize) \
	F(encodeCodecConfig.hevcConfig.idrPeriod) \
	F(encodeCodecConfig.hevcConfig.vpsId) \
	F(encodeCodecConfig.hevcConfig.spsId) \
	F(encodeCodecConfig.hevcConfig.ppsId) \
	F(encodeCodecConfig.hevcConfig.sliceMode) \
	F(encodeCodecConfig.hevcConfig.sliceModeData) \
	F(encodeCodecConfig.hevcConfig.maxTemporalLayersMinus1) \
	F(encodeCodecConfig.hevcConfig.ltrTrustMode) \
	F(encodeCodecConfig.hevcConfig.useBFramesAsRef) \
	F(encodeCodecConfig.hevcConfig.numRefL0) \
	F(encodeCodecConfig.hevcConfig.numRefL1) \
	F(encodeCodecConfig.hevcConfig.disableDeblockingFilterIDC) \
	F(encodeCodecConfig.hevcConfig.numTemporalLayers) \
	F(encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFullRangeFlag) \
	F(encodeCodecConfig.hevcConfig.hevcVUIParameters.colourPrimaries) \
	F(encodeCodecConfig.hevcConfig.hevcVUIParameters.transferCharacteristics) \
	F(encodeCodecConfig.hevcConfig.hevcVUIParameters.colourMatrix) \
	/* AV1 */ \
	F(encodeCodecConfig.av1Config.level) \
	F(encodeCodecConfig.av1Config.tier) \
	F(encodeCodecConfig.av1Config.minPartSize) \
	F(encodeCodecConfig.av1Config.maxPartSize) \
	F(encodeCodecConfig.av1Config.idrPeriod) \
	F(encodeCodecConfig.av1Config.intraRefreshPeriod) \
	F(encodeCodecConfig.av1Config.maxNumRefFramesInDPB) \
	F(encodeCodecConfig.av1Config.numTileColumns) \
	F(encodeCodecConfig.av1Config.numTileRows) \
	F(encodeCodecConfig.av1Config.maxTemporalLayersMinus1) \
	F(encodeCodecConfig.av1Config.colorPrimaries) \
	F(encodeCodecConfig.av1Config.transferCharacteristics) \
	F(encodeCodecConfig.av1Config.matrixCoefficients) \
	F(encodeCodecConfig.av1Config.colorRange) \
	F(encodeCodecConfig.av1Config.chromaSamplePosition) \
	F(encodeCodecConfig.av1Config.useBFramesAsRef) \
	F(encodeCodecConfig.av1Config.numFwdRefs) \
	F(encodeCodecConfig.av1Config.numBwdRefs) \
	F(encodeCodecConfig.av1Config.ltrNumFrames) \
	F(encodeCodecConfig.av1Config.numTemporalLayers)

#ifdef NVENC_12_2_OR_LATER
#define NVENC_CONFIG_FIELDS_12_2(F) \
	F(encodeCodecConfig.h264Config.inputBitDepth) \
	F(encodeCodecConfig.h264Config.outputBitDepth) \
	F(encodeCodecConfig.hevcConfig.inputBitDepth) \
	F(encodeCodecConfig.hevcConfig.outputBitDepth) \
	F(encodeCodecConfig.hevcConfig.tfLevel) \
	F(encodeCodecConfig.av1Config.inputBitDepth) \
	F(encodeCodecConfig.av1Config.outputBitDepth)
#else
#define NVENC_CONFIG_FIELDS_12_2(F)
#endif

#ifdef NVENC_13_0_OR_LATER
#define NVENC_CONFIG_FIELDS_13_0(F) \
	F(encodeCodecConfig.h264Config.tfLevel) \
	F(encodeCodecConfig.av1Config.tfLevel)
#else
#define NVENC_CONFIG_FIELDS_13_0(F)
#endif

struct nvenc_data;

#ifdef __cplusplus
extern "C" {
#endif

#if defined(NVENC_DEBUG_TRACE)

/* Create the trace context (called after the NVENC session is opened).
 * Returns NULL when tracing is disabled at runtime (env var not set). */
void *nvenc_trace_init(struct nvenc_data *enc);

/* Flush and close the trace files. */
void nvenc_trace_destroy(struct nvenc_data *enc);

/* Checkpoint 01: capability query result. */
void nvenc_trace_cap(struct nvenc_data *enc, NV_ENC_CAPS cap, int value, NVENCSTATUS status);

/* Checkpoint 02: preset config returned by NvEncGetEncodePresetConfigEx().
 * The preset is deep-copied internally and later used for the diff. */
void nvenc_trace_preset(struct nvenc_data *enc, const NV_ENC_PRESET_CONFIG *preset_config, uint32_t preset_idx,
			uint32_t tuning_idx, uint32_t multipass);

/* Checkpoints 03+04: final config and full initialize params, right before
 * NvEncInitializeEncoder(). Includes the semantic + raw diff vs the preset. */
void nvenc_trace_pre_init(struct nvenc_data *enc, const NV_ENC_CONFIG *config, const NV_ENC_INITIALIZE_PARAMS *params);

/* Checkpoint 07: NvEncInitializeEncoder() return status plus a comparison of
 * process memory after the call (labeled as NOT a driver state readback). */
void nvenc_trace_post_init(struct nvenc_data *enc, const NV_ENC_CONFIG *config, const NV_ENC_INITIALIZE_PARAMS *params,
			   NVENCSTATUS status);

/* Checkpoint 08: NvEncReconfigureEncoder() with a monotonic counter.
 * origin is the calling function name (e.g. "apply_nvenc_reconfigure"). */
void nvenc_trace_reconfigure(struct nvenc_data *enc, const char *origin, const NV_ENC_RECONFIGURE_PARAMS *params,
			     NVENCSTATUS status);

/* Checkpoint 09: per-frame picture parameters (verbosity >= 2).
 * eos=true marks the end-of-stream picture. */
void nvenc_trace_pic(struct nvenc_data *enc, const NV_ENC_PIC_PARAMS *pp, bool eos);

/* Checkpoint 10: sequence parameters returned by NvEncGetSequenceParams(). */
void nvenc_trace_sequence_params(struct nvenc_data *enc, const uint8_t *data, uint32_t size, NVENCSTATUS status);

#else /* !NVENC_DEBUG_TRACE - zero-overhead no-ops */

static inline void *nvenc_trace_init(struct nvenc_data *enc)
{
	(void)enc;
	return NULL;
}

static inline void nvenc_trace_destroy(struct nvenc_data *enc)
{
	(void)enc;
}

static inline void nvenc_trace_cap(struct nvenc_data *enc, NV_ENC_CAPS cap, int value, NVENCSTATUS status)
{
	(void)enc;
	(void)cap;
	(void)value;
	(void)status;
}

static inline void nvenc_trace_preset(struct nvenc_data *enc, const NV_ENC_PRESET_CONFIG *preset_config,
				      uint32_t preset_idx, uint32_t tuning_idx, uint32_t multipass)
{
	(void)enc;
	(void)preset_config;
	(void)preset_idx;
	(void)tuning_idx;
	(void)multipass;
}

static inline void nvenc_trace_pre_init(struct nvenc_data *enc, const NV_ENC_CONFIG *config,
					const NV_ENC_INITIALIZE_PARAMS *params)
{
	(void)enc;
	(void)config;
	(void)params;
}

static inline void nvenc_trace_post_init(struct nvenc_data *enc, const NV_ENC_CONFIG *config,
					 const NV_ENC_INITIALIZE_PARAMS *params, NVENCSTATUS status)
{
	(void)enc;
	(void)config;
	(void)params;
	(void)status;
}

static inline void nvenc_trace_reconfigure(struct nvenc_data *enc, const char *origin,
					   const NV_ENC_RECONFIGURE_PARAMS *params, NVENCSTATUS status)
{
	(void)enc;
	(void)origin;
	(void)params;
	(void)status;
}

static inline void nvenc_trace_pic(struct nvenc_data *enc, const NV_ENC_PIC_PARAMS *pp, bool eos)
{
	(void)enc;
	(void)pp;
	(void)eos;
}

static inline void nvenc_trace_sequence_params(struct nvenc_data *enc, const uint8_t *data, uint32_t size,
					       NVENCSTATUS status)
{
	(void)enc;
	(void)data;
	(void)size;
	(void)status;
}

#endif /* NVENC_DEBUG_TRACE */

#ifdef __cplusplus
}
#endif
