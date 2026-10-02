// Video capture backend using libvtcapture (webOS 5.x+)
//
// libvtcapture is a C++ library and vtCapture_create() may throw, hence this
// backend is built as C++ - the rest of the interface is plain C.
//
// Note: libvtcapture registers its own Luna service names
// (com.webos.rm.client.*, com.webos.service.capture.client*), so this only
// works in a process whose Luna role allows them - eg. the elevated
// org.webosbrew.vncserver.service.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <exception>

extern "C" {
#include <vtcapture/vtCaptureApi_c.h>

#include "capture.h"
#include "log.h"
}

#define VTCAPTURE_CALLER "webos-vncserver"

// vtCapture_init() returns this when capture is not possible right now
// (eg. no video is playing)
#define VTCAPTURE_NOT_READY 17
#define VTCAPTURE_PERMISSION_DENIED 11

// Blended output can't be wider than this - wider capture requests are
// accepted, but yield 1920 pixels wide frames in a different format. Display
// output (video plane only) can be captured at full 4K resolution, so it's
// used instead for wider framebuffers, with UI layers blended in software.
#define VTCAPTURE_BLENDED_MAX_WIDTH 1920
#define VTCAPTURE_BLENDED_MAX_HEIGHT 1080

// Capture frame rate limit above 1920x1080 - frames can't be processed nearly
// this fast anyway, and capture buffers are overwritten less often, which
// matters as copying them takes longer
#define VTCAPTURE_LARGE_MAX_FRAMERATE 15

// Every this many rows of a copied capture buffer are compared with the buffer
// afterwards, to detect it changing while copied
#define VTCAPTURE_VERIFY_STEP 8
// Copy attempts per frame
#define VTCAPTURE_COPY_ATTEMPTS 3
// Maximum wait for capture buffer rotation, in microseconds
#define VTCAPTURE_ROTATION_TIMEOUT_US 150000

// Capture buffer not changing for this long means video is gone
#define VTCAPTURE_STALE_TIMEOUT_US 1500000

enum vtcapture_dump_location {
	VTCAPTURE_SCALER_INPUT = 0,
	VTCAPTURE_SCALER_OUTPUT,
	VTCAPTURE_DISPLAY_OUTPUT,
	VTCAPTURE_BLENDED_OUTPUT,
	VTCAPTURE_OSD_OUTPUT,
};

static VT_DRIVER* driver = NULL;
static VT_CLIENTID_T client[128];
static _LibVtCaptureProperties props;
static _LibVtCapturePlaneInfo plane;
static bool started = false;
// Whether capture buffers are copied only after hardware has moved on from
// them (see acquire)
static bool copy_after_rotation = false;
static bool dump_location_forced = false;

static const char* last_buffer = NULL;
static uint64_t last_buffer_change = 0;

// Copy of the current capture buffer. Hardware keeps rotating the buffers
// regardless of their users, and usually starts overwriting the current one
// well before a frame is converted (tens of ms), which shows as tearing -
// copying both planes out takes just a few ms.
static uint8_t* snapshot = NULL;
static size_t snapshot_size = 0;

// Copy statistics (debug logging)
static int stats_frames = 0;
static int stats_retries = 0;
static int stats_torn = 0;

static uint64_t now_us() {
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (uint64_t) tp.tv_sec * 1000000 + tp.tv_nsec / 1000;
}

extern "C" int video_capture_init(uint32_t width, uint32_t height, uint32_t framerate) {
	try {
		driver = vtCapture_create();
	} catch (const std::exception& err) {
		ERR("vtCapture_create failed: %s", err.what());
		driver = NULL;
	} catch (...) {
		ERR("vtCapture_create failed: unknown exception");
		driver = NULL;
	}

	if (driver == NULL) {
		return -1;
	}

	// For testing - capture at a different resolution than framebuffer one
	if (getenv("VNCSERVER_VTCAPTURE_SIZE")) {
		unsigned int w, h;
		if (sscanf(getenv("VNCSERVER_VTCAPTURE_SIZE"), "%ux%u", &w, &h) == 2 && w > 0 && h > 0) {
			width = w & ~1u;
			height = h & ~1u;
		}
	}

	memset(&props, 0, sizeof(props));
	// Blended output contains video with UI layers composited on top, exactly
	// as displayed on screen.
	props.dump = width > VTCAPTURE_BLENDED_MAX_WIDTH ? VTCAPTURE_DISPLAY_OUTPUT : VTCAPTURE_BLENDED_OUTPUT;
	props.loc.x = 0;
	props.loc.y = 0;
	props.reg.w = width;
	props.reg.h = height;
	props.buf_cnt = 3;
	props.frm = framerate > 0 && framerate < 60 ? framerate : 60;
	copy_after_rotation = width > VTCAPTURE_BLENDED_MAX_WIDTH || height > VTCAPTURE_BLENDED_MAX_HEIGHT;
	if (copy_after_rotation && props.frm > VTCAPTURE_LARGE_MAX_FRAMERATE) {
		props.frm = VTCAPTURE_LARGE_MAX_FRAMERATE;
	}

	if (getenv("VNCSERVER_VTCAPTURE_DUMP")) {
		props.dump = atoi(getenv("VNCSERVER_VTCAPTURE_DUMP"));
		dump_location_forced = true;
	}

	return 0;
}

static int capture_start() {
	int ret;

	snprintf(client, sizeof(client), "%s", "00");

	if ((ret = vtCapture_init(driver, VTCAPTURE_CALLER, client)) != 0) {
		if (ret == VTCAPTURE_NOT_READY) {
			DBG("vtCapture_init: not ready");
			return VIDEO_CAPTURE_UNAVAILABLE;
		}

		if (ret == VTCAPTURE_PERMISSION_DENIED) {
			ERR("vtCapture_init: permission denied");
		} else {
			ERR("vtCapture_init failed: %d", ret);
		}
		return -2;
	}

	DBG("vtCapture_init done, client: %s", client);

	if ((ret = vtCapture_preprocess(driver, client, &props)) != 0) {
		vtCapture_finalize(driver, client);

		if (ret == 1) {
			DBG("vtCapture_preprocess: not ready");
			return VIDEO_CAPTURE_UNAVAILABLE;
		}

		ERR("vtCapture_preprocess (dump location %d) failed: %d", props.dump, ret);
		return -3;
	}

	if ((ret = vtCapture_planeInfo(driver, client, &plane)) != 0) {
		ERR("vtCapture_planeInfo failed: %d", ret);
		vtCapture_postprocess(driver, client);
		vtCapture_finalize(driver, client);
		return -4;
	}

	INFO("vtcapture dump location: %d, stride: %d, region: %dx%d+%d+%d, active region: %dx%d+%d+%d",
		props.dump, plane.stride,
		plane.planeregion.c, plane.planeregion.d, plane.planeregion.a, plane.planeregion.b,
		plane.activeregion.c, plane.activeregion.d, plane.activeregion.a, plane.activeregion.b);

	if ((ret = vtCapture_process(driver, client)) != 0) {
		ERR("vtCapture_process failed: %d", ret);
		vtCapture_stop(driver, client);
		vtCapture_postprocess(driver, client);
		vtCapture_finalize(driver, client);
		return -5;
	}

	return 0;
}

static int wait_rotation(_LibVtCaptureBufferInfo* buff);

extern "C" int video_capture_start(void) {
	int ret;

	if (started) {
		return 0;
	}

	ret = capture_start();

	if (ret < 0 && props.dump == VTCAPTURE_DISPLAY_OUTPUT && !dump_location_forced
		&& (props.reg.w > VTCAPTURE_BLENDED_MAX_WIDTH || props.reg.h > VTCAPTURE_BLENDED_MAX_HEIGHT)) {
		double scale_w = (double) VTCAPTURE_BLENDED_MAX_WIDTH / props.reg.w;
		double scale_h = (double) VTCAPTURE_BLENDED_MAX_HEIGHT / props.reg.h;
		double scale = scale_w < scale_h ? scale_w : scale_h;
		uint32_t width = (uint32_t) (props.reg.w * scale) & ~1u;
		uint32_t height = (uint32_t) (props.reg.h * scale) & ~1u;

		WARN("Display output capture at %dx%d failed, falling back to blended output at %ux%u", props.reg.w,
			props.reg.h, width, height);
		props.dump = VTCAPTURE_BLENDED_OUTPUT;
		props.reg.w = width;
		props.reg.h = height;
		ret = capture_start();
	}

	if (ret < 0 && props.dump == VTCAPTURE_BLENDED_OUTPUT && !dump_location_forced) {
		WARN("Blended output capture failed, falling back to display output");
		props.dump = VTCAPTURE_DISPLAY_OUTPUT;
		ret = capture_start();
	}

	if (ret == 0) {
		started = true;
		last_buffer = NULL;

		// Until hardware has written every capture buffer, they may contain
		// stale frames from previous capture sessions
		_LibVtCaptureBufferInfo buff;
		if (vtCapture_currentCaptureBuffInfo(driver, &buff) == 0 && buff.start_addr0 != NULL) {
			for (int i = 0; i < props.buf_cnt && wait_rotation(&buff) == 0; i++) {
			}
		}
	}

	return ret;
}

// Whether snapshot matches capture buffer, judging by sampled rows
static bool snapshot_intact(const _LibVtCaptureBufferInfo* buff, uint32_t height) {
	const size_t luma_size = (size_t) plane.stride * height;
	const uint32_t chroma_height = (height + 1) / 2;

	for (uint32_t y = 0; y < height; y += VTCAPTURE_VERIFY_STEP) {
		const size_t offset = (size_t) y * plane.stride;
		if (memcmp(snapshot + offset, buff->start_addr0 + offset, plane.stride) != 0) {
			return false;
		}
	}

	for (uint32_t y = 0; y < chroma_height; y += VTCAPTURE_VERIFY_STEP / 2) {
		const size_t offset = (size_t) y * plane.stride;
		if (memcmp(snapshot + luma_size + offset, buff->start_addr1 + offset, plane.stride) != 0) {
			return false;
		}
	}

	return true;
}

// Waits for buff to stop being the current capture buffer, ie. hardware moving
// on to the next one, and updates it to the new current buffer
static int wait_rotation(_LibVtCaptureBufferInfo* buff) {
	const char* current = buff->start_addr0;
	const uint64_t deadline = now_us() + VTCAPTURE_ROTATION_TIMEOUT_US;

	while (now_us() < deadline) {
		usleep(1000);
		if (vtCapture_currentCaptureBuffInfo(driver, buff) != 0 || buff->start_addr0 == NULL || buff->start_addr1 == NULL) {
			return -1;
		}
		if (buff->start_addr0 != current) {
			return 0;
		}
	}

	return -1;
}

extern "C" int video_capture_acquire(video_frame_t* frame) {
	_LibVtCaptureBufferInfo buff;
	int ret;

	if (!started) {
		return -1;
	}

	if ((ret = vtCapture_currentCaptureBuffInfo(driver, &buff)) != 0) {
		DBG("vtCapture_currentCaptureBuffInfo failed: %d", ret);
		return VIDEO_CAPTURE_UNAVAILABLE;
	}

	if (buff.start_addr0 == NULL || buff.start_addr1 == NULL) {
		return VIDEO_CAPTURE_UNAVAILABLE;
	}

	// Capture buffers rotate on every captured frame - if they don't, there's
	// no video signal anymore and we'd keep returning a frozen frame.
	uint64_t now = now_us();
	if (buff.start_addr0 != last_buffer) {
		last_buffer = buff.start_addr0;
		last_buffer_change = now;
	} else if (now - last_buffer_change > VTCAPTURE_STALE_TIMEOUT_US) {
		return VIDEO_CAPTURE_UNAVAILABLE;
	}

	uint32_t height = plane.planeregion.d;
	size_t luma_size = (size_t) plane.stride * height;
	size_t chroma_size = (size_t) plane.stride * ((height + 1) / 2);

	if (snapshot_size < luma_size + chroma_size) {
		free(snapshot);
		snapshot_size = 0;
		if ((snapshot = (uint8_t*) malloc(luma_size + chroma_size)) == NULL) {
			ERR("Capture buffer allocation failed");
			return -2;
		}
		snapshot_size = luma_size + chroma_size;
	}

	// Copying a large frame takes long enough to quite often overlap with
	// hardware still writing the current buffer - it's most stable right after
	// hardware has moved on to the next one, so it's copied then.
	if (copy_after_rotation) {
		_LibVtCaptureBufferInfo next = buff;
		wait_rotation(&next);
	}

	// Copies are verified, and retried if buffer changed in the meantime
	for (int attempt = 0;; attempt++) {
		memcpy(snapshot, buff.start_addr0, luma_size);
		memcpy(snapshot + luma_size, buff.start_addr1, chroma_size);

		if (snapshot_intact(&buff, height)) {
			break;
		}

		if (attempt == VTCAPTURE_COPY_ATTEMPTS - 1) {
			stats_torn++;
			break;
		}
		stats_retries++;
	}

	if (++stats_frames == 100) {
		DBG("Last %d frames: %d copy retries, %d torn frames", stats_frames, stats_retries, stats_torn);
		stats_frames = stats_retries = stats_torn = 0;
	}

	frame->format = VIDEO_FORMAT_NV12;
	frame->width = plane.planeregion.c;
	frame->height = height;
	frame->planes[0] = snapshot;
	frame->strides[0] = plane.stride;
	frame->planes[1] = snapshot + luma_size;
	frame->strides[1] = plane.stride;
	frame->includes_ui = props.dump == VTCAPTURE_BLENDED_OUTPUT;

	return 0;
}

extern "C" int video_capture_release(void) {
	return 0;
}

extern "C" int video_capture_stop(void) {
	if (!started) {
		return 0;
	}

	started = false;
	vtCapture_stop(driver, client);
	vtCapture_postprocess(driver, client);
	vtCapture_finalize(driver, client);

	return 0;
}

extern "C" int video_capture_destroy(void) {
	video_capture_stop();

	if (driver != NULL) {
		vtCapture_release(driver);
		driver = NULL;
	}

	free(snapshot);
	snapshot = NULL;
	snapshot_size = 0;

	return 0;
}
