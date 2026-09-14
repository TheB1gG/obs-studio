/******************************************************************************
    Copyright (C) 2026 by OBS Studio contributors

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

#include "graphics/graphics.h"

/* Rank a render color space by fidelity (precision). GS_CS_SRGB and GS_CS_SRGB_16F share the same
 * gamut/transfer and differ only in bit depth, so they can be converted between with a plain draw
 * (see scale-filter.c, which uses a plain "Draw" for these transitions). HDR spaces require
 * tonemapping to reach SDR, so they are not eligible for cross-space reuse and only match exactly. */
static inline int render_space_fidelity(enum gs_color_space space)
{
	switch (space) {
	case GS_CS_SRGB:
		return 0; /* SDR sRGB (8-bit)          */
	case GS_CS_SRGB_16F:
		return 1; /* high-precision SDR sRGB     */
	default:
		return -1; /* HDR / other: exact match only */
	}
}

/* A mix may reuse another mix's already-composited texture when the source is at least as faithful
 * as the destination within the same color-space family (we only ever convert DOWN, e.g. a 16F
 * master feeding an 8-bit encoder-only mix). Exact matches are always allowed. */
static inline bool render_space_reusable(const enum gs_color_space src, const enum gs_color_space dst)
{
	if (src == dst)
		return true;

	const int s = render_space_fidelity(src);
	const int d = render_space_fidelity(dst);
	return s >= 0 && d >= 0 && s > d;
}

/* Which path a mix takes once a reusable source texture has been found:
 *   MIX_REUSE_ALIAS     - Option B: same color space and no scaling needed, so the source texture can
 *                         be aliased directly into the scale/convert pass (no redundant copy).
 *   MIX_REUSE_BLIT_DOWN - Option A: draw the (higher-fidelity) source down into this mix's texture. */
enum mix_reuse_path {
	MIX_REUSE_BLIT_DOWN,
	MIX_REUSE_ALIAS,
};

static inline enum mix_reuse_path get_mix_reuse_path(const enum gs_color_space src_space,
                                                     const enum gs_color_space dst_space,
                                                     const uint32_t out_width, const uint32_t out_height,
                                                     const uint32_t base_width, const uint32_t base_height)
{
	if (src_space == dst_space && out_width == base_width && out_height == base_height)
		return MIX_REUSE_ALIAS;
	return MIX_REUSE_BLIT_DOWN;
}

/* Human-readable name for a render color space. The gs_color_space values are NOT aligned with the
 * video_colorspace enum, so this must not be fed through get_video_colorspace_name(). */
static inline const char *mix_reuse_space_name(const enum gs_color_space space)
{
	switch (space) {
	case GS_CS_SRGB:
		return "SRGB";
	case GS_CS_SRGB_16F:
		return "SRGB_16F";
	case GS_CS_709_EXTENDED:
		return "709_EXTENDED";
	default:
		return "other";
	}
}

/* Human-readable description of which texture-reuse path a mix takes. */
static inline const char *mix_reuse_path_name(const enum mix_reuse_path path)
{
	switch (path) {
	case MIX_REUSE_ALIAS:
		return "Option B (alias source texture)";
	case MIX_REUSE_BLIT_DOWN:
	default:
		return "Option A (blit-down from higher-fidelity composite)";
	}
}
