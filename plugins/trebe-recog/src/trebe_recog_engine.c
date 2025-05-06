/*
 * Copyright 2025 Irontec S.L.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Mandatory rules concerning plugin implementation.
 * 1. Each plugin MUST implement a plugin/engine creator function
 *    with the exact signature and name (the main entry point)
 *        MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t
 * *pool)
 * 2. Each plugin MUST declare its version number
 *        MRCP_PLUGIN_VERSION_DECLARE
 * 3. One and only one response MUST be sent back to the received request.
 * 4. Methods (callbacks) of the MRCP engine channel MUST not block.
 *   (asynchronous response can be sent from the context of other thread)
 * 5. Methods (callbacks) of the MPF engine stream MUST not block.
 */
#include "apt_consumer_task.h"
#include "apt_log.h"
#include "mpf_activity_detector.h"
#include "mrcp_recog_engine.h"
#include <apr_hash.h>
#include <apr_uuid.h>
#include <json-c/json.h>
#include <libwebsockets.h>

#define RECOG_ENGINE_TASK_NAME "Trebe Recog Engine"

typedef struct trebe_recog_engine_t trebe_recog_engine_t;
typedef struct trebe_recog_channel_t trebe_recog_channel_t;
typedef struct trebe_recog_msg_t trebe_recog_msg_t;

/** Declaration of recognizer engine methods */
static apt_bool_t trebe_recog_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t trebe_recog_engine_open(mrcp_engine_t *engine);
static apt_bool_t trebe_recog_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t *trebe_recog_engine_channel_create(mrcp_engine_t *engine,
                                                                apr_pool_t *pool);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
    trebe_recog_engine_destroy, trebe_recog_engine_open, trebe_recog_engine_close,
    trebe_recog_engine_channel_create
};

/** Declaration of recognizer channel methods */
static apt_bool_t trebe_recog_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t trebe_recog_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t trebe_recog_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t trebe_recog_channel_request_process(mrcp_engine_channel_t *channel,
                                                      mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
    trebe_recog_channel_destroy, trebe_recog_channel_open, trebe_recog_channel_close,
    trebe_recog_channel_request_process
};

/** Declaration of recognizer audio stream methods */
static apt_bool_t trebe_recog_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t trebe_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t trebe_recog_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t trebe_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
    trebe_recog_stream_destroy, NULL, NULL, NULL, trebe_recog_stream_open, trebe_recog_stream_close,
    trebe_recog_stream_write,   NULL
};

static apt_bool_t trebe_recog_recognition_complete(trebe_recog_channel_t *recog_channel,
                                                   mrcp_recog_completion_cause_e cause);

/** Declaration of demo recognizer channel */
struct trebe_recog_channel_t {
    /** Back pointer to engine */
    trebe_recog_engine_t *trebe_engine;
    /** Engine channel base */
    mrcp_engine_channel_t *channel;

    /** Active (in-progress) recognition request */
    mrcp_message_t *recog_request;
    /** Pending stop response */
    mrcp_message_t *stop_response;
    /** Indicates whether input timers are started */
    apt_bool_t timers_started;
    /** Voice activity detector */
    mpf_activity_detector_t *detector;
    /** File to write utterance to */
    FILE *audio_out;
    /** Websocket context */
    struct lws_context *websocket_context;
    /** Websocket connection */
    struct lws *websocket;
    /** Websocket thread */
    pthread_t websocket_thread;
    /** Session ID */
    const char *session_id;
    /** Final transcription */
    const char *stable_text;
};

typedef enum {
    TREBE_RECOG_MSG_OPEN_CHANNEL,
    TREBE_RECOG_MSG_CLOSE_CHANNEL,
    TREBE_RECOG_MSG_REQUEST_PROCESS
} trebe_recog_msg_type_e;

/** Declaration of demo recognizer task message */
struct trebe_recog_msg_t {
    trebe_recog_msg_type_e type;
    mrcp_engine_channel_t *channel;
    mrcp_message_t *request;
};

static unsigned char *create_initial_json_message(size_t *out_size, const char **out_str,
                                                  char *wav_name)
{
    return NULL;
    json_object *json_msg = json_object_new_object();
    json_object_object_add(json_msg, "mode", json_object_new_string("2pass"));
    json_object_object_add(json_msg, "wav_name", json_object_new_string(wav_name));
    json_object_object_add(json_msg, "is_speaking", json_object_new_boolean(1));
    json_object_object_add(json_msg, "wav_format", json_object_new_string("pcm"));

    json_object *chunk_size_array = json_object_new_array();
    json_object_array_add(chunk_size_array, json_object_new_int(5));
    json_object_array_add(chunk_size_array, json_object_new_int(10));
    json_object_array_add(chunk_size_array, json_object_new_int(5));
    json_object_object_add(json_msg, "chunk_size", chunk_size_array);

    const char *json_str = json_object_to_json_string_ext(json_msg, JSON_C_TO_STRING_PLAIN);
    size_t json_str_len = strlen(json_str);
    *out_str = strdup(json_str);

    size_t buffer_size = LWS_SEND_BUFFER_PRE_PADDING + json_str_len + LWS_SEND_BUFFER_POST_PADDING;
    unsigned char *buffer = (unsigned char *)malloc(buffer_size);
    if (buffer) {
        memcpy(buffer + LWS_SEND_BUFFER_PRE_PADDING, json_str, json_str_len);
        *out_size = json_str_len;
    }

    json_object_put(json_msg);

    return buffer;
}

static unsigned char *create_final_json_message(size_t *out_size, const char **out_str)
{
    json_object *json_msg = json_object_new_object();
    json_object_object_add(json_msg, "is_speaking", json_object_new_boolean(0));

    const char *json_str = json_object_to_json_string_ext(json_msg, JSON_C_TO_STRING_PLAIN);
    size_t json_str_len = strlen(json_str);
    *out_str = strdup(json_str);

    size_t buffer_size = LWS_SEND_BUFFER_PRE_PADDING + json_str_len + LWS_SEND_BUFFER_POST_PADDING;
    unsigned char *buffer = (unsigned char *)malloc(buffer_size);
    if (buffer) {
        memcpy(buffer + LWS_SEND_BUFFER_PRE_PADDING, json_str, json_str_len);
        *out_size = json_str_len;
    }

    json_object_put(json_msg);

    return buffer;
}

static void send_authorization_message(struct lws *wsi)
{
    struct json_object *auth = json_object_new_object();
    struct json_object *payload = json_object_new_object();
    json_object_object_add(payload, "key",
                           json_object_new_string("aL8oLaGlk5tZyWeZQW6BmVc19UocpRre"));
    json_object_object_add(auth, "message_type", json_object_new_string("authorization"));
    json_object_object_add(auth, "payload", payload);

    const char *msg = json_object_to_json_string(auth);
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "Authorization sent: %.*s\n", (int)strlen(msg),
            (char *)msg);
    lws_write(wsi, (unsigned char *)msg, strlen(msg), LWS_WRITE_TEXT);
    // json_object_put(auth);
}

static void send_configuration_message(struct lws *wsi)
{
    struct json_object *cfg = json_object_new_object();
    struct json_object *payload = json_object_new_object();

    json_object_object_add(payload, "audio_format", json_object_new_string("audio/l16;rate=8000"));
    json_object_object_add(payload, "recv_transcription", json_object_new_boolean(1));
    json_object_object_add(payload, "recv_translation", json_object_new_boolean(0));
    json_object_object_add(payload, "transcription_interim_results", json_object_new_boolean(0));

    json_object_object_add(cfg, "message_type", json_object_new_string("configuration"));
    json_object_object_add(cfg, "payload", payload);

    const char *msg = json_object_to_json_string(cfg);
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "Configuration sent: %.*s\n", (int)strlen(msg),
            (char *)msg);
    lws_write(wsi, (unsigned char *)msg, strlen(msg), LWS_WRITE_TEXT);
    // json_object_put(cfg);

    lws_callback_on_writable(wsi);
}

static void process_response_message(struct lws *wsi, const char *response, size_t size)
{
    json_object *parsed_json, *jmsg_type, *jsession_id, *jpayload, *jstable_text;

    trebe_recog_channel_t *recog_channel = lws_context_user(lws_get_context(wsi));
    if (!recog_channel) {
        apt_log(APT_LOG_MARK, APT_PRIO_WARNING,
                "Failed to get recog channel from WebSocket context");
        return;
    }

    json_tokener *tok = json_tokener_new();
    parsed_json = json_tokener_parse_ex(tok, response, size);
    json_tokener_free(tok);

    if (!json_object_object_get_ex(parsed_json, "message_type", &jmsg_type)) {
        apt_log(APT_LOG_MARK, APT_PRIO_WARNING, "Failed to parse message_type from response");
        json_object_put(parsed_json);
        return;
    }

    const char *msg_type = json_object_get_string(jmsg_type);
    if (strcmp(msg_type, "session_welcome") == 0) {
        if (json_object_object_get_ex(parsed_json, "payload", &jpayload) &&
            json_object_object_get_ex(jpayload, "session_id", &jsession_id)) {
            recog_channel->session_id = strdup(json_object_get_string(jsession_id));
            apt_log(APT_LOG_MARK, APT_PRIO_INFO, "Session ID: %s", recog_channel->session_id);
        } else {
            apt_log(APT_LOG_MARK, APT_PRIO_WARNING, "Failed to parse session_id from payload");
        }
        send_configuration_message(wsi);
    }

    if (strcmp(msg_type, "result") == 0) {
        if (json_object_object_get_ex(parsed_json, "payload", &jpayload) &&
            json_object_object_get_ex(jpayload, "stable_text", &jstable_text)) {
            recog_channel->stable_text = strdup(json_object_get_string(jstable_text));
            apt_log(APT_LOG_MARK, APT_PRIO_INFO, "Stable text: %s", recog_channel->stable_text);
            trebe_recog_recognition_complete(recog_channel, RECOGNIZER_COMPLETION_CAUSE_SUCCESS);
        } else {
            apt_log(APT_LOG_MARK, APT_PRIO_WARNING, "Failed to parse stable_text from payload");
        }
    }

    json_object_put(parsed_json);
}

static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in,
                       size_t len)
{
    switch (reason) {
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        apt_log(APT_LOG_MARK, APT_PRIO_WARNING, "WebSocket connection error: %s", (const char *)in);
        break;
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        apt_log(APT_LOG_MARK, APT_PRIO_INFO, "WebSocket connection established");
        send_authorization_message(wsi);
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        apt_log(APT_LOG_MARK, APT_PRIO_INFO, "Received data: %.*s\n", (int)len, (char *)in);
        process_response_message(wsi, (const char *)in, len);
        break;
    default:
        break;
    }

    return 0;
}

static struct lws_protocols protocols[] = { {
                                                .name = "ws-protocol",
                                                .callback = callback_ws,
                                                .per_session_data_size = 0,
                                                .rx_buffer_size = 4096,
                                            },
                                            { NULL, NULL, 0, 0 } };

typedef struct trebe_recog_engine_t {
    apt_consumer_task_t *task;
} trebe_recog_engine_t;

static apt_bool_t trebe_recog_msg_signal(trebe_recog_msg_type_e type,
                                         mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t trebe_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source
 * priority. <source name="RECOG-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(RECOG_PLUGIN, "RECOG-PLUGIN")

/** Use custom log source mark */
#define RECOG_LOG_MARK APT_LOG_MARK_DECLARE(RECOG_PLUGIN)

/** Create demo recognizer engine */
MRCP_PLUGIN_DECLARE(mrcp_engine_t *)
mrcp_plugin_create(apr_pool_t *pool)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> mrcp_plugin_create");
    trebe_recog_engine_t *trebe_engine = apr_palloc(pool, sizeof(trebe_recog_engine_t));
    apt_task_t *task;
    apt_task_vtable_t *vtable;
    apt_task_msg_pool_t *msg_pool;

    msg_pool = apt_task_msg_pool_create_dynamic(sizeof(trebe_recog_msg_t), pool);
    trebe_engine->task = apt_consumer_task_create(trebe_engine, msg_pool, pool);
    if (!trebe_engine->task) {
        return NULL;
    }

    task = apt_consumer_task_base_get(trebe_engine->task);
    apt_task_name_set(task, RECOG_ENGINE_TASK_NAME);
    vtable = apt_task_vtable_get(task);
    if (vtable) {
        vtable->process_msg = trebe_recog_msg_process;
    }

    /* create engine base */
    return mrcp_engine_create(MRCP_RECOGNIZER_RESOURCE, /* MRCP resource identifier */
                              trebe_engine,             /* object to associate */
                              &engine_vtable,           /* virtual methods table of engine */
                              pool);                    /* pool to allocate memory from */
}

/** Destroy recognizer engine */
static apt_bool_t trebe_recog_engine_destroy(mrcp_engine_t *engine)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_engine_destroy");
    trebe_recog_engine_t *trebe_engine = engine->obj;
    if (trebe_engine->task) {
        apt_task_t *task = apt_consumer_task_base_get(trebe_engine->task);
        apt_task_destroy(task);
        trebe_engine->task = NULL;
    }
    return TRUE;
}

void *websocket_event_loop(void *arg)
{
    trebe_recog_channel_t *recog_channel = (trebe_recog_channel_t *)arg;
    while (1) {
        lws_service(recog_channel->websocket_context, 100); // 100ms timeout
    }
    return NULL;
}

/** Open recognizer engine */
static apt_bool_t trebe_recog_engine_open(mrcp_engine_t *engine)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_engine_open");
    trebe_recog_engine_t *trebe_engine = engine->obj;
    if (trebe_engine->task) {
        apt_task_t *task = apt_consumer_task_base_get(trebe_engine->task);
        apt_task_start(task);
    }

    return mrcp_engine_open_respond(engine, TRUE);
}

/** Close recognizer engine */
static apt_bool_t trebe_recog_engine_close(mrcp_engine_t *engine)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_engine_close");
    return mrcp_engine_close_respond(engine);
}

static mrcp_engine_channel_t *trebe_recog_engine_channel_create(mrcp_engine_t *engine,
                                                                apr_pool_t *pool)
{
    mpf_stream_capabilities_t *capabilities;
    mpf_termination_t *termination;
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_engine_channel_create");

    /* create demo recog channel */
    trebe_recog_channel_t *recog_channel = apr_palloc(pool, sizeof(trebe_recog_channel_t));
    recog_channel->trebe_engine = engine->obj;
    recog_channel->recog_request = NULL;
    recog_channel->stop_response = NULL;
    recog_channel->detector = mpf_activity_detector_create(pool);
    recog_channel->audio_out = NULL;

    capabilities = mpf_sink_stream_capabilities_create(pool);
    mpf_codec_capabilities_add(&capabilities->codecs, MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
                               "LPCM");

    /* create media termination */
    termination = mrcp_engine_audio_termination_create(
        recog_channel,        /* object to associate */
        &audio_stream_vtable, /* virtual methods table of audio stream */
        capabilities,         /* stream capabilities */
        pool);                /* pool to allocate memory from */

    /* create engine channel base */
    recog_channel->channel =
        mrcp_engine_channel_create(engine,          /* engine */
                                   &channel_vtable, /* virtual methods table of engine channel */
                                   recog_channel,   /* object to associate */
                                   termination,     /* associated media termination */
                                   pool);           /* pool to allocate memory from */

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.user = recog_channel;

    recog_channel->websocket_context = lws_create_context(&info);
    if (!recog_channel->websocket_context) {
        apt_log(APT_LOG_MARK, APT_PRIO_WARNING, "Failed to create WebSocket context");
        return FALSE;
    }

    struct lws_client_connect_info connect_info = {
        .context = recog_channel->websocket_context,
        .address = "realtime-irontec.trebesrv.com",
        .port = 443,
        .host = "realtime-irontec.trebesrv.com",
        .origin = "realtime-irontec.trebesrv.com",
        .protocol = protocols[0].name,
        .ssl_connection =
            LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK,
        .path = "/?model=es_eu-Telephonic",
    };

    recog_channel->websocket = lws_client_connect_via_info(&connect_info);
    if (!recog_channel->websocket) {
        apt_log(APT_LOG_MARK, APT_PRIO_WARNING, "Failed to connect WebSocket");
        lws_context_destroy(recog_channel->websocket_context);
        recog_channel->websocket_context = NULL;
        return FALSE;
    }

    if (pthread_create(&recog_channel->websocket_thread, NULL, websocket_event_loop,
                       recog_channel) != 0) {
        apt_log(APT_LOG_MARK, APT_PRIO_WARNING, "Failed to create WebSocket event loop thread");
        return FALSE;
    }
    pthread_detach(recog_channel->websocket_thread);

    return recog_channel->channel;
}

/** Destroy engine channel */
static apt_bool_t trebe_recog_channel_destroy(mrcp_engine_channel_t *channel)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_channel_destroy");

    trebe_recog_channel_t *recog_channel = channel->method_obj;
    if (recog_channel->websocket_context) {
        pthread_cancel(recog_channel->websocket_thread);
        lws_context_destroy(recog_channel->websocket_context);
        recog_channel->websocket_context = NULL;
    }

    return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t trebe_recog_channel_open(mrcp_engine_channel_t *channel)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_channel_open");
    if (channel->attribs) {
        /* process attributes */
        const apr_array_header_t *header = apr_table_elts(channel->attribs);
        apr_table_entry_t *entry = (apr_table_entry_t *)header->elts;
        for (int i = 0; i < header->nelts; i++) {
            apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Attrib name [%s] value [%s]", entry[i].key,
                    entry[i].val);
        }
    }

    return trebe_recog_msg_signal(TREBE_RECOG_MSG_OPEN_CHANNEL, channel, NULL);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t trebe_recog_channel_close(mrcp_engine_channel_t *channel)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_channel_close");
    return trebe_recog_msg_signal(TREBE_RECOG_MSG_CLOSE_CHANNEL, channel, NULL);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t trebe_recog_channel_request_process(mrcp_engine_channel_t *channel,
                                                      mrcp_message_t *request)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_channel_request_process");
    return trebe_recog_msg_signal(TREBE_RECOG_MSG_REQUEST_PROCESS, channel, request);
}

/** Process RECOGNIZE request */
static apt_bool_t trebe_recog_channel_recognize(mrcp_engine_channel_t *channel,
                                                mrcp_message_t *request, mrcp_message_t *response)
{
    /* process RECOGNIZE request */
    mrcp_recog_header_t *recog_header;
    trebe_recog_channel_t *recog_channel = channel->method_obj;
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_channel_recognize");
    const mpf_codec_descriptor_t *descriptor = mrcp_engine_sink_stream_codec_get(channel);

    if (!descriptor) {
        apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, "Failed to Get Codec Descriptor " APT_SIDRES_FMT,
                MRCP_MESSAGE_SIDRES(request));
        response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
        return FALSE;
    }

    recog_channel->timers_started = TRUE;

    /* get recognizer header */
    recog_header = mrcp_resource_header_get(request);
    if (recog_header) {
        if (mrcp_resource_header_property_check(request, RECOGNIZER_HEADER_START_INPUT_TIMERS) ==
            TRUE) {
            recog_channel->timers_started = recog_header->start_input_timers;
        }
        if (mrcp_resource_header_property_check(request, RECOGNIZER_HEADER_NO_INPUT_TIMEOUT) ==
            TRUE) {
            mpf_activity_detector_noinput_timeout_set(recog_channel->detector,
                                                      recog_header->no_input_timeout);
        }
        if (mrcp_resource_header_property_check(
                request, RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT) == TRUE) {
            mpf_activity_detector_silence_timeout_set(recog_channel->detector,
                                                      recog_header->speech_complete_timeout);
        }
    }

    response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
    /* send asynchronous response */
    mrcp_engine_channel_message_send(channel, response);
    recog_channel->recog_request = request;
    return TRUE;
}

/** Process STOP request */
static apt_bool_t trebe_recog_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request,
                                           mrcp_message_t *response)
{

    /* process STOP request */
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_channel_stop");
    trebe_recog_channel_t *recog_channel = channel->method_obj;
    /* store STOP request, make sure there is no more activity and only then
     * send the response */
    recog_channel->stop_response = response;
    return TRUE;
}

/** Process START-INPUT-TIMERS request */
static apt_bool_t trebe_recog_channel_timers_start(mrcp_engine_channel_t *channel,
                                                   mrcp_message_t *request,
                                                   mrcp_message_t *response)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_channel_timers_start");
    trebe_recog_channel_t *recog_channel = channel->method_obj;
    recog_channel->timers_started = TRUE;
    return mrcp_engine_channel_message_send(channel, response);
}

/** Dispatch MRCP request */
static apt_bool_t trebe_recog_channel_request_dispatch(mrcp_engine_channel_t *channel,
                                                       mrcp_message_t *request)
{
    apt_bool_t processed = FALSE;
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG,
            "trebe method ==> trebe_recog_channel_request_dispatch");
    mrcp_message_t *response = mrcp_response_create(request, request->pool);
    switch (request->start_line.method_id) {
    case RECOGNIZER_SET_PARAMS:
        break;
    case RECOGNIZER_GET_PARAMS:
        break;
    case RECOGNIZER_DEFINE_GRAMMAR:
        break;
    case RECOGNIZER_RECOGNIZE:
        processed = trebe_recog_channel_recognize(channel, request, response);
        break;
    case RECOGNIZER_GET_RESULT:
        break;
    case RECOGNIZER_START_INPUT_TIMERS:
        processed = trebe_recog_channel_timers_start(channel, request, response);
        break;
    case RECOGNIZER_STOP:
        processed = trebe_recog_channel_stop(channel, request, response);
        break;
    default:
        break;
    }
    if (processed == FALSE) {
        /* send asynchronous response for not handled request */
        mrcp_engine_channel_message_send(channel, response);
    }
    return TRUE;
}

/** Callback is called from MPF engine context to destroy any additional data
 * associated with audio stream */
static apt_bool_t trebe_recog_stream_destroy(mpf_audio_stream_t *stream)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_stream_destroy");
    return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open
 */
static apt_bool_t trebe_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_stream_open");
    return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close
 */
static apt_bool_t trebe_recog_stream_close(mpf_audio_stream_t *stream)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_stream_close");
    return TRUE;
}

/* Raise demo START-OF-INPUT event */
static apt_bool_t trebe_recog_start_of_input(trebe_recog_channel_t *recog_channel)
{
    /* create START-OF-INPUT event */
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_start_of_input");
    mrcp_message_t *message =
        mrcp_event_create(recog_channel->recog_request, RECOGNIZER_START_OF_INPUT,
                          recog_channel->recog_request->pool);
    if (!message) {
        return FALSE;
    }

    /* set request state */
    message->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
    /* send asynch event */
    return mrcp_engine_channel_message_send(recog_channel->channel, message);
}

static apt_bool_t trebe_recog_result_load(trebe_recog_channel_t *recog_channel,
                                          mrcp_message_t *message)
{
    apt_str_t *body = &message->body;
    if (recog_channel->stable_text == NULL) {
        body->buf = apr_psprintf(message->pool, "NO_RESULT");
    } else {
        body->buf = apr_psprintf(message->pool,
                                 "<?xml version=\"1.0\"?>\n"
                                 "<result>\n"
                                 "  <interpretation confidence=\"%d\">\n"
                                 "    <instance>%s</instance>\n"
                                 "    <input mode=\"speech\">%s</input>\n"
                                 "  </interpretation>\n"
                                 "</result>\n",
                                 99, recog_channel->stable_text, recog_channel->stable_text);
    }
    if (body->buf) {
        mrcp_generic_header_t *generic_header;
        generic_header = mrcp_generic_header_prepare(message);
        if (generic_header) {
            /* set content type */
            apt_string_assign(&generic_header->content_type, "application/x-nlsml", message->pool);
            mrcp_generic_header_property_add(message, GENERIC_HEADER_CONTENT_TYPE);
        }

        body->length = strlen(body->buf);
    }
    return TRUE;
}

/* Raise demo RECOGNITION-COMPLETE event */
static apt_bool_t trebe_recog_recognition_complete(trebe_recog_channel_t *recog_channel,
                                                   mrcp_recog_completion_cause_e cause)
{
    mrcp_recog_header_t *recog_header;
    /* create RECOGNITION-COMPLETE event */
    mrcp_message_t *message =
        mrcp_event_create(recog_channel->recog_request, RECOGNIZER_RECOGNITION_COMPLETE,
                          recog_channel->recog_request->pool);
    if (!message) {
        return FALSE;
    }

    /* get/allocate recognizer header */
    recog_header = mrcp_resource_header_prepare(message);
    if (recog_header) {
        /* set completion cause */
        recog_header->completion_cause = cause;
        mrcp_resource_header_property_add(message, RECOGNIZER_HEADER_COMPLETION_CAUSE);
    }
    /* set request state */
    message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

    if (cause == RECOGNIZER_COMPLETION_CAUSE_SUCCESS) {
        trebe_recog_result_load(recog_channel, message);
    }

    recog_channel->recog_request = NULL;
    /* send asynch event */
    return mrcp_engine_channel_message_send(recog_channel->channel, message);
}

/* Raise demo RECOGNITION-COMPLETE event */
static apt_bool_t trebe_recog_write(trebe_recog_channel_t *recog_channel, const mpf_frame_t *frame)
{
    if (!recog_channel->session_id) {
        apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, "Websocket session not yet ready");
        return -1;
    }

    struct lws *wsi = recog_channel->websocket;
    size_t size = frame->codec_frame.size;
    const unsigned char *data = frame->codec_frame.buffer;
    unsigned char *buffer =
        malloc(LWS_SEND_BUFFER_PRE_PADDING + size + LWS_SEND_BUFFER_POST_PADDING);
    if (!buffer) {
        apt_log(RECOG_LOG_MARK, APT_PRIO_ERROR, "Failed to allocate memory for WebSocket write");
        return -1;
    }
    memcpy(buffer + LWS_SEND_BUFFER_PRE_PADDING, data, size);
    int result = lws_write(wsi, buffer + LWS_SEND_BUFFER_PRE_PADDING, size, LWS_WRITE_BINARY);
    free(buffer);
    return result > 0;
}

/** Callback is called from MPF engine context to write/send new frame */
static apt_bool_t trebe_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame)
{
    trebe_recog_channel_t *recog_channel = stream->obj;
    if (recog_channel->stop_response) {
        /* send asynchronous response to STOP request */
        mrcp_engine_channel_message_send(recog_channel->channel, recog_channel->stop_response);
        recog_channel->stop_response = NULL;
        recog_channel->recog_request = NULL;
        return TRUE;
    }

    if (frame->codec_frame.size) {
        trebe_recog_write(recog_channel, frame);
    }

    if (recog_channel->recog_request) {
        mpf_detector_event_e det_event =
            mpf_activity_detector_process(recog_channel->detector, frame);
        switch (det_event) {
        case MPF_DETECTOR_EVENT_ACTIVITY:
            apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Detected Voice Activity " APT_SIDRES_FMT,
                    MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
            trebe_recog_start_of_input(recog_channel);
            break;
        case MPF_DETECTOR_EVENT_INACTIVITY:
            apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Detected Voice Inactivity " APT_SIDRES_FMT,
                    MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
            trebe_recog_recognition_complete(recog_channel, RECOGNIZER_COMPLETION_CAUSE_SUCCESS);
            break;
        case MPF_DETECTOR_EVENT_NOINPUT:
            apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Detected Noinput " APT_SIDRES_FMT,
                    MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
            if (recog_channel->timers_started == TRUE) {
                trebe_recog_recognition_complete(recog_channel,
                                                 RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT);
            }
            break;
        default:
            break;
        }

        if (recog_channel->recog_request) {
            if ((frame->type & MEDIA_FRAME_TYPE_EVENT) == MEDIA_FRAME_TYPE_EVENT) {
                if (frame->marker == MPF_MARKER_START_OF_EVENT) {
                    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO,
                            "Detected Start of Event " APT_SIDRES_FMT " id:%d",
                            MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
                            frame->event_frame.event_id);
                } else if (frame->marker == MPF_MARKER_END_OF_EVENT) {
                    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO,
                            "Detected End of Event " APT_SIDRES_FMT " id:%d duration:%d ts",
                            MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
                            frame->event_frame.event_id, frame->event_frame.duration);
                }
            }
        }

        if (recog_channel->audio_out) {
            fwrite(frame->codec_frame.buffer, 1, frame->codec_frame.size, recog_channel->audio_out);
        }
    }
    return TRUE;
}

static apt_bool_t trebe_recog_msg_signal(trebe_recog_msg_type_e type,
                                         mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
    apt_bool_t status = FALSE;
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_msg_signal");
    trebe_recog_channel_t *trebe_channel = channel->method_obj;
    trebe_recog_engine_t *trebe_engine = trebe_channel->trebe_engine;
    apt_task_t *task = apt_consumer_task_base_get(trebe_engine->task);
    apt_task_msg_t *msg = apt_task_msg_get(task);
    if (msg) {
        trebe_recog_msg_t *trebe_msg;
        msg->type = TASK_MSG_USER;
        trebe_msg = (trebe_recog_msg_t *)msg->data;

        trebe_msg->type = type;
        trebe_msg->channel = channel;
        trebe_msg->request = request;
        status = apt_task_msg_signal(task, msg);
    }
    return status;
}

static apt_bool_t trebe_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "trebe method ==> trebe_recog_msg_process");
    trebe_recog_msg_t *trebe_msg = (trebe_recog_msg_t *)msg->data;
    switch (trebe_msg->type) {
    case TREBE_RECOG_MSG_OPEN_CHANNEL:
        /* open channel and send asynch response */
        mrcp_engine_channel_open_respond(trebe_msg->channel, TRUE);
        break;
    case TREBE_RECOG_MSG_CLOSE_CHANNEL: {
        /* close channel, make sure there is no activity and send asynch
         * response */
        trebe_recog_channel_t *recog_channel = trebe_msg->channel->method_obj;
        if (recog_channel->audio_out) {
            fclose(recog_channel->audio_out);
            recog_channel->audio_out = NULL;
        }

        mrcp_engine_channel_close_respond(trebe_msg->channel);
        break;
    }
    case TREBE_RECOG_MSG_REQUEST_PROCESS:
        trebe_recog_channel_request_dispatch(trebe_msg->channel, trebe_msg->request);
        break;
    default:
        break;
    }
    return TRUE;
}