#include "common_utils.h"

// ATTENTION: If D3D surfaces are used, DX9_D3D or DX11_D3D must be set in project settings or hardcoded here

#ifdef DX9_D3D
#include "common_directx.h"
#elif DX11_D3D
#include "common_directx11.h"
#include "common_directx9.h"
#endif

#include <util/windows/device-enum.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/pipe.h>
#include <util/dstr.h>

#include <intrin.h>
#include <inttypes.h>

/* =======================================================
 * Windows implementation of OS-specific utility functions
 */

mfxStatus Initialize(mfxIMPL impl, mfxVersion ver, MFXVideoSession *pSession,
		     mfxFrameAllocator *pmfxAllocator, mfxHDL *deviceHandle,
		     bool bCreateSharedHandles, bool dx9hack)
{
	bCreateSharedHandles; // (Lain) Currently unused
	pmfxAllocator;        // (Lain) Currently unused

	mfxStatus sts = MFX_ERR_NONE;

	// If mfxFrameAllocator is provided it means we need to setup DirectX device and memory allocator
	if (pmfxAllocator && !dx9hack) {
		// Initialize Intel Media SDK Session
		sts = pSession->Init(impl, &ver);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		// Create DirectX device context
		if (deviceHandle == NULL || *deviceHandle == NULL) {
			sts = CreateHWDevice(*pSession, deviceHandle, NULL,
					     bCreateSharedHandles);
			MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
		}

		if (deviceHandle == NULL || *deviceHandle == NULL)
			return MFX_ERR_DEVICE_FAILED;

		// Provide device manager to Media SDK
		sts = pSession->SetHandle(DEVICE_MGR_TYPE, *deviceHandle);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		pmfxAllocator->pthis =
			*pSession; // We use Media SDK session ID as the allocation identifier
		pmfxAllocator->Alloc = simple_alloc;
		pmfxAllocator->Free = simple_free;
		pmfxAllocator->Lock = simple_lock;
		pmfxAllocator->Unlock = simple_unlock;
		pmfxAllocator->GetHDL = simple_gethdl;

		// Since we are using video memory we must provide Media SDK with an external allocator
		sts = pSession->SetFrameAllocator(pmfxAllocator);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	} else if (pmfxAllocator && dx9hack) {
		// Initialize Intel Media SDK Session
		sts = pSession->Init(impl, &ver);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		// Create DirectX device context
		if (deviceHandle == NULL || *deviceHandle == NULL) {
			sts = DX9_CreateHWDevice(*pSession, deviceHandle, NULL,
						 false);
			MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
		}
		if (*deviceHandle == NULL)
			return MFX_ERR_DEVICE_FAILED;

		// Provide device manager to Media SDK
		sts = pSession->SetHandle(MFX_HANDLE_D3D9_DEVICE_MANAGER,
					  *deviceHandle);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		pmfxAllocator->pthis =
			*pSession; // We use Media SDK session ID as the allocation identifier
		pmfxAllocator->Alloc = dx9_simple_alloc;
		pmfxAllocator->Free = dx9_simple_free;
		pmfxAllocator->Lock = dx9_simple_lock;
		pmfxAllocator->Unlock = dx9_simple_unlock;
		pmfxAllocator->GetHDL = dx9_simple_gethdl;

		// Since we are using video memory we must provide Media SDK with an external allocator
		sts = pSession->SetFrameAllocator(pmfxAllocator);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	} else {
		// Initialize Intel Media SDK Session
		sts = pSession->Init(impl, &ver);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	}
	return sts;
}

void Release()
{
#if defined(DX9_D3D) || defined(DX11_D3D)
	CleanupHWDevice();
	DX9_CleanupHWDevice();
#endif
}

void mfxGetTime(mfxTime *timestamp)
{
	QueryPerformanceCounter(timestamp);
}

double TimeDiffMsec(mfxTime tfinish, mfxTime tstart)
{
	static LARGE_INTEGER tFreq = {0};

	if (!tFreq.QuadPart)
		QueryPerformanceFrequency(&tFreq);

	double freq = (double)tFreq.QuadPart;
	return 1000.0 * ((double)tfinish.QuadPart - (double)tstart.QuadPart) /
	       freq;
}

void util_cpuid(int cpuinfo[4], int flags)
{
	return __cpuid(cpuinfo, flags);
}

static bool enum_luids(void *param, uint32_t idx, uint64_t luid)
{
	struct dstr *cmd = (struct dstr *)param;
	dstr_catf(cmd, " %" PRIx64, luid);
	UNUSED_PARAMETER(idx);
	return true;
}

void check_adapters(struct adapter_info *adapters, size_t *adapter_count)
{
	char *test_exe = os_get_executable_path_ptr("obs-qsv-test.exe");
	struct dstr cmd = {0};
	struct dstr caps_str = {0};
	os_process_pipe_t *pp = nullptr;
	config_t *config = nullptr;
	const char *error = nullptr;
	size_t config_adapter_count;

	dstr_copy(&cmd, test_exe);
	enum_graphics_device_luids(enum_luids, &cmd);

	pp = os_process_pipe_create(cmd.array, "r");
	if (!pp) {
		blog(LOG_INFO, "Failed to launch the QSV test process I guess");
		goto fail;
	}

	for (;;) {
		char data[2048];
		size_t len =
			os_process_pipe_read(pp, (uint8_t *)data, sizeof(data));
		if (!len)
			break;

		dstr_ncat(&caps_str, data, len);
	}

	if (dstr_is_empty(&caps_str)) {
		blog(LOG_INFO, "Seems the QSV test subprocess crashed. "
			       "Better there than here I guess. "
			       "Let's just skip loading QSV then I suppose.");
		goto fail;
	}

	if (config_open_string(&config, caps_str.array) != 0) {
		blog(LOG_INFO, "Couldn't open QSV configuration string");
		goto fail;
	}

	error = config_get_string(config, "error", "string");
	if (error) {
		blog(LOG_INFO, "Error querying QSV support: %s", error);
		goto fail;
	}

	config_adapter_count = config_num_sections(config);

	if (config_adapter_count < *adapter_count)
		*adapter_count = config_adapter_count;

	for (size_t i = 0; i < *adapter_count; i++) {
		char section[16];
		snprintf(section, sizeof(section), "%d", (int)i);

		struct adapter_info *adapter = &adapters[i];
		adapter->is_intel =
			config_get_bool(config, section, "is_intel");
		adapter->is_dgpu = config_get_bool(config, section, "is_dgpu");
		adapter->supports_av1 =
			config_get_bool(config, section, "supports_av1");
		adapter->supports_hevc =
			config_get_bool(config, section, "supports_hevc");
	}

fail:
	config_close(config);
	dstr_free(&caps_str);
	dstr_free(&cmd);
	os_process_pipe_destroy(pp);
	bfree(test_exe);
}

/* (Lain) Functions currently unused */
#if 0
void ClearYUVSurfaceVMem(mfxMemId memId)
{
#if defined(DX9_D3D) || defined(DX11_D3D)
    ClearYUVSurfaceD3D(memId);
#endif
}

void ClearRGBSurfaceVMem(mfxMemId memId)
{
#if defined(DX9_D3D) || defined(DX11_D3D)
    ClearRGBSurfaceD3D(memId);
#endif
}
#endif
