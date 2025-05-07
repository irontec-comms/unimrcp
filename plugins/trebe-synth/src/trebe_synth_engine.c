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
 *        MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
 * 2. Each plugin MUST declare its version number
 *        MRCP_PLUGIN_VERSION_DECLARE
 * 3. One and only one response MUST be sent back to the received request.
 * 4. Methods (callbacks) of the MRCP engine channel MUST not block.
 *   (asynchronous response can be sent from the context of other thread)
 * 5. Methods (callbacks) of the MPF engine stream MUST not block.
 */

#include <curl/curl.h>
#include <stdlib.h>
#include "mrcp_synth_engine.h"
#include "apt_consumer_task.h"
#include "apt_log.h"

#define SYNTH_ENGINE_TASK_NAME "Trebe Synth Engine"
#define API_KEY "aL8oLaGlk5tZyWeZQW6BmVc19UocpRre"
#define BASE_URL "https://api.trebesrv.com/synthesis/v1"

typedef struct trebe_synth_engine_t trebe_synth_engine_t;
typedef struct trebe_synth_channel_t trebe_synth_channel_t;
typedef struct trebe_synth_msg_t trebe_synth_msg_t;

/** Declaration of synthesizer engine methods */
static apt_bool_t trebe_synth_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t trebe_synth_engine_open(mrcp_engine_t *engine);
static apt_bool_t trebe_synth_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* trebe_synth_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
	trebe_synth_engine_destroy,
	trebe_synth_engine_open,
	trebe_synth_engine_close,
	trebe_synth_engine_channel_create
};


/** Declaration of synthesizer channel methods */
static apt_bool_t trebe_synth_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t trebe_synth_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t trebe_synth_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t trebe_synth_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	trebe_synth_channel_destroy,
	trebe_synth_channel_open,
	trebe_synth_channel_close,
	trebe_synth_channel_request_process
};

/** Declaration of synthesizer audio stream methods */
static apt_bool_t trebe_synth_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t trebe_synth_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t trebe_synth_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t trebe_synth_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	trebe_synth_stream_destroy,
	trebe_synth_stream_open,
	trebe_synth_stream_close,
	trebe_synth_stream_read,
	NULL,
	NULL,
	NULL,
	NULL
};

/** Declaration of trebe synthesizer engine */
struct trebe_synth_engine_t {
	apt_consumer_task_t    *task;
};

/** Declaration of trebe synthesizer channel */
struct trebe_synth_channel_t {
	/** Back pointer to engine */
	trebe_synth_engine_t   *trebe_engine;
	/** Engine channel base */
	mrcp_engine_channel_t *channel;

	/** Active (in-progress) speak request */
	mrcp_message_t        *speak_request;
	/** Pending stop response */
	mrcp_message_t        *stop_response;
	/** Estimated time to complete */
	apr_size_t             time_to_complete;
	/** Is paused */
	apt_bool_t             paused;
	/** Speech source (used instead of actual synthesis) */
	FILE                  *audio_file;
};

typedef enum {
	TREBE_SYNTH_MSG_OPEN_CHANNEL,
	TREBE_SYNTH_MSG_CLOSE_CHANNEL,
	TREBE_SYNTH_MSG_REQUEST_PROCESS
} trebe_synth_msg_type_e;

/** Declaration of trebe synthesizer task message */
struct trebe_synth_msg_t {
	trebe_synth_msg_type_e  type;
	mrcp_engine_channel_t *channel;
	mrcp_message_t        *request;
};


static apt_bool_t trebe_synth_msg_signal(trebe_synth_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t trebe_synth_msg_process(apt_task_t *task, apt_task_msg_t *msg);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source priority.
 *    <source name="SYNTH-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(SYNTH_PLUGIN,"SYNTH-PLUGIN")

/** Use custom log source mark */
#define SYNTH_LOG_MARK   APT_LOG_MARK_DECLARE(SYNTH_PLUGIN)

/** Create trebe synthesizer engine */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	/* create trebe engine */
	trebe_synth_engine_t *trebe_engine = apr_palloc(pool,sizeof(trebe_synth_engine_t));
	apt_task_t *task;
	apt_task_vtable_t *vtable;
	apt_task_msg_pool_t *msg_pool;

	/* create task/thread to run trebe engine in the context of this task */
	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(trebe_synth_msg_t),pool);
	trebe_engine->task = apt_consumer_task_create(trebe_engine,msg_pool,pool);
	if(!trebe_engine->task) {
		return NULL;
	}
	task = apt_consumer_task_base_get(trebe_engine->task);
	apt_task_name_set(task,SYNTH_ENGINE_TASK_NAME);
	vtable = apt_task_vtable_get(task);
	if(vtable) {
		vtable->process_msg = trebe_synth_msg_process;
	}

	/* create engine base */
	return mrcp_engine_create(
				MRCP_SYNTHESIZER_RESOURCE, /* MRCP resource identifier */
				trebe_engine,               /* object to associate */
				&engine_vtable,            /* virtual methods table of engine */
				pool);                     /* pool to allocate memory from */
}

/** Destroy synthesizer engine */
static apt_bool_t trebe_synth_engine_destroy(mrcp_engine_t *engine)
{
	trebe_synth_engine_t *trebe_engine = engine->obj;
	if(trebe_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(trebe_engine->task);
		apt_task_destroy(task);
		trebe_engine->task = NULL;
	}
	return TRUE;
}

/** Open synthesizer engine */
static apt_bool_t trebe_synth_engine_open(mrcp_engine_t *engine)
{
	trebe_synth_engine_t *trebe_engine = engine->obj;
	if(trebe_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(trebe_engine->task);
		apt_task_start(task);
	}
	return mrcp_engine_open_respond(engine,TRUE);
}

/** Close synthesizer engine */
static apt_bool_t trebe_synth_engine_close(mrcp_engine_t *engine)
{
	trebe_synth_engine_t *trebe_engine = engine->obj;
	if(trebe_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(trebe_engine->task);
		apt_task_terminate(task,TRUE);
	}
	return mrcp_engine_close_respond(engine);
}

/** Create trebe synthesizer channel derived from engine channel base */
static mrcp_engine_channel_t* trebe_synth_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
	mpf_stream_capabilities_t *capabilities;
	mpf_termination_t *termination;

	/* create trebe synth channel */
	trebe_synth_channel_t *synth_channel = apr_palloc(pool,sizeof(trebe_synth_channel_t));
	synth_channel->trebe_engine = engine->obj;
	synth_channel->speak_request = NULL;
	synth_channel->stop_response = NULL;
	synth_channel->time_to_complete = 0;
	synth_channel->paused = FALSE;
	synth_channel->audio_file = NULL;

	capabilities = mpf_source_stream_capabilities_create(pool);
	mpf_codec_capabilities_add(
			&capabilities->codecs,
			MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
			"LPCM");

	/* create media termination */
	termination = mrcp_engine_audio_termination_create(
			synth_channel,        /* object to associate */
			&audio_stream_vtable, /* virtual methods table of audio stream */
			capabilities,         /* stream capabilities */
			pool);                /* pool to allocate memory from */

	/* create engine channel base */
	synth_channel->channel = mrcp_engine_channel_create(
			engine,               /* engine */
			&channel_vtable,      /* virtual methods table of engine channel */
			synth_channel,        /* object to associate */
			termination,          /* associated media termination */
			pool);                /* pool to allocate memory from */

	return synth_channel->channel;
}

/** Destroy engine channel */
static apt_bool_t trebe_synth_channel_destroy(mrcp_engine_channel_t *channel)
{
	/* nothing to destroy */
	return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t trebe_synth_channel_open(mrcp_engine_channel_t *channel)
{
	return trebe_synth_msg_signal(TREBE_SYNTH_MSG_OPEN_CHANNEL,channel,NULL);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t trebe_synth_channel_close(mrcp_engine_channel_t *channel)
{
	return trebe_synth_msg_signal(TREBE_SYNTH_MSG_CLOSE_CHANNEL,channel,NULL);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t trebe_synth_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	return trebe_synth_msg_signal(TREBE_SYNTH_MSG_REQUEST_PROCESS,channel,request);
}

size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    strncat((char *)userp, (char *)contents, total_size);
    return total_size;
}

static apt_bool_t trebe_synth_job_create(const char *text, const char *voice, int normalize, char *job_id) {
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    char payload[1024];
    char url[256];
    snprintf(url, sizeof(url), "%s/jobs", BASE_URL);

    snprintf(payload, sizeof(payload),
             "{\"normalize\":%s,\"voice\":\"%s\",\"text\":\"%s\"}",
             normalize ? "true" : "false", voice, text);

    curl = curl_easy_init();
    if (curl) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "apikey: " API_KEY);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        char response[1024] = {0};
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
			apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "curl_easy_perform() failed: %s", curl_easy_strerror(res));
            return FALSE;
        }

        // Extract job_id from response (basic parsing)
        char *id_start = strstr(response, "\"id\":\"");
        if (id_start) {
            id_start += 6; // Skip past '"id":"'
            char *id_end = strchr(id_start, '"');
            if (id_end) {
                strncpy(job_id, id_start, id_end - id_start);
                job_id[id_end - id_start] = '\0';
            }
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

static apt_bool_t trebe_synth_job_wait_completion(const char *job_id, int interval, int timeout) {
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    char url[256];
    snprintf(url, sizeof(url), "%s/jobs/%s/info", BASE_URL, job_id);

    curl = curl_easy_init();
    if (curl) {
        headers = curl_slist_append(headers, "apikey: " API_KEY);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        for (int elapsed = 0; elapsed < timeout; elapsed += interval) {
            char response[1024] = {0};
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

            res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
				apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "curl_easy_perform() failed: %s", curl_easy_strerror(res));
				return FALSE;
            }

            // Extract status from response (basic parsing)
            char *status_start = strstr(response, "\"status\":");
            if (status_start) {
                int status = atoi(status_start + 9);
				apt_log(APT_LOG_MARK, APT_PRIO_INFO, "Trebe TTS job status: %d", status);
                if (status == 2) {
                    curl_slist_free_all(headers);
                    curl_easy_cleanup(curl);
                    return TRUE; // Job completed
                } else if (status == -1) {
					apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "Trebe TTS job failed.");
					return FALSE;
                }
            }

            sleep(interval);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

	apt_log(APT_LOG_MARK, APT_PRIO_ERROR,  "Job did not complete in time.");
	return FALSE;
}

static apt_bool_t trebe_snyth_job_download_result(const char *job_id, const char *output_file) {
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    char url[256];
    snprintf(url, sizeof(url), "%s/jobs/%s/result", BASE_URL, job_id);

    FILE *file = fopen(output_file, "wb");
    if (!file) {
		apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "Failed to open file %s for writing", output_file);
		return FALSE;
    }

    curl = curl_easy_init();
    if (curl) {
        headers = curl_slist_append(headers, "apikey: " API_KEY);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
			apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "curl_easy_perform() failed [%d]: %s", res, curl_easy_strerror(res));
			return FALSE;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    fclose(file);
	apt_log(APT_LOG_MARK, APT_PRIO_INFO, "Trebe file generated in: %s", output_file);
}

static apt_bool_t trebe_snyth_resample(const char *input_file, const char *output_file) {
	char command[512];
	snprintf(command, sizeof(command), "sox %s -r 8000 %s", input_file, output_file);
	int ret = system(command);
	if (ret != 0) {
		apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "Failed to convert audio to 8000Hz");
		return FALSE;
	}
	apt_log(APT_LOG_MARK, APT_PRIO_INFO, "Audio converted to 8000Hz: %s", output_file);
	return TRUE;
}

static apt_bool_t trebe_synth_text_to_speech(const char *text, apr_size_t length, char *outfile)
{
	char infile[256] = {0};
	char job_id[64] = {0};
	char input[1024] = {0};
	strncpy(input, text, length);

	trebe_synth_job_create(input, "nerea", 1, job_id);
	apt_log(APT_LOG_MARK, APT_PRIO_INFO, "Trebe synth job id: %s", job_id);

    if (trebe_synth_job_wait_completion(job_id, 2, 30) == TRUE) {
		sprintf(infile, "trebe-%s-input.wav", job_id);
		sprintf(outfile, "trebe-%s-output.wav", job_id);
        trebe_snyth_job_download_result(job_id, infile);
		trebe_snyth_resample(infile, outfile);
    }

	// TODO error handling
	return TRUE;
}

static apt_bool_t synth_response_construct(mrcp_message_t *response, mrcp_status_code_e status_code, mrcp_synth_completion_cause_e completion_cause)
{
	mrcp_synth_header_t *synth_header = mrcp_resource_header_prepare(response);
	if(!synth_header) {
		return FALSE;
	}

	response->start_line.status_code = status_code;
	synth_header->completion_cause = completion_cause;
	mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_COMPLETION_CAUSE);
	return TRUE;
}

/** Process SPEAK request */
static apt_bool_t trebe_synth_channel_speak(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	char *file_path = NULL;
	trebe_synth_channel_t *synth_channel = channel->method_obj;
	const mpf_codec_descriptor_t *descriptor = mrcp_engine_source_stream_codec_get(channel);

	if(!descriptor) {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"Failed to Get Codec Descriptor " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
		return FALSE;
	}

	apt_str_t *body;
	synth_channel->speak_request = request;
	body = &synth_channel->speak_request->body;
	if(!body->length) {
		synth_channel->speak_request = NULL;
		synth_response_construct(response, MRCP_STATUS_CODE_MISSING_PARAM, SYNTHESIZER_COMPLETION_CAUSE_ERROR);
		mrcp_engine_channel_message_send(synth_channel->channel, response);
		return FALSE;
	}

	synth_channel->time_to_complete = 0;

	/* send asynchronous response */
	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	mrcp_engine_channel_message_send(channel,response);

	/* request text to speech conversion */
	synth_channel->paused = TRUE;
	file_path = malloc(256);
	trebe_synth_text_to_speech(body->buf, body->length, file_path);
	synth_channel->paused = FALSE;

	// if(channel->engine) {
	// 	char *file_name = apr_psprintf(channel->pool,"trebe-%dkHz.pcm",descriptor->sampling_rate/1000);
	// 	file_path = apt_datadir_filepath_get(channel->engine->dir_layout,file_name,channel->pool);
	// }
	if(file_path) {
		synth_channel->audio_file = fopen(file_path,"rb");
		if(synth_channel->audio_file) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Set [%s] as Speech Source " APT_SIDRES_FMT,
				file_path,
				MRCP_MESSAGE_SIDRES(request));
		}
		else {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"No Speech Source [%s] Found " APT_SIDRES_FMT,
				file_path,
				MRCP_MESSAGE_SIDRES(request));
			/* calculate estimated time to complete */
			if(mrcp_generic_header_property_check(request,GENERIC_HEADER_CONTENT_LENGTH) == TRUE) {
				mrcp_generic_header_t *generic_header = mrcp_generic_header_get(request);
				if(generic_header) {
					synth_channel->time_to_complete = generic_header->content_length * 10; /* 10 msec per character */
				}
			}
		}
	}

	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	synth_channel->speak_request = request;
	return TRUE;
}

/** Process STOP request */
static apt_bool_t trebe_synth_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	trebe_synth_channel_t *synth_channel = channel->method_obj;
	/* store the request, make sure there is no more activity and only then send the response */
	synth_channel->stop_response = response;
	return TRUE;
}

/** Process PAUSE request */
static apt_bool_t trebe_synth_channel_pause(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	trebe_synth_channel_t *synth_channel = channel->method_obj;
	synth_channel->paused = TRUE;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process RESUME request */
static apt_bool_t trebe_synth_channel_resume(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	trebe_synth_channel_t *synth_channel = channel->method_obj;
	synth_channel->paused = FALSE;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process SET-PARAMS request */
static apt_bool_t trebe_synth_channel_set_params(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	mrcp_synth_header_t *req_synth_header;
	/* get synthesizer header */
	req_synth_header = mrcp_resource_header_get(request);
	if(req_synth_header) {
		/* check voice age header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_AGE) == TRUE) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Set Voice Age [%"APR_SIZE_T_FMT"]",
				req_synth_header->voice_param.age);
		}
		/* check voice name header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_NAME) == TRUE) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Set Voice Name [%s]",
				req_synth_header->voice_param.name.buf);
		}
	}

	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process GET-PARAMS request */
static apt_bool_t trebe_synth_channel_get_params(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	mrcp_synth_header_t *req_synth_header;
	/* get synthesizer header */
	req_synth_header = mrcp_resource_header_get(request);
	if(req_synth_header) {
		mrcp_synth_header_t *res_synth_header = mrcp_resource_header_prepare(response);
		/* check voice age header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_AGE) == TRUE) {
			res_synth_header->voice_param.age = 25;
			mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_VOICE_AGE);
		}
		/* check voice name header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_NAME) == TRUE) {
			apt_string_set(&res_synth_header->voice_param.name,"David");
			mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_VOICE_NAME);
		}
	}

	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Dispatch MRCP request */
static apt_bool_t trebe_synth_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t processed = FALSE;
	mrcp_message_t *response = mrcp_response_create(request,request->pool);
	switch(request->start_line.method_id) {
		case SYNTHESIZER_SET_PARAMS:
			processed = trebe_synth_channel_set_params(channel,request,response);
			break;
		case SYNTHESIZER_GET_PARAMS:
			processed = trebe_synth_channel_get_params(channel,request,response);
			break;
		case SYNTHESIZER_SPEAK:
			processed = trebe_synth_channel_speak(channel,request,response);
			break;
		case SYNTHESIZER_STOP:
			processed = trebe_synth_channel_stop(channel,request,response);
			break;
		case SYNTHESIZER_PAUSE:
			processed = trebe_synth_channel_pause(channel,request,response);
			break;
		case SYNTHESIZER_RESUME:
			processed = trebe_synth_channel_resume(channel,request,response);
			break;
		case SYNTHESIZER_BARGE_IN_OCCURRED:
			processed = trebe_synth_channel_stop(channel,request,response);
			break;
		case SYNTHESIZER_CONTROL:
			break;
		case SYNTHESIZER_DEFINE_LEXICON:
			break;
		default:
			break;
	}
	if(processed == FALSE) {
		/* send asynchronous response for not handled request */
		mrcp_engine_channel_message_send(channel,response);
	}
	return TRUE;
}

/** Callback is called from MPF engine context to destroy any additional data associated with audio stream */
static apt_bool_t trebe_synth_stream_destroy(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t trebe_synth_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t trebe_synth_stream_close(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to read/get new frame */
static apt_bool_t trebe_synth_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame)
{
	trebe_synth_channel_t *synth_channel = stream->obj;
	/* check if STOP was requested */
	if(synth_channel->stop_response) {
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(synth_channel->channel,synth_channel->stop_response);
		synth_channel->stop_response = NULL;
		synth_channel->speak_request = NULL;
		synth_channel->paused = FALSE;
		if(synth_channel->audio_file) {
			fclose(synth_channel->audio_file);
			synth_channel->audio_file = NULL;
		}
		return TRUE;
	}

	/* check if there is active SPEAK request and it isn't in paused state */
	if(synth_channel->speak_request && synth_channel->paused == FALSE) {
		/* normal processing */
		apt_bool_t completed = FALSE;
		if(synth_channel->audio_file) {
			/* read speech from file */
			apr_size_t size = frame->codec_frame.size;
			if(fread(frame->codec_frame.buffer,1,size,synth_channel->audio_file) == size) {
				frame->type |= MEDIA_FRAME_TYPE_AUDIO;
			}
			else {
				completed = TRUE;
			}
		}
		else {
			/* fill with silence in case no file available */
			if(synth_channel->time_to_complete >= stream->rx_descriptor->frame_duration) {
				memset(frame->codec_frame.buffer,0,frame->codec_frame.size);
				frame->type |= MEDIA_FRAME_TYPE_AUDIO;
				synth_channel->time_to_complete -= stream->rx_descriptor->frame_duration;
			}
			else {
				completed = TRUE;
			}
		}

		if(completed) {
			/* raise SPEAK-COMPLETE event */
			mrcp_message_t *message = mrcp_event_create(
								synth_channel->speak_request,
								SYNTHESIZER_SPEAK_COMPLETE,
								synth_channel->speak_request->pool);
			if(message) {
				/* get/allocate synthesizer header */
				mrcp_synth_header_t *synth_header = mrcp_resource_header_prepare(message);
				if(synth_header) {
					/* set completion cause */
					synth_header->completion_cause = SYNTHESIZER_COMPLETION_CAUSE_NORMAL;
					mrcp_resource_header_property_add(message,SYNTHESIZER_HEADER_COMPLETION_CAUSE);
				}
				/* set request state */
				message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

				synth_channel->speak_request = NULL;
				if(synth_channel->audio_file) {
					fclose(synth_channel->audio_file);
					synth_channel->audio_file = NULL;
				}
				/* send asynch event */
				mrcp_engine_channel_message_send(synth_channel->channel,message);
			}
		}
	}
	return TRUE;
}

static apt_bool_t trebe_synth_msg_signal(trebe_synth_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t status = FALSE;
	trebe_synth_channel_t *trebe_channel = channel->method_obj;
	trebe_synth_engine_t *trebe_engine = trebe_channel->trebe_engine;
	apt_task_t *task = apt_consumer_task_base_get(trebe_engine->task);
	apt_task_msg_t *msg = apt_task_msg_get(task);
	if(msg) {
		trebe_synth_msg_t *trebe_msg;
		msg->type = TASK_MSG_USER;
		trebe_msg = (trebe_synth_msg_t*) msg->data;

		trebe_msg->type = type;
		trebe_msg->channel = channel;
		trebe_msg->request = request;
		status = apt_task_msg_signal(task,msg);
	}
	return status;
}

static apt_bool_t trebe_synth_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
	trebe_synth_msg_t *trebe_msg = (trebe_synth_msg_t*)msg->data;
	switch(trebe_msg->type) {
		case TREBE_SYNTH_MSG_OPEN_CHANNEL:
			/* open channel and send asynch response */
			mrcp_engine_channel_open_respond(trebe_msg->channel,TRUE);
			break;
		case TREBE_SYNTH_MSG_CLOSE_CHANNEL:
			/* close channel, make sure there is no activity and send asynch response */
			mrcp_engine_channel_close_respond(trebe_msg->channel);
			break;
		case TREBE_SYNTH_MSG_REQUEST_PROCESS:
			trebe_synth_channel_request_dispatch(trebe_msg->channel,trebe_msg->request);
			break;
		default:
			break;
	}
	return TRUE;
}
