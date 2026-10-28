#include "nvenc-internal.h"
#include "nvenc-helpers.h"

/*
 * NVENC implementation using CUDA context and arrays
 */

/* ------------------------------------------------------------------------- */
/* CUDA Context management                                                   */

bool cuda_ctx_init(struct nvenc_data *enc, obs_data_t *settings, const bool texture)
{
#ifdef _WIN32
	if (texture)
		return true;
#endif

	int count;
	CUdevice device;

	int gpu = (int)obs_data_get_int(settings, "device");
#ifndef _WIN32
	/* CUDA can do fairly efficient cross-GPU OpenGL mappings, allow it as
	 * a hidden option for experimentation. */
	bool force_cuda_tex = obs_data_get_bool(settings, "force_cuda_tex");
#endif

	if (gpu == -1)
		gpu = 0;

	CU_FAILED(cu->cuInit(0))
	CU_FAILED(cu->cuDeviceGetCount(&count))
	if (!count) {
		NV_FAIL("No CUDA devices found");
		return false;
	}
#ifdef _WIN32
	CU_FAILED(cu->cuDeviceGet(&device, gpu))
#else
	if (!texture || force_cuda_tex) {
		CU_FAILED(cu->cuDeviceGet(&device, gpu))
	} else {
		unsigned int ctx_count = 0;
		CUdevice devices[2];

		obs_enter_graphics();
		CUresult res = cu->cuGLGetDevices(&ctx_count, devices, 2, CU_GL_DEVICE_LIST_ALL);
		obs_leave_graphics();

		if (res != CUDA_SUCCESS || !ctx_count) {
			/* Probably running on iGPU, should just fall back to
			 * non-texture encoder. */
			if (res == CUDA_ERROR_INVALID_GRAPHICS_CONTEXT) {
				info("Not running on NVIDIA GPU, falling back "
				     "to non-texture encoder");
			} else {
				const char *name, *desc;
				if (cuda_get_error_desc(res, &name, &desc)) {
					error("Failed to get a CUDA device for "
					      "the current OpenGL context: "
					      "%s: %s",
					      name, desc);
				} else {
					error("Failed to get a CUDA device for "
					      "the current OpenGL context: %d",
					      res);
				}
			}
			return false;
		}

		/* Documentation indicates this should only ever happen with
		 * SLI, i.e. never for OBS. */
		if (ctx_count > 1) {
			warn("Got more than one CUDA devices for OpenGL context,"
			     " this is untested.");
		}

		device = devices[0];
		debug("Loading up CUDA on device %u", device);
	}
#endif
	CU_FAILED(cu->cuCtxCreate(&enc->cu_ctx, 0, device))
	CU_FAILED(cu->cuCtxPopCurrent(NULL))

	return true;
}

void cuda_ctx_free(struct nvenc_data *enc)
{
	if (enc->cu_ctx) {
		cu->cuCtxPopCurrent(NULL);
		cu->cuCtxDestroy(enc->cu_ctx);
	}
}

/* ------------------------------------------------------------------------- */
/* CUDA Surface management                                                   */

static bool cuda_surface_init(struct nvenc_data *enc, struct nv_cuda_surface *nvsurf)
{
	CUDA_ARRAY3D_DESCRIPTOR desc;
	desc.Width = enc->cx;
	desc.Height = enc->cy;
	desc.Depth = 0;
	desc.Flags = CUDA_ARRAY3D_SURFACE_LDST;
	desc.NumChannels = 1;

	switch (enc->surface_format) {
	case NV_ENC_BUFFER_FORMAT_NV12:
		desc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
		// Additional half-height plane for UV data
		desc.Height += enc->cy / 2;
		break;
	case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
		desc.Format = CU_AD_FORMAT_UNSIGNED_INT16; // 2 bytes per element
		desc.Height += enc->cy / 2;
		break;
	case NV_ENC_BUFFER_FORMAT_YUV444:
		desc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
		desc.Height *= 3; // 3 full-size planes
		break;
	case NV_ENC_BUFFER_FORMAT_AYUV:
		desc.Format = CU_AD_FORMAT_UNSIGNED_INT32;
		break;
	default:
		error("Unknown input format: %d", enc->surface_format);
		return false;
	}

	CU_FAILED(cu->cuArray3DCreate(&nvsurf->tex, &desc))

	NV_ENC_REGISTER_RESOURCE res = {0};
	res.version = NV_ENC_REGISTER_RESOURCE_VER;
	res.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY;
	res.resourceToRegister = (void *)nvsurf->tex;
	res.width = enc->cx;
	res.height = enc->cy;
	res.pitch = (uint32_t)(desc.Width * desc.NumChannels);
	res.bufferFormat = enc->surface_format;

	if (NV_FAILED(nv.nvEncRegisterResource(enc->session, &res))) {
		return false;
	}

	nvsurf->res = res.registeredResource;
	nvsurf->mapped_res = NULL;
	return true;
}

bool cuda_init_surfaces(struct nvenc_data *enc)
{
	if (enc->non_texture) {
		switch (enc->in_format) {
		case VIDEO_FORMAT_P010:
			enc->surface_format = NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
			break;
		case VIDEO_FORMAT_I444:
			enc->surface_format = NV_ENC_BUFFER_FORMAT_YUV444;
			break;
		default:
			enc->surface_format = NV_ENC_BUFFER_FORMAT_NV12;
		}
	} else {
		if (obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_P010)) {
			enc->surface_format = NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
		} else if (obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_GBRA) ||
			   obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_AYUV)) {
			enc->surface_format = NV_ENC_BUFFER_FORMAT_AYUV;
		} else {
			enc->surface_format = NV_ENC_BUFFER_FORMAT_NV12;
		}
	}

	da_reserve(enc->surfaces, enc->buf_count);

	CU_FAILED(cu->cuCtxPushCurrent(enc->cu_ctx))
	for (uint32_t i = 0; i < enc->buf_count; i++) {
		struct nv_cuda_surface buf;
		if (!cuda_surface_init(enc, &buf)) {
			return false;
		}

		da_push_back(enc->surfaces, &buf);
	}
	CU_FAILED(cu->cuCtxPopCurrent(NULL))

	return true;
}

static void cuda_surface_free(struct nvenc_data *enc, struct nv_cuda_surface *nvsurf)
{
	if (nvsurf->res) {
		if (nvsurf->mapped_res) {
			nv.nvEncUnmapInputResource(enc->session, nvsurf->mapped_res);
		}
		nv.nvEncUnregisterResource(enc->session, nvsurf->res);
		cu->cuArrayDestroy(nvsurf->tex);
	}
}

void cuda_free_surfaces(struct nvenc_data *enc)
{
	if (!enc->cu_ctx)
		return;

	cu->cuCtxPushCurrent(enc->cu_ctx);
	for (size_t i = 0; i < enc->surfaces.num; i++) {
		cuda_surface_free(enc, &enc->surfaces.array[i]);
	}
	cu->cuCtxPopCurrent(NULL);
}

/* ------------------------------------------------------------------------- */
/* Actual encoding stuff                                                     */

static inline bool copy_frame(struct nvenc_data *enc, struct encoder_frame *frame, struct nv_cuda_surface *surf)
{
	bool success = true;
	size_t height = enc->cy;
	size_t width = enc->cx;
	CUDA_MEMCPY2D m = {0};

	m.srcMemoryType = CU_MEMORYTYPE_HOST;
	m.dstMemoryType = CU_MEMORYTYPE_ARRAY;
	m.dstArray = surf->tex;
	m.WidthInBytes = width;
	m.Height = height;

	CU_FAILED(cu->cuCtxPushCurrent(enc->cu_ctx))

	if (enc->surface_format == NV_ENC_BUFFER_FORMAT_NV12) {
		/* Page-locks the host memory so that it can be DMAd directly
		 * rather than CUDA doing an internal copy to page-locked
		 * memory before actually DMA-ing to the GPU. */
		CU_CHECK(cu->cuMemHostRegister(frame->data[0], frame->linesize[0] * height, 0))
		CU_CHECK(cu->cuMemHostRegister(frame->data[1], frame->linesize[1] * height / 2, 0))

		m.srcPitch = frame->linesize[0];
		m.srcHost = frame->data[0];
		CU_FAILED(cu->cuMemcpy2D(&m))

		m.srcPitch = frame->linesize[1];
		m.srcHost = frame->data[1];
		m.dstY += height;
		m.Height /= 2;
		CU_FAILED(cu->cuMemcpy2D(&m))
	} else if (enc->surface_format == NV_ENC_BUFFER_FORMAT_YUV420_10BIT) {
		CU_CHECK(cu->cuMemHostRegister(frame->data[0], frame->linesize[0] * height, 0))
		CU_CHECK(cu->cuMemHostRegister(frame->data[1], frame->linesize[1] * height / 2, 0))

		// P010 lines are double the size (16 bit per pixel)
		m.WidthInBytes *= 2;

		m.srcPitch = frame->linesize[0];
		m.srcHost = frame->data[0];
		CU_FAILED(cu->cuMemcpy2D(&m))

		m.srcPitch = frame->linesize[1];
		m.srcHost = frame->data[1];
		m.dstY += height;
		m.Height /= 2;
		CU_FAILED(cu->cuMemcpy2D(&m))
	} else { // I444
		CU_CHECK(cu->cuMemHostRegister(frame->data[0], frame->linesize[0] * height, 0))
		CU_CHECK(cu->cuMemHostRegister(frame->data[1], frame->linesize[1] * height, 0))
		CU_CHECK(cu->cuMemHostRegister(frame->data[2], frame->linesize[2] * height, 0))

		m.srcPitch = frame->linesize[0];
		m.srcHost = frame->data[0];
		CU_FAILED(cu->cuMemcpy2D(&m))

		m.srcPitch = frame->linesize[1];
		m.srcHost = frame->data[1];
		m.dstY += height;
		CU_FAILED(cu->cuMemcpy2D(&m))

		m.srcPitch = frame->linesize[2];
		m.srcHost = frame->data[2];
		m.dstY += height;
		CU_FAILED(cu->cuMemcpy2D(&m))
	}

unmap:
	if (frame->data[0])
		cu->cuMemHostUnregister(frame->data[0]);
	if (frame->data[1])
		cu->cuMemHostUnregister(frame->data[1]);
	if (frame->data[2])
		cu->cuMemHostUnregister(frame->data[2]);

	CU_FAILED(cu->cuCtxPopCurrent(NULL))

	return success;
}

/* Bilinear-scale an NV12 frame from the source (new) size into destination buffers of the
 * current session (old) size. Used while a staged multitrack resize waits for the shared
 * keyframe boundary, so every frame is encoded with continuous DTS (mirrors obs-x264).
 * Shared with the D3D11 CPU readback scaler (see nvenc-internal.h). */
void nvenc_scale_nv12(const uint8_t *src_y, const uint8_t *src_uv, int src_w, int src_h, int src_ls0,
		      int src_ls1, uint8_t *dst_y, uint8_t *dst_uv, int dst_w, int dst_h, int dst_ls0,
		      int dst_ls1)
{
	const double x_ratio = (double)src_w / dst_w;
	const double y_ratio = (double)src_h / dst_h;

	for (int y = 0; y < dst_h; y++) {
		double sy = (y + 0.5) * y_ratio - 0.5;
		if (sy < 0)
			sy = 0;
		const int y0 = (int)sy;
		const int y1 = (y0 + 1 < src_h) ? y0 + 1 : y0;
		const double fy = sy - y0;
		const uint8_t *s0 = src_y + (size_t)y0 * src_ls0;
		const uint8_t *s1 = src_y + (size_t)y1 * src_ls0;
		uint8_t *d = dst_y + (size_t)y * dst_ls0;
		for (int x = 0; x < dst_w; x++) {
			double sx = (x + 0.5) * x_ratio - 0.5;
			if (sx < 0)
				sx = 0;
			const int x0 = (int)sx;
			const int x1 = (x0 + 1 < src_w) ? x0 + 1 : x0;
			const double fx = sx - x0;
			const double top = s0[x0] * (1.0 - fx) + s0[x1] * fx;
			const double bot = s1[x0] * (1.0 - fx) + s1[x1] * fx;
			d[x] = (uint8_t)(top * (1.0 - fy) + bot * fy + 0.5);
		}
	}

	/* UV plane: half resolution, interleaved U/V pairs */
	const int src_uv_w = src_w / 2, src_uv_h = src_h / 2;
	const int dst_uv_w = dst_w / 2, dst_uv_h = dst_h / 2;
	const double ux_ratio = (double)src_uv_w / dst_uv_w;
	const double uy_ratio = (double)src_uv_h / dst_uv_h;
	for (int y = 0; y < dst_uv_h; y++) {
		double sy = (y + 0.5) * uy_ratio - 0.5;
		if (sy < 0)
			sy = 0;
		const int y0 = (int)sy;
		const int y1 = (y0 + 1 < src_uv_h) ? y0 + 1 : y0;
		const double fy = sy - y0;
		for (int x = 0; x < dst_uv_w; x++) {
			double sx = (x + 0.5) * ux_ratio - 0.5;
			if (sx < 0)
				sx = 0;
			const int x0 = (int)sx;
			const int x1 = (x0 + 1 < src_uv_w) ? x0 + 1 : x0;
			const double fx = sx - x0;
			for (int c = 0; c < 2; c++) {
				const size_t off = (size_t)y * dst_ls1 + x * 2 + c;
				const double top = src_uv[(size_t)y0 * src_ls1 + x0 * 2 + c] * (1.0 - fx) +
						   src_uv[(size_t)y0 * src_ls1 + x1 * 2 + c] * fx;
				const double bot = src_uv[(size_t)y1 * src_ls1 + x0 * 2 + c] * (1.0 - fx) +
						   src_uv[(size_t)y1 * src_ls1 + x1 * 2 + c] * fx;
				dst_uv[off] = (uint8_t)(top * (1.0 - fy) + bot * fy + 0.5);
			}
		}
	}
}

/* Bilinear-scale a P010 frame (u16 Y + interleaved u16 UV) from the source (new) size into
 * destination buffers of the current session (old) size. Strides are in bytes. Used while a
 * staged multitrack resize waits for the shared keyframe boundary. Shared with the D3D11
 * CPU readback scaler (see nvenc-internal.h). */
void nvenc_scale_p010(const uint8_t *src_y, const uint8_t *src_uv, int src_w, int src_h, int src_ls0,
		      int src_ls1, uint8_t *dst_y, uint8_t *dst_uv, int dst_w, int dst_h, int dst_ls0,
		      int dst_ls1)
{
	const double x_ratio = (double)src_w / dst_w;
	const double y_ratio = (double)src_h / dst_h;

	for (int y = 0; y < dst_h; y++) {
		double sy = (y + 0.5) * y_ratio - 0.5;
		if (sy < 0)
			sy = 0;
		const int y0 = (int)sy;
		const int y1 = (y0 + 1 < src_h) ? y0 + 1 : y0;
		const double fy = sy - y0;
		const uint16_t *s0 = (const uint16_t *)(src_y + (size_t)y0 * src_ls0);
		const uint16_t *s1 = (const uint16_t *)(src_y + (size_t)y1 * src_ls0);
		uint16_t *d = (uint16_t *)(dst_y + (size_t)y * dst_ls0);
		for (int x = 0; x < dst_w; x++) {
			double sx = (x + 0.5) * x_ratio - 0.5;
			if (sx < 0)
				sx = 0;
			const int x0 = (int)sx;
			const int x1 = (x0 + 1 < src_w) ? x0 + 1 : x0;
			const double fx = sx - x0;
			const double top = s0[x0] * (1.0 - fx) + s0[x1] * fx;
			const double bot = s1[x0] * (1.0 - fx) + s1[x1] * fx;
			d[x] = (uint16_t)(top * (1.0 - fy) + bot * fy + 0.5);
		}
	}

	/* UV plane: half resolution, interleaved U/V u16 pairs */
	const int src_uv_w = src_w / 2, src_uv_h = src_h / 2;
	const int dst_uv_w = dst_w / 2, dst_uv_h = dst_h / 2;
	const double ux_ratio = (double)src_uv_w / dst_uv_w;
	const double uy_ratio = (double)src_uv_h / dst_uv_h;
	for (int y = 0; y < dst_uv_h; y++) {
		double sy = (y + 0.5) * uy_ratio - 0.5;
		if (sy < 0)
			sy = 0;
		const int y0 = (int)sy;
		const int y1 = (y0 + 1 < src_uv_h) ? y0 + 1 : y0;
		const double fy = sy - y0;
		for (int x = 0; x < dst_uv_w; x++) {
			double sx = (x + 0.5) * ux_ratio - 0.5;
			if (sx < 0)
				sx = 0;
			const int x0 = (int)sx;
			const int x1 = (x0 + 1 < src_uv_w) ? x0 + 1 : x0;
			const double fx = sx - x0;
			for (int c = 0; c < 2; c++) {
				const size_t off = (size_t)y * (dst_ls1 / 2) + x * 2 + c;
				const uint16_t *u0 = (const uint16_t *)(src_uv + (size_t)y0 * src_ls1);
				const uint16_t *u1 = (const uint16_t *)(src_uv + (size_t)y1 * src_ls1);
				const double top = u0[x0 * 2 + c] * (1.0 - fx) + u0[x1 * 2 + c] * fx;
				const double bot = u1[x0 * 2 + c] * (1.0 - fx) + u1[x1 * 2 + c] * fx;
				((uint16_t *)dst_uv)[off] = (uint16_t)(top * (1.0 - fy) + bot * fy + 0.5);
			}
		}
	}
}

/* Bilinear-scale an I444 frame (three full-resolution u8 planes) from the source (new) size
 * into destination buffers of the current session (old) size. Strides are in bytes. */
static void nvenc_scale_i444(const uint8_t *src[3], int src_w, int src_h, const int src_ls[3], uint8_t *dst[3],
			     int dst_w, int dst_h, const int dst_ls[3])
{
	const double x_ratio = (double)src_w / dst_w;
	const double y_ratio = (double)src_h / dst_h;

	for (int plane = 0; plane < 3; plane++) {
		for (int y = 0; y < dst_h; y++) {
			double sy = (y + 0.5) * y_ratio - 0.5;
			if (sy < 0)
				sy = 0;
			const int y0 = (int)sy;
			const int y1 = (y0 + 1 < src_h) ? y0 + 1 : y0;
			const double fy = sy - y0;
			const uint8_t *s0 = src[plane] + (size_t)y0 * src_ls[plane];
			const uint8_t *s1 = src[plane] + (size_t)y1 * src_ls[plane];
			uint8_t *d = dst[plane] + (size_t)y * dst_ls[plane];
			for (int x = 0; x < dst_w; x++) {
				double sx = (x + 0.5) * x_ratio - 0.5;
				if (sx < 0)
					sx = 0;
				const int x0 = (int)sx;
				const int x1 = (x0 + 1 < src_w) ? x0 + 1 : x0;
				const double fx = sx - x0;
				const double top = s0[x0] * (1.0 - fx) + s0[x1] * fx;
				const double bot = s1[x0] * (1.0 - fx) + s1[x1] * fx;
				d[x] = (uint8_t)(top * (1.0 - fy) + bot * fy + 0.5);
			}
		}
	}
}

bool cuda_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received_packet)
{
	struct nvenc_data *enc = data;
	struct nv_cuda_surface *surf;
	struct nv_bitstream *bs;

	/* Multitrack live changes (staged resize / staged FPS change, timed re-init at the shared
	 * keyframe boundary): handled before any input copy so a boundary re-init can recreate the
	 * session and surfaces at the new size before this frame is copied. */
	if (!nvenc_prepare_frame(enc, frame->pts)) {
		*received_packet = false;
		return true; // skip this frame - encoder is transitioning
	}

	bs = &enc->bitstreams.array[enc->next_bitstream];
	surf = &enc->surfaces.array[enc->next_bitstream];

	deque_push_back(&enc->dts_list, &frame->pts, sizeof(frame->pts));

	/* ------------------------------------ */
	/* copy to CUDA surface                 */

	if (enc->resize_pending && enc->scale_buf[0]) {
		/* Staged resize waiting for the shared keyframe boundary: scale the new-size frame
		 * down to the current session size so it can be encoded with the old-size session.
		 * A frame that still carries the old size (one-tick mix rebind race - its plane-0
		 * stride is still the session width) is encoded as-is. */
		const uint32_t old_ls0 = enc->in_format == VIDEO_FORMAT_P010 ? 2 * enc->cx : enc->cx;
		if ((uint32_t)frame->linesize[0] == old_ls0) {
			if (!copy_frame(enc, frame, surf))
				return false;
		} else {
			const int src_w = (int)obs_encoder_get_width(enc->encoder);
			const int src_h = (int)obs_encoder_get_height(enc->encoder);
			struct encoder_frame scaled = *frame;
			for (int i = 0; i < 3; i++) {
				scaled.data[i] = enc->scale_buf[i];
				scaled.linesize[i] = enc->scale_stride[i];
			}
			switch (enc->in_format) {
			case VIDEO_FORMAT_P010:
				nvenc_scale_p010(frame->data[0], frame->data[1], src_w, src_h, (int)frame->linesize[0],
						 (int)frame->linesize[1], enc->scale_buf[0], enc->scale_buf[1], (int)enc->cx, (int)enc->cy,
						 enc->scale_stride[0], enc->scale_stride[1]);
				break;
			case VIDEO_FORMAT_I444: {
				const uint8_t *src_planes[3] = {frame->data[0], frame->data[1], frame->data[2]};
				uint8_t *dst_planes[3] = {enc->scale_buf[0], enc->scale_buf[1], enc->scale_buf[2]};
				const int src_ls[3] = {(int)frame->linesize[0], (int)frame->linesize[1],
						       (int)frame->linesize[2]};
				nvenc_scale_i444(src_planes, src_w, src_h, src_ls, dst_planes, (int)enc->cx, (int)enc->cy,
						 enc->scale_stride);
				break;
			}
			default: /* NV12 */
				nvenc_scale_nv12(frame->data[0], frame->data[1], src_w, src_h, (int)frame->linesize[0],
						 (int)frame->linesize[1], enc->scale_buf[0], enc->scale_buf[1], (int)enc->cx, (int)enc->cy,
						 enc->scale_stride[0], enc->scale_stride[1]);
				break;
			}
			if (!copy_frame(enc, &scaled, surf))
				return false;
		}
	} else if (!copy_frame(enc, frame, surf)) {
		return false;
	}

	/* ------------------------------------ */
	/* map output tex so nvenc can use it   */

	NV_ENC_MAP_INPUT_RESOURCE map = {NV_ENC_MAP_INPUT_RESOURCE_VER};
	map.registeredResource = surf->res;
	map.mappedBufferFmt = enc->surface_format;

	if (NV_FAILED(nv.nvEncMapInputResource(enc->session, &map)))
		return false;

	surf->mapped_res = map.mappedResource;

	/* ------------------------------------ */
	/* do actual encode call                */

	return nvenc_encode_base(enc, bs, surf->mapped_res, frame->pts, packet, received_packet);
}
