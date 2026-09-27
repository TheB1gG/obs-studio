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

#include "obs.h"
#include "obs-internal.h"
#include "graphics/vec4.h"

#include <stdio.h>

/* GDI+ text renderer (implemented in obs-debug-overlay-text.cpp on Windows) */
gs_texture_t *obs_debug_overlay_create_text_texture(const char *text);

#ifndef _WIN32
/* Stub for non-Windows platforms — overlay is not available without GDI+ */
gs_texture_t *obs_debug_overlay_create_text_texture(const char *text)
{
	(void)text;
	return NULL;
}
#endif

/* Global flag: whether the debug overlay is enabled.
 * Written from the frontend thread, read from the render thread.
 * Worst case: one-frame delay on enable/disable. */
static bool g_debug_overlay_active = false;
static int g_debug_overlay_corner = 0; /* 0=TL, 1=TR, 2=BL, 3=BR */
static float g_debug_overlay_size_scale = 1.0f;

void obs_debug_overlay_set_active(bool active)
{
	g_debug_overlay_active = active;
}

bool obs_debug_overlay_is_active(void)
{
	return g_debug_overlay_active;
}

void obs_debug_overlay_set_corner(int corner)
{
	g_debug_overlay_corner = corner;
}

void obs_debug_overlay_set_size(int size)
{
	switch (size) {
	case 0: g_debug_overlay_size_scale = 0.15f; break; /* very small */
	case 1: g_debug_overlay_size_scale = 0.25f; break; /* small */
	case 3: g_debug_overlay_size_scale = 0.7f;  break; /* large */
	case 4: g_debug_overlay_size_scale = 1.2f;  break; /* very large */
	default: g_debug_overlay_size_scale = 0.4f; break; /* medium */
	}
}

static void overlay_build_text(struct obs_core_video_mix *mix, char *out, size_t out_size)
{
	uint32_t w = mix->ovi.output_width;
	uint32_t h = mix->ovi.output_height;

	snprintf(out, out_size, "%ux%u", w, h);
}

void obs_debug_overlay_draw(struct obs_core_video_mix *mix)
{
	/* Only draw on encoder-only mixes (per-rendition outputs).
	 * The main canvas mix shows the source resolution which is not useful here. */
	if (!mix->encoder_only_mix)
		return;

	/* Check if we need to (re)create the texture - only 4 int comparisons when unchanged */
	if (!mix->debug_overlay_texture || mix->debug_overlay_width != mix->ovi.output_width ||
	    mix->debug_overlay_height != mix->ovi.output_height ||
	    mix->debug_overlay_fps_num != mix->ovi.fps_num ||
	    mix->debug_overlay_fps_den != mix->ovi.fps_den) {

		if (mix->debug_overlay_texture) {
			gs_texture_destroy(mix->debug_overlay_texture);
			mix->debug_overlay_texture = NULL;
		}

		char text[64];
		overlay_build_text(mix, text, sizeof(text));
		mix->debug_overlay_texture = obs_debug_overlay_create_text_texture(text);
		mix->debug_overlay_width = mix->ovi.output_width;
		mix->debug_overlay_height = mix->ovi.output_height;
		mix->debug_overlay_fps_num = mix->ovi.fps_num;
		mix->debug_overlay_fps_den = mix->ovi.fps_den;
	}

	if (!mix->debug_overlay_texture)
		return;

	uint32_t tex_w = gs_texture_get_width(mix->debug_overlay_texture);
	uint32_t tex_h = gs_texture_get_height(mix->debug_overlay_texture);

	/* Scale based on BASE height (the actual render target size), not output height.
	 * Target: text height ≈ 4% of the render target height, multiplied by user size setting. */
	float scale = ((float)(mix->ovi.base_height * 4) / (float)(tex_h * 100)) * g_debug_overlay_size_scale;

	uint32_t draw_w = (uint32_t)((float)tex_w * scale);
	uint32_t draw_h = (uint32_t)((float)tex_h * scale);
	if (draw_w < 1) draw_w = 1;
	if (draw_h < 1) draw_h = 1;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *param = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(param, mix->debug_overlay_texture);

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	/* Ensure clean rendering state: reset viewport and projection.
	 * In OBS's ortho, Y=0 is at the TOP. */
	gs_set_viewport(0, 0, mix->ovi.base_width, mix->ovi.base_height);
	gs_ortho(0.0f, (float)mix->ovi.base_width, 0.0f, (float)mix->ovi.base_height, -100.0f, 100.0f);

	/* Draw at the selected corner. The texture has padding built in for edge margin. */
	float x = 0.0f, y = 0.0f;
	switch (g_debug_overlay_corner) {
	case 1: x = (float)mix->ovi.base_width - (float)draw_w; break;        /* top-right */
	case 2: y = (float)mix->ovi.base_height - (float)draw_h; break;       /* bottom-left */
	case 3: x = (float)mix->ovi.base_width - (float)draw_w;               /* bottom-right */
	       y = (float)mix->ovi.base_height - (float)draw_h; break;
	default: break;                                                           /* top-left */
	}

	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(mix->debug_overlay_texture, 0, draw_w, draw_h);
	gs_matrix_pop();

	gs_blend_state_pop();
}

void obs_debug_overlay_free(struct obs_core_video_mix *mix)
{
	if (mix->debug_overlay_texture) {
		gs_texture_destroy(mix->debug_overlay_texture);
		mix->debug_overlay_texture = NULL;
	}
	mix->debug_overlay_width = 0;
	mix->debug_overlay_height = 0;
	mix->debug_overlay_fps_num = 0;
	mix->debug_overlay_fps_den = 0;
}
