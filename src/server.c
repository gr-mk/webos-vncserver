#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>

#include "log.h"
#include "server.h"
#include "settings.h"
#include "capture.h"
#include "uinput.h"

unsigned int screenwidth = 1920;
unsigned int screenheight = 1080;
const unsigned int bpp = 4;

unsigned int nativewidth = 1920;
unsigned int nativeheight = 1080;

#define FBSIZE (screenwidth * screenheight * bpp)

static gboolean server_frame_handler(gpointer data) {
	server_t* server = (server_t*) data;
   	server_update(data);
	return server->running;
}

// One-off update scheduled by client threads (possibly after server stop)
static gboolean server_idle_handler(gpointer data) {
	server_t* server = (server_t*) data;
	if (server->running) {
		server_update(server);
	}
	return G_SOURCE_REMOVE;
}

int capture_backend_load(capture_backend_t* backend, const char* name) {
	void* handle;

	handle = dlopen(name, RTLD_NOW);

	if (handle == NULL) {
		DBG("Failed to load %s: %s", name, dlerror());
		return -1;
	}

	backend->name = name;
	backend->init = dlsym(handle, "capture_init");
	backend->execute = dlsym(handle, "capture_execute");
	backend->destroy = dlsym(handle, "capture_destroy");

	return 0;
}

int capture_backend_init(capture_backend_t* backend, uint32_t width, uint32_t height) {
	int ret;

	if ((ret = capture_backend_load(backend, "libcapture_halgal.so")) != 0) {
		DBG("%s load failed: %d", "libcapture_halgal", ret);
	} else {
		if ((ret = backend->init(width, height)) != 0) {
			DBG("%s init failed: %d", "libcapture_halgal", ret);
		} else {
			return 0;
		}
	}

	if ((ret = capture_backend_load(backend, "libcapture_gm.so")) != 0) {
		DBG("%s load failed: %d", "libcapture_gm", ret);
	} else {
		if ((ret = backend->init(width, height)) != 0) {
			DBG("%s init failed: %d", "libcapture_gm", ret);
		} else {
			return 0;
		}
	}

	WARN("No eligible capture backends found");

	return -1;
}

// Frames are captured at least this often, even if clients haven't received
// the previous one yet, in microseconds
#define FRAME_PACING_TIMEOUT (5 * G_USEC_PER_SEC)

// Height of horizontal bands framebuffer changes are detected in
#define DIFF_BAND_HEIGHT 16

// Maximum capture interval while framebuffer doesn't change, in microseconds
#define IDLE_MAX_DELAY (G_USEC_PER_SEC / 2)

// UI layer blended over video is captured at most at this resolution -
// capturing UI layers at 4K takes ~10x as long as at 1080p (~600 ms)
#define UI_OVERLAY_MAX_WIDTH 1920
#define UI_OVERLAY_MAX_HEIGHT 1080
// UI layer blended over video is captured this often (it rarely changes, and
// is usually fully transparent while video is playing), or at least this often
// while clients send input, in microseconds
#define UI_OVERLAY_INTERVAL (G_USEC_PER_SEC / 2)
#define UI_OVERLAY_INPUT_INTERVAL (G_USEC_PER_SEC / 8)

// Video capture start retry interval, in microseconds
#define VIDEO_RETRY_INTERVAL (5 * G_USEC_PER_SEC)
// Consecutive frame acquisition failures before video capture is restarted
#define VIDEO_MAX_FAILURES 30

static const char* video_backends[] = {
	"libcapture_vtcapture.so", // webOS 5.x+
	"libcapture_dile_vt.so",   // webOS 3.x - 4.x
	NULL,
};

static int video_backend_load(video_backend_t* backend, const char* name) {
	void* handle;

	handle = dlopen(name, RTLD_NOW);

	if (handle == NULL) {
		DBG("Failed to load %s: %s", name, dlerror());
		return -1;
	}

	backend->name = name;
	backend->init = dlsym(handle, "video_capture_init");
	backend->start = dlsym(handle, "video_capture_start");
	backend->acquire = dlsym(handle, "video_capture_acquire");
	backend->release = dlsym(handle, "video_capture_release");
	backend->stop = dlsym(handle, "video_capture_stop");
	backend->destroy = dlsym(handle, "video_capture_destroy");

	if (!backend->init || !backend->start || !backend->acquire || !backend->release || !backend->stop || !backend->destroy) {
		WARN("%s: missing symbols", name);
		dlclose(handle);
		return -2;
	}

	return 0;
}

static int video_backend_init(video_backend_t* backend, uint32_t width, uint32_t height, uint32_t framerate) {
	int ret;

	for (const char** name = video_backends; *name != NULL; name++) {
		if ((ret = video_backend_load(backend, *name)) != 0) {
			DBG("%s load failed: %d", *name, ret);
			continue;
		}

		if ((ret = backend->init(width, height, framerate)) != 0) {
			DBG("%s init failed: %d", *name, ret);
			continue;
		}

		return 0;
	}

	return -1;
}

static void server_video_init(server_t* server, settings_t* settings) {
	server->video_loaded = false;
	server->video_running = false;
	server->video_retry_time = 0;
	server->video_failures = 0;
	server->ui_buffer = NULL;
	server->video_color_override = -1;

	const char* color = getenv("VNCSERVER_VIDEO_COLOR");
	if (color != NULL) {
		const char* names[] = {"bt709", "bt2020", "pq", "hlg"};
		for (int i = 0; i < 4; i++) {
			if (strcmp(color, names[i]) == 0) {
				server->video_color_override = i;
			}
		}
	}

	if (!settings->video_capture) {
		INFO("Video capture disabled");
		return;
	}

	if (video_backend_init(&server->video, settings->width, settings->height, settings->framerate) != 0) {
		WARN("No video capture backend available, only UI layers will be captured");
		return;
	}

	if (compositor_init(&server->compositor, settings->width, settings->height) != 0) {
		ERR("Compositor initialization failed");
		compositor_destroy(&server->compositor);
		server->video.destroy();
		return;
	}

	INFO("Using video capture backend: %s", server->video.name);
	server->video_loaded = true;
}

static void server_video_stop(server_t* server) {
	if (server->video_running) {
		server->video.stop();
		server->video_running = false;
	}
}

static void server_video_destroy(server_t* server) {
	if (!server->video_loaded) {
		return;
	}

	server_video_stop(server);
	server->video.destroy();
	compositor_destroy(&server->compositor);
	free(server->ui_buffer);
	server->ui_buffer = NULL;
	server->video_loaded = false;
}

// Reinitializes UI capture backend at given resolution, if needed
static int server_ui_resize(server_t* server, uint32_t width, uint32_t height) {
	int ret;

	if (server->ui_width == width && server->ui_height == height) {
		return 0;
	}

	if (server->ui_width != 0) {
		server->capture.destroy();
	}
	free(server->ui_buffer);
	server->ui_buffer = NULL;
	server->ui_width = 0;
	server->ui_height = 0;

	if ((ret = server->capture.init(width, height)) != 0) {
		ERR("capture init at %ux%u failed: %d", width, height, ret);
		return ret;
	}

	DBG("UI capture resolution: %ux%u", width, height);
	server->ui_width = width;
	server->ui_height = height;
	server->ui_next_refresh = 0;
	return 0;
}

// Whether any pixel of BGRA buffer isn't fully transparent
static bool server_ui_visible(const uint8_t* buffer, size_t pixels) {
	const uint32_t* p = (const uint32_t*) buffer;

	for (size_t i = 0; i < pixels; i += 64) {
		const size_t end = MIN(i + 64, pixels);
		uint32_t any = 0;
		for (size_t j = i; j < end; j++) {
			any |= p[j];
		}
		if (any >> 24) {
			return true;
		}
	}

	return false;
}

// Captures UI layer to be blended over video into ui_buffer, when due
static int server_ui_overlay(server_t* server, bool interacted, gint64 now) {
	uint32_t width = server->screen->width;
	uint32_t height = server->screen->height;
	int ret;

	if (width > UI_OVERLAY_MAX_WIDTH || height > UI_OVERLAY_MAX_HEIGHT) {
		double scale = MIN((double) UI_OVERLAY_MAX_WIDTH / width, (double) UI_OVERLAY_MAX_HEIGHT / height);
		width = (uint32_t) (width * scale) & ~1u;
		height = (uint32_t) (height * scale) & ~1u;
	}

	if ((ret = server_ui_resize(server, width, height)) != 0) {
		return -5;
	}

	// Catch UI reacting to client input soon
	if (interacted) {
		server->ui_next_refresh = MIN(server->ui_next_refresh, now + UI_OVERLAY_INPUT_INTERVAL);
	}

	if (now < server->ui_next_refresh) {
		return 0;
	}

	size_t size = (size_t) width * height * 4;
	if (server->ui_buffer == NULL && (server->ui_buffer = malloc(size)) == NULL) {
		ERR("UI buffer allocation failed");
		return -6;
	}

	if ((ret = server->capture.execute(server->ui_buffer, size)) != 0) {
		ERR("capture execute failed: %08x", ret);
		return -5;
	}

	server->ui_empty = !server_ui_visible(server->ui_buffer, (size_t) width * height);
	server->ui_next_refresh = now + UI_OVERLAY_INTERVAL;
	return 0;
}

static video_color_t server_video_color(server_t* server, const video_frame_t* frame) {
	if (server->video_color_override >= 0) {
		return (video_color_t) server->video_color_override;
	}

	switch (server->video_dynamic_range) {
	case DYNAMIC_RANGE_PQ:
		// HDR video blended with UI has already been tone mapped by the TV
		return frame->includes_ui ? VIDEO_COLOR_BT2020 : VIDEO_COLOR_BT2020_PQ;
	case DYNAMIC_RANGE_HLG:
		return frame->includes_ui ? VIDEO_COLOR_BT2020 : VIDEO_COLOR_BT2020_HLG;
	default:
		return VIDEO_COLOR_BT709;
	}
}

// Starts video capture if it's not running (with retry backoff), returns
// whether video frames can be acquired.
static bool server_video_poll(server_t* server) {
	int ret;

	if (!server->video_loaded) {
		return false;
	}

	if (server->video_presence == VIDEO_PRESENCE_NONE) {
		if (server->video_running) {
			INFO("No video displayed, stopping video capture");
			server_video_stop(server);
		}
		return false;
	}

	if (server->video_running) {
		return true;
	}

	gint64 now = g_get_monotonic_time();
	if (now < server->video_retry_time) {
		return false;
	}

	if ((ret = server->video.start()) != 0) {
		if (ret != VIDEO_CAPTURE_UNAVAILABLE) {
			WARN("Video capture start failed: %d", ret);
		}
		server->video_retry_time = now + VIDEO_RETRY_INTERVAL;
		return false;
	}

	INFO("Video capture started");
	server->video_running = true;
	server->video_failures = 0;

	return true;
}

// libvncserver's Tight JPEG subsampling levels (cl->turboSubsampLevel)
enum {
	SUBSAMP_444 = 0,
	SUBSAMP_420 = 1,
	SUBSAMP_422 = 2,
	SUBSAMP_GRAY = 3,
};

typedef struct {
	gint64 update_start;
	gint64 stats_start;
	int updates;
	int update_time;

	// JPEG settings last requested by client, and subsampling level last set
	// by server (to detect client changing it)
	int client_quality;
	int client_subsamp;
	int applied_subsamp;
} client_data_t;

// Video frames have 4:2:0 subsampled chroma of at most 1080p resolution, so
// JPEG compression with full resolution chroma (default for higher quality
// levels) only wastes time and bandwidth on them - one third of it at 4K.
static void server_choose_subsampling(server_t* server, rfbClientPtr cl, client_data_t* data) {
	if (cl->turboQualityLevel != data->client_quality || cl->turboSubsampLevel != data->applied_subsamp) {
		data->client_quality = cl->turboQualityLevel;
		data->client_subsamp = cl->turboSubsampLevel;
	}

	int subsamp = data->client_subsamp;
	if (server->front_low_chroma && (subsamp == SUBSAMP_444 || subsamp == SUBSAMP_422)) {
		subsamp = SUBSAMP_420;
	}

	cl->turboSubsampLevel = subsamp;
	data->applied_subsamp = subsamp;
}

static void server_client_gone(rfbClientPtr cl) {
	server_t* server = (server_t*) cl->screen->screenData;
	free(cl->clientData);
	cl->clientData = NULL;

	INFO("%s [%d]: Client disconnected", cl->host, server->active_clients);

	server->active_clients -= 1;
}

static enum rfbNewClientAction server_client_incoming(rfbClientPtr cl) {
	server_t* server = (server_t*) cl->screen->screenData;
	server->active_clients += 1;
	g_atomic_int_set(&server->frame_consumed, TRUE);
	client_data_t* data = calloc(1, sizeof(client_data_t));
	if (data != NULL) {
		data->client_quality = -2;
		data->applied_subsamp = -1;
	}
	cl->clientData = data;

	// Encoding speed is the bottleneck rather than network throughput - use
	// fast zlib compression, unless client asks for something else. (Level 2
	// is both faster and more effective than 1 for upscaled video frames.)
	cl->zlibCompressLevel = 2;

	cl->clientGoneHook = &server_client_gone;

	INFO("%s [%d]: New client connected", cl->host, server->active_clients);

	return RFB_CLIENT_ACCEPT;
}

// Called from client threads just before a framebuffer update is sent
static void server_display(rfbClientPtr cl) {
	server_t* server = (server_t*) cl->screen->screenData;
	client_data_t* data = (client_data_t*) cl->clientData;

	// Framebuffers can't be swapped while update is being sent
	pthread_rwlock_rdlock(&server->fb_lock);

	if (data == NULL) {
		return;
	}

	data->update_start = g_get_monotonic_time();
	server_choose_subsampling(server, cl, data);

	if (data->stats_start == 0) {
		char encoding[64];
		INFO("%s: using %s encoding (JPEG quality: %d, subsampling: %d), %d bpp, shifts: %d/%d/%d%s", cl->host,
			encodingName(cl->preferredEncoding, encoding, sizeof(encoding)), cl->turboQualityLevel,
			data->client_subsamp, cl->format.bitsPerPixel, cl->format.redShift, cl->format.greenShift,
			cl->format.blueShift, cl->translateFn == rfbTranslateNone ? " (native)" : "");
		data->stats_start = data->update_start;
	}
}

// Called from client threads after a framebuffer update has been sent
static void server_display_finished(rfbClientPtr cl, int result) {
	server_t* server = (server_t*) cl->screen->screenData;
	client_data_t* data = (client_data_t*) cl->clientData;

	pthread_rwlock_unlock(&server->fb_lock);
	g_atomic_int_set(&server->frame_consumed, TRUE);
	// Swap in next frame right away, instead of waiting for the timer
	g_idle_add(server_idle_handler, server);

	if (data == NULL) {
		return;
	}

	gint64 now = g_get_monotonic_time();
	data->updates += 1;
	data->update_time += (now - data->update_start) / 1000;

	if (now - data->stats_start >= 10 * G_USEC_PER_SEC) {
		DBG("%s: %.1f updates/s, %d ms per update", cl->host,
			data->updates * (double) G_USEC_PER_SEC / (now - data->stats_start), data->update_time / data->updates);
		data->stats_start = now;
		data->updates = 0;
		data->update_time = 0;
	}
}

static void keyevent(rfbBool down, rfbKeySym key, rfbClientPtr cl) {
	server_t* server = (server_t*) cl->screen->screenData;
	g_atomic_int_set(&server->input_activity, TRUE);
	uinput_key_command(down, key);
}

static void ptrevent(int buttonMask, int x, int y, rfbClientPtr cl) {
	server_t* server = (server_t*) cl->screen->screenData;
	g_atomic_int_set(&server->input_activity, TRUE);
	// fprintf(stderr, "%03d x %03d: %08x\n", x, y, buttonMask);
	ptr_abs(x * 1920 / cl->screen->width, y * 1080 / cl->screen->height, buttonMask);
}

// Finds horizontal bands of back buffer that differ from front buffer,
// returns whether anything has changed.
static bool server_diff_frames(server_t* server) {
	rfbScreenInfoPtr screen = server->screen;
	const size_t stride = screen->width * bpp;
	const uint8_t* front = (const uint8_t*) screen->frameBuffer;
	bool changed = false;

	for (int band = 0, y = 0; y < screen->height; band++, y += DIFF_BAND_HEIGHT) {
		const size_t offset = y * stride;
		const size_t length = (y + DIFF_BAND_HEIGHT <= screen->height ? DIFF_BAND_HEIGHT : screen->height - y) * stride;

		server->changed_bands[band] = memcmp(server->back_buffer + offset, front + offset, length) != 0;
		changed |= server->changed_bands[band];
	}

	return changed;
}

static void server_mark_changed_bands(server_t* server) {
	rfbScreenInfoPtr screen = server->screen;
	int band_start = -1;
	int band = 0;

	for (int y = 0; y < screen->height; band++, y += DIFF_BAND_HEIGHT) {
		if (server->changed_bands[band]) {
			if (band_start < 0) {
				band_start = y;
			}
		} else if (band_start >= 0) {
			rfbMarkRectAsModified(screen, 0, band_start, screen->width, y);
			band_start = -1;
		}
	}

	if (band_start >= 0) {
		rfbMarkRectAsModified(screen, 0, band_start, screen->width, screen->height);
	}
}

// Swaps prepared back buffer in, once clients have received previous frame
static void server_try_swap(server_t* server, gint64 now) {
	if (!server->back_ready) {
		return;
	}

	if (!g_atomic_int_get(&server->frame_consumed) && now - server->last_frame_time < FRAME_PACING_TIMEOUT) {
		return;
	}

	// Some client is still sending an update
	if (pthread_rwlock_trywrlock(&server->fb_lock) != 0) {
		return;
	}

	uint8_t* front = (uint8_t*) server->screen->frameBuffer;
	server->screen->frameBuffer = (char*) server->back_buffer;
	server->back_buffer = front;
	server->front_low_chroma = server->back_low_chroma;
	server_mark_changed_bands(server);

	pthread_rwlock_unlock(&server->fb_lock);

	g_atomic_int_set(&server->frame_consumed, FALSE);
	server->back_ready = false;
	server->last_frame_time = now;
}

int server_start(server_t* server, settings_t* settings) {
	int ret;

	if ((ret = capture_backend_init(&server->capture, settings->width, settings->height)) != 0) {
		ERR("capture_init() failed: %d", ret);
		return -2;
	}
	server->ui_width = settings->width;
	server->ui_height = settings->height;
	server->ui_empty = true;
	server->ui_next_refresh = 0;

	INFO("Using capture backend: %s", server->capture.name);

	rfbLogEnable(getenv("VNCSERVER_DEBUG") != NULL);

	rfbScreenInfoPtr screen = rfbGetScreen(NULL, NULL, settings->width, settings->height, 8, 3, bpp);

	if (screen == NULL) {
		ERR("rfbGetScreen() initialization failed");
		return -3;
	}

	server->active_clients = 0;
	server->settings = settings;
	server->screen = screen;
	screen->screenData = (void*) server;

	screen->newClientHook = server_client_incoming;

	if ((ret = initialize_uinput()) != 0) {
		ERR("uinput initialization failed: %d", ret);
		return -4;
	}

	// switch red and blue channels
	int tmp = screen->serverFormat.redShift;
	screen->serverFormat.redShift = screen->serverFormat.blueShift;
	screen->serverFormat.blueShift = tmp;

	screen->kbdAddEvent = keyevent;
	screen->ptrAddEvent = ptrevent;
	screen->displayHook = server_display;
	screen->displayFinishedHook = server_display_finished;

	int fbsize = screen->width * screen->height * bpp;
	screen->frameBuffer = calloc(1, fbsize);
	server->back_buffer = calloc(1, fbsize);
	server->changed_bands = calloc((screen->height + DIFF_BAND_HEIGHT - 1) / DIFF_BAND_HEIGHT, 1);
	server->back_ready = false;
	pthread_rwlock_init(&server->fb_lock, NULL);

	if (screen->frameBuffer == NULL || server->back_buffer == NULL || server->changed_bands == NULL) {
		ERR("Framebuffer allocation failed");
		return -5;
	}

	if (settings->password && strlen(settings->password)) {
		char** passwords = calloc(2, sizeof(char*));
		passwords[0] = settings->password;
		screen->authPasswdData = (void*)passwords;
		screen->passwordCheck = rfbCheckPasswordByList;
	}

	server_video_init(server, settings);

	rfbInitServer(screen);

	// Run event loop in background thread
	rfbRunEventLoop(screen, -1, TRUE);

	INFO("VNC server running on %d", screen->port);

	server->running = true;

	return 0;
}

void server_bind_gmainloop(server_t* server) {
	server->timeout_ref = g_timeout_add(1000 / server->settings->framerate, server_frame_handler, (gpointer) server);
}

int server_update(server_t* server) {
	int ret;
	rfbScreenInfoPtr screen = server->screen;
	int fbsize = screen->width * screen->height * bpp;

	if (server->active_clients <= 0) {
		// Release video capture hardware while nobody is watching
		if (server->video_running) {
			INFO("No active clients, stopping video capture");
			server_video_stop(server);
		}
		return 0;
	}

	// Next frame is only captured after previous one has been swapped in -
	// encoding is usually much slower than capture.
	gint64 now = g_get_monotonic_time();
	server_try_swap(server, now);
	if (server->back_ready) {
		return 0;
	}

	// Only valid after any swap above - capturing into a stale pointer would
	// overwrite frame being sent to clients
	uint8_t* framebuffer = server->back_buffer;

	// Poll less often while nothing changes, unless clients are interacting
	bool interacted = false;
	if (g_atomic_int_get(&server->input_activity)) {
		g_atomic_int_set(&server->input_activity, FALSE);
		server->idle_delay = 0;
		interacted = true;
	} else if (now < server->next_capture_time) {
		return 0;
	}

	bool composited = false;
	bool low_chroma = false;

	if (server_video_poll(server)) {
		video_frame_t frame;

		if ((ret = server->video.acquire(&frame)) == 0) {
			const uint8_t* ui = NULL;

			// UI layer needs to be captured separately and blended over
			// video, unless capture hardware has done it already.
			if (!frame.includes_ui) {
				if ((ret = server_ui_overlay(server, interacted, now)) != 0) {
					server->video.release();
					return ret;
				}
				ui = server->ui_empty ? NULL : server->ui_buffer;
			}

			ret = compositor_run(&server->compositor, framebuffer, ui, server->ui_width, server->ui_height, &frame,
				server_video_color(server, &frame));
			server->video.release();
			server->video_failures = 0;

			if (ret == 0) {
				composited = true;
				// Video has 4:2:0 chroma - so does the whole frame, unless UI
				// of more than half framebuffer resolution was blended over it
				low_chroma =(frame.format == VIDEO_FORMAT_NV12 || frame.format == VIDEO_FORMAT_NV21)
					&& (frame.includes_ui || ui == NULL || server->ui_width * 2 <= screen->width);
			} else {
				DBG("compositor_run failed: %d", ret);
			}
		} else if (++server->video_failures >= VIDEO_MAX_FAILURES) {
			INFO("No video frames (%d), stopping video capture", ret);
			server_video_stop(server);
			server->video_retry_time = g_get_monotonic_time() + VIDEO_RETRY_INTERVAL;
		}
	}

	// No video available - UI layer only, at full resolution
	if (!composited) {
		if (server_ui_resize(server, screen->width, screen->height) != 0) {
			return -5;
		}

		if ((ret = server->capture.execute(framebuffer, fbsize)) != 0) {
			ERR("capture execute failed: %08x", ret);
			return -5;
		}
	}

	gint64 captured = g_get_monotonic_time();
	bool changed = server_diff_frames(server);
	gint64 diffed = g_get_monotonic_time();

	server->stats_frames += 1;
	server->stats_capture_time += captured - now;
	server->stats_diff_time += diffed - captured;
	if (diffed - server->stats_start >= 10 * G_USEC_PER_SEC) {
		DBG("%.1f frames/s, %d ms capture, %d ms diff per frame",
			server->stats_frames * (double) G_USEC_PER_SEC / (diffed - server->stats_start),
			(int) (server->stats_capture_time / server->stats_frames / 1000),
			(int) (server->stats_diff_time / server->stats_frames / 1000));
		server->stats_start = diffed;
		server->stats_frames = 0;
		server->stats_capture_time = 0;
		server->stats_diff_time = 0;
	}

	// Unchanged frames don't need to be sent at all
	const gint64 frame_interval = G_USEC_PER_SEC / MAX(server->settings->framerate, 1);
	if (changed) {
		server->back_ready = true;
		server->back_low_chroma = low_chroma;
		server->idle_delay = 0;
		server_try_swap(server, diffed);
	} else {
		server->idle_delay = MIN(MAX(server->idle_delay * 2, frame_interval), IDLE_MAX_DELAY);
	}
	server->next_capture_time = now + MAX(server->idle_delay, frame_interval);

	return 0;
}


int server_stop(server_t* server) {
	INFO("Shutting down...");
	g_source_remove(server->timeout_ref);
	server->running = false;
	server_video_destroy(server);
	if (server->ui_width != 0) {
		server->capture.destroy();
	}
	rfbShutdownServer(server->screen, TRUE);
	free(server->screen->frameBuffer);
	free(server->back_buffer);
	free(server->changed_bands);
	server->back_buffer = NULL;
	server->changed_bands = NULL;
	pthread_rwlock_destroy(&server->fb_lock);
	rfbScreenCleanup(server->screen);
	shutdown_uinput();

	return 0;
}
