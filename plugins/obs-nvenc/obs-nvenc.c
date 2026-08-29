#include <obs-module.h>

#include "obs-nvenc.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-nvenc", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "NVIDIA Encoder (NVENC) Plugin";
}

bool obs_module_load(void)
{
	if (!nvenc_supported()) {
		blog(LOG_INFO, "NVENC not supported");
		return false;
	}

	blog(LOG_INFO, "[obs-nvenc] build-stamp: keyframe-boundary staging v6 (post-reset queue rewind + resources registered after resize reconfigure; silent-failure logging)");

	obs_nvenc_load();
	obs_cuda_load();

	return true;
}

void obs_module_unload(void)
{
	obs_cuda_unload();
	obs_nvenc_unload();
}
