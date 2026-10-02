#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <pbnjson.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "service.h"
#include "log.h"

#define SERVICE_NAME "org.webosbrew.vncserver.service"

#define AUTOSTART_SYMLINK_DIR "/var/lib/webosbrew/init.d"
#define AUTOSTART_SYMLINK_PATH AUTOSTART_SYMLINK_DIR "/webos-vncserver"
#define AUTOSTART_SCRIPT_PATH "/media/developer/apps/usr/palm/services/org.webosbrew.vncserver.service/autostart.sh"

#define VIDEOOUTPUT_STATUS_URI "luna://com.webos.service.videooutput/getStatus"
#define VIDEOOUTPUT_RETRY_INTERVAL 10

static const char* dynamic_range_names[] = {"SDR", "PQ", "HLG"};

server_t* server_p;
settings_t* settings_p;
GMainLoop* loop_p;

bool method_start(LSHandle *sh, LSMessage *message, void *data) {
	int ret;
	LSError lserror;
	LSErrorInit(&lserror);

	jvalue_ref jobj = jobject_create();
	if (jis_null(jobj)) {
		j_release(&jobj);
		return false;
	}

	if (server_p->running) {
		jobject_set(jobj, j_cstr_to_buffer("returnValue"), jboolean_create(TRUE));
		jobject_set(jobj, j_cstr_to_buffer("message"), jstring_create("Running already"));
	} else {
		if ((ret = server_start(server_p, settings_p)) == 0) {
			server_bind_gmainloop(server_p);
			INFO("Server started, replying...");
			server_p->running = true;
			jobject_set(jobj, j_cstr_to_buffer("returnValue"), jboolean_create(TRUE));
			jobject_set(jobj, j_cstr_to_buffer("message"), jstring_create("Started"));
		} else {
			jobject_set(jobj, j_cstr_to_buffer("returnValue"), jboolean_create(FALSE));
			jobject_set(jobj, j_cstr_to_buffer("message"), jstring_create("Startup failed"));
			jobject_set(jobj, j_cstr_to_buffer("errorCode"), jnumber_create_i32(ret));
		}
	}

	LSMessageReply(sh, message, jvalue_tostring_simple(jobj), &lserror);
	j_release(&jobj);

	return true;
}

bool method_stop(LSHandle *sh, LSMessage *message, void *data) {
	LSError lserror;
	LSErrorInit(&lserror);

	jvalue_ref jobj = jobject_create();
	if (jis_null(jobj)) {
		j_release(&jobj);
		return false;
	}

	if (server_p->running) {
		server_stop(server_p);
		server_p->running = false;
		jobject_set(jobj, j_cstr_to_buffer("returnValue"), jboolean_create(TRUE));
		jobject_set(jobj, j_cstr_to_buffer("message"), jstring_create("Stopped"));
	} else {
		jobject_set(jobj, j_cstr_to_buffer("returnValue"), jboolean_create(TRUE));
		jobject_set(jobj, j_cstr_to_buffer("message"), jstring_create("Already stopped"));
	}

	LSMessageReply(sh, message, jvalue_tostring_simple(jobj), &lserror);
	j_release(&jobj);

	return true;
}

bool method_configure(LSHandle *sh, LSMessage *message, void *data) {
	int ret;
	LSError lserror;
	JSchemaInfo schema;
	jvalue_ref parsed;

	LSErrorInit(&lserror);

	jschema_info_init (&schema, jschema_all(), NULL, NULL);
	parsed = jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(message)), DOMOPT_NOOPT, &schema);

	if (jis_null(parsed)) {
		j_release(&parsed);
		return false;
	}

	bool was_running = server_p->running;

	if (was_running) {
		server_stop(server_p);
	}

	settings_load_json(settings_p, parsed);

	INFO("settings: %dx%d - password: %s", settings_p->width, settings_p->height, settings_p->password);

	jvalue_ref jobj = jobject_create();
	if (jis_null(jobj)) {
		j_release(&jobj);
		return false;
	}

	if (settings_p->autostart) {
		// While we generally are guaranteed to have /var/lib/webosbrew, it's
		// not the case with /var/lib/webosbrew/init.d...
		mkdir(AUTOSTART_SYMLINK_DIR, 0755);

		if (symlink(AUTOSTART_SCRIPT_PATH, AUTOSTART_SYMLINK_PATH) != 0 && errno != EEXIST) {
			WARN("Autostart script creation failed: %s", strerror(errno));
		}
	} else {
		if (unlink(AUTOSTART_SYMLINK_PATH) != 0 && errno != ENOENT) {
			WARN("Autostart script removal failed: %s", strerror(errno));
		}
	}

	if (was_running) {
		if ((ret = server_start(server_p, settings_p)) == 0) {
			server_bind_gmainloop(server_p);
			INFO("Server started, replying...");
			jobject_set(jobj, j_cstr_to_buffer("returnValue"), jboolean_create(TRUE));
			jobject_set(jobj, j_cstr_to_buffer("message"), jstring_create("Restarted"));
		} else {
			jobject_set(jobj, j_cstr_to_buffer("returnValue"), jboolean_create(FALSE));
			jobject_set(jobj, j_cstr_to_buffer("message"), jstring_create("Startup failed"));
			jobject_set(jobj, j_cstr_to_buffer("errorCode"), jnumber_create_i32(ret));
		}
	} else {
		jobject_set(jobj, j_cstr_to_buffer("returnValue"), jboolean_create(TRUE));
		jobject_set(jobj, j_cstr_to_buffer("message"), jstring_create("Reconfigured"));
	}

	settings_save_file(settings_p, SETTINGS_PERSISTENCE_PATH);

	LSMessageReply(sh, message, jvalue_tostring_simple(jobj), &lserror);
	j_release(&jobj);

	return true;
}

bool method_status(LSHandle *sh, LSMessage *message, void *data) {
	LSError lserror;
	LSErrorInit(&lserror);

	jvalue_ref jobj = jobject_create();
	if (jis_null(jobj)) {
		j_release(&jobj);
		return false;
	}

	jobject_set(jobj, j_cstr_to_buffer("returnValue"), jboolean_create(getuid() == 0));
	jobject_set(jobj, j_cstr_to_buffer("running"), jboolean_create(server_p->running));
	jobject_set(jobj, j_cstr_to_buffer("activeClients"), jnumber_create_i32(server_p->active_clients));
	jobject_set(jobj, j_cstr_to_buffer("videoBackend"), server_p->video_loaded ? jstring_create(server_p->video.name) : jnull());
	jobject_set(jobj, j_cstr_to_buffer("videoCapturing"), jboolean_create(server_p->video_running));
	jobject_set(jobj, j_cstr_to_buffer("videoDynamicRange"), jstring_create(dynamic_range_names[server_p->video_dynamic_range]));

	jvalue_ref settings_obj = jobject_create();
	settings_save_json(settings_p, settings_obj);
	jobject_set(jobj, j_cstr_to_buffer("settings"), settings_obj);

	LSMessageReply(sh, message, jvalue_tostring_simple(jobj), &lserror);
	j_release(&jobj);

	return true;
}

bool method_quit(LSHandle *sh, LSMessage *message, void *data) {
	LSError lserror;
	LSErrorInit(&lserror);

	jvalue_ref jobj = jobject_create();
	if (jis_null(jobj)) {
		j_release(&jobj);
		return false;
	}

	jobject_set(jobj, j_cstr_to_buffer("returnValue"), jboolean_create(TRUE));

	LSMessageReply(sh, message, jvalue_tostring_simple(jobj), &lserror);
	j_release(&jobj);

	g_main_loop_quit(loop_p);

	return true;
}

// hdrType is eg. "none", "HDR10", "HLG", "DolbyVision"
static dynamic_range_t parse_dynamic_range(raw_buffer hdr_type) {
	char value[64];
	size_t len = hdr_type.m_len < sizeof(value) - 1 ? hdr_type.m_len : sizeof(value) - 1;

	for (size_t i = 0; i < len; i++) {
		value[i] = tolower((unsigned char) hdr_type.m_str[i]);
	}
	value[len] = 0;

	if (strstr(value, "hlg")) {
		return DYNAMIC_RANGE_HLG;
	}

	if (strstr(value, "hdr") || strstr(value, "dolby") || strstr(value, "pq")) {
		return DYNAMIC_RANGE_PQ;
	}

	return DYNAMIC_RANGE_SDR;
}

static gboolean videooutput_subscribe(gpointer data);

static bool videooutput_status_cb(LSHandle *sh, LSMessage *message, void *data) {
	JSchemaInfo schema;
	jschema_info_init(&schema, jschema_all(), NULL, NULL);
	jvalue_ref parsed = jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(message)), DOMOPT_NOOPT, &schema);
	jvalue_ref video = jobject_get(parsed, j_cstr_to_buffer("video"));
	jvalue_ref value;

	bool success = true;
	if ((value = jobject_get(parsed, j_cstr_to_buffer("returnValue"))) && jis_boolean(value)) jboolean_get(value, &success);

	if (!success) {
		// Eg. video output service not running yet - retry later
		WARN("Video output status subscription failed: %s", LSMessageGetPayload(message));
		g_timeout_add_seconds(VIDEOOUTPUT_RETRY_INTERVAL, videooutput_subscribe, sh);
		j_release(&parsed);
		return true;
	}

	for (ssize_t i = 0; jis_array(video) && i < jarray_size(video); i++) {
		jvalue_ref item = jarray_get(video, i);
		jvalue_ref sink = jobject_get(item, j_cstr_to_buffer("sink"));

		if (!jis_string(sink) || !jstring_equal2(sink, j_cstr_to_buffer("MAIN"))) {
			continue;
		}

		bool connected = false;
		int32_t width = 0, height = 0;
		if ((value = jobject_get(item, j_cstr_to_buffer("connected"))) && jis_boolean(value)) jboolean_get(value, &connected);
		if ((value = jobject_get(item, j_cstr_to_buffer("width"))) && jis_number(value)) jnumber_get_i32(value, &width);
		if ((value = jobject_get(item, j_cstr_to_buffer("height"))) && jis_number(value)) jnumber_get_i32(value, &height);

		video_presence_t presence = connected && width > 0 && height > 0 ? VIDEO_PRESENCE_ACTIVE : VIDEO_PRESENCE_NONE;
		if (presence != server_p->video_presence) {
			INFO("Video %s (%dx%d)", presence == VIDEO_PRESENCE_ACTIVE ? "displayed" : "not displayed", width, height);
			server_p->video_presence = presence;
			// Start capturing new video immediately
			server_p->video_retry_time = 0;
			server_p->video_failures = 0;
		}

		dynamic_range_t range = DYNAMIC_RANGE_SDR;
		jvalue_ref info = jobject_get(item, j_cstr_to_buffer("videoInfo"));
		if (jis_object(info)) {
			jvalue_ref hdr_type = jobject_get(info, j_cstr_to_buffer("hdrType"));
			if (jis_string(hdr_type)) {
				range = parse_dynamic_range(jstring_get_fast(hdr_type));
			}
		}

		if (range != server_p->video_dynamic_range) {
			INFO("Video dynamic range: %s", dynamic_range_names[range]);
			server_p->video_dynamic_range = range;
		}
	}

	j_release(&parsed);

	return true;
}

// Track displayed video state, needed for proper color conversion
static gboolean videooutput_subscribe(gpointer data) {
	LSHandle* handle = (LSHandle*) data;
	LSError lserror;
	LSErrorInit(&lserror);

	if (!LSCall(handle, VIDEOOUTPUT_STATUS_URI, "{\"subscribe\":true}", videooutput_status_cb, NULL, NULL, &lserror)) {
		WARN("Video output status subscription failed: %s", lserror.message);
		LSErrorFree(&lserror);
	}

	return G_SOURCE_REMOVE;
}

LSMethod service_methods[] = {
	{"start", method_start},
	{"stop", method_stop},
	{"configure", method_configure},
	{"status", method_status},
	{"quit", method_quit},
	{0, 0},
};

int service_init(GMainLoop* loop, server_t* server, settings_t* settings) {
	static LSError lserror;
	static LSHandle *handle = NULL;
	bool ret = FALSE;

	LSErrorInit(&lserror);

	ret = LSRegister(SERVICE_NAME, &handle, &lserror);
	if (ret == FALSE) {
		WARN("Unable to register service: %s", lserror.message);
		LSErrorFree(&lserror);
		return -1;
	}

	LSRegisterCategory(handle, "/", service_methods, NULL, NULL, &lserror);

	LSGmainAttach(handle, loop, &lserror);

	server_p = server;
	settings_p = settings;
	loop_p = loop;

	videooutput_subscribe(handle);

	return 0;
}
