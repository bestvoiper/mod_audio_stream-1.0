#ifndef MOD_AUDIO_STREAM_H
#define MOD_AUDIO_STREAM_H

#include <switch.h>
#include <speex/speex_resampler.h>
#include "buffer/ringbuffer.h"

#define MY_BUG_NAME "audio_stream"
#define MAX_SESSION_ID (256)
#define MAX_WS_URI (4096)
#define MAX_METADATA_LEN (8192)

#define EVENT_CONNECT           "mod_audio_stream::connect"
#define EVENT_DISCONNECT        "mod_audio_stream::disconnect"
#define EVENT_ERROR             "mod_audio_stream::error"
#define EVENT_JSON              "mod_audio_stream::json"
#define EVENT_PLAY              "mod_audio_stream::play"

typedef void (*responseHandler_t)(switch_core_session_t* session, const char* eventName, const char* json);

// Audio mix mode enumeration
typedef enum {
    MIX_MODE_MONO = 0,           // Single channel (read only)
    MIX_MODE_MIXED = 1,          // Mixed read+write in single mono channel
    MIX_MODE_STEREO = 2,         // Stereo (2 channels: left=read, right=write)
    MIX_MODE_ENHANCED_MIXED = 3  // Enhanced mixed with better quality mixing algorithm
} audio_mix_mode_t;

struct private_data {
    switch_mutex_t *mutex;
    char sessionId[MAX_SESSION_ID];
    SpeexResamplerState *resampler;
    responseHandler_t responseHandler;
    void *pAudioStreamer;
    char ws_uri[MAX_WS_URI];
    int sampling;
    int channels;
    int audio_paused:1;
    int close_requested:1;
    char initialMetadata[8192];
    RingBuffer *buffer;
    switch_buffer_t *sbuffer;
    uint8_t *data;
    int rtp_packets;
    uint32_t dbg_frames;
    uint32_t dbg_cng_skip;
    uint64_t dbg_bytes;
    uint32_t dbg_last_in;
    uint32_t dbg_last_out;

    // Audio mix mode for streaming
    audio_mix_mode_t mix_mode;

    // Buffers for enhanced mixing
    switch_buffer_t *read_buffer;
    switch_buffer_t *write_buffer;

    // Audio injection support
    switch_buffer_t *inject_buffer;
    switch_mutex_t *inject_mutex;
    SpeexResamplerState *inject_resampler;
    int inject_audio_enabled:1;
    int inject_sample_rate;
    int inject_channels;
};

typedef struct private_data private_t;

enum notifyEvent_t {
    CONNECT_SUCCESS,
    CONNECT_ERROR,
    CONNECTION_DROPPED,
    MESSAGE
};

#endif //MOD_AUDIO_STREAM_H
