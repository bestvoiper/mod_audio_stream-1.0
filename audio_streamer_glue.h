#ifndef AUDIO_STREAMER_GLUE_H
#define AUDIO_STREAMER_GLUE_H
#include "mod_audio_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

// Function declarations
int validate_ws_uri(const char* url, char *wsUri);
switch_status_t is_valid_utf8(const char *str);
switch_status_t stream_session_send_text(switch_core_session_t *session, char* text);
switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause);
switch_status_t stream_session_init(switch_core_session_t *session, responseHandler_t responseHandler,
    uint32_t samples_per_second, char *wsUri, int sampling, int channels, char* metadata, void **ppUserData);
void stream_session_start(void *pUserData);
switch_bool_t stream_frame(switch_media_bug_t *bug);
switch_status_t stream_session_cleanup(switch_core_session_t *session, char* text, int channelIsClosing);

// Module lifecycle functions
switch_status_t init_audio_stream_module(void);
void cleanup_audio_stream_module(void);

// Debug and management functions
void set_debug_level(int level);
int get_debug_level(void);
void debug_log_channels_status(void);
void debug_log_session_info(switch_core_session_t *session, const char* action);
switch_bool_t check_channel_limit(void);
void increment_active_channels(void);
void decrement_active_channels(void);
uint32_t get_active_channel_count(void);
void emergency_cleanup_sessions(void);
void sync_channel_counters(void);

#ifdef __cplusplus
}
#endif

#endif //AUDIO_STREAMER_GLUE_H
