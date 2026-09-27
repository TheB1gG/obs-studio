/******************************************************************************
    Copyright (C) 2025 by OBS Studio

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

/* GDI+ text rendering helper for the debug overlay.
 * Exposes a C-linkage function so it can be called from obs-debug-overlay.c */

#include "obs.h"

#include <combaseapi.h>
#include <gdiplus.h>
#include <cstring>
#include <string>
#include <vector>

using namespace Gdiplus;

static ULONG_PTR g_gdip_token = 0;

static void ensure_gdip()
{
	if (!g_gdip_token) {
		GdiplusStartupInput input;
		GdiplusStartup(&g_gdip_token, &input, nullptr);
	}
}

extern "C" gs_texture_t *obs_debug_overlay_create_text_texture(const char *text)
{
	ensure_gdip();

	/* Convert ASCII to wide string */
	std::wstring wtext;
	for (const char *p = text; *p; p++)
		wtext += (wchar_t)(*p);

	const float font_size = 256.0f;

	FontFamily family(L"Segoe UI");
	Font font(&family, font_size, FontStyleBold, UnitPixel);

	/* Measure text to determine initial bitmap size (generous) */
	uint8_t tmp_pixel[4] = {0};
	Bitmap tmp_bitmap(1, 1, 4, PixelFormat32bppARGB, tmp_pixel);
	Graphics g_measure(&tmp_bitmap);

	StringFormat fmt;
	fmt.SetAlignment(StringAlignmentNear);
	fmt.SetLineAlignment(StringAlignmentNear);

	RectF layout_rect(0, 0, 4096, 512);
	RectF bounds;
	g_measure.MeasureString(wtext.c_str(), (int)wtext.size(), &font, layout_rect, &fmt, &bounds);

	int tex_w = (int)(bounds.Width * 1.2f) + 64;
	int tex_h = (int)(bounds.Height * 1.2f) + 64;

	if (tex_w < 32)
		tex_w = 32;
	if (tex_h < 32)
		tex_h = 32;
	if (tex_w > 4096)
		tex_w = 4096;
	if (tex_h > 512)
		tex_h = 512;

	/* Allocate pixel buffer */
	std::vector<uint8_t> pixels(tex_w * tex_h * 4, 0);

	/* Create GDI+ bitmap over our buffer and render */
	Bitmap bitmap(tex_w, tex_h, 4 * tex_w, PixelFormat32bppARGB, pixels.data());
	Graphics gfx(&bitmap);

	/* Clear to fully transparent */
	gfx.Clear(Color(0));

	/* Draw text with thick black outline + white fill */
	gfx.SetTextRenderingHint(TextRenderingHintAntiAlias);
	gfx.SetSmoothingMode(SmoothingModeAntiAlias);

	RectF text_rect(32, 32, (float)(tex_w - 64), (float)(tex_h - 64));

	/* Outline: draw the text as a path with a thick pen */
	FontFamily family_ref;
	font.GetFamily(&family_ref);
	GraphicsPath path;
	path.AddString(wtext.c_str(), (int)wtext.size(), &family_ref, font.GetStyle(),
	               font.GetSize(), text_rect, &fmt);

	Pen outline_pen(Color(255, 0, 0, 0), 20.0f); /* thick black outline */
	outline_pen.SetLineJoin(LineJoinRound);
	gfx.DrawPath(&outline_pen, &path);

	/* Fill: draw white text on top */
	SolidBrush text_brush(Color(255, 255, 255, 255));
	gfx.FillPath(&text_brush, &path);

	/* Scan for non-transparent pixels to find tight content bounds */
	int min_x = tex_w, max_x = 0, min_y = tex_h, max_y = 0;
	for (int y = 0; y < tex_h; y++) {
		for (int x = 0; x < tex_w; x++) {
			uint32_t pixel = *(uint32_t *)&pixels[(y * tex_w + x) * 4];
			uint8_t alpha = (pixel >> 24) & 0xFF; /* ARGB format */
			if (alpha > 0) {
				if (x < min_x) min_x = x;
				if (x > max_x) max_x = x;
				if (y < min_y) min_y = y;
				if (y > max_y) max_y = y;
			}
		}
	}

	/* If nothing was drawn, return a 1x1 transparent texture */
	if (min_x > max_x || min_y > max_y) {
		uint8_t clear_pixel[4] = {0};
		obs_enter_graphics();
		const uint8_t *levels[] = {clear_pixel};
		gs_texture_t *tex = gs_texture_create(1, 1, GS_BGRA, 1, levels, GS_DYNAMIC);
		obs_leave_graphics();
		return tex;
	}

	/* Crop to content bounds (the outline extends the visible area) */
	int crop_w = max_x - min_x + 1;
	int crop_h = max_y - min_y + 1;

	/* Extract cropped pixels into a new tightly-fitted buffer */
	std::vector<uint8_t> cropped(crop_w * crop_h * 4, 0);
	for (int y = 0; y < crop_h; y++) {
		uint8_t *src_row = &pixels[((min_y + y) * tex_w + min_x) * 4];
		uint8_t *dst_row = &cropped[y * crop_w * 4];
		memcpy(dst_row, src_row, crop_w * 4);
	}

	/* Upload to GPU */
	const uint8_t *levels[] = { cropped.data() };
	obs_enter_graphics();
	gs_texture_t *tex = gs_texture_create(crop_w, crop_h, GS_BGRA, 1, levels, GS_DYNAMIC);
	obs_leave_graphics();

	return tex;
}
