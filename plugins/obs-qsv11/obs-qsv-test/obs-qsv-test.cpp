#define ONEVPL_EXPERIMENTAL
#include <vpl/mfxstructures.h>
#include <vpl/mfxadapter.h>
#include <vpl/mfxvideo++.h>
#include "../common_utils.h"

#include <util/windows/ComPtr.hpp>

#include <dxgi.h>
#include <d3d11.h>
#include <d3d11_1.h>

#include <vector>
#include <string>
#include <map>

#ifdef _MSC_VER
extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = 1;
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

struct adapter_caps {
	bool is_intel = false;
	bool is_dgpu = false;
	bool supports_av1 = false;
	bool supports_hevc = false;
	bool supports_vp9 = false;
	struct vp9_caps vp9{}; /* runtime-probed VP9 profile/format capabilities */
};

static std::vector<uint64_t> luid_order;
static std::map<uint32_t, adapter_caps> adapter_info;

static bool has_encoder(mfxSession m_session, mfxU32 codec_id)
{
	MFXVideoENCODE *session = new MFXVideoENCODE(m_session);

	mfxVideoParam video_param;
	memset(&video_param, 0, sizeof(video_param));
	video_param.mfx.CodecId = codec_id;
	video_param.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
	video_param.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
	video_param.mfx.FrameInfo.Width = MSDK_ALIGN16(1280);
	video_param.mfx.FrameInfo.Height = MSDK_ALIGN16(720);
	mfxStatus sts = session->Query(&video_param, &video_param);
	session->Close();

	return sts == MFX_ERR_NONE;
}

// Minimal VP9 capability probe for a single profile/format combo (spec §9-§11).
// Deliberately conservative -- LowPower=ON, CBR, no lookahead/tiles/extensions --
// so an advanced streaming option can never produce a false negative that masks
// real support. Returns the raw MFXVideoENCODE_Query status for this combo.
static mfxStatus vp9_probe_one(mfxSession session, mfxU16 profile, mfxU32 fourcc, mfxU16 chroma, mfxU16 bitdepth)
{
	mfxVideoParam p;
	memset(&p, 0, sizeof(p));
	p.mfx.CodecId = MFX_CODEC_VP9;
	p.mfx.CodecProfile = profile;
	p.mfx.LowPower = MFX_CODINGOPTION_ON;
	p.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
	p.mfx.TargetKbps = 2500;
	p.mfx.MaxKbps = 2500;
	p.mfx.BufferSizeInKB = 626;
	p.mfx.InitialDelayInKB = 313;
	p.AsyncDepth = 4;
	p.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
	p.mfx.FrameInfo.FourCC = fourcc;
	p.mfx.FrameInfo.ChromaFormat = chroma;
	p.mfx.FrameInfo.BitDepthLuma = bitdepth;
	p.mfx.FrameInfo.BitDepthChroma = bitdepth;
	p.mfx.FrameInfo.Shift = (bitdepth > 8) ? 1 : 0;
	p.mfx.FrameInfo.Width = 1920;
	p.mfx.FrameInfo.Height = 1088; // aligned to 16
	p.mfx.FrameInfo.FrameRateExtN = 60;
	p.mfx.FrameInfo.FrameRateExtD = 1;
	p.mfx.GopPicSize = 120;
	p.mfx.GopRefDist = 1;
	p.mfx.NumSlice = 0;

	MFXVideoENCODE *enc = new MFXVideoENCODE(session);
	mfxStatus sts = enc->Query(&p, &p);
	enc->Close();
	delete enc;
	return sts;
}

// Probe all four VP9 profile/format combos against a session bound to the given
// MFX adapter index and fill *caps (spec §8, §13-§16). Each profile is probed
// independently so one failure never masks the others. encoder_supported is true
// if ANY profile succeeds.
static void probe_vp9_profiles(mfxLoader loader, uint32_t mfx_adapter_idx, struct vp9_caps *caps)
{
	memset(caps, 0, sizeof(*caps));

	mfxSession session = nullptr;
	if (MFXCreateSession(loader, mfx_adapter_idx, &session) != MFX_ERR_NONE || !session) {
		printf("  [vp9] MFXCreateSession failed for adapter %u\n", mfx_adapter_idx);
		return; // all profiles stay false -> encoder_supported stays false
	}

	mfxVersion ver;
	MFXQueryVersion(session, &ver);

	caps->profile0_nv12_8bit =
	    (vp9_probe_one(session, MFX_PROFILE_VP9_0, MFX_FOURCC_NV12, MFX_CHROMAFORMAT_YUV420, 8) == MFX_ERR_NONE);
	caps->profile1_ayuv_8bit =
	    (vp9_probe_one(session, MFX_PROFILE_VP9_1, MFX_FOURCC_AYUV, MFX_CHROMAFORMAT_YUV444, 8) == MFX_ERR_NONE);
	caps->profile2_p010_10bit =
	    (vp9_probe_one(session, MFX_PROFILE_VP9_2, MFX_FOURCC_P010, MFX_CHROMAFORMAT_YUV420, 10) == MFX_ERR_NONE);
	caps->profile3_y410_10bit =
	    (vp9_probe_one(session, MFX_PROFILE_VP9_3, MFX_FOURCC_Y410, MFX_CHROMAFORMAT_YUV444, 10) == MFX_ERR_NONE);

	caps->encoder_supported = caps->profile0_nv12_8bit || caps->profile1_ayuv_8bit ||
	                          caps->profile2_p010_10bit || caps->profile3_y410_10bit;

	printf("  [vp9] adapter %u runtime %u.%u: P0/NV12=%d P1/AYUV=%d P2/P010=%d P3/Y410=%d\n", mfx_adapter_idx,
	       ver.Major, ver.Minor, (int)caps->profile0_nv12_8bit, (int)caps->profile1_ayuv_8bit,
	       (int)caps->profile2_p010_10bit, (int)caps->profile3_y410_10bit);

	MFXClose(session);
}

static inline uint32_t get_adapter_idx(uint32_t adapter_idx, LUID luid)
{
	for (size_t i = 0; i < luid_order.size(); i++) {
		if (luid_order[i] == *(uint64_t *)&luid) {
			return (uint32_t)i;
		}
	}

	return adapter_idx;
}

static bool get_adapter_caps(IDXGIFactory *factory, mfxLoader loader, mfxSession m_session, uint32_t adapter_idx)
{
	HRESULT hr;
	static uint32_t idx_adjustment = 0;

	ComPtr<IDXGIAdapter> adapter;
	hr = factory->EnumAdapters(adapter_idx, &adapter);
	if (FAILED(hr))
		return false;

	DXGI_ADAPTER_DESC desc;
	adapter->GetDesc(&desc);

	uint32_t luid_idx = get_adapter_idx(adapter_idx, desc.AdapterLuid);
	adapter_caps &caps = adapter_info[luid_idx];
	if (desc.VendorId != INTEL_VENDOR_ID) {
		idx_adjustment++;
		return true;
	}

	caps.is_intel = true;
	const uint32_t mfx_idx = adapter_idx - idx_adjustment;

	// VP9: authoritative per-profile runtime probe (spec §8, §14-§16). This is
	// more precise than the codec-list check and drives UI/registration. Done
	// before MFXEnumImplementations so it still runs if that call fails.
	probe_vp9_profiles(loader, mfx_idx, &caps.vp9);
	caps.supports_vp9 = caps.vp9.encoder_supported;

	mfxImplDescription *idesc;
	mfxStatus sts = MFXEnumImplementations(loader, mfx_idx, MFX_IMPLCAPS_IMPLDESCSTRUCTURE,
					       reinterpret_cast<mfxHDL *>(&idesc));

	if (sts != MFX_ERR_NONE)
		return false;

	caps.is_dgpu = false;
	if (idesc->Dev.MediaAdapterType == MFX_MEDIA_DISCRETE)
		caps.is_dgpu = true;

	caps.supports_av1 = false;
	caps.supports_hevc = false;
	mfxEncoderDescription *enc = &idesc->Enc;
	if (enc->NumCodecs != 0) {
		for (int codec = 0; codec < enc->NumCodecs; codec++) {
			if (enc->Codecs[codec].CodecID == MFX_CODEC_AV1)
				caps.supports_av1 = true;
#if ENABLE_HEVC
			if (enc->Codecs[codec].CodecID == MFX_CODEC_HEVC)
				caps.supports_hevc = true;
#endif
		}
	} else {
		// Encoder information is not available before TGL for VPL, so the MSDK legacy approach is taken
		caps.supports_av1 = has_encoder(m_session, MFX_CODEC_AV1);
#if ENABLE_HEVC
		caps.supports_hevc = has_encoder(m_session, MFX_CODEC_HEVC);
#endif
	}

	MFXDispReleaseImplDescription(loader, idesc);

	return true;
}

#define CHECK_TIMEOUT_MS 10000

DWORD WINAPI TimeoutThread(LPVOID param)
{
	HANDLE hMainThread = (HANDLE)param;

	DWORD ret = WaitForSingleObject(hMainThread, CHECK_TIMEOUT_MS);
	if (ret == WAIT_TIMEOUT)
		TerminateProcess(GetCurrentProcess(), STATUS_TIMEOUT);

	CloseHandle(hMainThread);

	return 0;
}

// ---------------------------------------------------------------------------
// --vp9probe mode: run real minimal VP9 MFXVideoENCODE_Query() calls to isolate
// why the full-config Query fails in OBS. FFmpeg's working vp9_qsv config uses
// system-memory input, profile 0, CBR/VBR, LowPower(=VDENC) ON. We vary two
// things: IOPattern (system vs video memory) and the extension-buffer set.
// ---------------------------------------------------------------------------
static mfxVideoParam make_vp9_base(mfxU16 iopattern)
{
	mfxVideoParam p;
	memset(&p, 0, sizeof(p));
	p.mfx.CodecId = MFX_CODEC_VP9;
	p.mfx.CodecProfile = MFX_PROFILE_VP9_0;
	p.mfx.LowPower = MFX_CODINGOPTION_ON;
	p.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
	p.mfx.TargetKbps = 2500;
	p.mfx.MaxKbps = 2500;
	p.mfx.BufferSizeInKB = 626;
	p.mfx.InitialDelayInKB = 313;
	p.AsyncDepth = 4;
	p.IOPattern = iopattern;
	p.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
	p.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
	p.mfx.FrameInfo.BitDepthLuma = 8;
	p.mfx.FrameInfo.BitDepthChroma = 8;
	p.mfx.FrameInfo.Width = 1920;
	p.mfx.FrameInfo.Height = 1088; // aligned to 16
	p.mfx.FrameInfo.FrameRateExtN = 60;
	p.mfx.FrameInfo.FrameRateExtD = 1;
	p.mfx.GopPicSize = 120;
	p.mfx.GopRefDist = 1;
	p.mfx.NumSlice = 0;
	return p;
}

static void print_probe(const char *label, mfxStatus sts, const mfxVideoParam *p)
{
	const char *tag = "other";
	if (sts == MFX_ERR_NONE)
		tag = "OK";
	else if (sts == MFX_ERR_UNSUPPORTED)
		tag = "UNSUPPORTED";
	else if (sts == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM)
		tag = "INCOMPATIBLE(modified)";

	printf("  %-30s -> status=%d (%s)\n", label, (int)sts, tag);
	if (p) {
		printf("      returned: IOPattern=0x%04x LowPower=%u RateCtrl=%u Profile=%u "
		       "TargetKbps=%u MaxKbps=%u GopRefDist=%u NumSlice=%u\n",
		       (unsigned)p->IOPattern, (unsigned)p->mfx.LowPower, (unsigned)p->mfx.RateControlMethod,
		       (unsigned)p->mfx.CodecProfile, (unsigned)p->mfx.TargetKbps, (unsigned)p->mfx.MaxKbps,
		       (unsigned)p->mfx.GopRefDist, (unsigned)p->mfx.NumSlice);
	}
	fflush(stdout);
}

static void run_vp9_probe(mfxLoader loader)
{
	printf("=== VP9 QSV Query diagnostic ===\n");

	mfxConfig cfg = MFXCreateConfig(loader);
	mfxVariant v;
	v.Type = MFX_VARIANT_TYPE_U32;
	v.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
	MFXSetConfigFilterProperty(cfg, (const mfxU8 *)"mfxImplDescription.Impl", v);
	v.Data.U32 = INTEL_VENDOR_ID;
	MFXSetConfigFilterProperty(cfg, (const mfxU8 *)"mfxImplDescription.VendorID", v);

	mfxSession session = nullptr;
	mfxStatus sts = MFXCreateSession(loader, 0, &session);
	if (sts != MFX_ERR_NONE || !session) {
		printf("MFXCreateSession failed: %d\n", (int)sts);
		return;
	}

	mfxVersion ver;
	MFXQueryVersion(session, &ver);
	printf("VPL runtime version: %u.%u\n", ver.Major, ver.Minor);

	MFXVideoENCODE *enc = new MFXVideoENCODE(session);

	// Probes WITHOUT extension buffers (isolate IOPattern alone)
	mfxVideoParam p;
	p = make_vp9_base(MFX_IOPATTERN_IN_SYSTEM_MEMORY);
	print_probe("A: sysmem, no ext", enc->Query(&p, &p), &p);

	p = make_vp9_base(MFX_IOPATTERN_IN_VIDEO_MEMORY);
	print_probe("B: videomem, no ext", enc->Query(&p, &p), &p);

	// OBS-like extension buffers: CodingOption2 + CodingOption3 + VP9Param
	mfxExtCodingOption2 co2;
	memset(&co2, 0, sizeof(co2));
	co2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
	co2.Header.BufferSz = sizeof(co2);
	co2.RepeatPPS = MFX_CODINGOPTION_OFF;
	co2.AdaptiveB = MFX_CODINGOPTION_OFF;
	co2.LookAheadDepth = 0;

	mfxExtCodingOption3 co3;
	memset(&co3, 0, sizeof(co3));
	co3.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
	co3.Header.BufferSz = sizeof(co3);
	co3.ScenarioInfo = MFX_SCENARIO_LIVE_STREAMING;

	mfxExtVP9Param vp9p;
	memset(&vp9p, 0, sizeof(vp9p));
	vp9p.Header.BufferId = MFX_EXTBUFF_VP9_PARAM;
	vp9p.Header.BufferSz = sizeof(vp9p);
	vp9p.WriteIVFHeaders = MFX_CODINGOPTION_OFF;

	mfxExtBuffer *exts[3] = {(mfxExtBuffer *)&co2, (mfxExtBuffer *)&co3, (mfxExtBuffer *)&vp9p};

	p = make_vp9_base(MFX_IOPATTERN_IN_SYSTEM_MEMORY);
	p.ExtParam = exts;
	p.NumExtParam = 3;
	print_probe("C: sysmem + ext(co2,co3,vp9)", enc->Query(&p, &p), &p);

	p = make_vp9_base(MFX_IOPATTERN_IN_VIDEO_MEMORY);
	p.ExtParam = exts;
	p.NumExtParam = 3;
	print_probe("D: videomem + ext(co2,co3,vp9)", enc->Query(&p, &p), &p);

	// The 4th buffer OBS attaches on Windows: VideoSignalInfo (SDR Rec.709)
	mfxExtVideoSignalInfo vsi;
	memset(&vsi, 0, sizeof(vsi));
	vsi.Header.BufferId = MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
	vsi.Header.BufferSz = sizeof(vsi);
	vsi.VideoFormat = 0; // MFX_VIDF_UNPROCESSED
	vsi.VideoFullRange = 0; // MFX_CONTENT_DEFAULT_RANGE
	vsi.ColourDescriptionPresent = 1;
	vsi.ColourPrimaries = 1; // BT.709
	vsi.TransferCharacteristics = 1; // BT.709
	vsi.MatrixCoefficients = 1; // BT.709

	mfxExtBuffer *exts4[4] = {(mfxExtBuffer *)&co2, (mfxExtBuffer *)&co3, (mfxExtBuffer *)&vp9p, (mfxExtBuffer *)&vsi};

	p = make_vp9_base(MFX_IOPATTERN_IN_SYSTEM_MEMORY);
	p.ExtParam = exts4;
	p.NumExtParam = 4;
	print_probe("E: sysmem + ext(co2,co3,vp9,vsi)", enc->Query(&p, &p), &p);

	// Bit depth left at 0 (as OBS's NV12 path currently does)
	p = make_vp9_base(MFX_IOPATTERN_IN_SYSTEM_MEMORY);
	p.mfx.FrameInfo.BitDepthLuma = 0;
	p.mfx.FrameInfo.BitDepthChroma = 0;
	print_probe("F: sysmem, BitDepth=0/0", enc->Query(&p, &p), &p);

	// Exact OBS replica: 4 buffers + BitDepth 0/0
	p = make_vp9_base(MFX_IOPATTERN_IN_SYSTEM_MEMORY);
	p.mfx.FrameInfo.BitDepthLuma = 0;
	p.mfx.FrameInfo.BitDepthChroma = 0;
	p.ExtParam = exts4;
	p.NumExtParam = 4;
	print_probe("G: OBS replica (4ext, BitDepth0)", enc->Query(&p, &p), &p);

	enc->Close();
	delete enc;
	MFXClose(session);
}

int main(int argc, char *argv[])
try {
	ComPtr<IDXGIFactory> factory;
	HRESULT hr;

	HANDLE hMainThread;
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &hMainThread, 0, FALSE,
			DUPLICATE_SAME_ACCESS);
	DWORD threadId;
	HANDLE hThread;
	hThread = CreateThread(NULL, 0, TimeoutThread, hMainThread, 0, &threadId);
	CloseHandle(hThread);

	/* --------------------------------------------------------- */
	/* parse expected LUID order                                 */

	bool vp9probe = false;
	for (int i = 1; i < argc; i++) {
		if (std::string(argv[i]) == "--vp9probe")
			vp9probe = true;
		else
			luid_order.push_back(strtoull(argv[i], NULL, 16));
	}

	/* --------------------------------------------------------- */
	/* query qsv support                                         */

	hr = CreateDXGIFactory1(__uuidof(IDXGIFactory), (void **)&factory);
	if (FAILED(hr))
		throw "CreateDXGIFactory1 failed";

	mfxLoader loader = MFXLoad();
	if (!loader)
		throw "MFXLoad failed";

	if (vp9probe) {
		run_vp9_probe(loader);
		return 0;
	}

	mfxConfig cfg = MFXCreateConfig(loader);
	if (!cfg)
		throw "MFXCreateConfig failed";

	mfxVariant impl;

	// Low latency is disabled due to encoding capabilities not being provided before TGL for VPL
	impl.Type = MFX_VARIANT_TYPE_U32;
	impl.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
	MFXSetConfigFilterProperty(cfg, (const mfxU8 *)"mfxImplDescription.Impl", impl);

	mfxSession m_session = nullptr;
	mfxStatus sts = MFXCreateSession(loader, 0, &m_session);

	uint32_t idx = 0;
	while (get_adapter_caps(factory, loader, m_session, idx++) == true)
		;

	if (m_session)
		MFXClose(m_session);

	MFXUnload(loader);

	for (auto &[idx, caps] : adapter_info) {
		printf("[%u]\n", idx);
		printf("is_intel=%s\n", caps.is_intel ? "true" : "false");
		printf("is_dgpu=%s\n", caps.is_dgpu ? "true" : "false");
		printf("supports_av1=%s\n", caps.supports_av1 ? "true" : "false");
		printf("supports_hevc=%s\n", caps.supports_hevc ? "true" : "false");
		printf("supports_vp9=%s\n", caps.supports_vp9 ? "true" : "false");
		printf("vp9_p0_nv12=%s\n", caps.vp9.profile0_nv12_8bit ? "true" : "false");
		printf("vp9_p1_ayuv=%s\n", caps.vp9.profile1_ayuv_8bit ? "true" : "false");
		printf("vp9_p2_p010=%s\n", caps.vp9.profile2_p010_10bit ? "true" : "false");
		printf("vp9_p3_y410=%s\n", caps.vp9.profile3_y410_10bit ? "true" : "false");
		printf("vp9_max_width=%u\n", (unsigned)caps.vp9.max_width);
		printf("vp9_max_height=%u\n", (unsigned)caps.vp9.max_height);
	}

	return 0;
} catch (const char *text) {
	printf("[error]\nstring=%s\n", text);
	return 0;
}
