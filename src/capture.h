#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * UI (graphics plane) capture backends - libcapture_halgal.so, libcapture_gm.so
 *
 * Frames are written to target as 32-bit BGRA pixels (0xAARRGGBB, little
 * endian), alpha being the UI layer opacity.
 */
int capture_init(uint32_t width, uint32_t height);
int capture_execute(uint8_t* target, uint32_t size);
int capture_destroy();

/*
 * Video plane capture backends - libcapture_vtcapture.so,
 * libcapture_dile_vt.so
 *
 * These capture hardware-decoded video surfaces (HDMI inputs, apps using
 * hardware video playback, etc.) that are not visible to UI capture backends.
 */
typedef enum {
	VIDEO_FORMAT_NV12 = 0, // Y plane, interleaved UV plane - 4:2:0
	VIDEO_FORMAT_NV21,     // Y plane, interleaved VU plane - 4:2:0
	VIDEO_FORMAT_NV16,     // Y plane, interleaved UV plane - 4:2:2
	VIDEO_FORMAT_RGB888,   // packed R, G, B bytes
} video_format_t;

typedef struct {
	video_format_t format;
	uint32_t width;
	uint32_t height;
	const uint8_t* planes[2];
	uint32_t strides[2];
	// Frame already contains UI layers blended over video (by hardware)
	bool includes_ui;
} video_frame_t;

// video_capture_start() / video_capture_acquire() return value used when
// there's no capturable video right now (eg. no video playing, capture
// blocked) - caller should retry later.
#define VIDEO_CAPTURE_UNAVAILABLE 1

int video_capture_init(uint32_t width, uint32_t height, uint32_t framerate);
int video_capture_start(void);
// Frame planes are valid until video_capture_release() is called
int video_capture_acquire(video_frame_t* frame);
int video_capture_release(void);
int video_capture_stop(void);
int video_capture_destroy(void);
