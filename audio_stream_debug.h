#ifndef AUDIO_STREAM_DEBUG_H
#define AUDIO_STREAM_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

// Debug level constants
#define DEBUG_LEVEL_NONE     0
#define DEBUG_LEVEL_ERROR    1
#define DEBUG_LEVEL_WARNING  2
#define DEBUG_LEVEL_INFO     3
#define DEBUG_LEVEL_DEBUG    4
#define DEBUG_LEVEL_VERBOSE  5

// Channel management
#define MAX_CONCURRENT_CHANNELS 50

// Debug functions
void set_debug_level(int level);
int get_debug_level(void);
void debug_log_channels_status(void);
void debug_log_session_info(switch_core_session_t *session, const char* action);

// Channel management functions
bool check_channel_limit(void);
void increment_active_channels(void);
void decrement_active_channels(void);
uint32_t get_active_channel_count(void);

// Emergency functions
void emergency_cleanup_sessions(void);
void sync_channel_counters(void);

// Module lifecycle
switch_status_t init_audio_stream_module(void);
void cleanup_audio_stream_module(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_STREAM_DEBUG_H
