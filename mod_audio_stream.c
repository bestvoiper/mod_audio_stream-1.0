/*
 * mod_audio_stream FreeSWITCH module to stream audio to websocket and receive response
 */
#include "mod_audio_stream.h"
#include "audio_streamer_glue.h"

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_stream_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_audio_stream_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_audio_stream_load);

SWITCH_MODULE_DEFINITION(mod_audio_stream, mod_audio_stream_load, mod_audio_stream_shutdown, NULL /*mod_audio_stream_runtime*/);

static void responseHandler(switch_core_session_t* session, const char* eventName, const char* json) {
    switch_event_t *event;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, eventName);
    switch_channel_event_set_data(channel, event);
    if (json) switch_event_add_body(event, "%s", json);
    switch_event_fire(&event);
}

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    switch_core_session_t *session = switch_core_media_bug_get_session(bug);
    private_t *tech_pvt = (private_t *)user_data;

    switch (type) {
        case SWITCH_ABC_TYPE_INIT:
            break;

        case SWITCH_ABC_TYPE_CLOSE:
            {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_CLOSE.\n");
                // Check if this is a normal channel closure or a requested closure
                int channelIsClosing = tech_pvt->close_requested ? 0 : 1;
                stream_session_cleanup(session, NULL, channelIsClosing);
            }
            break;

        case SWITCH_ABC_TYPE_READ:
            if (tech_pvt->close_requested) {
                return SWITCH_FALSE;
            }
            return stream_frame(bug);
            break;

        case SWITCH_ABC_TYPE_READ_REPLACE:
            /* mono: actual decoded frame, no 30ms media-bug padding. */
            if (tech_pvt->close_requested) {
                return SWITCH_FALSE;
            }
            return stream_frame(bug);
            break;

        case SWITCH_ABC_TYPE_WRITE:
            /* write-only capture (far-end / early media on A-leg). Mixed/stereo
               already consume both buffers from the READ callback. */
            if (tech_pvt->close_requested) {
                return SWITCH_FALSE;
            }
            if (!switch_core_media_bug_test_flag(bug, SMBF_READ_STREAM)) {
                return stream_frame(bug);
            }
            break;

        default:
            break;
    }

    return SWITCH_TRUE;
}

static switch_status_t start_capture(switch_core_session_t *session,
                                     switch_media_bug_flag_t flags,
                                     char* wsUri,
                                     int sampling,
                                     char* metadata)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug;
    switch_status_t status;
    switch_codec_t* read_codec;

    void *pUserData = NULL;
    int channels = (flags & SMBF_STEREO) ? 2 : 1;

    if (switch_channel_get_private(channel, MY_BUG_NAME)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_stream: bug already attached!\n");
        return SWITCH_STATUS_FALSE;
    }

    /* Early media: execute_on_media can fire before the read codec is ready.
       Prefer read_impl (often set first) and wait at most ~100ms so we do not
       miss the start of a voicemail greeting. */
    if (!switch_channel_media_ready(channel)) {
        if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "mod_audio_stream: pre-answer not ready yet, waiting for media (early media)...\n");
        }
    }

    {
        int tries;
        switch_codec_implementation_t read_impl = { 0 };
        for (tries = 0; tries < 20; tries++) {
            if (switch_channel_media_ready(channel)) {
                read_codec = switch_core_session_get_read_codec(session);
                if (read_codec && read_codec->implementation &&
                    read_codec->implementation->actual_samples_per_second &&
                    read_codec->implementation->decoded_bytes_per_packet) {
                    break;
                }
                if (switch_core_session_get_read_impl(session, &read_impl) == SWITCH_STATUS_SUCCESS &&
                    read_impl.actual_samples_per_second && read_impl.decoded_bytes_per_packet) {
                    break;
                }
            }
            switch_yield(5000);
        }
    }

    {
        switch_codec_implementation_t read_impl = { 0 };
        uint32_t native_rate = 0;

        read_codec = switch_core_session_get_read_codec(session);
        if (read_codec && read_codec->implementation &&
            read_codec->implementation->actual_samples_per_second) {
            native_rate = read_codec->implementation->actual_samples_per_second;
        } else if (switch_core_session_get_read_impl(session, &read_impl) == SWITCH_STATUS_SUCCESS) {
            native_rate = read_impl.actual_samples_per_second;
        }

        if (!native_rate) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "mod_audio_stream: read codec not ready. Start after CHANNEL_PROGRESS_MEDIA / execute_on_media.\n");
            return SWITCH_STATUS_FALSE;
        }

        if (!switch_channel_media_ready(channel) &&
            switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_stream: channel must have reached pre-answer status before calling start!\n");
            return SWITCH_STATUS_FALSE;
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                          "mod_audio_stream: starting capture native=%uHz target=%dHz media_ready=%d answered=%d early=%d flags=0x%x\n",
                          native_rate, sampling,
                          switch_channel_media_ready(channel) ? 1 : 0,
                          switch_channel_test_flag(channel, CF_ANSWERED) ? 1 : 0,
                          switch_channel_test_flag(channel, CF_EARLY_MEDIA) ? 1 : 0,
                          flags);

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "calling stream_session_init.\n");
        if (SWITCH_STATUS_FALSE == stream_session_init(session, responseHandler, native_rate,
                                                     wsUri, sampling, channels, metadata, &pUserData)) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing mod_audio_stream session.\n");
            return SWITCH_STATUS_FALSE;
        }
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "adding bug.\n");
        if ((status = switch_core_media_bug_add(session, MY_BUG_NAME, NULL, capture_callback, pUserData, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
            stream_session_cleanup(session, NULL, 0);
            return status;
        }
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "setting bug private data.\n");
        switch_channel_set_private(channel, MY_BUG_NAME, bug);
        /* Bug is attached: now open the websocket so early media can queue from packet 1. */
        stream_session_start(pUserData);
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "exiting start_capture.\n");
    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_stop(switch_core_session_t *session, char* text)
{
    switch_status_t status = SWITCH_STATUS_SUCCESS;

    if (text) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_stream: stop w/ final text %s\n", text);
    }
    else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_stream: stop\n");
    }
    status = stream_session_cleanup(session, text, 0);

    return status;
}

static switch_status_t do_pauseresume(switch_core_session_t *session, int pause)
{
    switch_status_t status = SWITCH_STATUS_SUCCESS;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_stream: %s\n", pause ? "pause" : "resume");
    status = stream_session_pauseresume(session, pause);

    return status;
}

static switch_status_t send_text(switch_core_session_t *session, char* text) {
    switch_status_t status = SWITCH_STATUS_FALSE;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = switch_channel_get_private(channel, MY_BUG_NAME);

    if (bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_stream: sending text: %s.\n", text);
        status = stream_session_send_text(session, text);
    }
    else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_stream: no bug, failed sending text: %s.\n", text);
    }
    return status;
}

#define STREAM_API_SYNTAX "<uuid> [start | stop | send_text | pause | resume | graceful-shutdown ] [wss-url | path] [mono | mixed | stereo | write] [8000 | 16000] [metadata]"
SWITCH_STANDARD_API(stream_function)
{
    char *mycmd = NULL, *argv[6] = { 0 };
    int argc = 0;

    switch_status_t status = SWITCH_STATUS_FALSE;

    if (!zstr(cmd) && (mycmd = strdup(cmd))) {
        argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
    }
    assert(cmd);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "mod_audio_stream cmd: %s\n", cmd ? cmd : "");

    if (zstr(cmd) || argc < 2 || (0 == strcmp(argv[1], "start") && argc < 4)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s %s %s.\n", cmd, argv[0], argv[1]);
        stream->write_function(stream, "-USAGE: %s\n", STREAM_API_SYNTAX);
        goto done;
    } else {
        switch_core_session_t *lsession = NULL;
        if ((lsession = switch_core_session_locate(argv[0]))) {
            if (!strcasecmp(argv[1], "stop")) {
                if(argc > 2 && (is_valid_utf8(argv[2]) != SWITCH_STATUS_SUCCESS)) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "%s contains invalid utf8 characters\n", argv[2]);
                    switch_core_session_rwunlock(lsession);
                    goto done;
                }
                status = do_stop(lsession, argc > 2 ? argv[2] : NULL);
            } else if (!strcasecmp(argv[1], "pause")) {
                status = do_pauseresume(lsession, 1);
            } else if (!strcasecmp(argv[1], "resume")) {
                status = do_pauseresume(lsession, 0);
            } else if (!strcasecmp(argv[1], "send_text")) {
                if (argc < 3) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "send_text requires an argument specifying text to send\n");
                    switch_core_session_rwunlock(lsession);
                    goto done;
                }
                if(is_valid_utf8(argv[2]) != SWITCH_STATUS_SUCCESS) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "%s contains invalid utf8 characters\n", argv[2]);
                    switch_core_session_rwunlock(lsession);
                    goto done;
                }
                status = send_text(lsession, argv[2]);
            } else if (!strcasecmp(argv[1], "start")) {
                //switch_channel_t *channel = switch_core_session_get_channel(lsession);
                char wsUri[MAX_WS_URI];
                int sampling = 8000;
                switch_media_bug_flag_t flags = SMBF_READ_PING | SMBF_NO_PAUSE;
                char *metadata = argc > 5 ? argv[5] : NULL;
                if(metadata && (is_valid_utf8(argv[2]) != SWITCH_STATUS_SUCCESS)) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "%s contains invalid utf8 characters\n", argv[2]);
                    switch_core_session_rwunlock(lsession);
                    goto done;
                }
                if (0 == strcmp(argv[3], "mixed")) {
                    flags |= SMBF_READ_STREAM | SMBF_WRITE_STREAM;
                } else if (0 == strcmp(argv[3], "stereo")) {
                    flags |= SMBF_READ_STREAM | SMBF_WRITE_STREAM;
                    flags |= SMBF_STEREO;
                } else if (0 == strcmp(argv[3], "write")) {
                    /* Far-end only: A-leg write path (early media / voicemail greeting). */
                    flags |= SMBF_WRITE_STREAM;
                } else if (0 == strcmp(argv[3], "mono")) {
                    /* READ_STREAM tap (does not intercept the codec). READ_REPLACE
                       muted both the FS recording and the stream on this path. */
                    flags |= SMBF_READ_STREAM;
                } else {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "invalid mix type: %s, must be mono, mixed, stereo, or write\n", argv[3]);
                    switch_core_session_rwunlock(lsession);
                    goto done;
                }
                if (argc > 4) {
                    if (0 == strcmp(argv[4], "16k")) {
                        sampling = 16000;
                    } else if (0 == strcmp(argv[4], "8k")) {
                        sampling = 8000;
                    } else {
                        sampling = atoi(argv[4]);
                    }
                }
                if (!validate_ws_uri(argv[2], &wsUri[0])) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "invalid websocket uri: %s\n", argv[2]);
                } else if (sampling % 8000 != 0) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "invalid sample rate: %s\n", argv[4]);
                } else {
                    status = start_capture(lsession, flags, wsUri, sampling, metadata);
                }
            } else {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                  "unsupported mod_audio_stream cmd: %s\n", argv[1]);
            }
            switch_core_session_rwunlock(lsession);
        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error locating session %s\n",
                              argv[0]);
        }
    }

    if (status == SWITCH_STATUS_SUCCESS) {
        stream->write_function(stream, "+OK Success\n");
    } else {
        stream->write_function(stream, "-ERR Operation Failed\n");
    }

done:
    switch_safe_free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_audio_stream_load)
{
    switch_api_interface_t *api_interface;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_stream API loading..\n");

    /* connect my internal structure to the blank pointer passed to me */
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    /* Initialize audio stream module */
    if (init_audio_stream_module() != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to initialize audio stream module\n");
        return SWITCH_STATUS_TERM;
    }

    /* create/register custom event message types */
    if (switch_event_reserve_subclass(EVENT_JSON) != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_CONNECT) != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_ERROR) != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_DISCONNECT) != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_PLAY) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register an event subclass for mod_audio_stream API.\n");
        cleanup_audio_stream_module();
        return SWITCH_STATUS_TERM;
    }
    SWITCH_ADD_API(api_interface, "uuid_audio_stream", "audio_stream API", stream_function, STREAM_API_SYNTAX);
    switch_console_set_complete("add uuid_audio_stream ::console::list_uuid start wss-url metadata");
    switch_console_set_complete("add uuid_audio_stream ::console::list_uuid start wss-url");
    switch_console_set_complete("add uuid_audio_stream ::console::list_uuid stop");
    switch_console_set_complete("add uuid_audio_stream ::console::list_uuid pause");
    switch_console_set_complete("add uuid_audio_stream ::console::list_uuid resume");
    switch_console_set_complete("add uuid_audio_stream ::console::list_uuid send_text");

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_stream API successfully loaded\n");

    /* indicate that the module should continue to be loaded */
    return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_audio_stream_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_stream_shutdown)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_stream shutting down..\n");
    
    /* Clean up audio stream module */
    cleanup_audio_stream_module();
    
    switch_event_free_subclass(EVENT_JSON);
    switch_event_free_subclass(EVENT_CONNECT);
    switch_event_free_subclass(EVENT_DISCONNECT);
    switch_event_free_subclass(EVENT_ERROR);
    switch_event_free_subclass(EVENT_PLAY);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_stream shutdown complete\n");
    return SWITCH_STATUS_SUCCESS;
}
