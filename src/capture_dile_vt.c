// Video capture backend using libdile_vt (webOS 3.x - 4.x)

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <dile_vt.h>

#include "capture.h"
#include "log.h"

static DILE_VT_HANDLE vth = NULL;
static DILE_OUTPUTDEVICE_STATE output_state;
static DILE_VT_FRAMEBUFFER_PROPERTY vfbprop;
static DILE_VT_FRAMEBUFFER_CAPABILITY vfbcap;
static uint8_t*** vfbs = NULL;
static size_t vfb_size = 0;
static int mem_fd = -1;
static bool started = false;

static void set_freeze(bool freeze) {
	output_state.freezed = freeze;
	DILE_VT_SetVideoFrameOutputDeviceState(vth, DILE_VT_VIDEO_FRAME_OUTPUT_DEVICE_STATE_FREEZED, &output_state);
}

static void unmap_buffers() {
	if (vfbs != NULL) {
		for (uint32_t vfb = 0; vfb < vfbcap.numVfbs; vfb++) {
			if (vfbs[vfb] == NULL) {
				continue;
			}
			for (uint32_t plane = 0; plane < vfbcap.numPlanes; plane++) {
				if (vfbs[vfb][plane] != NULL && vfbs[vfb][plane] != MAP_FAILED) {
					munmap(vfbs[vfb][plane], vfb_size);
				}
			}
			free(vfbs[vfb]);
		}
		free(vfbs);
		vfbs = NULL;
	}

	if (vfbprop.ptr != NULL) {
		for (uint32_t vfb = 0; vfb < vfbcap.numVfbs; vfb++) {
			free(vfbprop.ptr[vfb]);
		}
		free(vfbprop.ptr);
		vfbprop.ptr = NULL;
	}

	if (mem_fd >= 0) {
		close(mem_fd);
		mem_fd = -1;
	}
}

int video_capture_init(uint32_t width, uint32_t height, uint32_t framerate) {
	int ret;

	if ((vth = DILE_VT_Create(0)) == NULL) {
		ERR("DILE_VT_Create failed");
		return -1;
	}

	DILE_VT_VIDEO_FRAME_OUTPUT_DEVICE_LIMITATION limitation;
	if (DILE_VT_GetVideoFrameOutputDeviceLimitation(vth, &limitation) != 0) {
		ERR("DILE_VT_GetVideoFrameOutputDeviceLimitation failed");
		ret = -2;
		goto err_destroy;
	}

	DBG("dile_vt max resolution: %dx%d, scale down limit: %dx%d",
		limitation.maxResolution.width, limitation.maxResolution.height,
		limitation.scaleDownLimitWidth, limitation.scaleDownLimitHeight);

	int dump_location = DILE_VT_DISPLAY_OUTPUT;
	if (DILE_VT_SetVideoFrameOutputDeviceDumpLocation(vth, dump_location) != 0) {
		WARN("DISPLAY dump location failed, attempting SCALER...");
		dump_location = DILE_VT_SCALER_OUTPUT;
		if (DILE_VT_SetVideoFrameOutputDeviceDumpLocation(vth, dump_location) != 0) {
			ERR("DILE_VT_SetVideoFrameOutputDeviceDumpLocation failed");
			ret = -3;
			goto err_destroy;
		}
	}

	if (limitation.maxResolution.width != 0 && width > limitation.maxResolution.width) {
		height = height * limitation.maxResolution.width / width;
		width = limitation.maxResolution.width;
	}
	if (limitation.maxResolution.height != 0 && height > limitation.maxResolution.height) {
		width = width * limitation.maxResolution.height / height;
		height = limitation.maxResolution.height;
	}

	DILE_VT_RECT region = {0, 0, width, height};
	if (DILE_VT_SetVideoFrameOutputDeviceOutputRegion(vth, dump_location, &region) != 0) {
		ERR("DILE_VT_SetVideoFrameOutputDeviceOutputRegion failed");
		ret = -4;
		goto err_destroy;
	}

	// Framerate can only be limited using a divider of content framerate
	// (usually 50/60fps).
	memset(&output_state, 0, sizeof(output_state));
	output_state.framerate = framerate == 0 || framerate >= 60 ? 1 : 60 / framerate;
	if (DILE_VT_SetVideoFrameOutputDeviceState(vth, DILE_VT_VIDEO_FRAME_OUTPUT_DEVICE_STATE_FRAMERATE_DIVIDE, &output_state) != 0) {
		ERR("DILE_VT_SetVideoFrameOutputDeviceState(FRAMERATE_DIVIDE) failed");
		ret = -5;
		goto err_destroy;
	}

	if (DILE_VT_SetVideoFrameOutputDeviceState(vth, DILE_VT_VIDEO_FRAME_OUTPUT_DEVICE_STATE_FREEZED, &output_state) != 0) {
		ERR("DILE_VT_SetVideoFrameOutputDeviceState(FREEZED) failed");
		ret = -6;
		goto err_destroy;
	}

	if (DILE_VT_GetVideoFrameBufferCapability(vth, &vfbcap) != 0) {
		ERR("DILE_VT_GetVideoFrameBufferCapability failed");
		ret = -7;
		goto err_destroy;
	}

	// Physical memory offsets table - ptr[numVfbs][numPlanes]
	vfbprop.ptr = calloc(vfbcap.numVfbs, sizeof(uint32_t*));
	for (uint32_t vfb = 0; vfb < vfbcap.numVfbs; vfb++) {
		vfbprop.ptr[vfb] = calloc(vfbcap.numPlanes, sizeof(uint32_t));
	}

	if (DILE_VT_GetAllVideoFrameBufferProperty(vth, &vfbcap, &vfbprop) != 0) {
		ERR("DILE_VT_GetAllVideoFrameBufferProperty failed");
		ret = -8;
		goto err_unmap;
	}

	INFO("dile_vt: vfbs: %d, planes: %d, pixel format: %d, %dx%d, stride: %d",
		vfbcap.numVfbs, vfbcap.numPlanes, vfbprop.pixelFormat, vfbprop.width, vfbprop.height, vfbprop.stride);

	if ((mem_fd = open("/dev/mem", O_RDONLY | O_SYNC)) < 0) {
		ERR("/dev/mem open failed: %s", strerror(errno));
		ret = -9;
		goto err_unmap;
	}

	vfb_size = vfbprop.stride * vfbprop.height;
	vfbs = calloc(vfbcap.numVfbs, sizeof(uint8_t**));
	for (uint32_t vfb = 0; vfb < vfbcap.numVfbs; vfb++) {
		vfbs[vfb] = calloc(vfbcap.numPlanes, sizeof(uint8_t*));
		for (uint32_t plane = 0; plane < vfbcap.numPlanes; plane++) {
			vfbs[vfb][plane] = mmap(0, vfb_size, PROT_READ, MAP_SHARED, mem_fd, vfbprop.ptr[vfb][plane]);
			if (vfbs[vfb][plane] == MAP_FAILED) {
				ERR("mmap of vfb[%d][%d] failed: %s", vfb, plane, strerror(errno));
				ret = -10;
				goto err_unmap;
			}
		}
	}

	switch (vfbprop.pixelFormat) {
	case DILE_VT_VIDEO_FRAME_BUFFER_PIXEL_FORMAT_YUV420_SEMI_PLANAR:
	case DILE_VT_VIDEO_FRAME_BUFFER_PIXEL_FORMAT_YUV422_SEMI_PLANAR:
		if (vfbcap.numPlanes < 2) {
			ERR("Unexpected number of planes: %d", vfbcap.numPlanes);
			ret = -11;
			goto err_unmap;
		}
		break;
	case DILE_VT_VIDEO_FRAME_BUFFER_PIXEL_FORMAT_RGB:
		break;
	default:
		ERR("Unsupported pixel format: %d", vfbprop.pixelFormat);
		ret = -12;
		goto err_unmap;
	}

	return 0;

err_unmap:
	unmap_buffers();

err_destroy:
	DILE_VT_Destroy(vth);
	vth = NULL;
	return ret;
}

int video_capture_start(void) {
	if (started) {
		return 0;
	}

	if (DILE_VT_Start(vth) != 0) {
		return VIDEO_CAPTURE_UNAVAILABLE;
	}

	started = true;
	return 0;
}

int video_capture_acquire(video_frame_t* frame) {
	uint32_t idx = 0;

	if (!started) {
		return -1;
	}

	set_freeze(true);

	if (DILE_VT_GetCurrentVideoFrameBufferProperty(vth, NULL, &idx) != 0 || idx >= vfbcap.numVfbs) {
		set_freeze(false);
		return VIDEO_CAPTURE_UNAVAILABLE;
	}

	frame->width = vfbprop.width;
	frame->height = vfbprop.height;
	frame->includes_ui = false;
	frame->planes[0] = vfbs[idx][0];
	frame->strides[0] = vfbprop.stride;

	if (vfbprop.pixelFormat == DILE_VT_VIDEO_FRAME_BUFFER_PIXEL_FORMAT_RGB) {
		frame->format = VIDEO_FORMAT_RGB888;
		// width reported is equal to stride for some reason
		frame->width = vfbprop.stride / 3;
		frame->planes[1] = NULL;
		frame->strides[1] = 0;
	} else {
		frame->format = vfbprop.pixelFormat == DILE_VT_VIDEO_FRAME_BUFFER_PIXEL_FORMAT_YUV422_SEMI_PLANAR ?
			VIDEO_FORMAT_NV16 : VIDEO_FORMAT_NV12;
		frame->planes[1] = vfbs[idx][1];
		frame->strides[1] = vfbprop.stride;
	}

	return 0;
}

int video_capture_release(void) {
	if (started) {
		set_freeze(false);
	}

	return 0;
}

int video_capture_stop(void) {
	if (!started) {
		return 0;
	}

	started = false;
	DILE_VT_Stop(vth);

	return 0;
}

int video_capture_destroy(void) {
	video_capture_stop();
	unmap_buffers();

	if (vth != NULL) {
		DILE_VT_Destroy(vth);
		vth = NULL;
	}

	return 0;
}
