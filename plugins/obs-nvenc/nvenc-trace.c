/*
 * obs-nvenc forensic trace implementation.
 *
 * Only compiled when NVENC_DEBUG_TRACE is defined (CMake: -DOBS_NVENC_DEBUG_TRACE=ON).
 * Runtime enablement via OBS_NVENC_TRACE=1|2|3 (see nvenc-trace.h).
 *
 * Output files (created in <config path>/nvenc-trace by default):
 *   nvenc-trace-<timestamp>-<codec>-<pid>.txt  human-readable trace
 *   nvenc-trace-<timestamp>-<codec>-<pid>.bin  raw structure snapshots (for diffing)
 */

#include "nvenc-internal.h"
#include "nvenc-trace.h"
#include "nvenc-helpers.h"
#include "cuda-helpers.h"

#include <util/base.h>
#include <util/platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>

#if !defined(NVENC_DEBUG_TRACE)
#error "nvenc-trace.c must only be compiled with NVENC_DEBUG_TRACE defined"
#endif

/* ------------------------------------------------------------------ */
/* SHA-256 (self-contained, FIPS 180-4)                                */
/* ------------------------------------------------------------------ */

struct sha256_ctx {
	uint32_t state[8];
	uint64_t bitlen;
	uint8_t buf[64];
	size_t buflen;
};

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_init(struct sha256_ctx *c)
{
	c->state[0] = 0x6a09e667;
	c->state[1] = 0xbb67ae85;
	c->state[2] = 0x3c6ef372;
	c->state[3] = 0xa54ff53a;
	c->state[4] = 0x510e527f;
	c->state[5] = 0x9b05688c;
	c->state[6] = 0x1f83d9ab;
	c->state[7] = 0x5be0cd19;
	c->bitlen = 0;
	c->buflen = 0;
}

static void sha256_block(struct sha256_ctx *c, const uint8_t *p)
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		const uint32_t s0 = SHA256_ROTR(w[i - 15], 7) ^ SHA256_ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const uint32_t s1 = SHA256_ROTR(w[i - 2], 17) ^ SHA256_ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
	uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];

	for (int i = 0; i < 64; i++) {
		const uint32_t S1 = SHA256_ROTR(e, 6) ^ SHA256_ROTR(e, 11) ^ SHA256_ROTR(e, 25);
		const uint32_t ch = (e & f) ^ (~e & g);
		const uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
		const uint32_t S0 = SHA256_ROTR(a, 2) ^ SHA256_ROTR(a, 13) ^ SHA256_ROTR(a, 22);
		const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
		const uint32_t t2 = S0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = cc;
		cc = b;
		b = a;
		a = t1 + t2;
	}

	c->state[0] += a;
	c->state[1] += b;
	c->state[2] += cc;
	c->state[3] += d;
	c->state[4] += e;
	c->state[5] += f;
	c->state[6] += g;
	c->state[7] += h;
}

static void sha256_update(struct sha256_ctx *c, const void *data, size_t len)
{
	const uint8_t *p = data;
	c->bitlen += (uint64_t)len * 8;
	while (len > 0) {
		c->buf[c->buflen++] = *p++;
		len--;
		if (c->buflen == 64) {
			sha256_block(c, c->buf);
			c->buflen = 0;
		}
	}
}

static void sha256_final(struct sha256_ctx *c, uint8_t out[32])
{
	const uint8_t pad = 0x80;
	c->buf[c->buflen++] = pad;
	if (c->buflen > 56) {
		while (c->buflen < 64)
			c->buf[c->buflen++] = 0;
		sha256_block(c, c->buf);
		c->buflen = 0;
	}
	while (c->buflen < 56)
		c->buf[c->buflen++] = 0;
	for (int i = 7; i >= 0; i--)
		c->buf[c->buflen++] = (uint8_t)(c->bitlen >> (i * 8));
	sha256_block(c, c->buf);
	for (int i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(c->state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)c->state[i];
	}
}

static void sha256_data(const void *data, size_t len, uint8_t out[32])
{
	struct sha256_ctx ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, data, len);
	sha256_final(&ctx, out);
}

static void print_sha256(FILE *f, const uint8_t digest[32])
{
	for (int i = 0; i < 32; i++)
		fprintf(f, "%02x", digest[i]);
}

/* ------------------------------------------------------------------ */
/* Trace context                                                       */
/* ------------------------------------------------------------------ */

#ifndef NVENC_TRACE_MAX_PATH
#define NVENC_TRACE_MAX_PATH 4096
#endif

struct nvenc_trace_ctx {
	FILE *txt;
	FILE *bin;
	char txt_path[NVENC_TRACE_MAX_PATH];
	char bin_path[NVENC_TRACE_MAX_PATH];

	int verbosity; /* 1..3 */
	bool dump_payloads;

	uint32_t event_count;
	uint64_t frame_count;
	uint32_t reconfigure_count;

	/* checkpoint 02 snapshot, used for the preset -> final diff */
	NV_ENC_CONFIG preset_snapshot;
	bool have_preset;

	/* copies taken right before NvEncInitializeEncoder(), compared in 07 */
	NV_ENC_CONFIG pre_init_config;
	NV_ENC_INITIALIZE_PARAMS pre_init_params;
	bool have_pre_init;
};

static struct nvenc_trace_ctx *get_ctx(struct nvenc_data *enc)
{
	return enc ? (struct nvenc_trace_ctx *)enc->trace : NULL;
}

static uint32_t trace_pid(void)
{
#ifdef _WIN32
	return (uint32_t)GetCurrentProcessId();
#else
	return (uint32_t)getpid();
#endif
}

static void timestamp_str(char *out, size_t len)
{
	time_t t = time(NULL);
	struct tm tmv;
#ifdef _WIN32
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	char base[32];
	strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tmv);
	uint64_t ms = 0;
#ifdef _WIN32
	ms = GetTickCount64() % 1000;
#endif
	snprintf(out, len, "%s.%03" PRIu64, base, ms);
}

static const char *codec_name(struct nvenc_data *enc)
{
	switch (enc->codec) {
	case CODEC_H264:
		return "H264";
	case CODEC_HEVC:
		return "HEVC";
	case CODEC_AV1:
		return "AV1";
	default:
		return "UNKNOWN";
	}
}

static void event_begin(struct nvenc_trace_ctx *t, const char *id, const char *title)
{
	t->event_count++;
	char ts[64];
	timestamp_str(ts, sizeof(ts));
	fprintf(t->txt, "\n================================================================\n");
	fprintf(t->txt, "EVENT %04u | %s | %s | %s\n", t->event_count, id, ts, title);
	fprintf(t->txt, "----------------------------------------------------------------\n");
}

static void event_end(struct nvenc_trace_ctx *t)
{
	fflush(t->txt);
}

/* ------------------------------------------------------------------ */
/* Raw dumps and hashes                                                */
/* ------------------------------------------------------------------ */

static void hex_dump(FILE *f, const char *label, const void *data, size_t size)
{
	const uint8_t *p = data;
	uint8_t digest[32];
	sha256_data(data, size, digest);

	fprintf(f, "RAW %s\n", label);
	fprintf(f, "  size   = %zu bytes\n", size);
	fprintf(f, "  sha256 = ");
	print_sha256(f, digest);
	fputc('\n', f);

	for (size_t off = 0; off < size; off += 16) {
		fprintf(f, "  +0x%04zx: ", off);
		size_t n = size - off;
		if (n > 16)
			n = 16;
		for (size_t i = 0; i < 16; i++)
			fprintf(f, "%s%02x", i < n ? "" : " ", i < n ? p[off + i] : 0);
		fputc(' ', f);
		for (size_t i = 0; i < n; i++) {
			const char c = (char)p[off + i];
			fputc((c >= 0x20 && c < 0x7f) ? c : '.', f);
		}
		fprintf(f, "\n");
	}
}

/* Append a raw snapshot record to the binary sidecar.
 * Record layout (little endian):
 *   u32 magic 0x4354564E ('NVTC') | u32 event_id | u32 api_version
 *   u32 struct_size | u16 name_len | char name[] | u8 sha256[32]
 *   u32 data_len | u8 data[]
 */
static void bin_record(struct nvenc_trace_ctx *t, uint32_t api_version, const char *name, const void *data,
		       size_t size)
{
	if (!t->bin || !data)
		return;

	uint8_t digest[32];
	sha256_data(data, size, digest);

	const uint32_t magic = 0x4354564E;
	const uint32_t event_id = t->event_count;
	fwrite(&magic, sizeof(magic), 1, t->bin);
	fwrite(&event_id, sizeof(event_id), 1, t->bin);
	uint32_t api_version_u = api_version;
	fwrite(&api_version_u, sizeof(api_version_u), 1, t->bin);
	uint32_t struct_size = (uint32_t)size;
	fwrite(&struct_size, sizeof(struct_size), 1, t->bin);
	uint16_t name_len = (uint16_t)strlen(name);
	fwrite(&name_len, sizeof(name_len), 1, t->bin);
	fwrite(name, 1, name_len, t->bin);
	fwrite(digest, 1, 32, t->bin);
	fwrite(&struct_size, sizeof(struct_size), 1, t->bin);
	fwrite(data, 1, size, t->bin);
	fflush(t->bin);
}

static void dump_guid(FILE *f, const char *name, const GUID *g)
{
	fprintf(f, "  %-32s = {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n", name, g->Data1, g->Data2,
		g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5],
		g->Data4[6], g->Data4[7]);
}

/* ------------------------------------------------------------------ */
/* Enum name tables                                                    */
/* ------------------------------------------------------------------ */

static const char *cap_name(NV_ENC_CAPS cap)
{
	switch (cap) {
	case NV_ENC_CAPS_NUM_MAX_BFRAMES:
		return "NUM_MAX_BFRAMES";
	case NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES:
		return "SUPPORTED_RATECONTROL_MODES";
	case NV_ENC_CAPS_SUPPORT_FIELD_ENCODING:
		return "SUPPORT_FIELD_ENCODING";
	case NV_ENC_CAPS_SUPPORT_MONOCHROME:
		return "SUPPORT_MONOCHROME";
	case NV_ENC_CAPS_SUPPORT_FMO:
		return "SUPPORT_FMO";
	case NV_ENC_CAPS_SUPPORT_QPELMV:
		return "SUPPORT_QPELMV";
	case NV_ENC_CAPS_SUPPORT_BDIRECT_MODE:
		return "SUPPORT_BDIRECT_MODE";
	case NV_ENC_CAPS_SUPPORT_CABAC:
		return "SUPPORT_CABAC";
	case NV_ENC_CAPS_SUPPORT_ADAPTIVE_TRANSFORM:
		return "SUPPORT_ADAPTIVE_TRANSFORM";
	case NV_ENC_CAPS_SUPPORT_STEREO_MVC:
		return "SUPPORT_STEREO_MVC";
	case NV_ENC_CAPS_NUM_MAX_TEMPORAL_LAYERS:
		return "NUM_MAX_TEMPORAL_LAYERS";
	case NV_ENC_CAPS_SUPPORT_HIERARCHICAL_PFRAMES:
		return "SUPPORT_HIERARCHICAL_PFRAMES";
	case NV_ENC_CAPS_SUPPORT_HIERARCHICAL_BFRAMES:
		return "SUPPORT_HIERARCHICAL_BFRAMES";
	case NV_ENC_CAPS_LEVEL_MAX:
		return "LEVEL_MAX";
	case NV_ENC_CAPS_LEVEL_MIN:
		return "LEVEL_MIN";
	case NV_ENC_CAPS_SEPARATE_COLOUR_PLANE:
		return "SEPARATE_COLOUR_PLANE";
	case NV_ENC_CAPS_WIDTH_MAX:
		return "WIDTH_MAX";
	case NV_ENC_CAPS_HEIGHT_MAX:
		return "HEIGHT_MAX";
	case NV_ENC_CAPS_SUPPORT_TEMPORAL_SVC:
		return "SUPPORT_TEMPORAL_SVC";
	case NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE:
		return "SUPPORT_DYN_RES_CHANGE";
	case NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE:
		return "SUPPORT_DYN_BITRATE_CHANGE";
	case NV_ENC_CAPS_SUPPORT_DYN_FORCE_CONSTQP:
		return "SUPPORT_DYN_FORCE_CONSTQP";
	case NV_ENC_CAPS_SUPPORT_DYN_RCMODE_CHANGE:
		return "SUPPORT_DYN_RCMODE_CHANGE";
	case NV_ENC_CAPS_SUPPORT_SUBFRAME_READBACK:
		return "SUPPORT_SUBFRAME_READBACK";
	case NV_ENC_CAPS_SUPPORT_CONSTRAINED_ENCODING:
		return "SUPPORT_CONSTRAINED_ENCODING";
	case NV_ENC_CAPS_SUPPORT_INTRA_REFRESH:
		return "SUPPORT_INTRA_REFRESH";
	case NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE:
		return "SUPPORT_CUSTOM_VBV_BUF_SIZE";
	case NV_ENC_CAPS_SUPPORT_DYNAMIC_SLICE_MODE:
		return "SUPPORT_DYNAMIC_SLICE_MODE";
	case NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION:
		return "SUPPORT_REF_PIC_INVALIDATION";
	case NV_ENC_CAPS_PREPROC_SUPPORT:
		return "PREPROC_SUPPORT";
	case NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT:
		return "ASYNC_ENCODE_SUPPORT";
	case NV_ENC_CAPS_MB_NUM_MAX:
		return "MB_NUM_MAX";
	case NV_ENC_CAPS_MB_PER_SEC_MAX:
		return "MB_PER_SEC_MAX";
	case NV_ENC_CAPS_SUPPORT_YUV444_ENCODE:
		return "SUPPORT_YUV444_ENCODE";
	case NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE:
		return "SUPPORT_LOSSLESS_ENCODE";
	case NV_ENC_CAPS_SUPPORT_SAO:
		return "SUPPORT_SAO";
	case NV_ENC_CAPS_SUPPORT_MEONLY_MODE:
		return "SUPPORT_MEONLY_MODE";
	case NV_ENC_CAPS_SUPPORT_LOOKAHEAD:
		return "SUPPORT_LOOKAHEAD";
	case NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ:
		return "SUPPORT_TEMPORAL_AQ";
	case NV_ENC_CAPS_SUPPORT_10BIT_ENCODE:
		return "SUPPORT_10BIT_ENCODE";
	case NV_ENC_CAPS_NUM_MAX_LTR_FRAMES:
		return "NUM_MAX_LTR_FRAMES";
	case NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION:
		return "SUPPORT_WEIGHTED_PREDICTION";
	case NV_ENC_CAPS_DYNAMIC_QUERY_ENCODER_CAPACITY:
		return "DYNAMIC_QUERY_ENCODER_CAPACITY";
	case NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE:
		return "SUPPORT_BFRAME_REF_MODE";
	case NV_ENC_CAPS_SUPPORT_EMPHASIS_LEVEL_MAP:
		return "SUPPORT_EMPHASIS_LEVEL_MAP";
	case NV_ENC_CAPS_WIDTH_MIN:
		return "WIDTH_MIN";
	case NV_ENC_CAPS_HEIGHT_MIN:
		return "HEIGHT_MIN";
	case NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES:
		return "SUPPORT_MULTIPLE_REF_FRAMES";
	case NV_ENC_CAPS_SUPPORT_ALPHA_LAYER_ENCODING:
		return "SUPPORT_ALPHA_LAYER_ENCODING";
	case NV_ENC_CAPS_NUM_ENCODER_ENGINES:
		return "NUM_ENCODER_ENGINES";
	case NV_ENC_CAPS_SINGLE_SLICE_INTRA_REFRESH:
		return "SINGLE_SLICE_INTRA_REFRESH";
	case NV_ENC_CAPS_DISABLE_ENC_STATE_ADVANCE:
		return "DISABLE_ENC_STATE_ADVANCE";
	case NV_ENC_CAPS_OUTPUT_RECON_SURFACE:
		return "OUTPUT_RECON_SURFACE";
	case NV_ENC_CAPS_OUTPUT_BLOCK_STATS:
		return "OUTPUT_BLOCK_STATS";
	case NV_ENC_CAPS_OUTPUT_ROW_STATS:
		return "OUTPUT_ROW_STATS";
	case NV_ENC_CAPS_SUPPORT_TEMPORAL_FILTER:
		return "SUPPORT_TEMPORAL_FILTER";
	case NV_ENC_CAPS_SUPPORT_LOOKAHEAD_LEVEL:
		return "SUPPORT_LOOKAHEAD_LEVEL";
	case NV_ENC_CAPS_SUPPORT_UNIDIRECTIONAL_B:
		return "SUPPORT_UNIDIRECTIONAL_B";
	default:
		return NULL;
	}
}

static const char *rc_mode_name(NV_ENC_PARAMS_RC_MODE mode)
{
	switch (mode) {
	case NV_ENC_PARAMS_RC_CONSTQP:
		return "CONSTQP";
	case NV_ENC_PARAMS_RC_VBR:
		return "VBR";
	case NV_ENC_PARAMS_RC_CBR:
		return "CBR";
	default:
		return NULL;
	}
}

static const char *multi_pass_name(NV_ENC_MULTI_PASS pass)
{
	switch (pass) {
	case NV_ENC_MULTI_PASS_DISABLED:
		return "DISABLED";
	case NV_ENC_TWO_PASS_QUARTER_RESOLUTION:
		return "TWO_PASS_QUARTER_RESOLUTION";
	case NV_ENC_TWO_PASS_FULL_RESOLUTION:
		return "TWO_PASS_FULL_RESOLUTION";
	default:
		return NULL;
	}
}

static const char *pic_struct_name(NV_ENC_PIC_STRUCT s)
{
	switch (s) {
	case NV_ENC_PIC_STRUCT_FRAME:
		return "FRAME";
	case NV_ENC_PIC_STRUCT_FIELD_TOP_BOTTOM:
		return "FIELD_TOP_BOTTOM";
	case NV_ENC_PIC_STRUCT_FIELD_BOTTOM_TOP:
		return "FIELD_BOTTOM_TOP";
	default:
		return NULL;
	}
}

static const char *buffer_fmt_name(NV_ENC_BUFFER_FORMAT f)
{
	switch (f) {
	case NV_ENC_BUFFER_FORMAT_NV12:
		return "NV12";
	case NV_ENC_BUFFER_FORMAT_YV12:
		return "YV12";
	case NV_ENC_BUFFER_FORMAT_IYUV:
		return "IYUV";
	case NV_ENC_BUFFER_FORMAT_YUV444:
		return "YUV444";
	case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
		return "YUV420_10BIT";
	case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
		return "YUV444_10BIT";
	case NV_ENC_BUFFER_FORMAT_ARGB:
		return "ARGB";
	case NV_ENC_BUFFER_FORMAT_AYUV:
		return "AYUV";
	default:
		return NULL;
	}
}

static const char *bframe_ref_mode_name(NV_ENC_BFRAME_REF_MODE m)
{
	switch (m) {
	case NV_ENC_BFRAME_REF_MODE_DISABLED:
		return "DISABLED";
	case NV_ENC_BFRAME_REF_MODE_EACH:
		return "EACH";
	case NV_ENC_BFRAME_REF_MODE_MIDDLE:
		return "MIDDLE";
	case NV_ENC_BFRAME_REF_MODE_HIERARCHICAL:
		return "HIERARCHICAL";
	default:
		return NULL;
	}
}

static const char *tf_level_name(NV_ENC_TEMPORAL_FILTER_LEVEL l)
{
	/* SDK 13.x enum values are 0 and 4; older SDKs used int32_t 0..3. */
	switch ((int)l) {
	case 0:
		return "LEVEL_0(0)";
	case 4:
		return "LEVEL_4(4)";
	case 1:
		return "legacy LOW(1)";
	case 2:
		return "legacy MEDIUM(2)";
	case 3:
		return "legacy HIGH(3)";
	default:
		return NULL;
	}
}

static const char *lookahead_level_name(NV_ENC_LOOKAHEAD_LEVEL l)
{
	switch (l) {
	case NV_ENC_LOOKAHEAD_LEVEL_0:
		return "LEVEL_0(0)";
	case NV_ENC_LOOKAHEAD_LEVEL_1:
		return "LEVEL_1(1)";
	case NV_ENC_LOOKAHEAD_LEVEL_2:
		return "LEVEL_2(2)";
	case NV_ENC_LOOKAHEAD_LEVEL_3:
		return "LEVEL_3(3)";
	case NV_ENC_LOOKAHEAD_LEVEL_AUTOSELECT:
		return "AUTOSELECT(15)";
	default:
		return NULL;
	}
}

static const char *qp_map_mode_name(NV_ENC_QP_MAP_MODE m)
{
	switch (m) {
	case NV_ENC_QP_MAP_DISABLED:
		return "DISABLED";
	case NV_ENC_QP_MAP_EMPHASIS:
		return "EMPHASIS";
	case NV_ENC_QP_MAP_DELTA:
		return "DELTA";
	default:
		return NULL;
	}
}

static const char *bit_depth_name(NV_ENC_BIT_DEPTH d)
{
	switch (d) {
	case NV_ENC_BIT_DEPTH_8:
		return "8";
	case NV_ENC_BIT_DEPTH_10:
		return "10";
	default:
		return NULL;
	}
}

static const char *codec_guid_name(const GUID *g)
{
	if (memcmp(g, &NV_ENC_CODEC_H264_GUID, sizeof(GUID)) == 0)
		return "H264";
	if (memcmp(g, &NV_ENC_CODEC_HEVC_GUID, sizeof(GUID)) == 0)
		return "HEVC";
	if (memcmp(g, &NV_ENC_CODEC_AV1_GUID, sizeof(GUID)) == 0)
		return "AV1";
	return NULL;
}

/* ------------------------------------------------------------------ */
/* NV_ENC_CONFIG field list                                            */
/*                                                                     */
/* The single source of truth for the field inventory lives in         */
/* nvenc-trace.h (NVENC_CONFIG_FIELDS*).                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Named config dump and semantic diff                                 */
/* ------------------------------------------------------------------ */

static void print_named(FILE *f, const char *name, unsigned val, const char *nm)
{
	if (nm)
		fprintf(f, "  %-42s = %u (%s)\n", name, val, nm);
	else
		fprintf(f, "  %-42s = %u\n", name, val);
}

/* NOTE: the trailing ';' is required - the X-macro list concatenates entries
 * back-to-back, so each expansion must be a self-terminated statement. */
#define NVENC_TRACE_PRINT_FIELD(fld) \
	do { fprintf(f, "  %-42s = %u\n", #fld, (unsigned)(cfg->fld)); } while (0);

static void dump_config_named(FILE *f, const char *label, const NV_ENC_CONFIG *cfg)
{
	fprintf(f, "CONFIG %s (sizeof(NV_ENC_CONFIG)=%zu)\n", label, sizeof(*cfg));
	NVENC_CONFIG_FIELDS(NVENC_TRACE_PRINT_FIELD);
	NVENC_CONFIG_FIELDS_12_2(NVENC_TRACE_PRINT_FIELD);
	NVENC_CONFIG_FIELDS_13_0(NVENC_TRACE_PRINT_FIELD);
	dump_guid(f, "profileGUID", &cfg->profileGUID);

	/* enum-typed fields with symbolic names */
	print_named(f, "rcParams.rateControlMode (named)", (unsigned)cfg->rcParams.rateControlMode,
		    rc_mode_name(cfg->rcParams.rateControlMode));
	print_named(f, "rcParams.multiPass (named)", (unsigned)cfg->rcParams.multiPass,
		    multi_pass_name(cfg->rcParams.multiPass));
	print_named(f, "rcParams.qpMapMode (named)", (unsigned)cfg->rcParams.qpMapMode,
		    qp_map_mode_name(cfg->rcParams.qpMapMode));
#ifdef NVENC_12_2_OR_LATER
	print_named(f, "rcParams.lookaheadLevel (named)", (unsigned)cfg->rcParams.lookaheadLevel,
		    lookahead_level_name(cfg->rcParams.lookaheadLevel));
#endif
#ifdef NVENC_13_0_OR_LATER
	print_named(f, "h264.tfLevel (named)", (unsigned)cfg->encodeCodecConfig.h264Config.tfLevel,
	    tf_level_name(cfg->encodeCodecConfig.h264Config.tfLevel));
#endif
#ifdef NVENC_12_2_OR_LATER
	print_named(f, "hevc.tfLevel (named)", (unsigned)cfg->encodeCodecConfig.hevcConfig.tfLevel,
	    tf_level_name(cfg->encodeCodecConfig.hevcConfig.tfLevel));
#endif
#ifdef NVENC_13_0_OR_LATER
	print_named(f, "av1.tfLevel (named)", (unsigned)cfg->encodeCodecConfig.av1Config.tfLevel,
	    tf_level_name(cfg->encodeCodecConfig.av1Config.tfLevel));
#endif
}

#define NVENC_TRACE_DIFF_FIELD(fld) \
	if ((preset)->fld != (final)->fld) { \
		fprintf(f, "  DIFF %-40s preset=%-8u final=%-8u\n", #fld, (unsigned)(preset)->fld, (unsigned)(final)->fld); \
		n++; \
	}

static int diff_config_named(FILE *f, const NV_ENC_CONFIG *preset, const NV_ENC_CONFIG *final)
{
	int n = 0;
	fprintf(f, "SEMANTIC DIFF preset -> final (fields changed by OBS after preset lookup)\n");
	NVENC_CONFIG_FIELDS(NVENC_TRACE_DIFF_FIELD);
	NVENC_CONFIG_FIELDS_12_2(NVENC_TRACE_DIFF_FIELD);
	NVENC_CONFIG_FIELDS_13_0(NVENC_TRACE_DIFF_FIELD);
	if (memcmp(&preset->profileGUID, &final->profileGUID, sizeof(GUID)) != 0) {
		dump_guid(f, "DIFF profileGUID preset", &preset->profileGUID);
		dump_guid(f, "DIFF profileGUID final", &final->profileGUID);
		n++;
	}
	if (n == 0)
		fprintf(f, "  (no semantic differences)\n");
	else
		fprintf(f, "  %d field(s) differ\n", n);
	return n;
}

/* Raw byte-level diff between two memory regions of the same size. */
static void raw_byte_diff(FILE *f, const char *label, const void *a, const void *b, size_t size)
{
	const uint8_t *pa = a;
	const uint8_t *pb = b;
	size_t diffs = 0;
	int printed = 0;

	for (size_t off = 0; off < size; off++) {
		if (pa[off] != pb[off]) {
			diffs++;
			if (printed < 32) {
				fprintf(f, "  +0x%04zx: %02x -> %02x\n", off, pa[off], pb[off]);
				printed++;
			}
		}
	}
	fprintf(f, "RAW BYTE DIFF %s: %zu/%zu bytes differ%s\n", label, diffs, size,
		printed == 32 ? " (truncated)" : "");
}

/* ------------------------------------------------------------------ */
/* Public trace API                                                    */
/* ------------------------------------------------------------------ */

void *nvenc_trace_init(struct nvenc_data *enc)
{
	const char *env = getenv("OBS_NVENC_TRACE");
	if (!env || !*env)
		return NULL;

	const int verbosity = atoi(env);
	if (verbosity < 1 || verbosity > 3)
		return NULL;

	struct nvenc_trace_ctx *t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->verbosity = verbosity;
	t->dump_payloads = getenv("OBS_NVENC_TRACE_PAYLOADS") != NULL;

	char dir[NVENC_TRACE_MAX_PATH];
	const char *env_dir = getenv("OBS_NVENC_TRACE_DIR");
	if (env_dir && *env_dir) {
		snprintf(dir, sizeof(dir), "%s", env_dir);
	} else {
		char cfgpath[NVENC_TRACE_MAX_PATH];
		if (os_get_config_path(cfgpath, sizeof(cfgpath), "obs-studio") < 0) {
			blog(LOG_ERROR, "nvenc-trace: failed to resolve config path");
			free(t);
			return NULL;
		}
		snprintf(dir, sizeof(dir), "%s/nvenc-trace", cfgpath);
	}
	{
		const int mkdir_ret = os_mkdir(dir);
		if (mkdir_ret != MKDIR_SUCCESS && mkdir_ret != MKDIR_EXISTS) {
			blog(LOG_ERROR, "nvenc-trace: failed to create directory %s", dir);
			free(t);
			return NULL;
		}
	}

	char ts[32];
	time_t now = time(NULL);
	struct tm tmv;
#ifdef _WIN32
	localtime_s(&tmv, &now);
#else
	localtime_r(&now, &tmv);
#endif
	strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tmv);

	snprintf(t->txt_path, sizeof(t->txt_path), "%s/nvenc-trace-%s-%s-%u.txt", dir, ts, codec_name(enc),
		 trace_pid());
	snprintf(t->bin_path, sizeof(t->bin_path), "%s/nvenc-trace-%s-%s-%u.bin", dir, ts, codec_name(enc),
		 trace_pid());

	t->txt = fopen(t->txt_path, "w");
	if (!t->txt) {
		blog(LOG_ERROR, "nvenc-trace: failed to open %s", t->txt_path);
		free(t);
		return NULL;
	}
	t->bin = fopen(t->bin_path, "wb");

	event_begin(t, "00_SESSION_ENVIRONMENT", "session/environment checkpoint");
	fprintf(t->txt, "  trace file       = %s\n", t->txt_path);
	fprintf(t->txt, "  bin sidecar      = %s%s\n", t->bin_path, t->bin ? "" : " (FAILED TO OPEN)");
	fprintf(t->txt, "  verbosity        = %d%s\n", t->verbosity, t->dump_payloads ? " (payload dumps on)" : "");
	fprintf(t->txt, "  platform         = %s\n",
#ifdef _WIN32
	        "windows"
#else
	        "posix"
#endif
	);
	fprintf(t->txt, "  OBS version      = %s\n", obs_get_version_string());
	fprintf(t->txt, "  ffnvcodec loaded = API v%u (driver %d.%d)\n", nvenc_get_loaded_api_version(),
		nvenc_driver_version_major(), nvenc_driver_version_minor());
	fprintf(t->txt, "  codec            = %s\n", codec_name(enc));

	int device_count = 0;
	if (cu && cu->cuDeviceGetCount && cu->cuDeviceGetCount(&device_count) == CUDA_SUCCESS) {
		for (int i = 0; i < device_count; i++) {
			CUdevice dev;
			char name[128] = "?";
			if (cu->cuDeviceGet(&dev, i) == CUDA_SUCCESS && cu->cuDeviceGetName &&
			    cu->cuDeviceGetName(name, sizeof(name), dev) == CUDA_SUCCESS)
				fprintf(t->txt, "  cuda device %d      = %s\n", i, name);
		}
	} else {
		fprintf(t->txt, "  cuda devices     = (CUDA library not loaded)\n");
	}

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		fprintf(t->txt, "  base resolution  = %ux%u @ %u/%u fps\n", ovi.base_width, ovi.base_height,
			ovi.fps_num, ovi.fps_den);
		fprintf(t->txt, "  output resolution= %ux%u\n", ovi.output_width, ovi.output_height);
		fprintf(t->txt, "  colorspace       = %d, range = %d, format = %d\n", (int)ovi.colorspace,
			(int)ovi.range, (int)ovi.output_format);
	}

	fprintf(t->txt, "OBS encoder properties:\n");
	if (enc->props.data) {
		obs_data_item_t *item = obs_data_first(enc->props.data);
		while (item) {
			const char *key = obs_data_item_get_name(item);
			const char *val = obs_data_get_string(enc->props.data, key);
			fprintf(t->txt, "  prop %-28s = %s\n", key, val ? val : "(null)");
			obs_data_item_next(&item);
		}
	} else {
		fprintf(t->txt, "  (no settings data)\n");
	}

	event_end(t);
	blog(LOG_INFO, "nvenc-trace: tracing enabled (verbosity %d), file: %s", t->verbosity, t->txt_path);
	return t;
}

void nvenc_trace_destroy(struct nvenc_data *enc)
{
	struct nvenc_trace_ctx *t = get_ctx(enc);
	if (!t)
		return;

	event_begin(t, "99_SESSION_END", "session teardown");
	fprintf(t->txt, "  events           = %u\n", t->event_count);
	fprintf(t->txt, "  frames traced    = %llu\n", (unsigned long long)t->frame_count);
	fprintf(t->txt, "  reconfigurations = %u\n", t->reconfigure_count);
	event_end(t);

	if (t->bin)
		fclose(t->bin);
	if (t->txt)
		fclose(t->txt);
	free(t);
	enc->trace = NULL;
}

void nvenc_trace_cap(struct nvenc_data *enc, NV_ENC_CAPS cap, int value, NVENCSTATUS status)
{
	struct nvenc_trace_ctx *t = get_ctx(enc);
	if (!t)
		return;

	event_begin(t, "01_CAPABILITY_QUERY", "NvEncGetEncodeCaps");
	const char *nm = cap_name(cap);
	fprintf(t->txt, "  cap            = %s (enum value %d)\n", nm ? nm : "?", (int)cap);
	fprintf(t->txt, "  value          = %d\n", value);
	fprintf(t->txt, "  status         = %s\n", nv_error_name(status));
	event_end(t);
}

void nvenc_trace_preset(struct nvenc_data *enc, const NV_ENC_PRESET_CONFIG *preset_config, uint32_t preset_idx,
			uint32_t tuning_idx, uint32_t multipass)
{
	struct nvenc_trace_ctx *t = get_ctx(enc);
	if (!t || !preset_config)
		return;

	/* keep a copy for the later preset -> final diff */
	memcpy(&t->preset_snapshot, &preset_config->presetCfg, sizeof(t->preset_snapshot));
	t->have_preset = true;

	char title[128];
	snprintf(title, sizeof(title), "NvEncGetEncodePresetConfigEx returned (preset=%u tuning=%u multipass=%u)",
		 preset_idx, tuning_idx, multipass);
	event_begin(t, "02_PRESET_RETURNED_BY_NVENC", title);
	dump_config_named(t->txt, "PRESET_RETURNED_BY_NVENC", &preset_config->presetCfg);
	hex_dump(t->txt, "NV_ENC_CONFIG (preset)", &preset_config->presetCfg, sizeof(preset_config->presetCfg));
	bin_record(t, nvenc_get_loaded_api_version(), "02_PRESET_CONFIG", &preset_config->presetCfg,
		   sizeof(preset_config->presetCfg));
	event_end(t);
}

void nvenc_trace_pre_init(struct nvenc_data *enc, const NV_ENC_CONFIG *config, const NV_ENC_INITIALIZE_PARAMS *params)
{
	struct nvenc_trace_ctx *t = get_ctx(enc);
	if (!t || !config || !params)
		return;

	/* keep copies for the post-init memory comparison */
	memcpy(&t->pre_init_config, config, sizeof(t->pre_init_config));
	memcpy(&t->pre_init_params, params, sizeof(t->pre_init_params));
	t->have_pre_init = true;

	event_begin(t, "03_FINAL_CONFIG_SENT_TO_NVENC", "final NV_ENC_CONFIG after OBS modifications");
	dump_config_named(t->txt, "FINAL_CONFIG_SENT_TO_NVENC", config);
	hex_dump(t->txt, "NV_ENC_CONFIG (final)", config, sizeof(*config));
	bin_record(t, nvenc_get_loaded_api_version(), "03_FINAL_CONFIG", config, sizeof(*config));
	event_end(t);

	if (t->have_preset) {
		uint8_t h_preset[32], h_final[32];
		sha256_data(&t->preset_snapshot, sizeof(t->preset_snapshot), h_preset);
		sha256_data(config, sizeof(*config), h_final);

		event_begin(t, "04_PRESET_VS_FINAL_DIFF", "diff: preset returned by NVENC vs final config sent");
		fprintf(t->txt, "  sha256 preset = ");
		print_sha256(t->txt, h_preset);
		fputc('\n', t->txt);
		fprintf(t->txt, "  sha256 final  = ");
		print_sha256(t->txt, h_final);
		fputc('\n', t->txt);
		diff_config_named(t->txt, &t->preset_snapshot, config);
		raw_byte_diff(t->txt, "NV_ENC_CONFIG preset vs final", &t->preset_snapshot, config, sizeof(*config));
		event_end(t);
	}

	event_begin(t, "05_INITIALIZE_PARAMS", "full NV_ENC_INITIALIZE_PARAMS before NvEncInitializeEncoder()");
	fprintf(t->txt, "INITIALIZE_PARAMS (sizeof=%zu)\n", sizeof(*params));
#ifdef NVENC_13_0_OR_LATER
	dump_guid(t->txt, "encodeGUID", &params->encodeGUID);
	dump_guid(t->txt, "presetGUID", &params->presetGUID);
#else
	dump_guid(t->txt, "codecGuid", &params->codecGuid);
#endif
	fprintf(t->txt, "  %-42s = %u\n", "version", params->version);
	fprintf(t->txt, "  %-42s = %ux%u\n", "encodeWidth x encodeHeight", params->encodeWidth, params->encodeHeight);
#ifdef NVENC_13_0_OR_LATER
	fprintf(t->txt, "  %-42s = %ux%u\n", "darWidth x darHeight", params->darWidth, params->darHeight);
#endif
	fprintf(t->txt, "  %-42s = %u/%u\n", "frameRateNum / frameRateDen", params->frameRateNum, params->frameRateDen);
	fprintf(t->txt, "  %-42s = %u\n", "enableEncodeAsync", params->enableEncodeAsync);
	fprintf(t->txt, "  %-42s = %u\n", "enablePTD", params->enablePTD);
#ifdef NVENC_13_0_OR_LATER
	fprintf(t->txt, "  %-42s = %u\n", "maxEncodeWidth", params->maxEncodeWidth);
	fprintf(t->txt, "  %-42s = %u\n", "maxEncodeHeight", params->maxEncodeHeight);
#endif
	print_named(t->txt, "tuningInfo", (unsigned)params->tuningInfo, NULL);
	print_named(t->txt, "bufferFormat", (unsigned)params->bufferFormat, buffer_fmt_name(params->bufferFormat));
	fprintf(t->txt, "  %-42s = %p\n", "encodeConfig", (const void *)params->encodeConfig);
	if (params->encodeConfig)
		fprintf(t->txt, "  (encodeConfig points to the final config dumped in event 03)\n");

	/* normalized raw dump: pointer fields zeroed so the hash is deterministic */
	NV_ENC_INITIALIZE_PARAMS norm = *params;
	memset(&norm.encodeConfig, 0, sizeof(norm.encodeConfig));
#ifdef NVENC_12_2_OR_LATER
	memset(&norm.privData, 0, sizeof(norm.privData));
#endif
	hex_dump(t->txt, "NV_ENC_INITIALIZE_PARAMS (normalized: pointers zeroed)", &norm, sizeof(norm));
	bin_record(t, nvenc_get_loaded_api_version(), "05_INIT_PARAMS_NORMALIZED", &norm, sizeof(norm));
	event_end(t);
}

void nvenc_trace_post_init(struct nvenc_data *enc, const NV_ENC_CONFIG *config, const NV_ENC_INITIALIZE_PARAMS *params,
			   NVENCSTATUS status)
{
	struct nvenc_trace_ctx *t = get_ctx(enc);
	if (!t || !config || !params)
		return;

	event_begin(t, "06_INITIALIZE_CALL_RETURN", "NvEncInitializeEncoder() return");
	fprintf(t->txt, "  status         = %s\n", nv_error_name(status));
	event_end(t);

	if (t->have_pre_init) {
		event_begin(t, "07_MEMORY_AFTER_INITIALIZE_CALL",
			    "process memory after NvEncInitializeEncoder() returned; NOT a driver/firmware state readback");
		fprintf(t->txt,
			"NOTE: no public NVENC API exposes the effective encoder state.\n"
			"      This section only shows which bytes of the caller-owned\n"
			"      structures changed between the pre-call copy and now.\n");

		uint8_t h_pre[32], h_post[32];
		sha256_data(&t->pre_init_config, sizeof(t->pre_init_config), h_pre);
		sha256_data(config, sizeof(*config), h_post);
		fprintf(t->txt, "  sha256 config pre-init  = ");
		print_sha256(t->txt, h_pre);
		fputc('\n', t->txt);
		fprintf(t->txt, "  sha256 config post-init = ");
		print_sha256(t->txt, h_post);
		fputc('\n', t->txt);
		diff_config_named(t->txt, &t->pre_init_config, config);
		raw_byte_diff(t->txt, "NV_ENC_CONFIG pre vs post init", &t->pre_init_config, config, sizeof(*config));

		sha256_data(&t->pre_init_params, sizeof(t->pre_init_params), h_pre);
		sha256_data(params, sizeof(*params), h_post);
		fprintf(t->txt, "  sha256 params pre-init  = ");
		print_sha256(t->txt, h_pre);
		fputc('\n', t->txt);
		fprintf(t->txt, "  sha256 params post-init = ");
		print_sha256(t->txt, h_post);
		fputc('\n', t->txt);
		raw_byte_diff(t->txt, "NV_ENC_INITIALIZE_PARAMS pre vs post init", &t->pre_init_params, params,
			      sizeof(*params));
		event_end(t);
	}
}

void nvenc_trace_reconfigure(struct nvenc_data *enc, const char *origin, const NV_ENC_RECONFIGURE_PARAMS *params,
			     NVENCSTATUS status)
{
	struct nvenc_trace_ctx *t = get_ctx(enc);
	if (!t || !params)
		return;

	t->reconfigure_count++;
	char title[160];
	snprintf(title, sizeof(title), "NvEncReconfigureEncoder #%u from %s", t->reconfigure_count, origin ? origin : "?");
	event_begin(t, "08_RECONFIGURE", title);
	fprintf(t->txt, "  version        = %u\n", params->version);
	fprintf(t->txt, "  resetEncoder   = %u\n", (unsigned)params->resetEncoder);
	fprintf(t->txt, "  forceIDR       = %u\n", (unsigned)params->forceIDR);

	const NV_ENC_INITIALIZE_PARAMS *ri = &params->reInitEncodeParams;
#ifdef NVENC_13_0_OR_LATER
	dump_guid(t->txt, "reInit.encodeGUID", &ri->encodeGUID);
	dump_guid(t->txt, "reInit.presetGUID", &ri->presetGUID);
#else
	dump_guid(t->txt, "reInit.codecGuid", &ri->codecGuid);
#endif
	fprintf(t->txt, "  %-42s = %ux%u\n", "reInit.encodeWidth x Height", ri->encodeWidth, ri->encodeHeight);
	fprintf(t->txt, "  %-42s = %u/%u\n", "reInit.frameRateNum / Den", ri->frameRateNum, ri->frameRateDen);
	print_named(t->txt, "reInit.tuningInfo", (unsigned)ri->tuningInfo, NULL);

	if (ri->encodeConfig) {
		dump_config_named(t->txt, "RECONFIGURE_CONFIG_SENT_TO_NVENC", ri->encodeConfig);
		bin_record(t, nvenc_get_loaded_api_version(), "08_RECONFIGURE_CONFIG", ri->encodeConfig,
			   sizeof(*ri->encodeConfig));
	} else {
		fprintf(t->txt, "  reInit.encodeConfig = NULL (NVENC keeps existing config / preset default)\n");
	}

	/* normalized raw dump of the whole reconfigure params */
	NV_ENC_RECONFIGURE_PARAMS norm = *params;
	memset(&norm.reInitEncodeParams.encodeConfig, 0, sizeof(void *));
#ifdef NVENC_12_2_OR_LATER
	memset(&norm.reInitEncodeParams.privData, 0, sizeof(void *));
#endif
	hex_dump(t->txt, "NV_ENC_RECONFIGURE_PARAMS (normalized: pointers zeroed)", &norm, sizeof(norm));
	bin_record(t, nvenc_get_loaded_api_version(), "08_RECONFIGURE_PARAMS_NORMALIZED", &norm, sizeof(norm));

	fprintf(t->txt, "  status         = %s\n", nv_error_name(status));
	event_end(t);
}

void nvenc_trace_pic(struct nvenc_data *enc, const NV_ENC_PIC_PARAMS *pp, bool eos)
{
	struct nvenc_trace_ctx *t = get_ctx(enc);
	if (!t || !pp || t->verbosity < 2)
		return;

	t->frame_count++;
	char title[96];
	snprintf(title, sizeof(title), "NvEncEncodePicture frame %llu%s", (unsigned long long)t->frame_count,
		 eos ? " (EOS)" : "");
	event_begin(t, "09_PIC_PARAMS", title);
	fprintf(t->txt, "  version        = %u\n", pp->version);
	fprintf(t->txt, "  input          = %ux%u pitch=%u\n", pp->inputWidth, pp->inputHeight, pp->inputPitch);
	fprintf(t->txt, "  frameIdx       = %llu\n", (unsigned long long)pp->frameIdx);
	fprintf(t->txt, "  timestamp      = %llu ns, duration = %llu ns\n", (unsigned long long)pp->inputTimeStamp,
		(unsigned long long)pp->inputDuration);
	print_named(t->txt, "bufferFmt", (unsigned)pp->bufferFmt, buffer_fmt_name(pp->bufferFmt));
	print_named(t->txt, "pictureStruct", (unsigned)pp->pictureStruct, pic_struct_name(pp->pictureStruct));
	fprintf(t->txt, "  %-42s = %u\n", "pictureType", (unsigned)pp->pictureType);

	/* encodePicFlags bit decode (NV_ENC_PIC_FLAGS) */
	const uint32_t flags = pp->encodePicFlags;
	fprintf(t->txt, "  %-42s = 0x%08X", "encodePicFlags", flags);
	if (flags == 0) {
		fputs(" (NONE)\n", t->txt);
	} else {
		fputc('\n', t->txt);
#define PRINT_FLAG(bit, name) \
		if (flags & (bit)) fprintf(t->txt, "    + %s\n", name)
		PRINT_FLAG(0x1, "FORCEINTRA");
		PRINT_FLAG(0x2, "FORCEIDR");
		PRINT_FLAG(0x4, "OUTPUT_SPSPPS");
		PRINT_FLAG(0x8, "EOS");
		PRINT_FLAG(0x10, "DISABLE_ENC_STATE_ADVANCE");
		PRINT_FLAG(0x20, "OUTPUT_RECON_FRAME");
		PRINT_FLAG(0x40, "PREQPDELTAMINQP");
		PRINT_FLAG(0x80, "PREQPDELTAMAXQP");
#undef PRINT_FLAG
	}

	fprintf(t->txt, "  inputBuffer    = %p\n", (const void *)pp->inputBuffer);
	fprintf(t->txt, "  outputBitstream= %p\n", (const void *)pp->outputBitstream);
	fprintf(t->txt, "  completionEvent= %p\n", pp->completionEvent);

	if (pp->qpDeltaMap && pp->qpDeltaMapSize > 0) {
		uint8_t digest[32];
		sha256_data(pp->qpDeltaMap, pp->qpDeltaMapSize, digest);
		fprintf(t->txt, "  %-42s = %p\n", "qpDeltaMap", (const void *)pp->qpDeltaMap);
		fprintf(t->txt, "  %-42s = %u bytes (mode from rcParams.qpMapMode)\n", "qpDeltaMap.size",
			pp->qpDeltaMapSize);
		fprintf(t->txt, "  %-42s = ", "qpDeltaMap.sha256");
		print_sha256(t->txt, digest);
		fputc('\n', t->txt);
		if (t->dump_payloads && t->verbosity >= 3)
			hex_dump(t->txt, "qpDeltaMap payload", pp->qpDeltaMap, pp->qpDeltaMapSize);
	} else {
		fprintf(t->txt, "  %-42s = (null) - client provides no QP map; qpMapMode=%u\n", "qpDeltaMap",
			enc ? enc->config.rcParams.qpMapMode : 0);
	}

	if (t->verbosity >= 3) {
		NV_ENC_PIC_PARAMS norm = *pp;
		memset(&norm.inputBuffer, 0, sizeof(void *));
		memset(&norm.outputBitstream, 0, sizeof(void *));
		memset(&norm.completionEvent, 0, sizeof(void *));
		memset(&norm.qpDeltaMap, 0, sizeof(norm.qpDeltaMap));
		hex_dump(t->txt, "NV_ENC_PIC_PARAMS (normalized: pointers zeroed)", &norm, sizeof(norm));
	}
	event_end(t);
}

void nvenc_trace_sequence_params(struct nvenc_data *enc, const uint8_t *data, uint32_t size, NVENCSTATUS status)
{
	struct nvenc_trace_ctx *t = get_ctx(enc);
	if (!t || !data)
		return;

	event_begin(t, "10_SEQUENCE_PARAMS", "NvEncGetSequenceParams");
	uint8_t digest[32];
	sha256_data(data, size, digest);
	fprintf(t->txt, "  status         = %s\n", nv_error_name(status));
	fprintf(t->txt, "  size           = %u bytes\n", size);
	fprintf(t->txt, "  sha256         = ");
	print_sha256(t->txt, digest);
	fputc('\n', t->txt);

	const uint32_t show = (t->verbosity >= 3) ? size : (size < 64 ? size : 64);
	for (uint32_t off = 0; off < show; off += 16) {
		fprintf(t->txt, "  +0x%04X: ", off);
		uint32_t n = size - off;
		if (n > 16)
			n = 16;
		for (uint32_t i = 0; i < 16; i++)
			fprintf(t->txt, "%s%02x", i < n ? "" : " ", i < n ? data[off + i] : 0);
		fputc('\n', t->txt);
	}
	if (show < size)
		fprintf(t->txt, "  ... (%u more bytes, set verbosity=3 for full dump)\n", size - show);

	bin_record(t, nvenc_get_loaded_api_version(), "10_SEQUENCE_PARAMS", data, size);
	event_end(t);
}
