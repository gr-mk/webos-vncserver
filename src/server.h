#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <rfb/rfb.h>
#include <glib.h>
#include "settings.h"
#include "capture.h"
#include "compositor.h"

typedef struct _capture_backend {
	const char* name;
	int (*init)(uint32_t width, uint32_t height);
	int (*execute)(uint8_t* target, uint32_t size);
	int (*destroy)(void);
} capture_backend_t;

typedef struct _video_backend {
	const char* name;
	int (*init)(uint32_t width, uint32_t height, uint32_t framerate);
	int (*start)(void);
	int (*acquire)(video_frame_t* frame);
	int (*release)(void);
	int (*stop)(void);
	int (*destroy)(void);
} video_backend_t;

typedef enum {
	DYNAMIC_RANGE_SDR = 0,
	DYNAMIC_RANGE_PQ,  // HDR10, Dolby Vision
	DYNAMIC_RANGE_HLG,
} dynamic_range_t;

typedef enum {
	VIDEO_PRESENCE_UNKNOWN = 0,
	VIDEO_PRESENCE_NONE,
	VIDEO_PRESENCE_ACTIVE,
} video_presence_t;

typedef struct {
	capture_backend_t capture;
	rfbScreenInfoPtr screen;
	int active_clients;
	settings_t* settings;
	bool running;

	// Video plane capture, composited under the UI layer
	video_backend_t video;
	bool video_loaded;
	bool video_running;
	gint64 video_retry_time;
	int video_failures;
	compositor_t compositor;
	// UI layer blended over video, unless capture hardware does it - captured
	// at reduced resolution and rate (capture backend is reinitialized at
	// ui_width x ui_height for it)
	uint8_t* ui_buffer;
	uint32_t ui_width;
	uint32_t ui_height;
	bool ui_empty;
	gint64 ui_next_refresh;
	// Whether any video is displayed and its dynamic range, updated by service
	video_presence_t video_presence;
	dynamic_range_t video_dynamic_range;
	// Video color encoding forced via VNCSERVER_VIDEO_COLOR, -1 if automatic
	int video_color_override;

	// Double buffering - next frame is captured into back buffer while
	// clients are sent the front one (screen->frameBuffer), which are swapped
	// once clients have received it (frame_consumed set by client threads).
	// fb_lock is held for reading by client threads while sending updates.
	uint8_t* back_buffer;
	bool back_ready;
	// Whether front/back buffer holds a video frame with 4:2:0 subsampled
	// chroma (of at most 1080p resolution), UI included
	bool front_low_chroma;
	bool back_low_chroma;
	// Horizontal bands changed in back buffer compared to front one
	uint8_t* changed_bands;
	pthread_rwlock_t fb_lock;
	gint frame_consumed;
	gint64 last_frame_time;
	// Capture is backed off while framebuffer doesn't change, until clients
	// send any input
	gint input_activity;
	gint64 idle_delay;
	gint64 next_capture_time;

	// Frame capture statistics (debug logging)
	gint64 stats_start;
	gint64 stats_capture_time;
	gint64 stats_diff_time;
	int stats_frames;

	guint timeout_ref;
} server_t;

int server_start(server_t* server, settings_t* settings);
int server_stop(server_t* server);
int server_update(server_t* server);
void server_bind_gmainloop(server_t* server);
