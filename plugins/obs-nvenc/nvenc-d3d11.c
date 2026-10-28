#include "nvenc-internal.h"
#include "nvenc-helpers.h"
#include "nvenc-cs-scale.h"

#include <stdio.h>
#include <string.h>

/*
 * NVENC implementation using Direct3D 11 context and textures
 */

/* ------------------------------------------------------------------------- */
/* D3D11 Context/Device management                                           */

static HANDLE get_lib(struct nvenc_data *enc, const char *lib)
{
	HMODULE mod = GetModuleHandleA(lib);
	if (mod)
		return mod;

	mod = LoadLibraryA(lib);
	if (!mod)
		error("Failed to load %s", lib);
	return mod;
}

typedef HRESULT(WINAPI *CREATEDXGIFACTORY1PROC)(REFIID, void **);

static bool d3d11_init_private_device(struct nvenc_data *enc);
static bool d3d11_create_gpu_scaler_shader(struct nvenc_data *enc);
static void nvenc_fault_module_suffix(const void *addr, char *buf, size_t buf_size);
static bool d3d11_cpu_create_copy_shader(struct nvenc_data *enc);

bool d3d11_init(struct nvenc_data *enc, obs_data_t *settings)
{
	HMODULE dxgi = get_lib(enc, "DXGI.dll");
	HMODULE d3d11 = get_lib(enc, "D3D11.dll");
	CREATEDXGIFACTORY1PROC create_dxgi;
	PFN_D3D11_CREATE_DEVICE create_device;
	IDXGIFactory1 *factory;
	IDXGIAdapter *adapter;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	HRESULT hr;

	if (!dxgi || !d3d11) {
		return false;
	}

	create_dxgi = (CREATEDXGIFACTORY1PROC)GetProcAddress(dxgi, "CreateDXGIFactory1");
	create_device = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d11, "D3D11CreateDevice");

	if (!create_dxgi || !create_device) {
		error("Failed to load D3D11/DXGI procedures");
		return false;
	}

	hr = create_dxgi(&IID_IDXGIFactory1, &factory);
	if (FAILED(hr)) {
		error_hr("CreateDXGIFactory1 failed");
		return false;
	}

	hr = factory->lpVtbl->EnumAdapters(factory, 0, &adapter);
	factory->lpVtbl->Release(factory);
	if (FAILED(hr)) {
		error_hr("EnumAdapters failed");
		return false;
	}

	hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, NULL,
			   &context);
	adapter->lpVtbl->Release(adapter);
	if (FAILED(hr)) {
		error_hr("D3D11CreateDevice failed");
		return false;
	}

	enc->device = device;
	enc->context = context;

	/* Eagerly create the fallback-scaler private device and compute shader while the
	 * process state is still clean (no NVENC session, no encode activity yet): on some
	 * driver stacks CreateComputeShader faults once live encoding has been running in
	 * the process (2026-10-22: faulted at staged-resize probe time after ~20 s of
	 * streaming; the same call succeeds in a clean probe process with an active session
	 * and 300 encode frames). If it still faults, shader_faulted latches and staged
	 * resizes fall back to the video processor / CPU readback scaler - never fatal for
	 * encoder creation. */
	if (d3d11_init_private_device(enc))
		d3d11_create_gpu_scaler_shader(enc);

	return true;
}

static void d3d11_free_video_processor(struct nvenc_data *enc)
{
	if (enc->vp_processor) {
		enc->vp_processor->lpVtbl->Release(enc->vp_processor);
		enc->vp_processor = NULL;
	}
	if (enc->vp_enumerator) {
		enc->vp_enumerator->lpVtbl->Release(enc->vp_enumerator);
		enc->vp_enumerator = NULL;
	}
	if (enc->video_context) {
		enc->video_context->lpVtbl->Release(enc->video_context);
		enc->video_context = NULL;
	}
	if (enc->video_device) {
		enc->video_device->lpVtbl->Release(enc->video_device);
		enc->video_device = NULL;
	}
}

/* Lazily create the shared private D3D11 device (same adapter as enc->device, mirrors
 * d3d11_init). The staged-resize GPU compute-shader scaler and CPU readback scaler run
 * on it because in some processes the NVENC device's d3d11 runtime state is corrupted:
 * validation-heavy calls (CreateComputeShader, Map on planar plane 1,
 * CreateVideoProcessor*) fail or fault there, while plain texture creation and
 * CopyResource keep working. */
static bool d3d11_init_private_device(struct nvenc_data *enc)
{
	if (enc->priv_dev)
		return true;

	HMODULE dxgi = GetModuleHandleA("DXGI.dll");
	HMODULE d3d11_mod = GetModuleHandleA("D3D11.dll");
	CREATEDXGIFACTORY1PROC create_dxgi;
	PFN_D3D11_CREATE_DEVICE create_device;
	IDXGIFactory1 *factory = NULL;
	IDXGIAdapter *adapter = NULL;
	HRESULT hr;

	if (!dxgi || !d3d11_mod) {
		error("staged resize: DXGI/D3D11 not loaded");
		return false;
	}
	create_dxgi = (CREATEDXGIFACTORY1PROC)GetProcAddress(dxgi, "CreateDXGIFactory1");
	create_device = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d11_mod, "D3D11CreateDevice");
	if (!create_dxgi || !create_device) {
		error("staged resize: failed to resolve D3D11 entry points");
		return false;
	}

	hr = create_dxgi(&IID_IDXGIFactory1, &factory);
	if (FAILED(hr)) {
		error_hr("staged resize: CreateDXGIFactory1 failed");
		return false;
	}
	hr = factory->lpVtbl->EnumAdapters(factory, 0, &adapter);
	factory->lpVtbl->Release(factory);
	if (FAILED(hr)) {
		error_hr("staged resize: EnumAdapters failed");
		return false;
	}
	hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &enc->priv_dev, NULL,
	                      &enc->priv_ctx);
	adapter->lpVtbl->Release(adapter);
	if (FAILED(hr)) {
		error_hr("staged resize: private D3D11CreateDevice failed");
		return false;
	}

	info("staged resize: private D3D11 device created for the fallback scalers");
	return true;
}

void d3d11_free(struct nvenc_data *enc)
{
	for (size_t i = 0; i < enc->input_textures.num; i++) {
		struct handle_tex *ht = &enc->input_textures.array[i];
		if (ht->vp_view)
			ht->vp_view->lpVtbl->Release(ht->vp_view);
		ht->tex->lpVtbl->Release(ht->tex);
		ht->km->lpVtbl->Release(ht->km);
	}
	d3d11_free_video_processor(enc);
	d3d11_free_gpu_scaler(enc);
	if (enc->priv_ctx) {
		enc->priv_ctx->lpVtbl->Release(enc->priv_ctx);
		enc->priv_ctx = NULL;
	}
	if (enc->priv_dev) {
		enc->priv_dev->lpVtbl->Release(enc->priv_dev);
		enc->priv_dev = NULL;
	}
	if (enc->context) {
		enc->context->lpVtbl->Release(enc->context);
	}
	if (enc->device) {
		enc->device->lpVtbl->Release(enc->device);
	}
}

/* ------------------------------------------------------------------------- */
/* D3D11 Surface management                                                  */

static DXGI_FORMAT d3d11_format(const obs_encoder_t *encoder)
{
	if (obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_NV12))
		return DXGI_FORMAT_NV12;
	if (obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_P010))
		return DXGI_FORMAT_P010;
	if (obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_GBRA) ||
	    obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_AYUV))
		return DXGI_FORMAT_AYUV;
	if (obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_Y410) ||
	    obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_GBR10))
		return DXGI_FORMAT_Y410;

	return DXGI_FORMAT_UNKNOWN;
}

static NV_ENC_BUFFER_FORMAT nvenc_format(const obs_encoder_t *encoder)
{
	if (obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_NV12))
		return NV_ENC_BUFFER_FORMAT_NV12;
	if (obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_P010))
		return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
	if (obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_GBRA) ||
	    obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_AYUV))
		return NV_ENC_BUFFER_FORMAT_AYUV;
	if (obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_Y410) ||
	    obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_GBR10))
		return NV_ENC_BUFFER_FORMAT_YUV444_10BIT;

	return NV_ENC_BUFFER_FORMAT_UNDEFINED;
}

static bool d3d11_texture_init(struct nvenc_data *enc, struct nv_texture *nvtex)
{
	D3D11_TEXTURE2D_DESC desc = {0};
	desc.Width = enc->cx;
	desc.Height = enc->cy;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = d3d11_format(enc->encoder);
	desc.SampleDesc.Count = 1;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;

	if (desc.Format == DXGI_FORMAT_Y410) {
		/* DXGI_FORMAT_Y410 cannot be bound as a render target, but that does not seem to be required here. */
		desc.BindFlags = 0;
	}

	ID3D11Device *const device = enc->device;
	ID3D11Texture2D *tex;
	HRESULT hr = device->lpVtbl->CreateTexture2D(device, &desc, NULL, &tex);
	if (FAILED(hr)) {
		error_hr("Failed to create texture");
		return false;
	}

	tex->lpVtbl->SetEvictionPriority(tex, DXGI_RESOURCE_PRIORITY_MAXIMUM);

	NV_ENC_REGISTER_RESOURCE res = {NV_ENC_REGISTER_RESOURCE_VER};
	res.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
	res.resourceToRegister = tex;
	res.width = enc->cx;
	res.height = enc->cy;
	res.bufferFormat = nvenc_format(enc->encoder);

	NVENCSTATUS err = nv.nvEncRegisterResource(enc->session, &res);
	if (NV_FAILED(err)) {
		error("nvEncRegisterResource failed at %ux%u (err=%d)", enc->cx, enc->cy, (int)err);
		tex->lpVtbl->Release(tex);
		return false;
	}

	nvtex->res = res.registeredResource;
	nvtex->tex = tex;
	nvtex->mapped_res = NULL;
	nvtex->vp_view = NULL;
	return true;
}

bool d3d11_init_textures(struct nvenc_data *enc)
{
	//blog(LOG_DEBUG, "buf count: %d", enc->buf_count);
	da_reserve(enc->textures, enc->buf_count);
	for (uint32_t i = 0; i < enc->buf_count; i++) {
		struct nv_texture texture = {0}; /* every field must be initialized: d3d11_texture_free()
		                                   * dereferences vp_view/tex without further checks */
		if (!d3d11_texture_init(enc, &texture)) {
			return false;
		}

		da_push_back(enc->textures, &texture);
	}

	return true;
}

static void d3d11_texture_free(struct nvenc_data *enc, struct nv_texture *nvtex)
{

	if (nvtex->res) {
		if (nvtex->mapped_res) {
			nv.nvEncUnmapInputResource(enc->session, nvtex->mapped_res);
		}
		nv.nvEncUnregisterResource(enc->session, nvtex->res);
		if (nvtex->vp_view)
			nvtex->vp_view->lpVtbl->Release(nvtex->vp_view);
		nvtex->tex->lpVtbl->Release(nvtex->tex);
	}
}

void d3d11_free_textures(struct nvenc_data *enc)
{
	for (size_t i = 0; i < enc->textures.num; i++) {
		d3d11_texture_free(enc, &enc->textures.array[i]);
	}
}

/* ------------------------------------------------------------------------- */
/* D3D11 video processor - staged-resize scaling                             */
/*                                                                           */
/* While a multitrack resize is staged (waiting for the shared keyframe      */
/* boundary), incoming frames already carry the NEW size while the NVENC     */
/* session still expects the OLD one. A plain CopyResource cannot bridge the */
/* two, so the D3D11 video processor scales each frame down to the current   */
/* session size before it is fed to the encoder. The processor chain         */
/* (device/context/enumerator/processor + per-texture input/output views)    */
/* is created lazily on first use and torn down in d3d11_free().             */

/* The Windows SDK headers installed on some machines declare the D3D11 video-processor
 * descriptor structs with layouts that do not match the actual driver ABI (e.g. extra
 * fields in D3D11_VIDEO_PROCESSOR_CONTENT_DESC, a missing SubsampleMode in the input-view
 * descriptor, a rearranged D3D11_VIDEO_PROCESSOR_STREAM). Passing those layouts makes
 * CreateVideoProcessorEnumerator / CreateVideoProcessorInputView / VideoProcessorBlt fail
 * with E_INVALIDARG because the driver reads zeroed or shifted fields. Define the real ABI
 * layouts here and cast at the call sites; on a stock SDK these are byte-identical to the
 * SDK types, so this is safe everywhere. */
struct nv_vp_content_desc {
	UINT InputFrameFormat; /* D3D11_VIDEO_FRAME_FORMAT */
	DXGI_RATIONAL InputFrameRate;
	UINT OutputWidth;
	UINT OutputHeight;
	UINT Usage; /* D3D11_VIDEO_USAGE */
};

struct nv_vp_input_view_desc {
	UINT FourCC; /* DXGI_FORMAT */
	UINT SubsampleMode; /* D3D11_VIDEO_SUBSAMPLE_TYPE */
	UINT ViewDimension; /* D3D11_VPIV_DIMENSION */
	union {
		struct {
			UINT MipSlice;
		} Texture2D;
		struct {
			UINT MipSlice;
			UINT FirstArraySlice;
			UINT NumSlices;
		} Texture2DArray;
	};
};

struct nv_vp_output_view_desc {
	UINT ViewDimension; /* D3D11_VPOV_DIMENSION */
	union {
		struct {
			UINT MipSlice;
		} Texture2D;
		struct {
			UINT MipSlice;
			UINT FirstArraySlice;
			UINT NumSlices;
		} Texture2DArray;
	};
};

struct nv_vp_stream {
	BOOL Enable;
	UINT OutputIndex;
	UINT RateConversionIndex;
	RECT SourceRect;
	RECT DestRect;
	UINT InputFrameOrField;
	ID3D11VideoProcessorInputView *pInputSurface;
};

/* D3D11_VIDEO_SUBSAMPLE_TYPE values (the enum is absent from some SDK revisions). */
#define NV_VP_SUBSAMPLE_NONE 0
#define NV_VP_SUBSAMPLE_Y_CbCr 3

/* ------------------------------------------------------------------------- */
/* D3D11 CPU readback scaler - staged-resize fallback without a video         */
/* processor. Some driver/SDK combinations do not support ID3D11VideoDevice   */
/* at all (CreateVideoProcessorEnumerator fails for every content desc), so   */
/* the new-size shared input is copied into an own two-plane texture on the   */
/* private device, both planes are dumped into single-plane textures by a     */
/* compute shader through format-override UAVs (RWTexture2D.Load/Store -      */
/* planar SRVs/RTVs, Map on subresource 1 of a two-plane texture, and         */
/* plane-level CopySubresourceRegion are all rejected or silently no-op'd by  */
/* some drivers), bilinear-scaled to the current session size on the CPU      */
/* (nvenc_scale_nv12/p010), written into old-size staging textures and        */
/* CS-copied back into the shared two-plane writeback texture. NV12/P010 only.*/

void d3d11_free_cpu_scaler(struct nvenc_data *enc)
{
	struct d3d11_cpu_scaler *cs = &enc->cpu_scaler;
	for (int i = 0; i < 2; i++) {
		if (cs->in_uav[i])
			cs->in_uav[i]->lpVtbl->Release(cs->in_uav[i]);
	}
	if (cs->y_dump_uav)
		cs->y_dump_uav->lpVtbl->Release(cs->y_dump_uav);
	if (cs->uv_dump_uav)
		cs->uv_dump_uav->lpVtbl->Release(cs->uv_dump_uav);
	if (cs->y_wb_def_uav)
		cs->y_wb_def_uav->lpVtbl->Release(cs->y_wb_def_uav);
	if (cs->uv_wb_def_uav)
		cs->uv_wb_def_uav->lpVtbl->Release(cs->uv_wb_def_uav);
	if (cs->wb_y_uav)
		cs->wb_y_uav->lpVtbl->Release(cs->wb_y_uav);
	if (cs->wb_uv_uav)
		cs->wb_uv_uav->lpVtbl->Release(cs->wb_uv_uav);
	if (cs->in_def)
		cs->in_def->lpVtbl->Release(cs->in_def);
	if (cs->y_dump_def)
		cs->y_dump_def->lpVtbl->Release(cs->y_dump_def);
	if (cs->uv_dump_def)
		cs->uv_dump_def->lpVtbl->Release(cs->uv_dump_def);
	if (cs->y_rb_tex)
		cs->y_rb_tex->lpVtbl->Release(cs->y_rb_tex);
	if (cs->uv_rb_tex)
		cs->uv_rb_tex->lpVtbl->Release(cs->uv_rb_tex);
	if (cs->y_wb_tex)
		cs->y_wb_tex->lpVtbl->Release(cs->y_wb_tex);
	if (cs->uv_wb_tex)
		cs->uv_wb_tex->lpVtbl->Release(cs->uv_wb_tex);
	if (cs->y_wb_def)
		cs->y_wb_def->lpVtbl->Release(cs->y_wb_def);
	if (cs->uv_wb_def)
		cs->uv_wb_def->lpVtbl->Release(cs->uv_wb_def);
	if (cs->wb_opened)
		cs->wb_opened->lpVtbl->Release(cs->wb_opened);
	if (cs->writeback_tex)
		cs->writeback_tex->lpVtbl->Release(cs->writeback_tex);
	if (cs->wb_km_enc)
		cs->wb_km_enc->lpVtbl->Release(cs->wb_km_enc);
	if (cs->wb_km)
		cs->wb_km->lpVtbl->Release(cs->wb_km);
	if (cs->cs_copy)
		cs->cs_copy->lpVtbl->Release(cs->cs_copy);
	if (cs->cb_in)
		cs->cb_in->lpVtbl->Release(cs->cb_in);
	if (cs->cb_out)
		cs->cb_out->lpVtbl->Release(cs->cb_out);
	bfree(cs->buf[0]);
	bfree(cs->buf[1]);
	memset(cs, 0, sizeof(*cs));
}

static bool d3d11_gpu_create_uavs(struct nvenc_data *enc, ID3D11Texture2D *tex, const DXGI_FORMAT fmt,
                                  ID3D11UnorderedAccessView *uav[2]);

/* Single-plane DEFAULT+UAV texture + override view on the private device. */
static bool d3d11_cpu_make_plane(struct nvenc_data *enc, const DXGI_FORMAT fmt, uint32_t w, uint32_t h, ID3D11Texture2D **tex,
                                  ID3D11UnorderedAccessView **uav)
{
	D3D11_TEXTURE2D_DESC td = {0};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	HRESULT hr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, tex);
	if (FAILED(hr))
		return false;

	D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {0};
	ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	ud.Format = fmt;
	hr = enc->priv_dev->lpVtbl->CreateUnorderedAccessView(enc->priv_dev, (ID3D11Resource *)*tex, &ud, uav);
	if (FAILED(hr)) {
		(*tex)->lpVtbl->Release(*tex);
		*tex = NULL;
		return false;
	}
	return true;
}

/* Identity-size cbuffer for the plane-copy shader: with src_size == dst_size the bilinear
 * scaler degenerates to an exact pixel copy (the fractional offset is zero at pixel centers). */
static bool d3d11_cpu_make_identity_cb(struct nvenc_data *enc, uint32_t w, uint32_t h, ID3D11Buffer **out)
{
	float v[8] = {(float)w, (float)h, (float)(w / 2), (float)(h / 2), (float)w, (float)h, (float)(w / 2), (float)(h / 2)};
	D3D11_BUFFER_DESC bd = {0};
	bd.ByteWidth = sizeof(v);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	D3D11_SUBRESOURCE_DATA sd = {v, sizeof(v), sizeof(v)};
	return SUCCEEDED(enc->priv_dev->lpVtbl->CreateBuffer(enc->priv_dev, &bd, &sd, out));
}

/* One dispatch of the plane-copy shader: copies both planes through their format-override
 * views (src_y/src_uv -> dst_y/dst_uv). All four views must be bound - the shader always
 * executes both the Y and the UV branch. */
static void d3d11_cpu_cs_copy(struct nvenc_data *enc, struct d3d11_cpu_scaler *cs, ID3D11UnorderedAccessView *src_y,
                               ID3D11UnorderedAccessView *src_uv, ID3D11UnorderedAccessView *dst_y,
                               ID3D11UnorderedAccessView *dst_uv, ID3D11Buffer *cb, uint32_t w, uint32_t h)
{
	ID3D11DeviceContext *sctx = enc->priv_ctx;
	ID3D11UnorderedAccessView *views[4] = {src_y, src_uv, dst_y, dst_uv};
	sctx->lpVtbl->CSSetUnorderedAccessViews(sctx, 0, 4, views, NULL);
	sctx->lpVtbl->CSSetShader(sctx, cs->cs_copy, NULL, 0);
	sctx->lpVtbl->CSSetConstantBuffers(sctx, 0, 1, &cb);
	sctx->lpVtbl->Dispatch(sctx, (unsigned int)((w + 7) / 8), (unsigned int)((h + 7) / 8), 1);
	ID3D11ComputeShader *null_cs = NULL;
	ID3D11UnorderedAccessView *nulls[4] = {NULL, NULL, NULL, NULL};
	sctx->lpVtbl->CSSetShader(sctx, null_cs, NULL, 0);
	sctx->lpVtbl->CSSetUnorderedAccessViews(sctx, 0, 4, nulls, NULL);
}

/* One-time creation of the plane-copy compute shader (the embedded scaler bytecode with an
 * identity-size cbuffer), SEH-guarded exactly like d3d11_create_gpu_scaler_shader: on some
 * NVIDIA driver stacks CreateComputeShader faults inside d3d11.dll once live encoding has
 * run in the process instead of returning an error. */
static bool d3d11_cpu_create_copy_shader(struct nvenc_data *enc)
{
	struct d3d11_cpu_scaler *cs = &enc->cpu_scaler;
	if (cs->cs_copy)
		return true;
	if (cs->cs_faulted)
		return false;

	ID3D11ComputeShader *csc = NULL;
	EXCEPTION_POINTERS *ep = NULL;
	HRESULT hr;

	__try {
		hr = enc->priv_dev->lpVtbl->CreateComputeShader(enc->priv_dev, NV_CS_SCALE_CODE, NV_CS_SCALE_CODE_SIZE, NULL, &csc);
		if (FAILED(hr)) {
			debug("staged resize unavailable: CPU scaler copy shader creation failed (hr=0x%08lX)", (uint32_t)hr);
			return false;
		}
	} __except ((ep = GetExceptionInformation()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_EXECUTE_HANDLER) {
		char mod[160] = "";
		__try {
			nvenc_fault_module_suffix(ep ? (const void *)ep->ExceptionRecord->ExceptionAddress : NULL, mod, sizeof(mod));
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			/* diagnostics must never mask the original fault */
		}
		warn("staged resize: D3D11 CreateComputeShader faulted (exception 0x%08lX at %p%s); the CPU readback "
		     "scaler is unavailable, staged resizes will apply immediately instead of scaling",
		     ep ? (unsigned long)ep->ExceptionRecord->ExceptionCode : 0,
		     ep ? (const void *)ep->ExceptionRecord->ExceptionAddress : NULL, mod);
		cs->cs_faulted = true;
		return false;
	}

	cs->cs_copy = csc;
	return true;
}

static bool d3d11_init_cpu_scaler(struct nvenc_data *enc)
{
	const DXGI_FORMAT fmt = d3d11_format(enc->encoder);
	if (fmt != DXGI_FORMAT_NV12 && fmt != DXGI_FORMAT_P010) {
		debug("staged resize unavailable: no video processor and format %u has no CPU readback path",
		      (unsigned)fmt);
		return false;
	}

	struct d3d11_cpu_scaler *cs = &enc->cpu_scaler;
	const uint32_t new_w = obs_encoder_get_width(enc->encoder);
	const uint32_t new_h = obs_encoder_get_height(enc->encoder);
	const uint32_t old_w = enc->cx, old_h = enc->cy;

	/* All CPU-mappable work runs on the shared private device (see d3d11_init_private_device). */
	if (!d3d11_init_private_device(enc))
		return false;

	HRESULT hr;

	/* Single-plane formats matching each plane of the delivery format. */
	const DXGI_FORMAT y_fmt = (fmt == DXGI_FORMAT_NV12) ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
	const DXGI_FORMAT uv_fmt = (fmt == DXGI_FORMAT_NV12) ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM;

	D3D11_TEXTURE2D_DESC td = {0};
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.SampleDesc.Count = 1;

	/* Two-plane DEFAULT texture at the NEW size: full-texture CopyResource target for the
	 * shared input and source of the CS plane dumps (a STAGING two-plane texture cannot carry
	 * UAV views, and Map on subresource 1 is rejected by some drivers). */
	td.Format = fmt;
	td.Width = new_w;
	td.Height = new_h;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	hr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, &cs->in_def);
	if (FAILED(hr)) {
		error_hr("cpu scaler: input texture creation failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}
	if (!d3d11_gpu_create_uavs(enc, cs->in_def, fmt, cs->in_uav)) {
		d3d11_free_cpu_scaler(enc);
		return false;
	}

	/* Single-plane DEFAULT+UAV dump targets at the NEW size: the CS plane copies land here
	 * (UAVs cannot target STAGING textures) before a same-format CopyResource into staging. */
	if (!d3d11_cpu_make_plane(enc, y_fmt, new_w, new_h, &cs->y_dump_def, &cs->y_dump_uav)) {
		error_hr("cpu scaler: Y dump texture creation failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}
	if (!d3d11_cpu_make_plane(enc, uv_fmt, new_w / 2, new_h / 2, &cs->uv_dump_def, &cs->uv_dump_uav)) {
		error_hr("cpu scaler: UV dump texture creation failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}

	/* Single-plane STAGING readback textures at the NEW size (Map READ after the dump copies). */
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
	td.Format = y_fmt;
	td.Width = new_w;
	td.Height = new_h;
	hr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, &cs->y_rb_tex);
	if (FAILED(hr)) {
		error_hr("cpu scaler: Y readback texture creation failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}

	td.Format = uv_fmt;
	td.Width = new_w / 2;
	td.Height = new_h / 2;
	hr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, &cs->uv_rb_tex);
	if (FAILED(hr)) {
		error_hr("cpu scaler: UV readback texture creation failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}

	/* Single-plane STAGING writeback textures at the OLD (session) size: the scaled frame is
	 * written into these via Map. */
	td.Format = y_fmt;
	td.Width = old_w;
	td.Height = old_h;
	hr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, &cs->y_wb_tex);
	if (FAILED(hr)) {
		error_hr("cpu scaler: Y writeback texture creation failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}

	td.Format = uv_fmt;
	td.Width = old_w / 2;
	td.Height = old_h / 2;
	hr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, &cs->uv_wb_tex);
	if (FAILED(hr)) {
		error_hr("cpu scaler: UV writeback texture creation failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}

	/* Single-plane DEFAULT+UAV writeback sources at the OLD size: same-format CopyResource
	 * targets for the staging textures, source of the CS plane copies into writeback_tex. */
	if (!d3d11_cpu_make_plane(enc, y_fmt, old_w, old_h, &cs->y_wb_def, &cs->y_wb_def_uav)) {
		error_hr("cpu scaler: Y writeback dump texture creation failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}
	if (!d3d11_cpu_make_plane(enc, uv_fmt, old_w / 2, old_h / 2, &cs->uv_wb_def, &cs->uv_wb_def_uav)) {
		error_hr("cpu scaler: UV writeback dump texture creation failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}

	/* Two-plane shared keyed-mutex writeback texture at the OLD size (DEFAULT usage: the CS
	 * plane copies write it through override views). */
	td.Format = fmt;
	td.Width = old_w;
	td.Height = old_h;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = 0;
	td.CPUAccessFlags = 0;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	hr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, &cs->writeback_tex);
	if (FAILED(hr)) {
		error_hr("cpu scaler: writeback texture creation failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}
	ID3D11UnorderedAccessView *wb_views[2] = {NULL};
	if (!d3d11_gpu_create_uavs(enc, cs->writeback_tex, fmt, wb_views)) {
		d3d11_free_cpu_scaler(enc);
		return false;
	}
	cs->wb_y_uav = wb_views[0];
	cs->wb_uv_uav = wb_views[1];

	hr = cs->writeback_tex->lpVtbl->QueryInterface(cs->writeback_tex, &IID_IDXGIKeyedMutex, (void **)&cs->wb_km);
	if (FAILED(hr)) {
		error_hr("cpu scaler: writeback QI(IDXGIKeyedMutex) failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}

	/* Open the shared writeback texture on the NVENC device for the final CopyResource. */
	IDXGIResource *wb_res = NULL;
	HANDLE wb_handle = NULL;
	hr = cs->writeback_tex->lpVtbl->QueryInterface(cs->writeback_tex, &IID_IDXGIResource, (void **)&wb_res);
	if (SUCCEEDED(hr)) {
		wb_res->lpVtbl->GetSharedHandle(wb_res, &wb_handle);
		wb_res->lpVtbl->Release(wb_res);
	}
	if (!wb_handle) {
		error("cpu scaler: writeback GetSharedHandle failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}
	hr = enc->device->lpVtbl->OpenSharedResource(enc->device, wb_handle, &IID_ID3D11Texture2D, (void **)&cs->wb_opened);
	if (FAILED(hr)) {
		error_hr("cpu scaler: writeback OpenSharedResource(NVENC device) failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}
	hr = cs->wb_opened->lpVtbl->QueryInterface(cs->wb_opened, &IID_IDXGIKeyedMutex, (void **)&cs->wb_km_enc);
	if (FAILED(hr)) {
		error_hr("cpu scaler: writeback QI(IDXGIKeyedMutex) on NVENC device failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}

	/* Plane-copy compute shader + identity cbuffers (readback at NEW size, writeback at OLD). */
	if (!d3d11_cpu_create_copy_shader(enc)) {
		d3d11_free_cpu_scaler(enc);
		return false;
	}
	if (!d3d11_cpu_make_identity_cb(enc, new_w, new_h, &cs->cb_in) ||
	    !d3d11_cpu_make_identity_cb(enc, old_w, old_h, &cs->cb_out)) {
		error_hr("cpu scaler: identity cbuffer creation failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}

	if (fmt == DXGI_FORMAT_NV12) {
		cs->stride[0] = (int)old_w;
		cs->buf[0] = bmalloc((size_t)old_w * old_h);
		/* nvenc_scale_nv12 indexes UV as dst_uv[y*dst_ls1 + x*2 + c] with x < old_w/2, so the
		 * UV row stride must be old_w (a full-width row of interleaved pairs), not old_w/2 -
		 * a smaller stride overflows the buffer on the last UV rows. */
		cs->stride[1] = (int)old_w;
		cs->buf[1] = bmalloc((size_t)old_w * (old_h / 2));
	} else { /* P010: u16 samples */
		cs->stride[0] = (int)(old_w * 2);
		cs->buf[0] = bmalloc((size_t)old_w * old_h * 2);
		/* nvenc_scale_p010 indexes UV as ((uint16_t *)dst_uv)[y*(dst_ls1/2) + x*2 + c], so
		 * dst_ls1 must be 2*old_w bytes (a full-width row of u16 pairs). */
		cs->stride[1] = (int)(old_w * 2);
		cs->buf[1] = bmalloc((size_t)old_w * old_h);
	}
	if (!cs->buf[0] || !cs->buf[1]) {
		error("cpu scaler: scale buffer allocation failed");
		d3d11_free_cpu_scaler(enc);
		return false;
	}

	cs->new_w = new_w;
	cs->new_h = new_h;
	cs->old_w = old_w;
	cs->old_h = old_h;
	cs->ready = true;
	info("staged resize: CPU readback scaler ready (new %ux%u -> session %ux%u, private device)", new_w, new_h, old_w,
	     old_h);
	return true;
}

static bool d3d11_cpu_scale_frame(struct nvenc_data *enc, struct handle_tex *ht, struct nv_texture *nvtex)
{
	struct d3d11_cpu_scaler *cs = &enc->cpu_scaler;
	ID3D11DeviceContext *ctx = enc->context; /* NVENC device context: only CopyResources touch it */
	ID3D11DeviceContext *sctx = enc->priv_ctx; /* shared private device context: all Map work runs here */
	const DXGI_FORMAT fmt = d3d11_format(enc->encoder);

	/* The shared input's actual size: the new size while staged, still the old size for one
	 * tick after a rebind, or an intermediate size if the target changed mid-wait. */
	D3D11_TEXTURE2D_DESC idesc = {0};
	ht->tex->lpVtbl->GetDesc(ht->tex, &idesc);
	if (idesc.Width == 0 || idesc.Height == 0) {
		error("cpu scaler: GetDesc on shared input failed");
		return false;
	}

	/* One-tick rebind race: a frame that still carries the OLD size is copied as-is. */
	if (idesc.Width == enc->cx && idesc.Height == enc->cy) {
		ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource *)nvtex->tex, (ID3D11Resource *)ht->tex);
		return true;
	}

	/* The input size may have changed mid-wait: re-create the NEW-size objects to match it.
	 * Build the new objects first and only swap them in on success, so a transient creation
	 * failure leaves the previous (valid) objects in place instead of NULL pointers that
	 * would be dereferenced on the next frame (same pattern as d3d11_gpu_scale_frame). */
	if (cs->new_w != idesc.Width || cs->new_h != idesc.Height) {
		const DXGI_FORMAT y_fmt = (fmt == DXGI_FORMAT_NV12) ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
		const DXGI_FORMAT uv_fmt = (fmt == DXGI_FORMAT_NV12) ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM;

		ID3D11Texture2D *n_in_def = NULL, *n_y_rb = NULL, *n_uv_rb = NULL, *n_y_dump = NULL, *n_uv_dump = NULL;
		ID3D11UnorderedAccessView *n_in_uav[2] = {NULL}, *n_y_dump_uav = NULL, *n_uv_dump_uav = NULL;
		ID3D11Buffer *n_cb_in = NULL;

		D3D11_TEXTURE2D_DESC td = {0};
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.SampleDesc.Count = 1;
		td.Format = fmt;
		td.Width = idesc.Width;
		td.Height = idesc.Height;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		HRESULT rhr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, &n_in_def);
		if (SUCCEEDED(rhr))
			rhr = d3d11_gpu_create_uavs(enc, n_in_def, fmt, n_in_uav) ? S_OK : E_FAIL;
		if (SUCCEEDED(rhr))
			rhr = d3d11_cpu_make_plane(enc, y_fmt, idesc.Width, idesc.Height, &n_y_dump, &n_y_dump_uav) ? S_OK : E_FAIL;
		if (SUCCEEDED(rhr))
			rhr = d3d11_cpu_make_plane(enc, uv_fmt, idesc.Width / 2, idesc.Height / 2, &n_uv_dump, &n_uv_dump_uav) ? S_OK : E_FAIL;
		if (SUCCEEDED(rhr)) {
			td.Usage = D3D11_USAGE_STAGING;
			td.BindFlags = 0;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
			td.Format = y_fmt;
			rhr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, &n_y_rb);
		}
		if (SUCCEEDED(rhr)) {
			td.Format = uv_fmt;
			td.Width = idesc.Width / 2;
			td.Height = idesc.Height / 2;
			rhr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, &n_uv_rb);
		}
		if (SUCCEEDED(rhr))
			rhr = d3d11_cpu_make_identity_cb(enc, idesc.Width, idesc.Height, &n_cb_in) ? S_OK : E_FAIL;

		if (FAILED(rhr)) {
			for (int i = 0; i < 2; i++)
				if (n_in_uav[i])
					n_in_uav[i]->lpVtbl->Release(n_in_uav[i]);
			if (n_y_dump_uav)
				n_y_dump_uav->lpVtbl->Release(n_y_dump_uav);
			if (n_uv_dump_uav)
				n_uv_dump_uav->lpVtbl->Release(n_uv_dump_uav);
			if (n_in_def)
				n_in_def->lpVtbl->Release(n_in_def);
			if (n_y_dump)
				n_y_dump->lpVtbl->Release(n_y_dump);
			if (n_uv_dump)
				n_uv_dump->lpVtbl->Release(n_uv_dump);
			if (n_y_rb)
				n_y_rb->lpVtbl->Release(n_y_rb);
			if (n_uv_rb)
				n_uv_rb->lpVtbl->Release(n_uv_rb);
			if (n_cb_in)
				n_cb_in->lpVtbl->Release(n_cb_in);
			error("cpu scaler: readback object re-creation failed (hr=0x%08lX)", (uint32_t)rhr);
			return false;
		}

		for (int i = 0; i < 2; i++) {
			cs->in_uav[i]->lpVtbl->Release(cs->in_uav[i]);
			cs->in_uav[i] = n_in_uav[i];
		}
		if (cs->y_dump_uav)
			cs->y_dump_uav->lpVtbl->Release(cs->y_dump_uav);
		if (cs->uv_dump_uav)
			cs->uv_dump_uav->lpVtbl->Release(cs->uv_dump_uav);
		if (cs->in_def)
			cs->in_def->lpVtbl->Release(cs->in_def);
		if (cs->y_dump_def)
			cs->y_dump_def->lpVtbl->Release(cs->y_dump_def);
		if (cs->uv_dump_def)
			cs->uv_dump_def->lpVtbl->Release(cs->uv_dump_def);
		if (cs->y_rb_tex)
			cs->y_rb_tex->lpVtbl->Release(cs->y_rb_tex);
		if (cs->uv_rb_tex)
			cs->uv_rb_tex->lpVtbl->Release(cs->uv_rb_tex);
		if (cs->cb_in)
			cs->cb_in->lpVtbl->Release(cs->cb_in);
		cs->in_def = n_in_def;
		cs->y_dump_uav = n_y_dump_uav;
		cs->uv_dump_uav = n_uv_dump_uav;
		cs->y_dump_def = n_y_dump;
		cs->uv_dump_def = n_uv_dump;
		cs->y_rb_tex = n_y_rb;
		cs->uv_rb_tex = n_uv_rb;
		cs->cb_in = n_cb_in;
		cs->new_w = idesc.Width;
		cs->new_h = idesc.Height;
	}

	/* 1. Pull the new-size shared input into the own two-plane texture on the private device
	 *    (full-texture CopyResource - cross-device copies are not allowed and plane-level
	 *    subresource copies silently no-op on some drivers), then dump both planes into
	 *    single-plane textures with the plane-copy shader, and copy those into the staging
	 *    textures for Map (the input's keyed mutex is held by the caller). OpenSharedResource
	 *    per frame: cheap, and avoids a cached handle going stale if OBS rebinds the texture. */
	ID3D11Texture2D *in_sdev = NULL;
	HRESULT hr = enc->priv_dev->lpVtbl->OpenSharedResource(enc->priv_dev, (HANDLE)(uintptr_t)ht->handle, &IID_ID3D11Texture2D,
	                                                   (void **)&in_sdev);
	if (FAILED(hr)) {
		error_hr("cpu scaler: OpenSharedResource on private device failed");
		return false;
	}
	sctx->lpVtbl->CopyResource(sctx, (ID3D11Resource *)cs->in_def, (ID3D11Resource *)in_sdev);
	in_sdev->lpVtbl->Release(in_sdev);

	d3d11_cpu_cs_copy(enc, cs, cs->in_uav[0], cs->in_uav[1], cs->y_dump_uav, cs->uv_dump_uav, cs->cb_in, cs->new_w,
	                  cs->new_h);
	sctx->lpVtbl->CopyResource(sctx, (ID3D11Resource *)cs->y_rb_tex, (ID3D11Resource *)cs->y_dump_def);
	sctx->lpVtbl->CopyResource(sctx, (ID3D11Resource *)cs->uv_rb_tex, (ID3D11Resource *)cs->uv_dump_def);

	D3D11_MAPPED_SUBRESOURCE mY = {0}, mUV = {0};
	hr = sctx->lpVtbl->Map(sctx, (ID3D11Resource *)cs->y_rb_tex, 0, D3D11_MAP_READ, 0, &mY);
	if (FAILED(hr)) {
		error_hr("cpu scaler: Y readback map failed");
		return false;
	}
	hr = sctx->lpVtbl->Map(sctx, (ID3D11Resource *)cs->uv_rb_tex, 0, D3D11_MAP_READ, 0, &mUV);
	if (FAILED(hr)) {
		sctx->lpVtbl->Unmap(sctx, (ID3D11Resource *)cs->y_rb_tex, 0);
		error_hr("cpu scaler: UV readback map failed");
		return false;
	}

	if (fmt == DXGI_FORMAT_NV12) {
		nvenc_scale_nv12(mY.pData, mUV.pData, idesc.Width, idesc.Height, (int)mY.RowPitch, (int)mUV.RowPitch,
			 cs->buf[0], cs->buf[1], enc->cx, enc->cy, cs->stride[0], cs->stride[1]);
	} else { /* P010 */
		nvenc_scale_p010(mY.pData, mUV.pData, idesc.Width, idesc.Height, (int)mY.RowPitch, (int)mUV.RowPitch,
			 cs->buf[0], cs->buf[1], enc->cx, enc->cy, cs->stride[0], cs->stride[1]);
	}

	sctx->lpVtbl->Unmap(sctx, (ID3D11Resource *)cs->y_rb_tex, 0);
	sctx->lpVtbl->Unmap(sctx, (ID3D11Resource *)cs->uv_rb_tex, 0);

	/* 2. Write the scaled old-size frame into the single-plane writeback staging textures
	 *    (Map on the private device), then CS-copy both planes into the shared two-plane
	 *    writeback texture, and finally copy that into the registered session-size input
	 *    texture on the NVENC device - the only operation that touches the possibly-corrupted
	 *    device. */
	if (FAILED(cs->wb_km->lpVtbl->AcquireSync(cs->wb_km, 0, INFINITE))) {
		error("cpu scaler: writeback AcquireSync failed");
		return false;
	}
	D3D11_MAPPED_SUBRESOURCE wY = {0}, wUV = {0};
	hr = sctx->lpVtbl->Map(sctx, (ID3D11Resource *)cs->y_wb_tex, 0, D3D11_MAP_WRITE, 0, &wY);
	if (SUCCEEDED(hr)) {
		hr = sctx->lpVtbl->Map(sctx, (ID3D11Resource *)cs->uv_wb_tex, 0, D3D11_MAP_WRITE, 0, &wUV);
	}
	const bool mapped = SUCCEEDED(hr);
	if (mapped) {
		/* A UV row holds w/2 interleaved pairs: w bytes for NV12 (u8 pairs), 2*w bytes for P010
		 * (u16 pairs) - the same width as a Y row in both formats. */
		const size_t y_row = (fmt == DXGI_FORMAT_NV12) ? enc->cx : (size_t)enc->cx * 2; /* bytes per row */
		for (uint32_t y = 0; y < enc->cy; y++) {
			memcpy((uint8_t *)wY.pData + (size_t)y * wY.RowPitch, cs->buf[0] + (size_t)y * cs->stride[0], y_row);
		}
		for (uint32_t y = 0; y < enc->cy / 2; y++) {
			memcpy((uint8_t *)wUV.pData + (size_t)y * wUV.RowPitch, cs->buf[1] + (size_t)y * cs->stride[1], y_row);
		}
		sctx->lpVtbl->Unmap(sctx, (ID3D11Resource *)cs->y_wb_tex, 0);
		sctx->lpVtbl->Unmap(sctx, (ID3D11Resource *)cs->uv_wb_tex, 0);

		sctx->lpVtbl->CopyResource(sctx, (ID3D11Resource *)cs->y_wb_def, (ID3D11Resource *)cs->y_wb_tex);
		sctx->lpVtbl->CopyResource(sctx, (ID3D11Resource *)cs->uv_wb_def, (ID3D11Resource *)cs->uv_wb_tex);
		d3d11_cpu_cs_copy(enc, cs, cs->y_wb_def_uav, cs->uv_wb_def_uav, cs->wb_y_uav, cs->wb_uv_uav, cs->cb_out,
		                  cs->old_w, cs->old_h);
	}
	cs->wb_km->lpVtbl->ReleaseSync(cs->wb_km, 0);
	if (!mapped) {
		error_hr("cpu scaler: writeback map failed");
		return false;
	}

	hr = cs->wb_km_enc->lpVtbl->AcquireSync(cs->wb_km_enc, 0, INFINITE);
	if (FAILED(hr)) {
		error_hr("cpu scaler: writeback AcquireSync (NVENC device) failed");
		return false;
	}
	ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource *)nvtex->tex, (ID3D11Resource *)cs->wb_opened);
	cs->wb_km_enc->lpVtbl->ReleaseSync(cs->wb_km_enc, 0);
	return true;
}

/* ------------------------------------------------------------------------- */
/* D3D11 GPU compute-shader scaler - staged-resize fallback ahead of the      */
/* CPU readback path. Some driver/SDK combinations do not support             */
/* ID3D11VideoDevice (and plane-1 staging Map / planar SRVs / cross-plane     */
/* CopySubresourceRegion are unreliable on top of that), so the new-size      */
/* shared input is copied into an own intermediate texture and bilinear-       */
/* scaled to the current session size by a compute shader that reads/writes   */
/* the planes through FORMAT-OVERRIDE UAVs (RWTexture2D.Load, no SRVs/RTVs).  */
/* The result is copied into the registered session-size input texture.       */
/* NV12/P010 delivery only; everything stays on the GPU.                      */

/* The cs_5_0 bytecode of the scaler shader is embedded (nvenc-cs-scale.h, generated
 * from NV_CS_SCALE_SRC by build_x64/gen_cs.cpp): it keeps runtime D3DCompile off the
 * UI/encode path. On some driver stacks in a process that already runs live encoding,
 * shader creation faults with STATUS_STACK_BUFFER_OVERRUN deep inside d3d11.dll /
 * NVIDIA driver code instead of returning an error (verified 2026-10-22: the fault
 * never reproduces in a clean probe process - active NVENC session, second device and
 * 300 encode frames included - only in OBS). Only CreateComputeShader remains on the
 * path, SEH-guarded. */

/* Bilinear scale of one NV12/P010 frame. The same source serves both formats: the
 * plane types (float / float2) come from the format-override UAV bindings (R8/R8G8
 * for NV12, R16/R16G16 for P010). Pixel-center mapping, UV at half resolution.
 * Source of record for the embedded NV_CS_SCALE_CODE blob: keep in sync with
 * build_x64/gen_cs.cpp when regenerating. */
static const char NV_CS_SCALE_SRC[] =
    "RWTexture2D<float>  src_y  : register(u0);\n"
    "RWTexture2D<float2> src_uv : register(u1);\n"
    "RWTexture2D<float>  dst_y  : register(u2);\n"
    "RWTexture2D<float2> dst_uv : register(u3);\n"
    "cbuffer cb : register(b0) { float4 src_size; float4 dst_size; };\n"
    "\n"
    "float bil1(RWTexture2D<float> t, float2 st)\n"
    "{\n"
    "    float2 i0 = floor(st);\n"
    "    float2 f  = st - i0;\n"
    "    int p0x = (int)clamp(i0.x, 0.0, src_size.x - 1.0);\n"
    "    int p0y = (int)clamp(i0.y, 0.0, src_size.y - 1.0);\n"
    "    int p1x = (int)clamp(i0.x + 1.0, 0.0, src_size.x - 1.0);\n"
    "    int p1y = (int)clamp(i0.y + 1.0, 0.0, src_size.y - 1.0);\n"
    "    float v00 = t.Load(int2(p0x, p0y));\n"
    "    float v10 = t.Load(int2(p1x, p0y));\n"
    "    float v01 = t.Load(int2(p0x, p1y));\n"
    "    float v11 = t.Load(int2(p1x, p1y));\n"
    "    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);\n"
    "}\n"
    "\n"
    "float2 bil2(RWTexture2D<float2> t, float2 st)\n"
    "{\n"
    "    float2 i0 = floor(st);\n"
    "    float2 f  = st - i0;\n"
    "    int p0x = (int)clamp(i0.x, 0.0, src_size.z - 1.0);\n"
    "    int p0y = (int)clamp(i0.y, 0.0, src_size.w - 1.0);\n"
    "    int p1x = (int)clamp(i0.x + 1.0, 0.0, src_size.z - 1.0);\n"
    "    int p1y = (int)clamp(i0.y + 1.0, 0.0, src_size.w - 1.0);\n"
    "    float2 v00 = t.Load(int2(p0x, p0y));\n"
    "    float2 v10 = t.Load(int2(p1x, p0y));\n"
    "    float2 v01 = t.Load(int2(p0x, p1y));\n"
    "    float2 v11 = t.Load(int2(p1x, p1y));\n"
    "    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);\n"
    "}\n"
    "\n"
    "[numthreads(8, 8, 1)]\n"
    "void cs_scale(uint3 tid : SV_DispatchThreadID)\n"
    "{\n"
    "    if (tid.x >= (uint)dst_size.x || tid.y >= (uint)dst_size.y) return;\n"
    "    float2 st = ((float2(tid.xy) + 0.5) / dst_size.xy) * src_size.xy - 0.5;\n"
    "    dst_y[tid.xy] = bil1(src_y, st);\n"
    "    if ((tid.x & 1) == 0 && (tid.y & 1) == 0) {\n"
    "        float2 stc = ((float2(tid.xy) * 0.5 + 0.5) / dst_size.zw) * src_size.zw - 0.5;\n"
    "        dst_uv[tid.xy >> 1] = bil2(src_uv, stc);\n"
    "    }\n"
    "}";

/* Releases only the size-dependent scaler objects (textures/UAVs/constant buffer),
 * keeping the compute shader and compiler DLL alive so a later probe
 * does not have to re-create them on the encode thread. */
static void d3d11_free_gpu_scaler_textures(struct nvenc_data *enc)
{
	struct d3d11_gpu_scaler *gs = &enc->gpu_scaler;
	for (int i = 0; i < 2; i++) {
		if (gs->src_uav[i])
			gs->src_uav[i]->lpVtbl->Release(gs->src_uav[i]);
		if (gs->dst_uav[i])
			gs->dst_uav[i]->lpVtbl->Release(gs->dst_uav[i]);
	}
	if (gs->src_tex)
		gs->src_tex->lpVtbl->Release(gs->src_tex);
	if (gs->dst_tex)
		gs->dst_tex->lpVtbl->Release(gs->dst_tex);
	if (gs->wb_opened)
		gs->wb_opened->lpVtbl->Release(gs->wb_opened);
	if (gs->wb_km_enc)
		gs->wb_km_enc->lpVtbl->Release(gs->wb_km_enc);
	if (gs->wb_km)
		gs->wb_km->lpVtbl->Release(gs->wb_km);
	if (gs->cb)
		gs->cb->lpVtbl->Release(gs->cb);
	gs->src_uav[0] = gs->src_uav[1] = NULL;
	gs->dst_uav[0] = gs->dst_uav[1] = NULL;
	gs->src_tex = NULL;
	gs->dst_tex = NULL;
	gs->wb_opened = NULL;
	gs->wb_km_enc = NULL;
	gs->wb_km = NULL;
	gs->cb = NULL;
	gs->src_w = gs->src_h = gs->dst_w = gs->dst_h = 0;
	gs->ready = false;
}

void d3d11_free_gpu_scaler(struct nvenc_data *enc)
{
	struct d3d11_gpu_scaler *gs = &enc->gpu_scaler;
	d3d11_free_gpu_scaler_textures(enc);
	if (gs->cs) {
		gs->cs->lpVtbl->Release(gs->cs);
		gs->cs = NULL;
	}
}

/* Identify the module owning an exception address for fault diagnostics: fills buf
 * with " in <name> at RVA 0xXXXX" (or leaves it empty when unknown). */
static void nvenc_fault_module_suffix(const void *addr, char *buf, size_t buf_size)
{
	struct module_info {
		void *base;
		uint64_t size;
		HMODULE module;
	};
	typedef BOOL(WINAPI *nv_enum_modules_proc)(HANDLE, struct module_info *, DWORD, LPDWORD, DWORD);

	buf[0] = '\0';
	if (!addr)
		return;

	HMODULE k32 = GetModuleHandleA("kernel32.dll"); /* Win10+ forwards the psapi exports to kernel32 */
	nv_enum_modules_proc enum_mod = k32 ? (nv_enum_modules_proc)GetProcAddress(k32, "EnumProcessModulesEx") : NULL;
	HMODULE psapi = NULL;
	if (!enum_mod) {
		psapi = LoadLibraryA("psapi.dll");
		enum_mod = psapi ? (nv_enum_modules_proc)GetProcAddress(psapi, "EnumProcessModulesEx") : NULL;
	}
	if (!enum_mod)
		return;

	struct module_info mods[256];
	DWORD needed = 0;
	if (!enum_mod(GetCurrentProcess(), mods, sizeof(mods), &needed, 0x02 /* LIST_MODULES_64BIT */))
		goto out;
	const DWORD count = (DWORD)(needed / sizeof(mods[0]));
	for (DWORD i = 0; i < count && i < ARRAYSIZE(mods); i++) {
		uint8_t *base = (uint8_t *)mods[i].base;
		if ((uint8_t *)addr >= base && (uint8_t *)addr < base + mods[i].size) {
			char name[MAX_PATH] = "unknown";
			GetModuleFileNameA(mods[i].module, name, sizeof(name));
			const char *slash = strrchr(name, '\\');
			snprintf(buf, buf_size, " in %s at RVA 0x%lX", slash ? slash + 1 : name, (unsigned long)((uint8_t *)addr - base));
			break;
		}
	}

out:
	if (psapi)
		FreeLibrary(psapi);
}

/* One-time creation of the compute shader backing the staged-resize GPU scaler. Called
 * eagerly from d3d11_init() (while the process state is still clean - see there) and
 * lazily again from d3d11_init_gpu_scaler(); the shader is created on the shared private
 * device (enc->priv_dev), never on the NVENC device. The embedded cs_5_0 bytecode
 * (NV_CS_SCALE_CODE) is used directly, so D3DCompile is not part of this path.
 *
 * CreateComputeShader is wrapped in SEH: on some NVIDIA driver stacks, inside a process
 * that already runs live encoding, it faults with STATUS_STACK_BUFFER_OVERRUN deep
 * inside d3d11.dll / driver code instead of returning an error (verified 2026-10-22:
 * clean single-purpose processes never fault - active session, second device and 300
 * encode frames included - the same call always faults in the OBS process). The fault
 * is caught here, the GPU scaler path is permanently disabled for this encoder instance
 * (shader_faulted latch), and staged resize falls back to the video processor /
 * CPU scaler. */
static bool d3d11_create_gpu_scaler_shader(struct nvenc_data *enc)
{
	struct d3d11_gpu_scaler *gs = &enc->gpu_scaler;

	if (gs->cs)
		return true;
	if (gs->shader_faulted)
		return false;

	ID3D11ComputeShader *cs = NULL;
	EXCEPTION_POINTERS *ep = NULL;
	HRESULT hr;

	__try {
		hr = enc->priv_dev->lpVtbl->CreateComputeShader(enc->priv_dev, NV_CS_SCALE_CODE, NV_CS_SCALE_CODE_SIZE, NULL, &cs);
		if (FAILED(hr)) {
			debug("staged resize unavailable: compute shader creation failed (hr=0x%08lX)", (uint32_t)hr);
			return false;
		}
	} __except ((ep = GetExceptionInformation()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_EXECUTE_HANDLER) {
		char mod[160] = "";
		__try {
			nvenc_fault_module_suffix(ep ? (const void *)ep->ExceptionRecord->ExceptionAddress : NULL, mod, sizeof(mod));
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			/* diagnostics must never mask the original fault */
		}
		warn("staged resize: D3D11 CreateComputeShader faulted (exception 0x%08lX at %p%s); disabling the GPU "
		     "compute-shader scaler for this encoder, staged resizes will use the video processor / CPU scaling",
		     ep ? (unsigned long)ep->ExceptionRecord->ExceptionCode : 0,
		     ep ? (const void *)ep->ExceptionRecord->ExceptionAddress : NULL, mod);
		gs->shader_faulted = true;
		return false;
	}

	gs->cs = cs;
	info("staged resize: GPU compute-shader scaler created on the private device");
	return true;
}

/* Format-override UAVs on the two planes of an NV12/P010 texture. Default (planar)
 * SRV/UAV descriptors are rejected by some drivers, but per-plane override formats
 * are a documented D3D11 feature and work for both reads and writes. */
static bool d3d11_gpu_create_uavs(struct nvenc_data *enc, ID3D11Texture2D *tex, const DXGI_FORMAT fmt,
                                   ID3D11UnorderedAccessView *uav[2])
{
	D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {0};
	ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	ud.Format = (fmt == DXGI_FORMAT_NV12) ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
	HRESULT hr = enc->priv_dev->lpVtbl->CreateUnorderedAccessView(enc->priv_dev, (ID3D11Resource *)tex, &ud, &uav[0]);
	if (FAILED(hr)) {
		debug("gpu scaler: Y plane UAV creation failed (hr=0x%08lX)", (uint32_t)hr);
		return false;
	}
	ud.Format = (fmt == DXGI_FORMAT_NV12) ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM;
	/* The texture lives on the private device, so BOTH plane views must be created there.
	 * Creating the UV view on enc->device (a different ID3D11Device that never opened this
	 * resource) is undefined behavior: the driver accepts it (hr=0) but the resulting view
	 * is broken - reads return zeros and writes are discarded, which decodes as a solid
	 * green frame (valid luma + zero chroma). Verified 2026-10-23 with uav_xdev_probe. */
	hr = enc->priv_dev->lpVtbl->CreateUnorderedAccessView(enc->priv_dev, (ID3D11Resource *)tex, &ud, &uav[1]);
	if (FAILED(hr)) {
		debug("gpu scaler: UV plane UAV creation failed (hr=0x%08lX)", (uint32_t)hr);
		uav[0]->lpVtbl->Release(uav[0]);
		uav[0] = NULL;
		return false;
	}
	return true;
}

static bool d3d11_init_gpu_scaler(struct nvenc_data *enc)
{
	const DXGI_FORMAT fmt = d3d11_format(enc->encoder);
	if (fmt != DXGI_FORMAT_NV12 && fmt != DXGI_FORMAT_P010) {
		debug("staged resize unavailable: no video processor and format %u has no GPU scaler path", (unsigned)fmt);
		return false;
	}

	struct d3d11_gpu_scaler *gs = &enc->gpu_scaler;
	const uint32_t new_w = obs_encoder_get_width(enc->encoder);
	const uint32_t new_h = obs_encoder_get_height(enc->encoder);
	const uint32_t old_w = enc->cx, old_h = enc->cy;

	/* All scaler objects live on the shared private device (see d3d11_init_private_device). */
	if (!d3d11_init_private_device(enc))
		return false;

	/* The compute shader is created here on first use (a fault inside d3d11.dll latches
	 * shader_faulted and is never retried - the CPU readback path remains as fallback). */
	if (!d3d11_create_gpu_scaler_shader(enc))
		return false;

	HRESULT hr;

	D3D11_TEXTURE2D_DESC td = {0};
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

	td.Width = new_w;
	td.Height = new_h;
	hr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, &gs->src_tex);
	if (FAILED(hr)) {
		debug("staged resize unavailable: src texture creation failed (hr=0x%08lX)", (uint32_t)hr);
		d3d11_free_gpu_scaler_textures(enc);
		return false;
	}

	td.Width = old_w;
	td.Height = old_h;
	/* Shared keyed mutex: the NVENC device opens this texture for the final CopyResource. */
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	hr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, &gs->dst_tex);
	if (FAILED(hr)) {
		debug("staged resize unavailable: dst texture creation failed (hr=0x%08lX)", (uint32_t)hr);
		d3d11_free_gpu_scaler_textures(enc);
		return false;
	}

	if (!d3d11_gpu_create_uavs(enc, gs->src_tex, fmt, gs->src_uav) || !d3d11_gpu_create_uavs(enc, gs->dst_tex, fmt, gs->dst_uav)) {
		d3d11_free_gpu_scaler_textures(enc);
		return false;
	}

	D3D11_BUFFER_DESC bd = {0};
	bd.ByteWidth = 32;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	float v[8] = {(float)new_w, (float)new_h, (float)(new_w / 2), (float)(new_h / 2),
	              (float)old_w, (float)old_h, (float)(old_w / 2), (float)(old_h / 2)};
	D3D11_SUBRESOURCE_DATA sd = {v, 32, 32};
	hr = enc->priv_dev->lpVtbl->CreateBuffer(enc->priv_dev, &bd, &sd, &gs->cb);
	if (FAILED(hr)) {
		debug("staged resize unavailable: constant buffer creation failed (hr=0x%08lX)", (uint32_t)hr);
		d3d11_free_gpu_scaler_textures(enc);
		return false;
	}

	/* Make dst_tex readable by the NVENC device for the final CopyResource: shared handle +
	 * keyed mutex, opened on enc->device (the only operation the frame path performs on the
	 * possibly-corrupted device). */
	hr = gs->dst_tex->lpVtbl->QueryInterface(gs->dst_tex, &IID_IDXGIKeyedMutex, (void **)&gs->wb_km);
	if (FAILED(hr)) {
		error_hr("gpu scaler: dst QI(IDXGIKeyedMutex) failed");
		d3d11_free_gpu_scaler_textures(enc);
		return false;
	}

	IDXGIResource *wb_res = NULL;
	HANDLE wb_handle = NULL;
	hr = gs->dst_tex->lpVtbl->QueryInterface(gs->dst_tex, &IID_IDXGIResource, (void **)&wb_res);
	if (SUCCEEDED(hr)) {
		wb_res->lpVtbl->GetSharedHandle(wb_res, &wb_handle);
		wb_res->lpVtbl->Release(wb_res);
	}
	if (!wb_handle) {
		error("gpu scaler: dst GetSharedHandle failed");
		d3d11_free_gpu_scaler_textures(enc);
		return false;
	}
	hr = enc->device->lpVtbl->OpenSharedResource(enc->device, wb_handle, &IID_ID3D11Texture2D, (void **)&gs->wb_opened);
	if (FAILED(hr)) {
		error_hr("gpu scaler: dst OpenSharedResource(NVENC device) failed");
		d3d11_free_gpu_scaler_textures(enc);
		return false;
	}
	hr = gs->wb_opened->lpVtbl->QueryInterface(gs->wb_opened, &IID_IDXGIKeyedMutex, (void **)&gs->wb_km_enc);
	if (FAILED(hr)) {
		error_hr("gpu scaler: dst QI(IDXGIKeyedMutex) on NVENC device failed");
		d3d11_free_gpu_scaler_textures(enc);
		return false;
	}

	gs->src_w = new_w;
	gs->src_h = new_h;
	gs->dst_w = old_w;
	gs->dst_h = old_h;
	gs->ready = true;
	debug("staged resize: GPU compute-shader scaler ready (new %ux%u -> session %ux%u, private device)", new_w, new_h,
	      old_w, old_h);
	return true;
}

/* Scales one staged-resize frame to the session size. Owns the input keyed-mutex cycle for
 * this frame: it acquires with lock_key and releases with *next_key, always on the device
 * that actually reads the shared input (probe T22: on this driver stack a keyed-mutex texture
 * is only readable by a device that holds its own AcquireSync - reading under a lock held by
 * another device silently delivers all-zero data). On return, *lock_handled is true iff the
 * handoff completed (pending key == *next_key); when false, the caller must restore
 * *next_key = lock_key because the lock was never touched. */
static bool d3d11_gpu_scale_frame(struct nvenc_data *enc, struct handle_tex *ht, struct nv_texture *nvtex,
				  uint64_t lock_key, uint64_t *next_key, bool *lock_handled)
{
	struct d3d11_gpu_scaler *gs = &enc->gpu_scaler;
	ID3D11DeviceContext *pctx = enc->priv_ctx; /* private device: input copy + CS dispatch */
	const DXGI_FORMAT fmt = d3d11_format(enc->encoder);
	*lock_handled = false;

	/* The shared input's actual size: the new size while staged, still the old size for one
	 * tick after a rebind, or an intermediate size if the target changed mid-wait. */
	D3D11_TEXTURE2D_DESC idesc = {0};
	ht->tex->lpVtbl->GetDesc(ht->tex, &idesc);

	/* One-tick rebind race: a frame that still carries the OLD size is copied as-is on the
	 * NVENC device, so the input lock is acquired/released there. */
	if (idesc.Width == enc->cx && idesc.Height == enc->cy) {
		ht->km->lpVtbl->AcquireSync(ht->km, lock_key, INFINITE);
		enc->context->lpVtbl->CopyResource(enc->context, (ID3D11Resource *)nvtex->tex, (ID3D11Resource *)ht->tex);
		ht->km->lpVtbl->ReleaseSync(ht->km, *next_key);
		*lock_handled = true;
		return true;
	}

	/* The input size may have changed mid-wait: re-create the src texture + UAVs + CB to match it.
	 * Build the new objects first and only swap them in on success, so a transient creation
	 * failure leaves the previous (valid) objects in place instead of NULL pointers that would
	 * be dereferenced on the next frame. */
	if (gs->src_w != idesc.Width || gs->src_h != idesc.Height) {
		ID3D11Texture2D *ntex = NULL;
		ID3D11UnorderedAccessView *nuav[2] = {NULL};
		ID3D11Buffer *ncb = NULL;

		D3D11_TEXTURE2D_DESC td = {0};
		td.Width = idesc.Width;
		td.Height = idesc.Height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = fmt;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		HRESULT rhr = enc->priv_dev->lpVtbl->CreateTexture2D(enc->priv_dev, &td, NULL, &ntex);
		if (SUCCEEDED(rhr))
			rhr = d3d11_gpu_create_uavs(enc, ntex, fmt, nuav) ? S_OK : E_FAIL;
		if (SUCCEEDED(rhr)) {
			D3D11_BUFFER_DESC bd = {0};
			bd.ByteWidth = 32;
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			float v[8] = {(float)idesc.Width, (float)idesc.Height, (float)(idesc.Width / 2), (float)(idesc.Height / 2),
			              (float)gs->dst_w, (float)gs->dst_h, (float)(gs->dst_w / 2), (float)(gs->dst_h / 2)};
			D3D11_SUBRESOURCE_DATA sd = {v, 32, 32};
			rhr = enc->priv_dev->lpVtbl->CreateBuffer(enc->priv_dev, &bd, &sd, &ncb);
		}
		if (FAILED(rhr)) {
			for (int i = 0; i < 2; i++)
				if (nuav[i])
					nuav[i]->lpVtbl->Release(nuav[i]);
			if (ntex)
				ntex->lpVtbl->Release(ntex);
			if (ncb)
				ncb->lpVtbl->Release(ncb);
			error("gpu scaler: src texture re-creation failed (hr=0x%08lX)", (uint32_t)rhr);
			return false;
		}

		for (int i = 0; i < 2; i++) {
			gs->src_uav[i]->lpVtbl->Release(gs->src_uav[i]);
			gs->src_uav[i] = nuav[i];
		}
		gs->src_tex->lpVtbl->Release(gs->src_tex);
		gs->src_tex = ntex;
		gs->cb->lpVtbl->Release(gs->cb);
		gs->cb = ncb;
		gs->src_w = idesc.Width;
		gs->src_h = idesc.Height;
	}

	/* 1. Pull the new-size shared input into the own intermediate texture. Cross-device
	 *    CopyResource is not allowed, so open the shared input on the private device AND
	 *    acquire its keyed mutex there: the reading device must hold the lock itself (T22). */
	ID3D11Texture2D *in_priv = NULL;
	HRESULT hr = enc->priv_dev->lpVtbl->OpenSharedResource(enc->priv_dev, (HANDLE)(uintptr_t)ht->handle, &IID_ID3D11Texture2D, (void **)&in_priv);
	if (FAILED(hr)) {
		error_hr("gpu scaler: OpenSharedResource on private device failed");
		return false; /* lock untouched */
	}
	IDXGIKeyedMutex *in_km = NULL;
	hr = in_priv->lpVtbl->QueryInterface(in_priv, &IID_IDXGIKeyedMutex, (void **)&in_km);
	if (FAILED(hr)) {
		error_hr("gpu scaler: input QI(IDXGIKeyedMutex) on private device failed");
		in_priv->lpVtbl->Release(in_priv);
		return false; /* lock untouched */
	}
	if (FAILED(in_km->lpVtbl->AcquireSync(in_km, lock_key, INFINITE))) {
		error("gpu scaler: input AcquireSync (private device) failed");
		in_km->lpVtbl->Release(in_km);
		in_priv->lpVtbl->Release(in_priv);
		return false; /* lock untouched */
	}
	pctx->lpVtbl->CopyResource(pctx, (ID3D11Resource *)gs->src_tex, (ID3D11Resource *)in_priv);

	in_km->lpVtbl->ReleaseSync(in_km, *next_key);
	*lock_handled = true;
	in_km->lpVtbl->Release(in_km);
	in_priv->lpVtbl->Release(in_priv);

	/* 2. Bilinear scale to the session size via format-override UAVs. The writeback keyed
	 *    mutex is held (private side) while the compute shader writes dst_tex, mirroring the
	 *    proven CPU-scaler locking pattern: private side acquire/release, then NVENC side
	 *    acquire/release around the final copy. */
	if (FAILED(gs->wb_km->lpVtbl->AcquireSync(gs->wb_km, 0, INFINITE))) {
		error("gpu scaler: writeback AcquireSync failed");
		return false; /* input lock already handed off */
	}
	ID3D11UnorderedAccessView *uavs[4] = {gs->src_uav[0], gs->src_uav[1], gs->dst_uav[0], gs->dst_uav[1]};
	pctx->lpVtbl->CSSetUnorderedAccessViews(pctx, 0, 4, uavs, NULL);
	pctx->lpVtbl->CSSetShader(pctx, gs->cs, NULL, 0);
	pctx->lpVtbl->CSSetConstantBuffers(pctx, 0, 1, &gs->cb);
	pctx->lpVtbl->Dispatch(pctx, (unsigned int)((enc->cx + 7) / 8), (unsigned int)((enc->cy + 7) / 8), 1);
	ID3D11ComputeShader *null_cs = NULL;
	ID3D11UnorderedAccessView *null_uavs[4] = {NULL, NULL, NULL, NULL};
	pctx->lpVtbl->CSSetShader(pctx, null_cs, NULL, 0);
	pctx->lpVtbl->CSSetUnorderedAccessViews(pctx, 0, 4, null_uavs, NULL);
	gs->wb_km->lpVtbl->ReleaseSync(gs->wb_km, 0);

	/* 3. Copy the scaled session-size frame into the registered input texture - the only
	 *    operation that touches the NVENC device. */
	if (FAILED(gs->wb_km_enc->lpVtbl->AcquireSync(gs->wb_km_enc, 0, INFINITE))) {
		error_hr("gpu scaler: writeback AcquireSync (NVENC device) failed");
		return false; /* input lock already handed off */
	}
	enc->context->lpVtbl->CopyResource(enc->context, (ID3D11Resource *)nvtex->tex, (ID3D11Resource *)gs->wb_opened);
	gs->wb_km_enc->lpVtbl->ReleaseSync(gs->wb_km_enc, 0);
	return true;
}

bool d3d11_staged_resize_supported(struct nvenc_data *enc)
{
	if (enc->vp_processor && enc->vp_dst_w == enc->cx && enc->vp_dst_h == enc->cy)
		return true;
	if (enc->gpu_scaler.ready) {
		if (enc->gpu_scaler.dst_w == enc->cx && enc->gpu_scaler.dst_h == enc->cy)
			return true;
		d3d11_free_gpu_scaler_textures(enc); /* session size changed since creation - re-create below; the
		                                       * compute shader stays alive */
	}
	if (enc->cpu_scaler.ready) {
		if (enc->cpu_scaler.old_w == enc->cx && enc->cpu_scaler.old_h == enc->cy)
			return true;
		d3d11_free_cpu_scaler(enc); /* session size changed since creation - re-create below */
	}

	/* The processor's content description fixes the output size at creation time, so a
	 * cached processor from an earlier session size must be recreated. Input and output
	 * views are bound to the enumerator they were created with, so invalidate the cached
	 * ones as well. */
	if (enc->vp_processor) {
		d3d11_free_video_processor(enc);
		for (size_t i = 0; i < enc->input_textures.num; i++) {
			struct handle_tex *ht = &enc->input_textures.array[i];
			if (ht->vp_view) {
				ht->vp_view->lpVtbl->Release(ht->vp_view);
				ht->vp_view = NULL;
			}
		}
		for (size_t i = 0; i < enc->textures.num; i++) {
			struct nv_texture *nvtex = &enc->textures.array[i];
			if (nvtex->vp_view) {
				nvtex->vp_view->lpVtbl->Release(nvtex->vp_view);
				nvtex->vp_view = NULL;
			}
		}
	}

	/* Fallback order (diagnostic 2026-10-21): try the GPU compute-shader scaler BEFORE the
	 * video processor. On some driver/adapter combinations CreateVideoProcessorEnumerator
	 * fails with E_INVALIDARG for every content desc, and CreateComputeShader on the NVENC
	 * device itself faults with an access violation inside d3d11.dll (crash reports
	 * 2026-10-20/21: c0000005 at d3d11.dll RVA 0xE182, reproducible on every live resize) -
	 * the GPU scaler therefore runs on the shared private device (SEH-guarded there too),
	 * and the CPU readback path (NV12/P010 only) remains the last fallback. */
	if (d3d11_init_gpu_scaler(enc))
		return true;

	ID3D11VideoDevice *vdev = NULL;
	ID3D11VideoContext *vctx = NULL;
	HRESULT hr = enc->device->lpVtbl->QueryInterface(enc->device, &IID_ID3D11VideoDevice, (void **)&vdev);
	if (FAILED(hr)) {
		warn("staged resize: no ID3D11VideoDevice - using the GPU compute-shader / CPU readback fallback");
		vdev = NULL;
	} else {
		hr = enc->context->lpVtbl->QueryInterface(enc->context, &IID_ID3D11VideoContext, (void **)&vctx);
	}
	if (SUCCEEDED(hr)) {
		struct nv_vp_content_desc desc = {0};
		desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
		desc.InputFrameRate.Numerator = 30;
		desc.InputFrameRate.Denominator = 1;
		desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
		desc.OutputWidth = enc->cx; /* current session size (scale destination) */
		desc.OutputHeight = enc->cy;

		ID3D11VideoProcessorEnumerator *vp_enum = NULL;
		hr = vdev->lpVtbl->CreateVideoProcessorEnumerator(vdev, (const D3D11_VIDEO_PROCESSOR_CONTENT_DESC *)&desc,
		                                                   &vp_enum);
		if (SUCCEEDED(hr) && vp_enum) {
			UINT flags = 0;
			/* Validate the active input format against the processor before using it. */
			HRESULT fmt_hr = vp_enum->lpVtbl->CheckVideoProcessorFormat(vp_enum, d3d11_format(enc->encoder), &flags);
			if (SUCCEEDED(fmt_hr)) {
				ID3D11VideoProcessor *vp = NULL;
				HRESULT cr_hr = vdev->lpVtbl->CreateVideoProcessor(vdev, vp_enum, 0, &vp);
				if (SUCCEEDED(cr_hr) && vp) {
					enc->video_device = vdev;
					enc->video_context = vctx;
					enc->vp_enumerator = vp_enum;
					enc->vp_processor = vp;
					enc->vp_dst_w = enc->cx;
					enc->vp_dst_h = enc->cy;
					vdev = NULL;
					vctx = NULL;
					vp_enum = NULL;
					return true;
				}
				if (vp)
					vp->lpVtbl->Release(vp);
				warn("staged resize unavailable: CreateVideoProcessor failed (hr=0x%08lX)", (uint32_t)cr_hr);
			} else {
				warn("staged resize unavailable: input format rejected by video processor (hr=0x%08lX)",
				     (uint32_t)fmt_hr);
			}
			vp_enum->lpVtbl->Release(vp_enum);
		} else {
			warn("staged resize unavailable: CreateVideoProcessorEnumerator failed (hr=0x%08lX)", (uint32_t)hr);
		}
	}
	if (vctx)
		vctx->lpVtbl->Release(vctx);
	if (vdev)
		vdev->lpVtbl->Release(vdev);

	return d3d11_init_cpu_scaler(enc);
}

static ID3D11VideoProcessorInputView *d3d11_get_input_view(struct nvenc_data *enc, struct handle_tex *ht)
{
	if (ht->vp_view)
		return ht->vp_view;

	struct nv_vp_input_view_desc desc = {0};
	const DXGI_FORMAT fmt = d3d11_format(enc->encoder);
	desc.FourCC = fmt;
	/* 4:2:0 planar input (NV12/P010) carries full-resolution Y plus half-resolution CbCr;
	 * packed formats (AYUV/Y410) are not subsampled. */
	desc.SubsampleMode = (fmt == DXGI_FORMAT_NV12 || fmt == DXGI_FORMAT_P010) ? NV_VP_SUBSAMPLE_Y_CbCr : NV_VP_SUBSAMPLE_NONE;
	desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	desc.Texture2D.MipSlice = 0;

	ID3D11VideoProcessorInputView *view = NULL;
	HRESULT hr = enc->video_device->lpVtbl->CreateVideoProcessorInputView(enc->video_device, (ID3D11Resource *)ht->tex, enc->vp_enumerator, (const D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC *)&desc, &view);
	if (FAILED(hr)) {
		debug("CreateVideoProcessorInputView failed (hr=0x%08lX)", (uint32_t)hr);
		return NULL;
	}
	ht->vp_view = view;
	return view;
}

static ID3D11VideoProcessorOutputView *d3d11_get_output_view(struct nvenc_data *enc, struct nv_texture *nvtex)
{
	if (nvtex->vp_view)
		return nvtex->vp_view;

	struct nv_vp_output_view_desc desc = {0};
	desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	desc.Texture2D.MipSlice = 0;

	ID3D11VideoProcessorOutputView *view = NULL;
	HRESULT hr = enc->video_device->lpVtbl->CreateVideoProcessorOutputView(enc->video_device, (ID3D11Resource *)nvtex->tex, enc->vp_enumerator, (const D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC *)&desc, &view);
	if (FAILED(hr)) {
		debug("CreateVideoProcessorOutputView failed (hr=0x%08lX)", (uint32_t)hr);
		return NULL;
	}
	nvtex->vp_view = view;
	return view;
}

bool d3d11_scale_to_session_size(struct nvenc_data *enc, struct handle_tex *ht, struct nv_texture *nvtex)
{
	if (enc->vp_processor) {
		ID3D11VideoProcessorInputView *in_view = d3d11_get_input_view(enc, ht);
		ID3D11VideoProcessorOutputView *out_view = in_view ? d3d11_get_output_view(enc, nvtex) : NULL;
		bool ok = false;

		if (in_view && out_view) {
			struct nv_vp_stream stream = {0};
			stream.Enable = TRUE;
			stream.OutputIndex = 0;
			stream.InputFrameOrField = 0; /* progressive */
			stream.pInputSurface = in_view;

			HRESULT hr = enc->video_context->lpVtbl->VideoProcessorBlt(enc->video_context, enc->vp_processor, out_view, 0,
			                                                            1, (const D3D11_VIDEO_PROCESSOR_STREAM *)&stream);
			ok = SUCCEEDED(hr);
			if (!ok)
				debug("VideoProcessorBlt failed (hr=0x%08lX)", (uint32_t)hr);
		}

		if (!ok) {
			/* The video processor cannot handle the current input - e.g. this driver stack
			 * rejects CreateVideoProcessorInputView for the new-size shared texture with
			 * E_INVALIDARG even though the processor itself was created fine. Drop it for
			 * this encoder so later frames go straight to the GPU/CPU scaler, and fall
			 * through instead of failing the frame (a failed frame errors the whole
			 * stream). The next staged resize re-probes the VP in d3d11_staged_resize_supported. */
			d3d11_free_video_processor(enc);
			enc->vp_dst_w = 0;
			enc->vp_dst_h = 0;
			for (size_t i = 0; i < enc->input_textures.num; i++) {
				struct handle_tex *t = &enc->input_textures.array[i];
				if (t->vp_view) {
					t->vp_view->lpVtbl->Release(t->vp_view);
					t->vp_view = NULL;
				}
			}
			for (size_t i = 0; i < enc->textures.num; i++) {
				struct nv_texture *t = &enc->textures.array[i];
				if (t->vp_view) {
					t->vp_view->lpVtbl->Release(t->vp_view);
					t->vp_view = NULL;
				}
			}
			warn("staged resize: video processor path failed, falling back to the GPU/CPU scaler");
		} else {
			return true;
		}
	}

	if (!enc->cpu_scaler.ready && !d3d11_init_cpu_scaler(enc))
		return false;

	return d3d11_cpu_scale_frame(enc, ht, nvtex);
}

/* ------------------------------------------------------------------------- */
/* Actual encoding stuff                                                     */

static struct handle_tex *get_ht_from_handle(struct nvenc_data *enc, uint32_t handle)
{
	ID3D11Device *device = enc->device;
	IDXGIKeyedMutex *km;
	ID3D11Texture2D *input_tex;
	HRESULT hr;

	for (size_t i = 0; i < enc->input_textures.num; i++) {
		struct handle_tex *ht = &enc->input_textures.array[i];
		if (ht->handle == handle)
			return ht;
	}

	info("new input texture handle %u", (unsigned)handle);
	hr = device->lpVtbl->OpenSharedResource(device, (HANDLE)(uintptr_t)handle, &IID_ID3D11Texture2D, &input_tex);
	if (FAILED(hr)) {
		error_hr("OpenSharedResource failed");
		return NULL;
	}

	hr = input_tex->lpVtbl->QueryInterface(input_tex, &IID_IDXGIKeyedMutex, &km);
	if (FAILED(hr)) {
		error_hr("QueryInterface(IDXGIKeyedMutex) failed");
		input_tex->lpVtbl->Release(input_tex);
		return NULL;
	}

	input_tex->lpVtbl->SetEvictionPriority(input_tex, DXGI_RESOURCE_PRIORITY_MAXIMUM);

	struct handle_tex new_ht = {handle, input_tex, km, NULL};
	da_push_back(enc->input_textures, &new_ht);
	return &enc->input_textures.array[enc->input_textures.num - 1];
}

bool d3d11_encode(void *data, struct encoder_texture *texture, int64_t pts, uint64_t lock_key, uint64_t *next_key,
		  struct encoder_packet *packet, bool *received_packet)
{
	struct nvenc_data *enc = data;
	ID3D11DeviceContext *context = enc->context;
	struct handle_tex *ht;
	ID3D11Texture2D *output_tex;
	IDXGIKeyedMutex *km;
	struct nv_texture *nvtex;
	struct nv_bitstream *bs;

	/* Multitrack live changes (staged resize / staged FPS change, timed re-init at the shared
	 * keyframe boundary): handled before any input copy so a boundary re-init can recreate the
	 * session and input textures at the new size before this frame is copied. */
	if (!nvenc_prepare_frame(enc, pts)) {
		*next_key = lock_key;
		*received_packet = false;
		return true; // skip this frame - encoder is transitioning
	}

	if (texture->handle == GS_INVALID_HANDLE) {
		error("Encode failed: bad texture handle");
		*next_key = lock_key;
		return false;
	}

	bs = &enc->bitstreams.array[enc->next_bitstream];
	nvtex = &enc->textures.array[enc->next_bitstream];

	ht = get_ht_from_handle(enc, texture->handle);
	output_tex = nvtex->tex;

	if (!ht) {
		*next_key = lock_key;
		return false;
	}
	km = ht->km;

	/* ------------------------------------ */
	/* copy to output tex                   */

	if (enc->resize_pending && enc->gpu_scaler.ready) {
		/* Staged resize waiting for the shared keyframe boundary. The GPU scaler reads the
		 * shared input on the private device, and on this driver stack a keyed-mutex texture
		 * is only readable by a device that holds its own AcquireSync (probe T22), so the whole
		 * acquire/release cycle happens inside d3d11_gpu_scale_frame on the reading side. On
		 * return the lock has always been handed off: pending key == *next_key (or *next_key was
		 * restored to lock_key because nothing touched the lock). */
		bool lock_handled = false;
		if (!d3d11_gpu_scale_frame(enc, ht, nvtex, lock_key, next_key, &lock_handled)) {
			enc->scale_fail_count++;
			if (!lock_handled)
				*next_key = lock_key; /* could not even acquire: leave the chain untouched */

			if (enc->scale_fail_count < 2)
				return true; // skip this frame - retry scaling on the next tick

			/* Two consecutive failures: no scaler can handle this driver. Apply the staged
			 * re-init immediately (off-grid) so the stream survives instead of erroring out;
			 * the forced IDR re-anchors keyframe alignment from here. */
			warn("staged resize: scaling failed twice in a row - applying the session "
			     "re-init immediately (off-grid)");
			if (!nvenc_apply_staged_changes_now(enc, pts))
				return false;

			/* The session is now at the new size: the ring textures were recreated by the
			 * re-init, so re-fetch this bitstream slot before encoding. */
			bs = &enc->bitstreams.array[enc->next_bitstream];
			nvtex = &enc->textures.array[enc->next_bitstream];
			output_tex = nvtex->tex;

			D3D11_TEXTURE2D_DESC idesc = {0};
			ht->tex->lpVtbl->GetDesc(ht->tex, &idesc);
			if (idesc.Width == enc->cx && idesc.Height == enc->cy) {
				/* The lock was handed off with *next_key: re-acquire it and release it
				 * unchanged for this direct copy. */
				km->lpVtbl->AcquireSync(km, *next_key, INFINITE);
				context->lpVtbl->CopyResource(context, (ID3D11Resource *)output_tex,
				                              (ID3D11Resource *)ht->tex);
				km->lpVtbl->ReleaseSync(km, *next_key);
			} else {
				return true; // skip this frame - next tick encodes at the new size
			}
		} else {
			enc->scale_fail_count = 0;
		}
	} else {
		km->lpVtbl->AcquireSync(km, lock_key, INFINITE);

		if (enc->resize_pending) {
			/* Staged resize on the CPU/VP fallback paths: the input is read under this lock.
			 * (The CPU path's private-device read is not legal here and delivers zeros on this
			 * stack - after two such failures the off-grid re-init below rescues the stream.) */
			if (!d3d11_scale_to_session_size(enc, ht, nvtex)) {
				enc->scale_fail_count++;
				if (enc->scale_fail_count < 2) {
					/* Skip this frame - retry scaling on the next tick. Release with *next_key and
					 * leave it untouched: the keyed-mutex "pending key" is set to the release key,
					 * and the next acquirer in the chain (or the render thread, if we are the last
					 * encoder) must acquire with exactly that key. Overwriting *next_key here would
					 * desync the handoff and wedge the render thread in AcquireSync. */
					km->lpVtbl->ReleaseSync(km, *next_key);
					return true; // skip this frame - retry scaling on the next tick
				}

				/* Two consecutive failures: no scaler can handle this driver (e.g. Map on planar
				 * plane 1 is rejected with E_INVALIDARG). Apply the staged re-init immediately
				 * (off-grid) so the stream survives instead of erroring out; the forced IDR
				 * re-anchors keyframe alignment from here. */
				warn("staged resize: scaling failed twice in a row - applying the session "
				     "re-init immediately (off-grid)");
				if (!nvenc_apply_staged_changes_now(enc, pts)) {
					km->lpVtbl->ReleaseSync(km, *next_key);
					return false;
				}

				/* The session is now at the new size: the ring textures were recreated by the
				 * re-init, so re-fetch this bitstream slot before encoding. */
				bs = &enc->bitstreams.array[enc->next_bitstream];
				nvtex = &enc->textures.array[enc->next_bitstream];
				output_tex = nvtex->tex;

				D3D11_TEXTURE2D_DESC idesc = {0};
				ht->tex->lpVtbl->GetDesc(ht->tex, &idesc);
				if (idesc.Width == enc->cx && idesc.Height == enc->cy) {
					context->lpVtbl->CopyResource(context, (ID3D11Resource *)output_tex,
					                              (ID3D11Resource *)ht->tex);
				} else {
					/* One-tick rebind race: the frame still carries the old size - skip it. */
					km->lpVtbl->ReleaseSync(km, *next_key);
					return true; // skip this frame - next tick encodes at the new size
				}
			} else {
				enc->scale_fail_count = 0;
			}
		} else {
			context->lpVtbl->CopyResource(context, (ID3D11Resource *)output_tex, (ID3D11Resource *)ht->tex);
		}

		km->lpVtbl->ReleaseSync(km, *next_key);
	}

	/* ------------------------------------ */
	/* map output tex so nvenc can use it   */

	NV_ENC_MAP_INPUT_RESOURCE map = {NV_ENC_MAP_INPUT_RESOURCE_VER};
	map.registeredResource = nvtex->res;
	NVENCSTATUS err = nv.nvEncMapInputResource(enc->session, &map);
	if (NV_FAILED(err)) {
		error("nvEncMapInputResource failed at %ux%u (err=%d)", enc->cx, enc->cy, (int)err);
		return false; // dts not pushed: no frame was submitted this tick
	}

	nvtex->mapped_res = map.mappedResource;

	deque_push_back(&enc->dts_list, &pts, sizeof(pts)); // only after a successful submission below

	/* ------------------------------------ */
	/* do actual encode call                */

	return nvenc_encode_base(enc, bs, nvtex->mapped_res, pts, packet, received_packet);
}
