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

#include "QSV_Encoder_Internal.h"
#include "QSV_Encoder.h"
#include <vpl/mfxstructures.h>
#include <vpl/mfxvideo++.h>
#include <vpl/mfxdispatcher.h>
#include <obs-module.h>
#ifdef _WIN32
#include "common_directx11.h"
#endif

#define do_log(level, format, ...) blog(level, "[qsv encoder: '%s'] " format, "msdk_impl", ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

mfxHDL QSV_Encoder_Internal::g_GFX_Handle = NULL;
mfxU16 QSV_Encoder_Internal::g_numEncodersOpen = 0;

QSV_Encoder_Internal::QSV_Encoder_Internal(mfxVersion &version, bool useTexAlloc)
	: m_pmfxSurfaces(NULL),
	  m_pmfxENC(NULL),
	  m_nSPSBufferSize(1024),
	  m_nPPSBufferSize(1024),
	  m_nTaskPool(0),
	  m_pTaskPool(NULL),
	  m_nTaskIdx(0),
	  m_nFirstSyncTask(0),
	  m_outBitstream(),
	  m_bUseD3D11(false),
	  m_bUseTexAlloc(useTexAlloc),
	  m_sessionData(NULL),
	  m_ver(version)
{
	mfxVariant tempImpl;
	mfxStatus sts;

	mfxLoader loader = MFXLoad();
	mfxConfig cfg = MFXCreateConfig(loader);

	tempImpl.Type = MFX_VARIANT_TYPE_U32;
	tempImpl.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
	MFXSetConfigFilterProperty(cfg, (const mfxU8 *)"mfxImplDescription.Impl", tempImpl);

	tempImpl.Type = MFX_VARIANT_TYPE_U32;
	tempImpl.Data.U32 = INTEL_VENDOR_ID;
	MFXSetConfigFilterProperty(cfg, (const mfxU8 *)"mfxImplDescription.VendorID", tempImpl);
#if defined(_WIN32)
	m_bUseD3D11 = true;
	m_bUseTexAlloc = true;

	tempImpl.Type = MFX_VARIANT_TYPE_U32;
	tempImpl.Data.U32 = MFX_ACCEL_MODE_VIA_D3D11;
	MFXSetConfigFilterProperty(cfg, (const mfxU8 *)"mfxImplDescription.AccelerationMode", tempImpl);
#else
	tempImpl.Type = MFX_VARIANT_TYPE_U32;
	tempImpl.Data.U32 = MFX_ACCEL_MODE_VIA_VAAPI;
	MFXSetConfigFilterProperty(cfg, (const mfxU8 *)"mfxImplDescription.AccelerationMode", tempImpl);
#endif
	sts = MFXCreateSession(loader, 0, &m_session);
	if (sts == MFX_ERR_NONE) {
		MFXQueryVersion(m_session, &version);
		MFXClose(m_session);
		MFXUnload(loader);

		blog(LOG_DEBUG, "\tsurf:           %s", m_bUseTexAlloc ? "Texture" : "SysMem");

		m_ver = version;
		return;
	}
}

QSV_Encoder_Internal::~QSV_Encoder_Internal()
{
	if (m_pmfxENC)
		ClearData();
}

mfxStatus QSV_Encoder_Internal::Open(qsv_param_t *pParams, enum qsv_codec codec)
{
	mfxStatus sts = MFX_ERR_NONE;

	if (m_bUseD3D11 | m_bUseTexAlloc)
		// Use texture surface
		sts = Initialize(m_ver, &m_session, &m_mfxAllocator, &g_GFX_Handle, false, codec, &m_sessionData);
	else
		sts = Initialize(m_ver, &m_session, NULL, NULL, false, codec, &m_sessionData);

	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	m_pmfxENC = new MFXVideoENCODE(m_session);

	{
		mfxVersion qv = {{0, 0}};
		MFXQueryVersion(m_session, &qv);
		blog(LOG_DEBUG, "QSV session opened: codec=%u VPL runtime=%u.%u IOPattern-surface=%s",
		     (unsigned)codec, qv.Major, qv.Minor, m_bUseTexAlloc ? "D3D11-video-memory" : "system-memory");
	}

	InitParams(pParams, codec);
	sts = m_pmfxENC->Query(&m_mfxEncParams, &m_mfxEncParams);
	if (sts == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
		blog(LOG_DEBUG, "[qsv encoder] MFXVideoENCODE_Query modified encoding parameters "
		     "(MFX_WRN_INCOMPATIBLE_VIDEO_PARAM). The driver may have changed your rate control "
		     "mode, profile, or other settings to values it supports. Check the log for the "
		     "actual parameters in use.");
	}
	MSDK_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = AllocateSurfaces();
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = m_pmfxENC->Init(&m_mfxEncParams);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = GetVideoParam(codec);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = InitBitstream();
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	if (sts >= MFX_ERR_NONE) {
		g_numEncodersOpen++;
	}
	return sts;
}

PRAGMA_WARN_PUSH
PRAGMA_WARN_DEPRECATION
static inline bool HasOptimizedBRCSupport(const mfxPlatform &platform, const mfxVersion &version, mfxU16 rateControl)
{
#if (MFX_VERSION_MAJOR >= 2 && MFX_VERSION_MINOR >= 13) || MFX_VERSION_MAJOR > 2
	if ((version.Major >= 2 && version.Minor >= 13) || version.Major > 2)
		if (rateControl == MFX_RATECONTROL_CBR &&
		    (platform.CodeName >= MFX_PLATFORM_BATTLEMAGE && platform.CodeName != MFX_PLATFORM_ALDERLAKE_N))
			return true;
#endif
	UNUSED_PARAMETER(platform);
	UNUSED_PARAMETER(version);
	UNUSED_PARAMETER(rateControl);
	return false;
}

static inline bool HasAV1ScreenContentSupport(const mfxPlatform &platform, const mfxVersion &version)
{
#if (MFX_VERSION_MAJOR >= 2 && MFX_VERSION_MINOR >= 12) || MFX_VERSION_MAJOR > 2
	// Platform enums needed are introduced in VPL version 2.12
	if ((version.Major >= 2 && version.Minor >= 12) || version.Major > 2)
		if (platform.CodeName >= MFX_PLATFORM_LUNARLAKE && platform.CodeName != MFX_PLATFORM_ALDERLAKE_N &&
		    platform.CodeName != MFX_PLATFORM_ARROWLAKE)
			return true;
#endif
	UNUSED_PARAMETER(platform);
	UNUSED_PARAMETER(version);
	return false;
}
PRAGMA_WARN_POP

mfxStatus QSV_Encoder_Internal::InitParams(qsv_param_t *pParams, enum qsv_codec codec)
{
	memset(&m_mfxEncParams, 0, sizeof(m_mfxEncParams));

	if (codec == QSV_CODEC_AVC)
		m_mfxEncParams.mfx.CodecId = MFX_CODEC_AVC;
	else if (codec == QSV_CODEC_AV1)
		m_mfxEncParams.mfx.CodecId = MFX_CODEC_AV1;
	else if (codec == QSV_CODEC_HEVC)
		m_mfxEncParams.mfx.CodecId = MFX_CODEC_HEVC;
	else if (codec == QSV_CODEC_VP9)
		m_mfxEncParams.mfx.CodecId = MFX_CODEC_VP9;

	if (codec == QSV_CODEC_HEVC) {
		m_mfxEncParams.mfx.NumSlice = 0;
		m_mfxEncParams.mfx.IdrInterval = 1;
	} else if (codec == QSV_CODEC_VP9) {
		// VP9 has no slice concept (it uses tiles instead); do not force a
		// slice structure.
		m_mfxEncParams.mfx.NumSlice = 0;
	} else {
		m_mfxEncParams.mfx.NumSlice = 1;
	}
	m_mfxEncParams.mfx.TargetUsage = pParams->nTargetUsage;
	m_mfxEncParams.mfx.CodecProfile = pParams->nCodecProfile;
	m_mfxEncParams.mfx.FrameInfo.FrameRateExtN = pParams->nFpsNum;
	m_mfxEncParams.mfx.FrameInfo.FrameRateExtD = pParams->nFpsDen;
	m_mfxEncParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
	if (pParams->video_fmt_p010) {
		m_mfxEncParams.mfx.FrameInfo.FourCC = MFX_FOURCC_P010;
		m_mfxEncParams.mfx.FrameInfo.BitDepthChroma = 10;
		m_mfxEncParams.mfx.FrameInfo.BitDepthLuma = 10;
		m_mfxEncParams.mfx.FrameInfo.Shift = 1;
	} else if (pParams->video_fmt_ayuv) {
		m_mfxEncParams.mfx.FrameInfo.FourCC = MFX_FOURCC_AYUV;
		m_mfxEncParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV444;
		// Explicit 8-bit 4:4:4 (spec §10): do not leave bit depth at 0.
		m_mfxEncParams.mfx.FrameInfo.BitDepthLuma = 8;
		m_mfxEncParams.mfx.FrameInfo.BitDepthChroma = 8;
		m_mfxEncParams.mfx.FrameInfo.Shift = 0;
	} else if (pParams->video_fmt_y410) {
		m_mfxEncParams.mfx.FrameInfo.FourCC = MFX_FOURCC_Y410;
		m_mfxEncParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV444;
		m_mfxEncParams.mfx.FrameInfo.BitDepthChroma = 10;
		m_mfxEncParams.mfx.FrameInfo.BitDepthLuma = 10;
		m_mfxEncParams.mfx.FrameInfo.Shift = 1;
	} else {
		m_mfxEncParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
		// Explicit 8-bit NV12 baseline (spec §6): do not leave bit depth at 0.
		m_mfxEncParams.mfx.FrameInfo.BitDepthLuma = 8;
		m_mfxEncParams.mfx.FrameInfo.BitDepthChroma = 8;
		m_mfxEncParams.mfx.FrameInfo.Shift = 0;
	}
	m_mfxEncParams.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
	m_mfxEncParams.mfx.FrameInfo.CropX = 0;
	m_mfxEncParams.mfx.FrameInfo.CropY = 0;
	m_mfxEncParams.mfx.FrameInfo.CropW = pParams->nWidth;
	m_mfxEncParams.mfx.FrameInfo.CropH = pParams->nHeight;
	m_mfxEncParams.mfx.GopRefDist = pParams->nbFrames + 1;
	if (codec == QSV_CODEC_VP9) {
		// No traditional B-frame pipeline for VP9: GopRefDist=1 means only I/P
		// frames. This does NOT disable VP9's native LAST/GOLDEN/ALTREF
		// reference machinery, which the encoder still uses for temporal
		// prediction.
		m_mfxEncParams.mfx.GopRefDist = 1;
	}

	mfxPlatform platform;
	MFXVideoCORE_QueryPlatform(m_session, &platform);

	PRAGMA_WARN_PUSH
	PRAGMA_WARN_DEPRECATION
	if (codec == QSV_CODEC_AVC || codec == QSV_CODEC_HEVC) {
		if (platform.CodeName >= MFX_PLATFORM_DG2)
			m_mfxEncParams.mfx.LowPower = MFX_CODINGOPTION_ON;
	} else if (codec == QSV_CODEC_AV1) {
		m_mfxEncParams.mfx.LowPower = MFX_CODINGOPTION_ON;
	} else if (codec == QSV_CODEC_VP9) {
		// Intel's VP9 hardware implementation is tied to the Low Power /
		// fixed-function path; the LowPower=OFF path is not supported. Always
		// enable it (no "try low power, then fall back" for VP9).
		m_mfxEncParams.mfx.LowPower = MFX_CODINGOPTION_ON;
	}
	PRAGMA_WARN_POP

	m_mfxEncParams.mfx.RateControlMethod = pParams->nRateControl;

	switch (pParams->nRateControl) {
	case MFX_RATECONTROL_CBR:
		m_mfxEncParams.mfx.TargetKbps = pParams->nTargetBitRate;

		if (codec == QSV_CODEC_VP9) {
			// True CBR for the bitrate-constrained streaming target: cap the
			// max bitrate at the target so the BRC stays within the hard
			// network budget.
			m_mfxEncParams.mfx.MaxKbps = pParams->nTargetBitRate;
		}

		if (HasOptimizedBRCSupport(platform, m_ver, pParams->nRateControl)) {
			m_mfxEncParams.mfx.BufferSizeInKB = (pParams->nTargetBitRate / 8) * 1;
		} else {
			m_mfxEncParams.mfx.BufferSizeInKB = (pParams->nTargetBitRate / 8) * 2;
		}

		m_mfxEncParams.mfx.InitialDelayInKB = m_mfxEncParams.mfx.BufferSizeInKB / 2;
		break;
	case MFX_RATECONTROL_VBR:
		m_mfxEncParams.mfx.TargetKbps = pParams->nTargetBitRate;
		m_mfxEncParams.mfx.MaxKbps = pParams->nMaxBitRate;
		m_mfxEncParams.mfx.BufferSizeInKB = (pParams->nTargetBitRate / 8) * 2;
		m_mfxEncParams.mfx.InitialDelayInKB = (pParams->nTargetBitRate / 8) * 1;
		break;
	case MFX_RATECONTROL_CQP:
		m_mfxEncParams.mfx.QPI = pParams->nQPI;
		m_mfxEncParams.mfx.QPB = pParams->nQPB;
		m_mfxEncParams.mfx.QPP = pParams->nQPP;
		break;
	case MFX_RATECONTROL_ICQ:
		m_mfxEncParams.mfx.ICQQuality = pParams->nICQQuality;
		break;
	default:
		break;
	}

	m_mfxEncParams.AsyncDepth = pParams->nAsyncDepth;
	m_mfxEncParams.mfx.GopPicSize =
		(pParams->nKeyIntSec) ? (mfxU16)(pParams->nKeyIntSec * pParams->nFpsNum / (float)pParams->nFpsDen)
				      : 240;

	memset(&m_co2, 0, sizeof(mfxExtCodingOption2));
	m_co2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
	m_co2.Header.BufferSz = sizeof(m_co2);
	if (pParams->bRepeatHeaders)
		m_co2.RepeatPPS = MFX_CODINGOPTION_ON;
	else
		m_co2.RepeatPPS = MFX_CODINGOPTION_OFF;

	m_co2.AdaptiveB = MFX_CODINGOPTION_ON;

	if (pParams->nbFrames > 1)
		m_co2.BRefType = MFX_B_REF_PYRAMID;

	if (codec == QSV_CODEC_VP9) {
		// Strict, predictable GOP for streaming: disable adaptive frame-type
		// decisions so bitrate/decoder behavior stays deterministic.
		m_co2.AdaptiveB = MFX_CODINGOPTION_OFF;
	}

	PRAGMA_WARN_PUSH
	PRAGMA_WARN_DEPRECATION
	// LA VME/ENC case for older platforms
	if (pParams->nLADEPTH && codec == QSV_CODEC_AVC && m_mfxEncParams.mfx.LowPower != MFX_CODINGOPTION_ON &&
	    platform.CodeName >= MFX_PLATFORM_ICELAKE) {
		if (pParams->nRateControl == MFX_RATECONTROL_CBR) {
			pParams->nRateControl = MFX_RATECONTROL_LA_HRD;
		} else if (pParams->nRateControl == MFX_RATECONTROL_VBR) {
			pParams->nRateControl = MFX_RATECONTROL_LA;
		} else if (pParams->nRateControl == MFX_RATECONTROL_ICQ) {
			pParams->nRateControl = MFX_RATECONTROL_LA_ICQ;
		}
		m_co2.LookAheadDepth = pParams->nLADEPTH;
	}
	PRAGMA_WARN_POP

	// LA VDENC case for newer platform, works only under CBR / VBR.
	// VP9 is excluded: generic Intel QSV lookahead BRC is unverified/unsupported
	// for the low-power VP9 hardware path, so we never attach a LookAheadDepth to
	// it (the OBS latency setting must not create hidden VP9 lookahead).
	if (pParams->nRateControl == MFX_RATECONTROL_CBR || pParams->nRateControl == MFX_RATECONTROL_VBR) {
		if (pParams->nLADEPTH && m_mfxEncParams.mfx.LowPower == MFX_CODINGOPTION_ON && codec != QSV_CODEC_VP9) {
			m_co2.LookAheadDepth = pParams->nLADEPTH;
		}
	}

	extendedBuffers.push_back((mfxExtBuffer *)&m_co2);

	const bool optBrcSupport = HasOptimizedBRCSupport(platform, m_ver, pParams->nRateControl);
	if (pParams->video_fmt_ayuv || pParams->video_fmt_y410 || optBrcSupport || codec == QSV_CODEC_VP9) {
		memset(&m_co3, 0, sizeof(mfxExtCodingOption3));
		m_co3.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
		m_co3.Header.BufferSz = sizeof(m_co3);

		if (optBrcSupport) {
			m_co3.WinBRCSize = pParams->nFpsNum / pParams->nFpsDen;

			if (codec == QSV_CODEC_AVC || codec == QSV_CODEC_HEVC) {
				m_co3.WinBRCMaxAvgKbps = mfxU16(1.3 * pParams->nTargetBitRate);
			} else if (codec == QSV_CODEC_AV1) {
				m_co3.WinBRCMaxAvgKbps = mfxU16(1.2 * pParams->nTargetBitRate);
			}
		}

		if (pParams->video_fmt_ayuv || pParams->video_fmt_y410) {
			m_co3.TargetChromaFormatPlus1 = MFX_CHROMAFORMAT_YUV444 + 1;
		}

		if (codec == QSV_CODEC_VP9) {
			// Hint the encoder that this is a live-streaming session. This is an
			// encoder hint only (it does not by itself change the algorithm); it
			// must be validated via Query and quality measurements.
			m_co3.ScenarioInfo = MFX_SCENARIO_LIVE_STREAMING;
		}

		extendedBuffers.push_back((mfxExtBuffer *)&m_co3);
	}

	if (codec == QSV_CODEC_HEVC) {
		if ((pParams->nWidth & 15) || (pParams->nHeight & 15)) {
			memset(&m_ExtHEVCParam, 0, sizeof(m_ExtHEVCParam));
			m_ExtHEVCParam.Header.BufferId = MFX_EXTBUFF_HEVC_PARAM;
			m_ExtHEVCParam.Header.BufferSz = sizeof(m_ExtHEVCParam);
			m_ExtHEVCParam.PicWidthInLumaSamples = pParams->nWidth;
			m_ExtHEVCParam.PicHeightInLumaSamples = pParams->nHeight;
			extendedBuffers.push_back((mfxExtBuffer *)&m_ExtHEVCParam);
		}
	}

	constexpr uint32_t pixelcount_4k = 3840 * 2160;
	/* If size is 4K+, set tile columns per frame to 2. */
	if (codec == QSV_CODEC_AV1 && (pParams->nWidth * pParams->nHeight) >= pixelcount_4k) {
		memset(&m_ExtAv1TileParam, 0, sizeof(m_ExtAv1TileParam));
		m_ExtAv1TileParam.Header.BufferId = MFX_EXTBUFF_AV1_TILE_PARAM;
		m_ExtAv1TileParam.Header.BufferSz = sizeof(m_ExtAv1TileParam);
		m_ExtAv1TileParam.NumTileColumns = 2;
		extendedBuffers.push_back((mfxExtBuffer *)&m_ExtAv1TileParam);
	}

	// AV1_SCREEN_CONTENT_TOOLS API is introduced in VPL version 2.11
#if (MFX_VERSION_MAJOR >= 2 && MFX_VERSION_MINOR >= 11) || MFX_VERSION_MAJOR > 2
	if (codec == QSV_CODEC_AV1 && HasAV1ScreenContentSupport(platform, m_ver)) {
		memset(&m_ExtAV1ScreenContentTools, 0, sizeof(m_ExtAV1ScreenContentTools));
		m_ExtAV1ScreenContentTools.Header.BufferId = MFX_EXTBUFF_AV1_SCREEN_CONTENT_TOOLS;
		m_ExtAV1ScreenContentTools.Header.BufferSz = sizeof(m_ExtAV1ScreenContentTools);
		m_ExtAV1ScreenContentTools.Palette = MFX_CODINGOPTION_ON;
		extendedBuffers.push_back((mfxExtBuffer *)&m_ExtAV1ScreenContentTools);
	}
#endif

	if (codec == QSV_CODEC_VP9) {
		// VP9-specific extension buffer. Do not copy AVC/HEVC/AV1 buffers into
		// the VP9 path. WriteIVFHeaders is OFF: OBS streaming passes the raw
		// VP9 bitstream through and does not emit IVF container headers. Tiles
		// are disabled for the initial bitrate-constrained streaming baseline.
		memset(&m_ExtVP9Param, 0, sizeof(m_ExtVP9Param));
		m_ExtVP9Param.Header.BufferId = MFX_EXTBUFF_VP9_PARAM;
		m_ExtVP9Param.Header.BufferSz = sizeof(m_ExtVP9Param);
		m_ExtVP9Param.WriteIVFHeaders = MFX_CODINGOPTION_OFF;
		m_ExtVP9Param.NumTileRows = 0;
		m_ExtVP9Param.NumTileColumns = 0;
		extendedBuffers.push_back((mfxExtBuffer *)&m_ExtVP9Param);
	}

#if defined(_WIN32)
	// The Arc/Battlemage low-power VP9 path rejects any config that carries
	// MFX_EXTBUFF_VIDEO_SIGNAL_INFO (MFXVideoENCODE_Query returns
	// MFX_ERR_UNSUPPORTED). Verified empirically: the identical minimal VP9
	// config Queries OK with {CodingOption2, CodingOption3, VP9Param} but fails
	// as soon as VideoSignalInfo is added. Do not attach it for VP9 (per spec:
	// only attach buffers demonstrably supported by the VP9 hardware path).
	if (codec != QSV_CODEC_VP9) {
		memset(&m_ExtVideoSignalInfo, 0, sizeof(m_ExtVideoSignalInfo));
		m_ExtVideoSignalInfo.Header.BufferId = MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
		m_ExtVideoSignalInfo.Header.BufferSz = sizeof(m_ExtVideoSignalInfo);
		m_ExtVideoSignalInfo.VideoFormat = pParams->VideoFormat;
		m_ExtVideoSignalInfo.VideoFullRange = pParams->VideoFullRange;
		m_ExtVideoSignalInfo.ColourDescriptionPresent = 1;
		m_ExtVideoSignalInfo.ColourPrimaries = pParams->ColourPrimaries;
		m_ExtVideoSignalInfo.TransferCharacteristics = pParams->TransferCharacteristics;
		m_ExtVideoSignalInfo.MatrixCoefficients = pParams->MatrixCoefficients;
		extendedBuffers.push_back((mfxExtBuffer *)&m_ExtVideoSignalInfo);
	}
#endif

	// CLL and Chroma location in HEVC only supported by VPL
	if (m_ver.Major >= 2) {
		// Chroma location is HEVC only
		if (codec == QSV_CODEC_HEVC) {
			memset(&m_ExtChromaLocInfo, 0, sizeof(m_ExtChromaLocInfo));
			m_ExtChromaLocInfo.Header.BufferId = MFX_EXTBUFF_CHROMA_LOC_INFO;
			m_ExtChromaLocInfo.Header.BufferSz = sizeof(m_ExtChromaLocInfo);
			m_ExtChromaLocInfo.ChromaLocInfoPresentFlag = 1;
			m_ExtChromaLocInfo.ChromaSampleLocTypeTopField = pParams->ChromaSampleLocTypeTopField;
			m_ExtChromaLocInfo.ChromaSampleLocTypeBottomField = pParams->ChromaSampleLocTypeBottomField;
			extendedBuffers.push_back((mfxExtBuffer *)&m_ExtChromaLocInfo);
		}
	}

	// AV1 HDR meta data is now supported by VPL.
	if (pParams->MaxContentLightLevel > 0) {
		memset(&m_ExtMasteringDisplayColourVolume, 0, sizeof(m_ExtMasteringDisplayColourVolume));
		m_ExtMasteringDisplayColourVolume.Header.BufferId = MFX_EXTBUFF_MASTERING_DISPLAY_COLOUR_VOLUME;
		m_ExtMasteringDisplayColourVolume.Header.BufferSz = sizeof(m_ExtMasteringDisplayColourVolume);
		m_ExtMasteringDisplayColourVolume.InsertPayloadToggle = MFX_PAYLOAD_IDR;
		m_ExtMasteringDisplayColourVolume.DisplayPrimariesX[0] = pParams->DisplayPrimariesX[0];
		m_ExtMasteringDisplayColourVolume.DisplayPrimariesX[1] = pParams->DisplayPrimariesX[1];
		m_ExtMasteringDisplayColourVolume.DisplayPrimariesX[2] = pParams->DisplayPrimariesX[2];
		m_ExtMasteringDisplayColourVolume.DisplayPrimariesY[0] = pParams->DisplayPrimariesY[0];
		m_ExtMasteringDisplayColourVolume.DisplayPrimariesY[1] = pParams->DisplayPrimariesY[1];
		m_ExtMasteringDisplayColourVolume.DisplayPrimariesY[2] = pParams->DisplayPrimariesY[2];
		m_ExtMasteringDisplayColourVolume.WhitePointX = pParams->WhitePointX;
		m_ExtMasteringDisplayColourVolume.WhitePointY = pParams->WhitePointY;
		m_ExtMasteringDisplayColourVolume.MaxDisplayMasteringLuminance = pParams->MaxDisplayMasteringLuminance;
		m_ExtMasteringDisplayColourVolume.MinDisplayMasteringLuminance = pParams->MinDisplayMasteringLuminance;
		extendedBuffers.push_back((mfxExtBuffer *)&m_ExtMasteringDisplayColourVolume);

		memset(&m_ExtContentLightLevelInfo, 0, sizeof(m_ExtContentLightLevelInfo));
		m_ExtContentLightLevelInfo.Header.BufferId = MFX_EXTBUFF_CONTENT_LIGHT_LEVEL_INFO;
		m_ExtContentLightLevelInfo.Header.BufferSz = sizeof(m_ExtContentLightLevelInfo);
		m_ExtContentLightLevelInfo.InsertPayloadToggle = MFX_PAYLOAD_IDR;
		m_ExtContentLightLevelInfo.MaxContentLightLevel = pParams->MaxContentLightLevel;
		m_ExtContentLightLevelInfo.MaxPicAverageLightLevel = pParams->MaxPicAverageLightLevel;
		extendedBuffers.push_back((mfxExtBuffer *)&m_ExtContentLightLevelInfo);
	}

	// Width must be a multiple of 16
	// Height must be a multiple of 16 in case of frame picture and a
	// multiple of 32 in case of field picture
	m_mfxEncParams.mfx.FrameInfo.Width = MSDK_ALIGN16(pParams->nWidth);
	m_mfxEncParams.mfx.FrameInfo.Height = MSDK_ALIGN16(pParams->nHeight);

	if (m_bUseTexAlloc)
		m_mfxEncParams.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
	else
		m_mfxEncParams.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

	m_mfxEncParams.ExtParam = extendedBuffers.data();
	m_mfxEncParams.NumExtParam = (mfxU16)extendedBuffers.size();

	// Log the full initialization configuration before Query so that any
	// driver-side modification of parameters is easy to diagnose (the Intel VPL
	// encoder may modify init params, and GetVideoParam() should be used to read
	// back the actual working values).
	blog(LOG_DEBUG, "MFX init params:\n"
	     "\tCodecId:          %c%c%c%c\n"
	     "\tProfile:          %u\n"
	     "\tLevel:            %u\n"
	     "\tLowPower:         %u\n"
	     "\tTargetUsage:      %u\n"
	     "\tRateControl:      %u\n"
	     "\tTargetKbps:       %u\n"
	     "\tMaxKbps:          %u\n"
	     "\tBufferSizeInKB:   %u\n"
	     "\tInitialDelayInKB: %u\n"
	     "\tBRCParamMult:     %u\n"
	     "\tGopPicSize:       %u\n"
	     "\tGopRefDist:       %u\n"
	     "\tNumRefFrame:      %u\n"
	     "\tNumSlice:         %u\n"
	     "\tFourCC:           %c%c%c%c\n"
	     "\tChromaFormat:     %u\n"
	     "\tBitDepthLuma:     %u\n"
	     "\tBitDepthChroma:   %u\n"
	     "\tWidth:            %u\n"
	     "\tHeight:           %u\n"
	     "\tFrameRate:        %u/%u\n"
	     "\tAsyncDepth:       %u\n"
	     "\tLookAheadDepth:   %u\n"
	     "\tNumExtParam:      %u",
	     (mfxU8)((m_mfxEncParams.mfx.CodecId >> 24) & 0xFF),
	     (mfxU8)((m_mfxEncParams.mfx.CodecId >> 16) & 0xFF),
	     (mfxU8)((m_mfxEncParams.mfx.CodecId >> 8) & 0xFF),
	     (mfxU8)((m_mfxEncParams.mfx.CodecId >> 0) & 0xFF),
	     (unsigned)m_mfxEncParams.mfx.CodecProfile,
	     (unsigned)m_mfxEncParams.mfx.CodecLevel,
	     (unsigned)m_mfxEncParams.mfx.LowPower,
	     (unsigned)m_mfxEncParams.mfx.TargetUsage,
	     (unsigned)m_mfxEncParams.mfx.RateControlMethod,
	     (unsigned)m_mfxEncParams.mfx.TargetKbps,
	     (unsigned)m_mfxEncParams.mfx.MaxKbps,
	     (unsigned)m_mfxEncParams.mfx.BufferSizeInKB,
	     (unsigned)m_mfxEncParams.mfx.InitialDelayInKB,
	     (unsigned)m_mfxEncParams.mfx.BRCParamMultiplier,
	     (unsigned)m_mfxEncParams.mfx.GopPicSize,
	     (unsigned)m_mfxEncParams.mfx.GopRefDist,
	     (unsigned)m_mfxEncParams.mfx.NumRefFrame,
	     (unsigned)m_mfxEncParams.mfx.NumSlice,
	     (mfxU8)((m_mfxEncParams.mfx.FrameInfo.FourCC >> 24) & 0xFF),
	     (mfxU8)((m_mfxEncParams.mfx.FrameInfo.FourCC >> 16) & 0xFF),
	     (mfxU8)((m_mfxEncParams.mfx.FrameInfo.FourCC >> 8) & 0xFF),
	     (mfxU8)((m_mfxEncParams.mfx.FrameInfo.FourCC >> 0) & 0xFF),
	     (unsigned)m_mfxEncParams.mfx.FrameInfo.ChromaFormat,
	     (unsigned)m_mfxEncParams.mfx.FrameInfo.BitDepthLuma,
	     (unsigned)m_mfxEncParams.mfx.FrameInfo.BitDepthChroma,
	     (unsigned)m_mfxEncParams.mfx.FrameInfo.Width,
	     (unsigned)m_mfxEncParams.mfx.FrameInfo.Height,
	     (unsigned)m_mfxEncParams.mfx.FrameInfo.FrameRateExtN,
	     (unsigned)m_mfxEncParams.mfx.FrameInfo.FrameRateExtD,
	     (unsigned)m_mfxEncParams.AsyncDepth,
	     (unsigned)m_co2.LookAheadDepth,
	     (unsigned)m_mfxEncParams.NumExtParam);

	// Dump every attached extension buffer (spec §4): index, BufferId (hex +
	// symbolic name), and size. This confirms the exact buffer combination sent
	// to Query/Init -- e.g. that VP9 now carries only {CodingOption2,
	// CodingOption3, VP9Param} and NOT VideoSignalInfo (which the Arc low-power
	// VP9 path rejects with MFX_ERR_UNSUPPORTED).
	blog(LOG_DEBUG, "MFX extension buffers (count=%u):", (unsigned)m_mfxEncParams.NumExtParam);
	for (mfxU16 i = 0; i < m_mfxEncParams.NumExtParam && m_mfxEncParams.ExtParam[i]; i++) {
		mfxU32 id = m_mfxEncParams.ExtParam[i]->BufferId;
		const char *name = "<unknown>";
		switch (id) {
		case MFX_EXTBUFF_CODING_OPTION:
			name = "MFX_EXTBUFF_CODING_OPTION";
			break;
		case MFX_EXTBUFF_CODING_OPTION2:
			name = "MFX_EXTBUFF_CODING_OPTION2";
			break;
		case MFX_EXTBUFF_CODING_OPTION3:
			name = "MFX_EXTBUFF_CODING_OPTION3";
			break;
		case MFX_EXTBUFF_VP9_PARAM:
			name = "MFX_EXTBUFF_VP9_PARAM";
			break;
		case MFX_EXTBUFF_VIDEO_SIGNAL_INFO:
			name = "MFX_EXTBUFF_VIDEO_SIGNAL_INFO";
			break;
		default:
			break;
		}
		blog(LOG_DEBUG, "\tExt[%u]: BufferId=0x%08X (%s), BufferSz=%u", (unsigned)i, (unsigned)id, name,
		     (unsigned)m_mfxEncParams.ExtParam[i]->BufferSz);
	}

	if (codec == QSV_CODEC_VP9) {
		blog(LOG_DEBUG, "MFX init params (VP9):\n"
		     "\tCodecProfile:     %u (1=MFX_PROFILE_VP9_0/8bit, 3=MFX_PROFILE_VP9_2/10bit)\n"
		     "\tWriteIVFHeaders:  %u\n"
		     "\tNumTileRows:      %u\n"
		     "\tNumTileColumns:   %u\n"
		     "\tScenarioInfo:     %u (MFX_SCENARIO_LIVE_STREAMING=%d)",
		     (unsigned)m_mfxEncParams.mfx.CodecProfile, (unsigned)m_ExtVP9Param.WriteIVFHeaders,
		     (unsigned)m_ExtVP9Param.NumTileRows, (unsigned)m_ExtVP9Param.NumTileColumns,
		     (unsigned)m_co3.ScenarioInfo, MFX_SCENARIO_LIVE_STREAMING);

		// mfxExtCodingOption2 IS attached for VP9; log its fields (spec §11).
		blog(LOG_DEBUG, "MFX CodingOption2 (VP9):\n"
		     "\tLookAheadDepth: %u\n"
		     "\tRepeatPPS:      %u\n"
		     "\tAdaptiveB:      %u\n"
		     "\tBRefType:       %u\n"
		     "\tExtBRC:         %u\n"
		     "\tMBBRC:          %u\n"
		     "\tIntRefType:     %u\n"
		     "\tMaxFrameSize:   %u",
		     (unsigned)m_co2.LookAheadDepth, (unsigned)m_co2.RepeatPPS, (unsigned)m_co2.AdaptiveB,
		     (unsigned)m_co2.BRefType, (unsigned)m_co2.ExtBRC, (unsigned)m_co2.MBBRC,
		     (unsigned)m_co2.IntRefType, (unsigned)m_co2.MaxFrameSize);
	}

	// We don't check what was valid or invalid here, just try changing LowPower.
	// Ensure set values are not overwritten so in case it wasn't lowPower we fail
	// during the parameter check.
	mfxVideoParam validParams = {0};
	memcpy(&validParams, &m_mfxEncParams, sizeof(validParams));
	mfxStatus sts = m_pmfxENC->Query(&m_mfxEncParams, &validParams);
	if (sts == MFX_ERR_UNSUPPORTED || sts == MFX_ERR_UNDEFINED_BEHAVIOR) {
		if (m_mfxEncParams.mfx.LowPower == MFX_CODINGOPTION_ON) {
			if (codec != QSV_CODEC_VP9) {
				m_mfxEncParams.mfx.LowPower = MFX_CODINGOPTION_OFF;
				m_co2.LookAheadDepth = 0;
			} else {
				// VP9 is tied to the Low Power path; LowPower=OFF is not
				// supported. Do NOT fall back - keep LowPower=ON so Open()'s
				// Query surfaces the unsupported configuration as a hard error
				// instead of silently producing a broken stream.
				warn("VP9: Query rejected LowPower=ON config (sts=%d); this "
				     "GPU/runtime does not support the low-power VP9 path.",
				     (int)sts);
			}
		}
	}

	memset(&m_ctrl, 0, sizeof(m_ctrl));
	m_force_idr_next = false;
	memset(&m_roi, 0, sizeof(m_roi));

	return sts;
}

bool QSV_Encoder_Internal::UpdateParams(qsv_param_t *pParams)
{
	/* Update rate control method */
	m_mfxEncParams.mfx.RateControlMethod = pParams->nRateControl;

	switch (pParams->nRateControl) {
	case MFX_RATECONTROL_CBR:
		m_mfxEncParams.mfx.TargetKbps = pParams->nTargetBitRate;
		m_mfxEncParams.mfx.BufferSizeInKB = (pParams->nTargetBitRate / 8) * 2;
		m_mfxEncParams.mfx.InitialDelayInKB = m_mfxEncParams.mfx.BufferSizeInKB / 2;
		break;
	case MFX_RATECONTROL_VBR:
		m_mfxEncParams.mfx.TargetKbps = pParams->nTargetBitRate;
		m_mfxEncParams.mfx.MaxKbps = pParams->nMaxBitRate;
		m_mfxEncParams.mfx.BufferSizeInKB = (pParams->nTargetBitRate / 8) * 2;
		m_mfxEncParams.mfx.InitialDelayInKB = (pParams->nTargetBitRate / 8) * 1;
		break;
	case MFX_RATECONTROL_CQP:
		m_mfxEncParams.mfx.QPI = pParams->nQPI;
		m_mfxEncParams.mfx.QPB = pParams->nQPB;
		m_mfxEncParams.mfx.QPP = pParams->nQPP;
		break;
	case MFX_RATECONTROL_ICQ:
		m_mfxEncParams.mfx.ICQQuality = pParams->nICQQuality;
		break;
	default:
		break;
	}

	/* Update the effective frame rate. nFpsNum/nFpsDen already carry the
	 * frame-rate divisor folded into the denominator (see update_params in
	 * obs-qsv11.c), so this pushes the true encoding rate to the BRC and keeps
	 * the per-frame bit budget / bitrate calculation correct after a live FPS
	 * change. It takes effect from the forced IDR of the following Reset(). */
	if (pParams->nFpsNum != 0 && pParams->nFpsDen != 0) {
		m_mfxEncParams.mfx.FrameInfo.FrameRateExtN = pParams->nFpsNum;
		m_mfxEncParams.mfx.FrameInfo.FrameRateExtD = pParams->nFpsDen;

		/* Refresh the fps-derived BRC window size (frames/second) so it keeps
		 * representing ~1 second of video at the new effective rate. */
		m_co3.WinBRCSize = pParams->nFpsNum / pParams->nFpsDen;
	}

	return true;
}

	mfxStatus QSV_Encoder_Internal::ReconfigureEncoder()
{
	return m_pmfxENC->Reset(&m_mfxEncParams);
}

mfxStatus QSV_Encoder_Internal::Resize(qsv_param_t *pParams)
{
	mfxStatus sts;

	/* Drain any pending encode operations */
	sts = Drain();
	if (sts < MFX_ERR_NONE)
		return sts;

	/* Flush GPU to ensure all D3D11 work referencing old surfaces is complete */
#ifdef _WIN32
	ID3D11DeviceContext *ctx = GetHWDeviceContext();
	if (ctx)
		ctx->Flush();
#endif

	/* Close and destroy the encoder object */
	if (m_pmfxENC) {
		m_pmfxENC->Close();
		delete m_pmfxENC;
		m_pmfxENC = NULL;
	}

	/* Free old surfaces */
	if (m_bUseTexAlloc) {
		m_mfxAllocator.Free(m_mfxAllocator.pthis, &m_mfxResponse);
	}
	if (m_pmfxSurfaces) {
		for (int i = 0; i < m_nSurfNum; i++) {
			if (!m_bUseTexAlloc)
				delete[] m_pmfxSurfaces[i]->Data.Y;
			delete m_pmfxSurfaces[i];
		}
		delete[] m_pmfxSurfaces;
		m_pmfxSurfaces = NULL;
		m_nSurfNum = 0;
	}

	/* Close the MFX session to clear all internal driver state */
	MFXClose(m_session);
	m_session = NULL;

	/* Recreate the MFX session (reuses existing D3D11 device handle) */
	sts = Initialize(m_ver, &m_session, &m_mfxAllocator, &g_GFX_Handle, false,
	                  (enum qsv_codec)(m_mfxEncParams.mfx.CodecId == MFX_CODEC_HEVC ? QSV_CODEC_HEVC :
	                                     m_mfxEncParams.mfx.CodecId == MFX_CODEC_AV1  ? QSV_CODEC_AV1  :
	                                     m_mfxEncParams.mfx.CodecId == MFX_CODEC_VP9  ? QSV_CODEC_VP9  : QSV_CODEC_AVC),
	                  &m_sessionData);
	if (sts != MFX_ERR_NONE)
		return sts;

	/* Update dimensions in the MFX params */
	m_mfxEncParams.mfx.FrameInfo.Width = MSDK_ALIGN16(pParams->nWidth);
	m_mfxEncParams.mfx.FrameInfo.Height = MSDK_ALIGN16(pParams->nHeight);
	m_mfxEncParams.mfx.FrameInfo.CropW = pParams->nWidth;
	m_mfxEncParams.mfx.FrameInfo.CropH = pParams->nHeight;

	/* Create a fresh encoder object */
	m_pmfxENC = new MFXVideoENCODE(m_session);

	/* Query to validate params with new dimensions */
	sts = m_pmfxENC->Query(&m_mfxEncParams, &m_mfxEncParams);
	if (sts < MFX_ERR_NONE)
		return sts;

	/* Reallocate bitstream buffers if BufferSizeInKB changed after Query */
	m_parameter.mfx.BufferSizeInKB = m_mfxEncParams.mfx.BufferSizeInKB;
	uint32_t newBSLen = m_parameter.mfx.BufferSizeInKB * 1000;
	if (m_pTaskPool) {
		for (int i = 0; i < m_nTaskPool; i++) {
			delete[] m_pTaskPool[i].mfxBS.Data;
			m_pTaskPool[i].mfxBS.MaxLength = newBSLen;
			m_pTaskPool[i].mfxBS.Data = new mfxU8[newBSLen];
			m_pTaskPool[i].mfxBS.DataOffset = 0;
			m_pTaskPool[i].mfxBS.DataLength = 0;
		}
	}
	delete[] m_outBitstream.Data;
	m_outBitstream.MaxLength = newBSLen;
	m_outBitstream.Data = new mfxU8[newBSLen];
	m_outBitstream.DataOffset = 0;
	m_outBitstream.DataLength = 0;

	/* Re-query and allocate surfaces at the new size */
	sts = AllocateSurfaces();
	if (sts != MFX_ERR_NONE)
		return sts;

	/* Initialize the encoder with updated params */
	sts = m_pmfxENC->Init(&m_mfxEncParams);
	if (sts != MFX_ERR_NONE)
		return sts;

	/* Refresh SPS/PPS buffers (resolution change produces new headers) */
	enum qsv_codec codec;
	switch (m_mfxEncParams.mfx.CodecId) {
	case MFX_CODEC_AVC:  codec = QSV_CODEC_AVC; break;
	case MFX_CODEC_HEVC: codec = QSV_CODEC_HEVC; break;
	case MFX_CODEC_VP9:  codec = QSV_CODEC_VP9; break;
	default:             codec = QSV_CODEC_AV1; break;
	}
	sts = GetVideoParam(codec);
	if (sts != MFX_ERR_NONE)
		return sts;

	/* Reset control state */
	memset(&m_ctrl, 0, sizeof(m_ctrl));
	m_force_idr_next = false;
	m_roi.NumROI = 0;
	m_ctrl.ExtParam = nullptr;
	m_ctrl.NumExtParam = 0;
	m_extbuf.clear();

	return MFX_ERR_NONE;
}

mfxStatus QSV_Encoder_Internal::AllocateSurfaces()
{
	// Query number of required surfaces for encoder
	mfxFrameAllocRequest EncRequest;
	memset(&EncRequest, 0, sizeof(EncRequest));
	mfxStatus sts = m_pmfxENC->QueryIOSurf(&m_mfxEncParams, &EncRequest);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	EncRequest.Type |= WILL_WRITE;

	// SNB hack. On some SNB, it seems to require more surfaces
	EncRequest.NumFrameSuggested += m_mfxEncParams.AsyncDepth;

	// Allocate required surfaces
	if (m_bUseTexAlloc) {
		sts = m_mfxAllocator.Alloc(m_mfxAllocator.pthis, &EncRequest, &m_mfxResponse);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		m_nSurfNum = m_mfxResponse.NumFrameActual;

		m_pmfxSurfaces = new mfxFrameSurface1 *[m_nSurfNum];
		MSDK_CHECK_POINTER(m_pmfxSurfaces, MFX_ERR_MEMORY_ALLOC);

		for (int i = 0; i < m_nSurfNum; i++) {
			m_pmfxSurfaces[i] = new mfxFrameSurface1;
			memset(m_pmfxSurfaces[i], 0, sizeof(mfxFrameSurface1));
			memcpy(&(m_pmfxSurfaces[i]->Info), &(m_mfxEncParams.mfx.FrameInfo), sizeof(mfxFrameInfo));
			m_pmfxSurfaces[i]->Data.MemId = m_mfxResponse.mids[i];
		}
	} else {
		mfxU16 width = (mfxU16)MSDK_ALIGN32(EncRequest.Info.Width);
		mfxU16 height = (mfxU16)MSDK_ALIGN32(EncRequest.Info.Height);
		mfxU8 bitsPerPixel = 12;
		mfxU32 surfaceSize = width * height * bitsPerPixel / 8;
		m_nSurfNum = EncRequest.NumFrameSuggested;

		m_pmfxSurfaces = new mfxFrameSurface1 *[m_nSurfNum];
		for (int i = 0; i < m_nSurfNum; i++) {
			m_pmfxSurfaces[i] = new mfxFrameSurface1;
			memset(m_pmfxSurfaces[i], 0, sizeof(mfxFrameSurface1));
			memcpy(&(m_pmfxSurfaces[i]->Info), &(m_mfxEncParams.mfx.FrameInfo), sizeof(mfxFrameInfo));

			mfxU8 *pSurface = (mfxU8 *)new mfxU8[surfaceSize];
			m_pmfxSurfaces[i]->Data.Y = pSurface;
			m_pmfxSurfaces[i]->Data.U = pSurface + width * height;
			m_pmfxSurfaces[i]->Data.V = pSurface + width * height + 1;
			m_pmfxSurfaces[i]->Data.Pitch = width;
		}
	}

	blog(LOG_DEBUG, "\tm_nSurfNum:     %d", m_nSurfNum);

	return sts;
}

// Log the effective (post-Query/Init) working parameters read back via
// MFXVideoENCODE_GetVideoParam(). Intel VPL may modify init params, so these are
// the authoritative values for diagnosing BRC/GOP behavior.
static void LogEffectiveParams(const mfxVideoParam *p)
{
	blog(LOG_DEBUG, "MFX effective params (post-GetVideoParam):\n"
	     "\tRateControl:      %u\n"
	     "\tTargetKbps:       %u\n"
	     "\tMaxKbps:          %u\n"
	     "\tBufferSizeInKB:   %u\n"
	     "\tInitialDelayInKB: %u\n"
	     "\tGopPicSize:       %u\n"
	     "\tGopRefDist:       %u\n"
	     "\tNumSlice:         %u\n"
	     "\tLowPower:         %u\n"
	     "\tAsyncDepth:       %u",
	     (unsigned)p->mfx.RateControlMethod,
	     (unsigned)p->mfx.TargetKbps,
	     (unsigned)p->mfx.MaxKbps,
	     (unsigned)p->mfx.BufferSizeInKB,
	     (unsigned)p->mfx.InitialDelayInKB,
	     (unsigned)p->mfx.GopPicSize,
	     (unsigned)p->mfx.GopRefDist,
	     (unsigned)p->mfx.NumSlice,
	     (unsigned)p->mfx.LowPower,
	     (unsigned)p->AsyncDepth);
}

mfxStatus QSV_Encoder_Internal::GetVideoParam(enum qsv_codec codec)
{
	memset(&m_parameter, 0, sizeof(m_parameter));

	std::vector<mfxExtBuffer *> extendedBuffers;
	extendedBuffers.reserve(2);

	if (codec != QSV_CODEC_VP9) {
		// VP9 has no SPS/PPS/VPS parameter-set model, so do not attach the
		// SPSPPS extension buffer for it (the bitstream is passed through as-is).
		mfxExtCodingOptionSPSPPS opt;
		opt.Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS;
		opt.Header.BufferSz = sizeof(mfxExtCodingOptionSPSPPS);

		opt.SPSBuffer = m_SPSBuffer;
		opt.PPSBuffer = m_PPSBuffer;
		opt.SPSBufSize = 1024; //  m_nSPSBufferSize;
		opt.PPSBufSize = 1024; //  m_nPPSBufferSize;

		mfxExtCodingOptionVPS opt_vps{};
		if (codec == QSV_CODEC_HEVC) {
			opt_vps.Header.BufferId = MFX_EXTBUFF_CODING_OPTION_VPS;
			opt_vps.Header.BufferSz = sizeof(mfxExtCodingOptionVPS);
			opt_vps.VPSBuffer = m_VPSBuffer;
			opt_vps.VPSBufSize = 1024;

			extendedBuffers.push_back((mfxExtBuffer *)&opt_vps);
		}

		extendedBuffers.push_back((mfxExtBuffer *)&opt);

		m_parameter.ExtParam = extendedBuffers.data();
		m_parameter.NumExtParam = (mfxU16)extendedBuffers.size();

		mfxStatus sts = m_pmfxENC->GetVideoParam(&m_parameter);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		if (codec == QSV_CODEC_HEVC)
			m_nVPSBufferSize = opt_vps.VPSBufSize;
		m_nSPSBufferSize = opt.SPSBufSize;
		m_nPPSBufferSize = opt.PPSBufSize;

		LogEffectiveParams(&m_parameter);
		return sts;
	}

	// VP9: no parameter sets to retrieve. Still call GetVideoParam (with an
	// empty extension list) so m_parameter carries the effective AsyncDepth and
	// BufferSizeInKB used by InitBitstream().
	m_nSPSBufferSize = 0;
	m_nPPSBufferSize = 0;
	mfxStatus sts = m_pmfxENC->GetVideoParam(&m_parameter);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	LogEffectiveParams(&m_parameter);
	return sts;
}

void QSV_Encoder_Internal::GetSPSPPS(mfxU8 **pSPSBuf, mfxU8 **pPPSBuf, mfxU16 *pnSPSBuf, mfxU16 *pnPPSBuf)
{
	*pSPSBuf = m_SPSBuffer;
	*pPPSBuf = m_PPSBuffer;
	*pnSPSBuf = m_nSPSBufferSize;
	*pnPPSBuf = m_nPPSBufferSize;
}

void QSV_Encoder_Internal::GetVpsSpsPps(mfxU8 **pVPSBuf, mfxU8 **pSPSBuf, mfxU8 **pPPSBuf, mfxU16 *pnVPSBuf,
					mfxU16 *pnSPSBuf, mfxU16 *pnPPSBuf)
{
	*pVPSBuf = m_VPSBuffer;
	*pnVPSBuf = m_nVPSBufferSize;

	*pSPSBuf = m_SPSBuffer;
	*pnSPSBuf = m_nSPSBufferSize;

	*pPPSBuf = m_PPSBuffer;
	*pnPPSBuf = m_nPPSBufferSize;
}

mfxStatus QSV_Encoder_Internal::InitBitstream()
{
	m_nTaskPool = m_parameter.AsyncDepth;
	m_nFirstSyncTask = 0;

	m_pTaskPool = new Task[m_nTaskPool];
	memset(m_pTaskPool, 0, sizeof(Task) * m_nTaskPool);

	for (int i = 0; i < m_nTaskPool; i++) {
		m_pTaskPool[i].mfxBS.MaxLength = m_parameter.mfx.BufferSizeInKB * 1000;
		m_pTaskPool[i].mfxBS.Data = new mfxU8[m_pTaskPool[i].mfxBS.MaxLength];
		m_pTaskPool[i].mfxBS.DataOffset = 0;
		m_pTaskPool[i].mfxBS.DataLength = 0;

		MSDK_CHECK_POINTER(m_pTaskPool[i].mfxBS.Data, MFX_ERR_MEMORY_ALLOC);
	}

	memset(&m_outBitstream, 0, sizeof(mfxBitstream));
	m_outBitstream.MaxLength = m_parameter.mfx.BufferSizeInKB * 1000;
	m_outBitstream.Data = new mfxU8[m_outBitstream.MaxLength];
	m_outBitstream.DataOffset = 0;
	m_outBitstream.DataLength = 0;

	blog(LOG_DEBUG, "\tm_nTaskPool:    %d", m_nTaskPool);

	return MFX_ERR_NONE;
}

mfxStatus QSV_Encoder_Internal::LoadP010(mfxFrameSurface1 *pSurface, uint8_t *pDataY, uint8_t *pDataUV,
					 uint32_t strideY, uint32_t strideUV)
{
	mfxU16 w, h, i, pitch;
	mfxU8 *ptr;
	mfxFrameInfo *pInfo = &pSurface->Info;
	mfxFrameData *pData = &pSurface->Data;

	if (pInfo->CropH > 0 && pInfo->CropW > 0) {
		w = pInfo->CropW;
		h = pInfo->CropH;
	} else {
		w = pInfo->Width;
		h = pInfo->Height;
	}

	pitch = pData->Pitch;
	ptr = pData->Y + pInfo->CropX + pInfo->CropY * pData->Pitch;
	const size_t line_size = w * 2;

	// load Y plane
	for (i = 0; i < h; i++)
		memcpy(ptr + i * pitch, pDataY + i * strideY, line_size);

	// load UV plane
	h /= 2;
	ptr = pData->UV + pInfo->CropX + (pInfo->CropY / 2) * pitch;

	for (i = 0; i < h; i++)
		memcpy(ptr + i * pitch, pDataUV + i * strideUV, line_size);

	return MFX_ERR_NONE;
}

mfxStatus QSV_Encoder_Internal::LoadNV12(mfxFrameSurface1 *pSurface, uint8_t *pDataY, uint8_t *pDataUV,
					 uint32_t strideY, uint32_t strideUV)
{
	mfxU16 w, h, i, pitch;
	mfxU8 *ptr;
	mfxFrameInfo *pInfo = &pSurface->Info;
	mfxFrameData *pData = &pSurface->Data;

	if (pInfo->CropH > 0 && pInfo->CropW > 0) {
		w = pInfo->CropW;
		h = pInfo->CropH;
	} else {
		w = pInfo->Width;
		h = pInfo->Height;
	}

	pitch = pData->Pitch;
	ptr = pData->Y + pInfo->CropX + pInfo->CropY * pData->Pitch;

	// load Y plane
	for (i = 0; i < h; i++)
		memcpy(ptr + i * pitch, pDataY + i * strideY, w);

	// load UV plane
	h /= 2;
	ptr = pData->UV + pInfo->CropX + (pInfo->CropY / 2) * pitch;

	for (i = 0; i < h; i++)
		memcpy(ptr + i * pitch, pDataUV + i * strideUV, w);

	return MFX_ERR_NONE;
}

int QSV_Encoder_Internal::GetFreeTaskIndex(Task *pTaskPool, mfxU16 nPoolSize)
{
	if (pTaskPool)
		for (int i = 0; i < nPoolSize; i++)
			if (!pTaskPool[i].syncp)
				return i;
	return MFX_ERR_NOT_FOUND;
}

mfxStatus QSV_Encoder_Internal::Encode(uint64_t ts, uint8_t *pDataY, uint8_t *pDataUV, uint32_t strideY,
				       uint32_t strideUV, mfxBitstream **pBS)
{
	mfxStatus sts = MFX_ERR_NONE;
	*pBS = NULL;
	int nTaskIdx = GetFreeTaskIndex(m_pTaskPool, m_nTaskPool);

#if 0
	info("MSDK Encode:\n"
		"\tTaskIndex: %d",
		nTaskIdx);
#endif

	int nSurfIdx = GetFreeSurfaceIndex(m_pmfxSurfaces, m_nSurfNum);
#if 0
	info("MSDK Encode:\n"
		"\tnSurfIdx: %d",
		nSurfIdx);
#endif

	while (MFX_ERR_NOT_FOUND == nTaskIdx || MFX_ERR_NOT_FOUND == nSurfIdx) {
		// No more free tasks or surfaces, need to sync
		sts = MFXVideoCORE_SyncOperation(m_session, m_pTaskPool[m_nFirstSyncTask].syncp, 60000);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		mfxU8 *pTemp = m_outBitstream.Data;
		memcpy(&m_outBitstream, &m_pTaskPool[m_nFirstSyncTask].mfxBS, sizeof(mfxBitstream));

		m_pTaskPool[m_nFirstSyncTask].mfxBS.Data = pTemp;
		m_pTaskPool[m_nFirstSyncTask].mfxBS.DataLength = 0;
		m_pTaskPool[m_nFirstSyncTask].mfxBS.DataOffset = 0;
		m_pTaskPool[m_nFirstSyncTask].syncp = NULL;
		nTaskIdx = m_nFirstSyncTask;
		m_nFirstSyncTask = (m_nFirstSyncTask + 1) % m_nTaskPool;
		*pBS = &m_outBitstream;

#if 0
		info("MSDK Encode:\n"
			"\tnew FirstSyncTask: %d\n"
			"\tTaskIndex:         %d",
			m_nFirstSyncTask,
			nTaskIdx);
#endif

		nSurfIdx = GetFreeSurfaceIndex(m_pmfxSurfaces, m_nSurfNum);
#if 0
		info("MSDK Encode:\n"
			"\tnSurfIdx: %d",
			nSurfIdx);
#endif
	}

	mfxFrameSurface1 *pSurface = m_pmfxSurfaces[nSurfIdx];
	if (m_bUseTexAlloc) {
		sts = m_mfxAllocator.Lock(m_mfxAllocator.pthis, pSurface->Data.MemId, &(pSurface->Data));
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	}

	sts = (pSurface->Info.FourCC == MFX_FOURCC_P010) ? LoadP010(pSurface, pDataY, pDataUV, strideY, strideUV)
							 : LoadNV12(pSurface, pDataY, pDataUV, strideY, strideUV);

	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	pSurface->Data.TimeStamp = ts;

	if (m_bUseTexAlloc) {
		sts = m_mfxAllocator.Unlock(m_mfxAllocator.pthis, pSurface->Data.MemId, &(pSurface->Data));
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	}

	if (m_force_idr_next) {
		m_ctrl.FrameType = MFX_FRAMETYPE_I;
		m_force_idr_next = false;
	}

	for (;;) {
		// Encode a frame asynchronously (returns immediately)
		sts = m_pmfxENC->EncodeFrameAsync(&m_ctrl, pSurface, &m_pTaskPool[nTaskIdx].mfxBS,
						  &m_pTaskPool[nTaskIdx].syncp);

		if (MFX_ERR_NONE < sts && !m_pTaskPool[nTaskIdx].syncp) {
			// Repeat the call if warning and no output
			if (MFX_WRN_DEVICE_BUSY == sts)
				MSDK_SLEEP(1); // Wait if device is busy, then repeat the same call
		} else if (MFX_ERR_NONE < sts && m_pTaskPool[nTaskIdx].syncp) {
			sts = MFX_ERR_NONE; // Ignore warnings if output is available
			break;
		} else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
			// Allocate more bitstream buffer memory here if needed...
			break;
		} else
			break;
	}

	m_ctrl.FrameType = MFX_FRAMETYPE_UNKNOWN; // reset for next frame
	return sts;
}

mfxStatus QSV_Encoder_Internal::Encode_tex(uint64_t ts, void *tex, uint64_t lock_key, uint64_t *next_key,
					   mfxBitstream **pBS)
{
	mfxStatus sts = MFX_ERR_NONE;
	*pBS = NULL;
	int nTaskIdx = GetFreeTaskIndex(m_pTaskPool, m_nTaskPool);
	int nSurfIdx = GetFreeSurfaceIndex(m_pmfxSurfaces, m_nSurfNum);

	while (MFX_ERR_NOT_FOUND == nTaskIdx || MFX_ERR_NOT_FOUND == nSurfIdx) {
		// No more free tasks or surfaces, need to sync
		sts = MFXVideoCORE_SyncOperation(m_session, m_pTaskPool[m_nFirstSyncTask].syncp, 60000);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		mfxU8 *pTemp = m_outBitstream.Data;
		memcpy(&m_outBitstream, &m_pTaskPool[m_nFirstSyncTask].mfxBS, sizeof(mfxBitstream));

		m_pTaskPool[m_nFirstSyncTask].mfxBS.Data = pTemp;
		m_pTaskPool[m_nFirstSyncTask].mfxBS.DataLength = 0;
		m_pTaskPool[m_nFirstSyncTask].mfxBS.DataOffset = 0;
		m_pTaskPool[m_nFirstSyncTask].syncp = NULL;
		nTaskIdx = m_nFirstSyncTask;
		m_nFirstSyncTask = (m_nFirstSyncTask + 1) % m_nTaskPool;
		*pBS = &m_outBitstream;

		nSurfIdx = GetFreeSurfaceIndex(m_pmfxSurfaces, m_nSurfNum);
	}

	mfxFrameSurface1 *pSurface = m_pmfxSurfaces[nSurfIdx];
	//copy to default surface directly
	pSurface->Data.TimeStamp = ts;
	if (m_bUseTexAlloc) {
		// mfxU64 isn't consistent with stdint, requiring a cast to be multi-platform.
		sts = simple_copytex(m_mfxAllocator.pthis, pSurface->Data.MemId, tex, lock_key, (mfxU64 *)next_key);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	}

	if (m_force_idr_next) {
		m_ctrl.FrameType = MFX_FRAMETYPE_I;
		m_force_idr_next = false;
	}

	for (;;) {
		// Encode a frame asynchronously (returns immediately)
		sts = m_pmfxENC->EncodeFrameAsync(&m_ctrl, pSurface, &m_pTaskPool[nTaskIdx].mfxBS,
						  &m_pTaskPool[nTaskIdx].syncp);

		if (MFX_ERR_NONE < sts && !m_pTaskPool[nTaskIdx].syncp) {
			// Repeat the call if warning and no output
			if (MFX_WRN_DEVICE_BUSY == sts)
				MSDK_SLEEP(1); // Wait if device is busy, then repeat the same call
		} else if (MFX_ERR_NONE < sts && m_pTaskPool[nTaskIdx].syncp) {
			sts = MFX_ERR_NONE; // Ignore warnings if output is available
			break;
		} else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
			// Allocate more bitstream buffer memory here if needed...
			blog(LOG_WARNING, "Encode_tex: MFX_ERR_NOT_ENOUGH_BUFFER");
			break;
		} else if (sts < MFX_ERR_NONE) {
			blog(LOG_DEBUG, "Encode_tex: EncodeFrameAsync failed: sts=%d", (int)sts);
			break;
		} else
			break;
	}

	m_ctrl.FrameType = MFX_FRAMETYPE_UNKNOWN; // reset for next frame
	return sts;
}

mfxStatus QSV_Encoder_Internal::Drain()
{
	mfxStatus sts = MFX_ERR_NONE;

	while (m_pTaskPool && m_pTaskPool[m_nFirstSyncTask].syncp) {
		sts = MFXVideoCORE_SyncOperation(m_session, m_pTaskPool[m_nFirstSyncTask].syncp, 60000);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		m_pTaskPool[m_nFirstSyncTask].syncp = NULL;
		m_nFirstSyncTask = (m_nFirstSyncTask + 1) % m_nTaskPool;
	}

	return sts;
}

mfxStatus QSV_Encoder_Internal::ClearData()
{
	mfxStatus sts = MFX_ERR_NONE;
	sts = Drain();

	if (m_pmfxENC) {
		sts = m_pmfxENC->Close();
		delete m_pmfxENC;
		m_pmfxENC = NULL;
	}

	if (m_bUseTexAlloc)
		m_mfxAllocator.Free(m_mfxAllocator.pthis, &m_mfxResponse);

	if (m_pmfxSurfaces) {
		for (int i = 0; i < m_nSurfNum; i++) {
			if (!m_bUseTexAlloc)
				delete m_pmfxSurfaces[i]->Data.Y;

			delete m_pmfxSurfaces[i];
		}
		MSDK_SAFE_DELETE_ARRAY(m_pmfxSurfaces);
	}

	if (m_pTaskPool) {
		for (int i = 0; i < m_nTaskPool; i++)
			delete m_pTaskPool[i].mfxBS.Data;
		MSDK_SAFE_DELETE_ARRAY(m_pTaskPool);
	}

	if (m_outBitstream.Data) {
		delete[] m_outBitstream.Data;
		m_outBitstream.Data = NULL;
	}

	if (sts >= MFX_ERR_NONE) {
		g_numEncodersOpen--;
	}

	if ((m_bUseTexAlloc) && (g_numEncodersOpen <= 0)) {
		Release();
		g_GFX_Handle = NULL;
	}
	MFXVideoENCODE_Close(m_session);
	ReleaseSessionData(m_sessionData);
	m_sessionData = NULL;
	return sts;
}

mfxStatus QSV_Encoder_Internal::Reset(qsv_param_t *pParams, enum qsv_codec codec)
{
	mfxStatus sts = ClearData();
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = Open(pParams, codec);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	return sts;
}

void QSV_Encoder_Internal::AddROI(mfxU32 left, mfxU32 top, mfxU32 right, mfxU32 bottom, mfxI16 delta)
{
	if (m_roi.NumROI == 256) {
		warn("Maximum number of ROIs hit, ignoring additional ROI!");
		return;
	}

	m_roi.Header.BufferId = MFX_EXTBUFF_ENCODER_ROI;
	m_roi.Header.BufferSz = sizeof(mfxExtEncoderROI);
	m_roi.ROIMode = MFX_ROI_MODE_QP_DELTA;
	/* The SDK will automatically align the values to block sizes so we
	 * don't have to do any maths here. */
	m_roi.ROI[m_roi.NumROI].Left = left;
	m_roi.ROI[m_roi.NumROI].Top = top;
	m_roi.ROI[m_roi.NumROI].Right = right;
	m_roi.ROI[m_roi.NumROI].Bottom = bottom;
	m_roi.ROI[m_roi.NumROI].DeltaQP = delta;
	m_roi.NumROI++;

	/* Right now ROI is the only thing we add so this is fine */
	if (m_extbuf.empty())
		m_extbuf.push_back((mfxExtBuffer *)&m_roi);

	m_ctrl.ExtParam = m_extbuf.data();
	m_ctrl.NumExtParam = (mfxU16)m_extbuf.size();
}

void QSV_Encoder_Internal::ClearROI()
{
	m_roi.NumROI = 0;
	m_ctrl.ExtParam = nullptr;
	m_ctrl.NumExtParam = 0;
	m_extbuf.clear();
}
