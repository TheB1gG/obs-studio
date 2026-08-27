/******************************************************************************
    Copyright (C) 2024 by Dennis Sädtler <dennis@obsproject.com>

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

#pragma once

#include "mp4-mux.h"

#include <util/darray.h>
#include <util/deque.h>
#include <util/serializer.h>

/* Flavour for target compatibility */
enum mp4_flavour {
	MP4,  /* ISO/IEC 14496-12 */
	MOV,  /* Apple QuickTime */
	CMAF, /* ISO/IEC 23000-19 */
};

enum mp4_track_type {
	TRACK_UNKNOWN,
	TRACK_VIDEO,
	TRACK_AUDIO,
	TRACK_CHAPTERS,
};

enum mp4_codec {
	CODEC_UNKNOWN,

	/* Video Codecs */
	CODEC_H264,
	CODEC_HEVC,
	CODEC_AV1,

	/* Audio Codecs */
	CODEC_AAC,
	CODEC_OPUS,
	CODEC_FLAC,
	CODEC_ALAC,
	CODEC_PCM_I16,
	CODEC_PCM_I24,
	CODEC_PCM_F32,

	/* Text/Chapter trakcs */
	CODEC_TEXT,
};

struct chunk {
	uint64_t offset;
	uint32_t size;
	uint32_t samples;
};

struct sample_delta {
	uint32_t count;
	uint32_t delta;
};

struct sample_offset {
	uint32_t count;
	int32_t offset;
};

struct fragment_sample {
	uint32_t size;
	int32_t offset;
	uint32_t duration;
};

struct mp4_track {
	enum mp4_track_type type;
	enum mp4_codec codec;

	/* Track ID in container */
	uint8_t track_id;
	/* Number of samples for this track  */
	uint64_t samples;
	/* Duration for this track  */
	uint64_t duration;

	/* Encoder associated with this track */
	obs_encoder_t *encoder;

	/* Time Base (1/FPS for video, 1/sample rate for audio) */
	uint32_t timebase_num;
	uint32_t timebase_den;
	/* Output timescale calculated from time base (Video only) */
	uint32_t timescale;

	/* First PTS this track has seen (in track timescale) */
	int64_t first_pts;
	/* Highest PTS this track has seen (in usec) */
	int64_t last_pts_usec;

	/* deque of encoder_packet belonging to this track */
	struct deque packets;

	/* Sample sizes (fixed for PCM) */
	uint32_t sample_size;
	DARRAY(uint32_t) sample_sizes;
	/* Data chunks in file containing samples for this track */
	DARRAY(struct chunk) chunks;
	/* Time delta between samples */
	DARRAY(struct sample_delta) deltas;

	/* Sample CT-DT offset, i.e. DTS-PTS offset (Video only) */
	bool needs_ctts;
	int32_t dts_offset;
	DARRAY(struct sample_offset) offsets;
	/* Sync samples, i.e. keyframes (Video only) */
	DARRAY(uint32_t) sync_samples;

	/* Temporary array with information about the samples to be included
	 * in the next fragment. */
	DARRAY(struct fragment_sample) fragment_samples;
};

struct mp4_mux {
	obs_output_t *output;
	struct serializer *serializer;

	/* Target format compatibility */
	enum mp4_flavour mode;

	/* Flags */
	enum mp4_mux_flags flags;

	uint32_t fragments_written;
	/* PTS where next fragmentation should take place */
	int64_t next_frag_pts;

	/* Creation time (seconds since Jan 1 1904) */
	uint64_t creation_time;

	/* Offset of placeholder atom/box to contain final mdat header */
	size_t placeholder_offset;

	uint8_t track_ctr;
	/* Audio/Video tracks */
	DARRAY(struct mp4_track) tracks;
	/* Special tracks */
	struct mp4_track *chapter_track;
};

/* clang-format off */
// Defined in ISO/IEC 14496-12:2015 Section 8.2.2.1
const int32_t UNITY_MATRIX[9] = {
	0x00010000,	0,		0,
	0,		0x00010000,	0,
	0,		0,		0x40000000
};
/* clang-format on */

enum tfhd_flags {
	BASE_DATA_OFFSET_PRESENT = 0x000001,
	SAMPLE_DESCRIPTION_INDEX_PRESENT = 0x000002,
	DEFAULT_SAMPLE_DURATION_PRESENT = 0x000008,
	DEFAULT_SAMPLE_SIZE_PRESENT = 0x000010,
	DEFAULT_SAMPLE_FLAGS_PRESENT = 0x000020,
	DURATION_IS_EMPTY = 0x010000,
	DEFAULT_BASE_IS_MOOF = 0x020000,
};

enum trun_flags {
	DATA_OFFSET_PRESENT = 0x000001,
	FIRST_SAMPLE_FLAGS_PRESENT = 0x000004,
	SAMPLE_DURATION_PRESENT = 0x000100,
	SAMPLE_SIZE_PRESENT = 0x000200,
	SAMPLE_FLAGS_PRESENT = 0x000400,
	SAMPLE_COMPOSITION_TIME_OFFSETS_PRESENT = 0x000800,
};

/*
 * ISO Standard structure (big endian so we can't easily use it):
 *
 * struct sample_flags {
 * 	uint32_t reserved : 4;
 * 	uint32_t is_leading : 2;
 *	uint32_t sample_depends_on : 2;
 *	uint32_t sample_is_depended_on : 2;
 *	uint32_t sample_has_redundancy : 2;
 *	uint32_t sample_padding_value : 3;
 *	uint32_t sample_is_non_sync_sample : 1;
 *	uint32_t sample_degradation_priority : 16;
};
*/

enum sample_flags {
	SAMPLE_FLAG_IS_NON_SYNC = 0x00010000,
	SAMPLE_FLAG_DEPENDS_YES = 0x01000000,
	SAMPLE_FLAG_DEPENDS_NO = 0x02000000,
};

#ifndef _WIN32
static inline size_t min(size_t a, size_t b)
{
	return a < b ? a : b;
}
#endif

static inline void get_speaker_positions(enum speaker_layout layout, uint8_t *arr, uint8_t *size, uint8_t *iso_layout)
{
	switch (layout) {
	case SPEAKERS_MONO:
		arr[0] = 2; // FC
		*size = 1;
		*iso_layout = 1;
		break;
	case SPEAKERS_UNKNOWN:
	case SPEAKERS_STEREO:
		arr[0] = 0; // FL
		arr[1] = 1; // FR
		*size = 2;
		*iso_layout = 2;
		break;
	case SPEAKERS_2POINT1:
		arr[0] = 0; // FL
		arr[1] = 1; // FR
		arr[2] = 3; // LFE
		*size = 3;
		break;
	case SPEAKERS_4POINT0:
		arr[0] = 0;  // FL
		arr[1] = 1;  // FR
		arr[2] = 2;  // FC
		arr[3] = 10; // RC
		*size = 4;
		*iso_layout = 4;
		break;
	case SPEAKERS_4POINT1:
		arr[0] = 0;  // FL
		arr[1] = 1;  // FR
		arr[2] = 2;  // FC
		arr[3] = 3;  // LFE
		arr[4] = 10; // RC
		*size = 5;
		break;
	case SPEAKERS_5POINT1:
		arr[0] = 0; // FL
		arr[1] = 1; // FR
		arr[2] = 2; // FC
		arr[3] = 3; // LFE
		arr[4] = 8; // RL
		arr[5] = 9; // RR
		*size = 6;
		break;
	case SPEAKERS_7POINT1:
		arr[0] = 0;  // FL
		arr[1] = 1;  // FR
		arr[2] = 2;  // FC
		arr[3] = 3;  // LFE
		arr[4] = 8;  // RL
		arr[5] = 9;  // RR
		arr[6] = 13; // SL
		arr[7] = 14; // SR
		*size = 8;
		*iso_layout = 12;
		break;
	}
}

/* Maps a per-encoder colorspace (published via obs_encoder_set_preferred_color_space)
 * to ITU-T T.3511 nclx values, mirroring what x264 writes into its SPS VUI so the colr
 * box always matches the stream's actual chroma signalling. */
static inline void preferred_colour_values(enum video_colorspace cs, bool is_rgb_family, uint16_t *pri, uint16_t *trc,
					    uint16_t *spc)
{
	if (is_rgb_family) {
		/* x264 stores RGB-family input (BGRA 8-bit and R10I/R10L 10-bit) as raw RGB
		 * components with an identity matrix and full range. There is no YUV transform
		 * for that data, so primaries/transfer are unspecified (0/0); only spc=0 +
		 * full_range are meaningful nclx fields. */
		*pri = 0; // OBSCOL_PRI_UNSPECIFIED
		*trc = 0; // OBSCOL_TRC_UNSPECIFIED
		*spc = 0; // identity matrix (GBR), matching the SPS col_matrix_coef=0
		return;
	}

	switch (cs) {
	case VIDEO_CS_601:
		*pri = 6; // OBSCOL_PRI_SMPTE170M
		*trc = 6;
		*spc = 6;
		break;
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_709:
		*pri = 1; // OBSCOL_PRI_BT709
		*trc = 1;
		*spc = 1;
		break;
	case VIDEO_CS_SRGB:
		*pri = 1;  // OBSCOL_PRI_BT709
		*trc = 13; // OBSCOL_TRC_IEC61966_2_1
		*spc = 1;  // OBSCOL_PRI_BT709
		break;
	case VIDEO_CS_2100_PQ:
		*pri = 9;  // OBSCOL_PRI_BT2020
		*trc = 16; // OBSCOL_TRC_SMPTE2084
		*spc = 9;  // OBSCOL_SPC_BT2020_NCL
		break;
	case VIDEO_CS_2100_HLG:
		*pri = 9;   // OBSCOL_PRI_BT2020
		*trc = 18;  // OBSCOL_TRC_ARIB_STD_B67
		*spc = 9;   // OBSCOL_SPC_BT2020_NCL
		break;
	default: /* unexpected enum: x264 leaves its VUI at bt.709 */
		*pri = 1;
		*trc = 1;
		*spc = 1;
		break;
	}
}

static inline void get_colour_information(obs_encoder_t *enc, uint16_t *pri, uint16_t *trc, uint16_t *spc,
					  uint8_t *full_range)
{
	video_t *video = obs_encoder_video(enc);
	const struct video_output_info *info = video_output_get_info(video);

	/* Prefer the encoder's published color settings (set by encoders that encode in a
	 * fixed format/space/range independent of the base video info, e.g. x264). This keeps
	 * the nclx box consistent with the stream's SPS VUI. */
	enum video_format fmt = obs_encoder_get_preferred_video_format(enc);
	enum video_colorspace cs = obs_encoder_get_preferred_color_space(enc);
	enum video_range_type rg = obs_encoder_get_preferred_range(enc);

	if (fmt != VIDEO_FORMAT_NONE || cs != VIDEO_CS_DEFAULT) {
		/* Raw RGB components (identity matrix) - same nclx signalling as the BGRA fix. */
		bool is_rgb_family =
			(fmt == VIDEO_FORMAT_BGRA) || (fmt == VIDEO_FORMAT_R10L) || (fmt == VIDEO_FORMAT_R10P);
		preferred_colour_values(cs, is_rgb_family, pri, trc, spc);
		bool full = is_rgb_family || rg == VIDEO_RANGE_FULL;
		*full_range = full ? 1 : 0;
		return;
	}

	/* No per-encoder override: fall back to the base video info */
	*full_range = info->range == VIDEO_RANGE_FULL ? 1 : 0;

	switch (info->colorspace) {
	case VIDEO_CS_601:
		*pri = 6; // OBSCOL_PRI_SMPTE170M
		*trc = 6;
		*spc = 6;
		break;
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_709:
		*pri = 1; // OBSCOL_PRI_BT709
		*trc = 1;
		*spc = 1;
		break;
	case VIDEO_CS_SRGB:
		*pri = 1;  // OBSCOL_PRI_BT709
		*trc = 13; // OBSCOL_TRC_IEC61966_2_1
		*spc = 1;  // OBSCOL_PRI_BT709
		break;
	case VIDEO_CS_2100_PQ:
		*pri = 9;  // OBSCOL_PRI_BT2020
		*trc = 16; // OBSCOL_TRC_SMPTE2084
		*spc = 9;  // OBSCOL_SPC_BT2020_NCL
		break;
	case VIDEO_CS_2100_HLG:
		*pri = 9;  // OBSCOL_PRI_BT2020
		*trc = 18; // OBSCOL_TRC_ARIB_STD_B67
		*spc = 9;  // OBSCOL_SPC_BT2020_NCL
	}
}

/* Chapter stubs (from libavformat/movenc.c) */

static const uint8_t TEXT_STUB_HEADER[] = {
	// TextSampleEntry
	0x00, 0x00, 0x00, 0x01, // displayFlags
	0x00, 0x00,             // horizontal + vertical justification
	0x00, 0x00, 0x00, 0x00, // bgColourRed/Green/Blue/Alpha
	// BoxRecord
	0x00, 0x00, 0x00, 0x00, // defTextBoxTop/Left
	0x00, 0x00, 0x00, 0x00, // defTextBoxBottom/Right
	// StyleRecord
	0x00, 0x00, 0x00, 0x00, // startChar + endChar
	0x00, 0x01,             // fontID
	0x00, 0x00,             // fontStyleFlags + fontSize
	0x00, 0x00, 0x00, 0x00, // fgColourRed/Green/Blue/Alpha
	// FontTableBox
	0x00, 0x00, 0x00, 0x0D, // box size
	'f', 't', 'a', 'b',     // box atom name
	0x00, 0x01,             // entry count
	// FontRecord
	0x00, 0x01, // font ID
	0x00,       // font name length
};

/* clang-format off */
static const char CHAPTER_PKT_FOOTER[12] = {
	0x00, 0x00, 0x00, 0x0C,
	'e',  'n',  'c',  'd',
	0x00, 0x00, 0x01, 0x00
};
/* clang-format on */
