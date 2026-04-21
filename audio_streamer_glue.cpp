#include <string>
#include <cstring>
#include "mod_audio_stream.h"

// =============================================================================
// WEBSOCKET LIBRARY CONFIGURATION: LIBWSC ONLY
// =============================================================================
// This module is configured to use EXCLUSIVELY the libwsc WebSocket library.
// IXWebSocket is NOT used and should not be included anywhere in this code.
// =============================================================================
#define USING_LIBWSC_ONLY 1
#include "libwsc/WebSocketClient.h"

#include <switch_json.h>
#include <fstream>
#include <switch_buffer.h>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <vector>
#include "base64.h"

#define FRAME_SIZE_8000  320 /* 1000x0.02 (20ms)= 160 x(16bit= 2 bytes) 320 frame size*/
#define INJECT_BUFFER_MULTIPLIER 100 /* keep about 2 seconds at 16k/20ms chunks when rtp_packets=1 */

// Debug level constants
#define DEBUG_LEVEL_NONE     0
#define DEBUG_LEVEL_ERROR    1
#define DEBUG_LEVEL_WARNING  2
#define DEBUG_LEVEL_INFO     3
#define DEBUG_LEVEL_DEBUG    4
#define DEBUG_LEVEL_VERBOSE  5

// Channel management constants
#define MAX_CONCURRENT_CHANNELS 2000 // Increased limit to handle more calls

// Thread-safe global channel management
static std::atomic<uint32_t> g_active_channels{0};
static std::mutex g_channel_mutex;
static std::unordered_set<std::string> g_active_sessions;
static std::atomic<int> g_debug_level{DEBUG_LEVEL_WARNING}; // Default to WARNING level

// Debug macros
#define DEBUG_LOG(level, session, format, ...) \
    do { \
        if (g_debug_level.load() >= level) { \
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), \
                level == DEBUG_LEVEL_ERROR ? SWITCH_LOG_ERROR : \
                level == DEBUG_LEVEL_WARNING ? SWITCH_LOG_WARNING : \
                level == DEBUG_LEVEL_INFO ? SWITCH_LOG_INFO : \
                SWITCH_LOG_DEBUG, \
                "[DEBUG][LEVEL%d] " format, level, ##__VA_ARGS__); \
        } \
    } while(0)

#define DEBUG_LOG_GLOBAL(level, format, ...) \
    do { \
        if (g_debug_level.load() >= level) { \
            switch_log_printf(SWITCH_CHANNEL_LOG, \
                level == DEBUG_LEVEL_ERROR ? SWITCH_LOG_ERROR : \
                level == DEBUG_LEVEL_WARNING ? SWITCH_LOG_WARNING : \
                level == DEBUG_LEVEL_INFO ? SWITCH_LOG_INFO : \
                SWITCH_LOG_DEBUG, \
                "[DEBUG][LEVEL%d] " format, level, ##__VA_ARGS__); \
        } \
    } while(0)

namespace {
    extern switch_bool_t filter_json_string(switch_core_session_t *session, const char* message);
    // Forward declarations for cleanup functions
    void finish(private_t* tech_pvt);
    void destroy_tech_pvt(private_t* tech_pvt);
}

// Forward declarations for functions used in cleanup
extern "C" {
    void decrement_active_channels();
    switch_status_t inject_audio_data(switch_core_session_t *session, const uint8_t* audio_data,
                                      size_t data_len, int sample_rate, int channels);
}

class AudioStreamer {
public:

    AudioStreamer(const char* uuid, const char* wsUri, responseHandler_t callback, int deflate, int heart_beat,
                    bool globalTrace, bool suppressLog, const char* extra_headers, bool no_reconnect): 
                    m_sessionId(uuid), m_notify(callback), m_global_trace(globalTrace), 
                    m_suppress_log(suppressLog), m_extra_headers(extra_headers), m_playFile(0), m_connected(false){

        if (!uuid || !wsUri || !callback) {
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_ERROR, "AudioStreamer constructor called with invalid parameters");
            return;
        }

        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Creating AudioStreamer for session %s, URI: %s", uuid, wsUri);
        
        try {
            // Register this session
            {
                std::lock_guard<std::mutex> lock(g_channel_mutex);
                auto result = g_active_sessions.insert(uuid);
                if (!result.second) {
                    DEBUG_LOG_GLOBAL(DEBUG_LEVEL_WARNING, "Session %s already exists in active sessions", uuid);
                } else {
                    DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Registered session %s. Total active sessions: %zu", 
                                   uuid, g_active_sessions.size());
                }
            }

            // Setup WebSocket headers safely
            WebSocketHeaders headers;
            if (m_extra_headers) {
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_VERBOSE, "Processing extra headers for session %s", uuid);
                cJSON *headers_json = cJSON_Parse(m_extra_headers);
                if (headers_json) {
                    cJSON *iterator = headers_json->child;
                    while (iterator) {
                        if (iterator->type == cJSON_String && iterator->valuestring != nullptr && iterator->string != nullptr) {
                            headers.set(iterator->string, iterator->valuestring);
                            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_VERBOSE, "Added header: %s = %s", iterator->string, iterator->valuestring);
                        }
                        iterator = iterator->next;
                    }
                    cJSON_Delete(headers_json);
                } else {
                    DEBUG_LOG_GLOBAL(DEBUG_LEVEL_WARNING, "Failed to parse extra headers JSON for session %s", uuid);
                }
            }

            // Configure WebSocket safely
            webSocket.setUrl(wsUri);
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "WebSocket URL set for session %s", uuid);

            if(heart_beat > 0) {
                webSocket.setPingInterval(heart_beat);
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Set ping interval to %d seconds for session %s", heart_beat, uuid);
            }
            
            if(deflate) {
                webSocket.enableCompression(false);
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Disabled compression for session %s", uuid);
            }
            
            if(!headers.empty()) {
                webSocket.setHeaders(headers);
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Set extra headers for session %s", uuid);
            }

            // Setup callbacks with enhanced error handling
            webSocket.setMessageCallback([this](const std::string& message){
                try {
                    if (m_connected) {
                        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_VERBOSE, "Received message for session %s, size: %zu", 
                                       m_sessionId.c_str(), message.size());
                        eventCallback(MESSAGE, message.c_str());
                    }
                } catch (const std::exception& e) {
                    DEBUG_LOG_GLOBAL(DEBUG_LEVEL_ERROR, "Exception in message callback for session %s: %s", 
                                   m_sessionId.c_str(), e.what());
                }
            });

            webSocket.setOpenCallback([this](){
                m_connected = true;
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_INFO, "WebSocket connection opened for session %s", m_sessionId.c_str());
                cJSON *root = cJSON_CreateObject();
                if (root) {
                    cJSON_AddStringToObject(root, "status", "connected");
                    char *json_str = cJSON_PrintUnformatted(root);
                    if (json_str) {
                        eventCallback(CONNECT_SUCCESS, json_str);
                        switch_safe_free(json_str);
                    }
                    cJSON_Delete(root);
                }
            });

            webSocket.setErrorCallback([this](int error_code, const std::string& error_message){
                m_connected = false;
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_ERROR, "WebSocket error for session %s: %s (code: %d)", 
                               m_sessionId.c_str(), error_message.c_str(), error_code);
                
                cJSON *root = cJSON_CreateObject();
                if (root) {
                    cJSON_AddStringToObject(root, "status", "error");
                    cJSON *message = cJSON_CreateObject();
                    if (message) {
                        cJSON_AddNumberToObject(message, "code", error_code);
                        cJSON_AddStringToObject(message, "error", error_message.c_str());
                        cJSON_AddItemToObject(root, "message", message);
                    }

                    char *json_str = cJSON_PrintUnformatted(root);
                    if (json_str) {
                        eventCallback(CONNECT_ERROR, json_str);
                        switch_safe_free(json_str);
                    }
                    cJSON_Delete(root);
                }
            });

            webSocket.setCloseCallback([this](int code, const std::string& reason){
                m_connected = false;
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_INFO, "WebSocket connection closed for session %s, code: %d, reason: %s", 
                               m_sessionId.c_str(), code, reason.c_str());
                
                cJSON *root = cJSON_CreateObject();
                if (root) {
                    cJSON_AddStringToObject(root, "status", "disconnected");
                    cJSON *message = cJSON_CreateObject();
                    if (message) {
                        cJSON_AddNumberToObject(message, "code", code);
                        cJSON_AddStringToObject(message, "reason", reason.c_str());
                        cJSON_AddItemToObject(root, "message", message);
                    }
                    
                    char *json_str = cJSON_PrintUnformatted(root);
                    if (json_str) {
                        eventCallback(CONNECTION_DROPPED, json_str);
                        switch_safe_free(json_str);
                    }
                    cJSON_Delete(root);
                }
            });

            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Starting WebSocket for session %s", uuid);
            
            // Set connection timeout to 10 seconds (default is 5)
            webSocket.setConnectionTimeout(10);
            
            webSocket.connect();
            
        } catch (const std::exception& e) {
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_ERROR, "Exception in AudioStreamer constructor for session %s: %s", 
                           uuid, e.what());
            m_connected = false;
            
            // Clean up on error
            {
                std::lock_guard<std::mutex> lock(g_channel_mutex);
                g_active_sessions.erase(uuid);
            }
        }
    }

    switch_media_bug_t *get_media_bug(switch_core_session_t *session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if(!channel) {
            return nullptr;
        }
        auto *bug = (switch_media_bug_t *) switch_channel_get_private(channel, MY_BUG_NAME);
        return bug;
    }

    inline void media_bug_close(switch_core_session_t *session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if (!channel) {
            return;
        }
        
        auto *bug = (switch_media_bug_t *) switch_channel_get_private(channel, MY_BUG_NAME);
        if(!bug) {
            return;
        }
        
        auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt) {
            switch_channel_set_private(channel, MY_BUG_NAME, nullptr);
            switch_core_media_bug_close(&bug, SWITCH_FALSE);
            return;
        }
        
        // Prevent multiple cleanup calls
        if (tech_pvt->close_requested) {
            return;
        }
        tech_pvt->close_requested = 1;
        
        // Clear private data FIRST to prevent "bug already attached" errors
        switch_channel_set_private(channel, MY_BUG_NAME, nullptr);
        
        // Do the actual cleanup
        if (tech_pvt->mutex) {
            switch_mutex_lock(tech_pvt->mutex);
        }
        
        auto* audioStreamer = (AudioStreamer *) tech_pvt->pAudioStreamer;
        if (audioStreamer) {
            audioStreamer->deleteFiles();
            finish(tech_pvt);
        }
        
        if (tech_pvt->mutex) {
            switch_mutex_unlock(tech_pvt->mutex);
        }
        
        // Close the bug - SWITCH_ABC_TYPE_CLOSE callback will find no bug and return early
        switch_core_media_bug_close(&bug, SWITCH_FALSE);
        
        // Final cleanup
        destroy_tech_pvt(tech_pvt);
        
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, 
            "media_bug_close: cleanup completed\n");
    }

    inline void send_initial_metadata(switch_core_session_t *session) {
        auto *bug = get_media_bug(session);
        if(bug) {
            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            if(tech_pvt && strlen(tech_pvt->initialMetadata) > 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                                          "sending initial metadata %s\n", tech_pvt->initialMetadata);
                writeText(tech_pvt->initialMetadata);
            }
        }
    }

    void eventCallback(notifyEvent_t event, const char* message) {
        switch_core_session_t* psession = switch_core_session_locate(m_sessionId.c_str());
        if(psession) {
            switch (event) {
                case CONNECT_SUCCESS:
                    send_initial_metadata(psession);
                    m_notify(psession, EVENT_CONNECT, message);
                    break;
                case CONNECTION_DROPPED:
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection closed\n");
                    m_notify(psession, EVENT_DISCONNECT, message);
                    break;
                case CONNECT_ERROR:
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection error\n");
                    m_notify(psession, EVENT_ERROR, message);

                    media_bug_close(psession);

                    break;
                case MESSAGE:
                    std::string msg(message);
                    if(processMessage(psession, msg) != SWITCH_TRUE) {
                        m_notify(psession, EVENT_JSON, msg.c_str());
                    }
                    if(!m_suppress_log)
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG, "response: %s\n", msg.c_str());
                    break;
            }
            switch_core_session_rwunlock(psession);
        }
    }

    switch_bool_t processMessage(switch_core_session_t* session, std::string& message) {
        cJSON* json = cJSON_Parse(message.c_str());
        switch_bool_t status = SWITCH_FALSE;
        if (!json) {
            return status;
        }
        const char* jsType = cJSON_GetObjectCstr(json, "type");
        if(jsType && strcmp(jsType, "streamAudio") == 0) {
            cJSON* jsonData = cJSON_GetObjectItem(json, "data");
            if(jsonData) {
                cJSON* jsonFile = nullptr;
                cJSON* jsonAudio = cJSON_DetachItemFromObject(jsonData, "audioData");
                const char* jsAudioDataType = cJSON_GetObjectCstr(jsonData, "audioDataType");
                std::string fileType;
                int sampleRate = 0;
                int channels = 1;
                if (!jsAudioDataType) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "(%s) processMessage - missing audioDataType\n", m_sessionId.c_str());
                } else if (0 == strcmp(jsAudioDataType, "raw")) {
                    cJSON* jsonSampleRate = cJSON_GetObjectItem(jsonData, "sampleRate");
                    sampleRate = jsonSampleRate && jsonSampleRate->valueint ? jsonSampleRate->valueint : 0;
                    cJSON* jsonChannels = cJSON_GetObjectItem(jsonData, "channels");
                    channels = jsonChannels && jsonChannels->valueint ? jsonChannels->valueint : 1;
                } else if (0 == strcmp(jsAudioDataType, "wav")) {
                    fileType = ".wav";
                } else if (0 == strcmp(jsAudioDataType, "mp3")) {
                    fileType = ".mp3";
                } else if (0 == strcmp(jsAudioDataType, "ogg")) {
                    fileType = ".ogg";
                } else {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%s) processMessage - unsupported audio type: %s\n",
                                      m_sessionId.c_str(), jsAudioDataType);
                }

                if(jsonAudio && jsonAudio->valuestring != nullptr) {
                    std::string rawAudio;
                    try {
                        rawAudio = base64_decode(jsonAudio->valuestring);
                    } catch (const std::exception& e) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%s) processMessage - base64 decode error: %s\n",
                                          m_sessionId.c_str(), e.what());
                        cJSON_Delete(jsonAudio); cJSON_Delete(json);
                        return status;
                    }

                    if (jsAudioDataType && 0 == strcmp(jsAudioDataType, "raw")) {
                        if (sampleRate <= 0) {
                            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                              "(%s) processMessage - invalid sampleRate for raw audio\n",
                                              m_sessionId.c_str());
                        } else {
                            if (inject_audio_data(session,
                                                  reinterpret_cast<const uint8_t *>(rawAudio.data()),
                                                  rawAudio.size(),
                                                  sampleRate,
                                                  channels) == SWITCH_STATUS_SUCCESS) {
                                status = SWITCH_TRUE;
                            } else {
                                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                                  "(%s) processMessage - inject_audio_data failed\n",
                                                  m_sessionId.c_str());
                            }
                        }
                    } else {
                        if (fileType.empty()) {
                            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                              "(%s) processMessage - unsupported or invalid audioDataType\n",
                                              m_sessionId.c_str());
                            if (jsonAudio)
                                cJSON_Delete(jsonAudio);
                            cJSON_Delete(json);
                            return status;
                        }

                        char filePath[256];
                        switch_snprintf(filePath, 256, "%s%s%s_%d.tmp%s", SWITCH_GLOBAL_dirs.temp_dir,
                                        SWITCH_PATH_SEPARATOR, m_sessionId.c_str(), m_playFile++, fileType.c_str());
                        std::ofstream fstream(filePath, std::ofstream::binary);
                        if (fstream.is_open()) {
                            fstream << rawAudio;
                            fstream.close();
                            m_Files.insert(filePath);
                            jsonFile = cJSON_CreateString(filePath);
                            cJSON_AddItemToObject(jsonData, "file", jsonFile);
                        } else {
                            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                              "(%s) processMessage - failed to create file: %s\n",
                                              m_sessionId.c_str(), filePath);
                        }
                    }
                }

                if(jsonFile) {
                    char *jsonString = cJSON_PrintUnformatted(jsonData);
                    m_notify(session, EVENT_PLAY, jsonString);
                    message.assign(jsonString);
                    free(jsonString);
                    status = SWITCH_TRUE;
                }
                if (jsonAudio)
                    cJSON_Delete(jsonAudio);

            } else {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%s) processMessage - no data in streamAudio\n", m_sessionId.c_str());
            }
        }
        cJSON_Delete(json);
        return status;
    }

    ~AudioStreamer() {
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Destroying AudioStreamer for session %s", m_sessionId.c_str());
        
        // Unregister this session
        {
            std::lock_guard<std::mutex> lock(g_channel_mutex);
            g_active_sessions.erase(m_sessionId);
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Unregistered session %s. Remaining active sessions: %zu", 
                           m_sessionId.c_str(), g_active_sessions.size());
        }
        m_connected = false;
        deleteFiles();
    }

    void disconnect() {
        m_connected = false;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "disconnecting session %s...\n", m_sessionId.c_str());
        webSocket.disconnect();
    }

    bool isConnected() {
        return m_connected && webSocket.isConnected();
    }

    void writeBinary(uint8_t* buffer, size_t len) {
        if(!this->isConnected()) {
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_VERBOSE, "Attempted to write binary data to disconnected session %s", 
                           m_sessionId.c_str());
            return;
        }
        try {
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_VERBOSE, "Writing %zu bytes to session %s", len, m_sessionId.c_str());
            webSocket.sendBinary(buffer, len);
        } catch (const std::exception& e) {
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_ERROR, "Error sending binary data to session %s: %s", 
                           m_sessionId.c_str(), e.what());
            m_connected = false;
        }
    }

    void writeText(const char* text) {
        if(!this->isConnected() || !text) return;
        try {
            webSocket.sendMessage(text);
        } catch (const std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error sending text: %s\n", e.what());
            m_connected = false;
        }
    }

    void deleteFiles() {
        if(m_playFile >0) {
            for (const auto &fileName: m_Files) {
                remove(fileName.c_str());
            }
        }
    }

private:
    std::string m_sessionId;
    responseHandler_t m_notify;
    WebSocketClient webSocket;
    bool m_suppress_log;
    bool m_global_trace;
    const char* m_extra_headers;
    int m_playFile;
    std::unordered_set<std::string> m_Files;
    std::atomic<bool> m_connected;
};


namespace {
    bool sentAlready = false;
    std::mutex prevMsgMutex;
    std::string prevMsg;

    switch_bool_t filter_json_string(switch_core_session_t *session, const char* message) {
        switch_bool_t send = SWITCH_FALSE;
        cJSON* json = cJSON_Parse(message);
        if (!json) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "parse - failed parsing incoming msg as JSON: %s\n", message);
            return send;
        }

        const cJSON *partial = cJSON_GetObjectItem(json, "partial");

        if(cJSON_IsString(partial)) {
            std::string currentMsg = partial->valuestring;
            prevMsgMutex.lock();
            if(currentMsg == prevMsg) {
                if(!sentAlready) {send = SWITCH_TRUE; sentAlready = true;}
            } else {
                prevMsg = currentMsg; send = SWITCH_TRUE;
            }
            prevMsgMutex.unlock();
        } else {
            send = SWITCH_TRUE;
        }
        cJSON_Delete(json);
        return send;
    }

    switch_status_t stream_data_init(private_t *tech_pvt, switch_core_session_t *session, char *wsUri,
                                     uint32_t sampling, int desiredSampling, int channels, char *metadata, responseHandler_t responseHandler,
                                     int deflate, int heart_beat, bool globalTrace, bool suppressLog, int rtp_packets, const char* extra_headers,
                                     bool no_reconnect, audio_mix_mode_t mix_mode)
    {
        int err; //speex

        switch_memory_pool_t *pool = switch_core_session_get_pool(session);

        memset(tech_pvt, 0, sizeof(private_t));

        strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID);
        strncpy(tech_pvt->ws_uri, wsUri, MAX_WS_URI);
        tech_pvt->sampling = desiredSampling;
        tech_pvt->responseHandler = responseHandler;
        tech_pvt->rtp_packets = rtp_packets;
        tech_pvt->channels = channels;
        tech_pvt->audio_paused = 0;
        tech_pvt->mix_mode = mix_mode;

        if (metadata) strncpy(tech_pvt->initialMetadata, metadata, MAX_METADATA_LEN);

        //size_t buflen = (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * 1000 / RTP_PERIOD * BUFFERED_SEC);
        const size_t buflen = (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * rtp_packets);

        auto* as = new AudioStreamer(tech_pvt->sessionId, wsUri, responseHandler, deflate, heart_beat, globalTrace, suppressLog, extra_headers, no_reconnect);

        tech_pvt->pAudioStreamer = static_cast<void *>(as);

        switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, pool);

        if (desiredSampling != sampling) {
            if (switch_buffer_create(pool, &tech_pvt->sbuffer, buflen) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                    "%s: Error creating switch buffer.\n", tech_pvt->sessionId);
                return SWITCH_STATUS_FALSE;
            }
        } else {
            size_t adjSize = 1; //adjust the buffer size to the closest pow2 size
            while(adjSize < buflen) {
                adjSize *= 2;
            }
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s: initializing buffer(%zu) to adjusted %zu bytes\n",
                          tech_pvt->sessionId, buflen, adjSize);
            tech_pvt->data = (uint8_t *) switch_core_alloc(pool, adjSize);
            if (!tech_pvt->data) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                  "%s: Error allocating memory for data buffer.\n", tech_pvt->sessionId);
                return SWITCH_STATUS_FALSE;
            }
            memset(tech_pvt->data, 0, adjSize);
            tech_pvt->buffer = (RingBuffer *) switch_core_alloc(pool, sizeof(RingBuffer));
            if (!tech_pvt->buffer) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                  "%s: Error allocating memory for ring buffer.\n", tech_pvt->sessionId);
                return SWITCH_STATUS_FALSE;
            }
            memset(tech_pvt->buffer, 0, sizeof(RingBuffer));
            ringBufferInit(tech_pvt->buffer, tech_pvt->data, adjSize);
        }

        if (desiredSampling != sampling) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) resampling from %u to %u\n", tech_pvt->sessionId, sampling, desiredSampling);
            tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
            if (0 != err) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
                return SWITCH_STATUS_FALSE;
            }
        }
        else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) no resampling needed for this call\n", tech_pvt->sessionId);
        }

        // Initialize enhanced mixing buffers if needed
        if (mix_mode == MIX_MODE_ENHANCED_MIXED) {
            // Create separate buffers for read and write audio for enhanced mixing
            if (switch_buffer_create(pool, &tech_pvt->read_buffer, buflen * 2) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                    "%s: Error creating read buffer for enhanced mixing.\n", tech_pvt->sessionId);
                return SWITCH_STATUS_FALSE;
            }
            if (switch_buffer_create(pool, &tech_pvt->write_buffer, buflen * 2) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                    "%s: Error creating write buffer for enhanced mixing.\n", tech_pvt->sessionId);
                return SWITCH_STATUS_FALSE;
            }
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, 
                "(%s) Enhanced mixing mode initialized with separate read/write buffers\n", tech_pvt->sessionId);
        }

        // Initialize audio injection resources.
        if (switch_buffer_create(pool, &tech_pvt->inject_buffer, buflen * INJECT_BUFFER_MULTIPLIER) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "%s: Error creating injection buffer.\n", tech_pvt->sessionId);
            return SWITCH_STATUS_FALSE;
        }
        switch_mutex_init(&tech_pvt->inject_mutex, SWITCH_MUTEX_NESTED, pool);
        tech_pvt->inject_audio_enabled = 0;
        tech_pvt->inject_sample_rate = desiredSampling;
        tech_pvt->inject_channels = 1;

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_data_init with mix_mode=%d\n", tech_pvt->sessionId, mix_mode);

        return SWITCH_STATUS_SUCCESS;
    }

    void destroy_tech_pvt(private_t* tech_pvt) {
        if (!tech_pvt) {
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_WARNING, "destroy_tech_pvt called with NULL pointer");
            return;
        }
        
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "destroy_tech_pvt for session %s", tech_pvt->sessionId);
        
        // Lock mutex if it exists
        if (tech_pvt->mutex) {
            if (switch_mutex_trylock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS) {
                // If we can't get the lock, wait a bit and try again
                switch_yield(10000); // 10ms
                if (switch_mutex_trylock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS) {
                    DEBUG_LOG_GLOBAL(DEBUG_LEVEL_WARNING, "Could not acquire mutex for session %s cleanup", tech_pvt->sessionId);
                    // Continue anyway to prevent resource leaks
                }
            }
        }
        
        // Clean up resampler
        if (tech_pvt->resampler) {
            speex_resampler_destroy(tech_pvt->resampler);
            tech_pvt->resampler = nullptr;
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Destroyed resampler for session %s", tech_pvt->sessionId);
        }

        if (tech_pvt->inject_resampler) {
            speex_resampler_destroy(tech_pvt->inject_resampler);
            tech_pvt->inject_resampler = nullptr;
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Destroyed inject resampler for session %s", tech_pvt->sessionId);
        }
        
        // Clean up AudioStreamer
        if (tech_pvt->pAudioStreamer) {
            try {
                auto* as = (AudioStreamer *) tech_pvt->pAudioStreamer;
                delete as;
                tech_pvt->pAudioStreamer = nullptr;
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Destroyed AudioStreamer for session %s", tech_pvt->sessionId);
            } catch (const std::exception& e) {
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_ERROR, "Exception destroying AudioStreamer for session %s: %s", 
                               tech_pvt->sessionId, e.what());
            }
        }
        
        // Clean up buffer data
        if (tech_pvt->buffer) {
            // Ring buffer data is cleaned up automatically with the pool
            tech_pvt->buffer = nullptr;
        }
        
        if (tech_pvt->sbuffer) {
            // Switch buffer is cleaned up automatically with the pool
            tech_pvt->sbuffer = nullptr;
        }
        
        // Unlock and destroy mutex
        if (tech_pvt->mutex) {
            switch_mutex_unlock(tech_pvt->mutex);
            switch_mutex_destroy(tech_pvt->mutex);
            tech_pvt->mutex = nullptr;
        }
        
        // Decrement active channels counter
        decrement_active_channels();
        
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Completed cleanup for session %s. Active channels: %u", 
                        tech_pvt->sessionId, g_active_channels.load());
    }

    void finish(private_t* tech_pvt) {
        std::shared_ptr<AudioStreamer> aStreamer;
        aStreamer.reset((AudioStreamer *)tech_pvt->pAudioStreamer);
        tech_pvt->pAudioStreamer = nullptr;

        // Immediately remove session from g_active_sessions to prevent race condition
        // when a new session with the same UUID is started before this thread completes
        {
            std::lock_guard<std::mutex> lock(g_channel_mutex);
            g_active_sessions.erase(tech_pvt->sessionId);
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Removed session %s from active sessions in finish(). Remaining: %zu", 
                           tech_pvt->sessionId, g_active_sessions.size());
        }

        std::thread t([aStreamer]{
            aStreamer->disconnect();
        });
        t.detach();
    }

}

extern "C" {
    // Emergency cleanup function
    void emergency_cleanup_sessions() {
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_WARNING, "Performing emergency session cleanup");
        
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        size_t before_count = g_active_sessions.size();
        uint32_t before_channels = g_active_channels.load();
        
        // Clear all sessions and reset counter
        g_active_sessions.clear();
        g_active_channels.store(0);
        
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_WARNING, "Emergency cleanup completed. Cleared %zu sessions, reset channels from %u to 0", 
                        before_count, before_channels);
    }

    // Function to force sync counters
    void sync_channel_counters() {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        uint32_t old_count = g_active_channels.load();
        uint32_t new_count = static_cast<uint32_t>(g_active_sessions.size());
        
        g_active_channels.store(new_count);
        
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_INFO, "Synced channel counters: %u -> %u", old_count, new_count);
    }

    // Debug functions
    void set_debug_level(int level) {
        if (level >= DEBUG_LEVEL_NONE && level <= DEBUG_LEVEL_VERBOSE) {
            int old_level = g_debug_level.exchange(level);
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_INFO, "Debug level changed from %d to %d", old_level, level);
        }
    }

    int get_debug_level() {
        return g_debug_level.load();
    }

    void debug_log_channels_status() {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_INFO, "=== CHANNELS STATUS ===");
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_INFO, "Active channels count: %u / %d", 
                        g_active_channels.load(), MAX_CONCURRENT_CHANNELS);
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_INFO, "Active sessions count: %zu", g_active_sessions.size());
        
        if (g_debug_level.load() >= DEBUG_LEVEL_DEBUG) {
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Active session IDs:");
            for (const auto& session_id : g_active_sessions) {
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "  - %s", session_id.c_str());
            }
        }
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_INFO, "======================");
    }

    void debug_log_session_info(switch_core_session_t *session, const char* action) {
        if (!session || !action) return;
        
        const char* uuid = switch_core_session_get_uuid(session);
        switch_channel_t *channel = switch_core_session_get_channel(session);
        const char* caller_id = switch_channel_get_variable(channel, "caller_id_number");
        const char* destination = switch_channel_get_variable(channel, "destination_number");
        
        DEBUG_LOG(DEBUG_LEVEL_INFO, session, "=== SESSION %s ===", action);
        DEBUG_LOG(DEBUG_LEVEL_INFO, session, "UUID: %s", uuid ? uuid : "NULL");
        DEBUG_LOG(DEBUG_LEVEL_INFO, session, "Caller ID: %s", caller_id ? caller_id : "Unknown");
        DEBUG_LOG(DEBUG_LEVEL_INFO, session, "Destination: %s", destination ? destination : "Unknown");
        DEBUG_LOG(DEBUG_LEVEL_INFO, session, "Channel State: %s", 
                switch_channel_state_name(switch_channel_get_state(channel)));
        DEBUG_LOG(DEBUG_LEVEL_INFO, session, "===============================");
    }

    // Enhanced channel management functions with debugging
    switch_bool_t check_channel_limit() {
        uint32_t current = g_active_channels.load();
        switch_bool_t result = (current < MAX_CONCURRENT_CHANNELS) ? SWITCH_TRUE : SWITCH_FALSE;
        
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Channel limit check: %u/%d - %s", 
                        current, MAX_CONCURRENT_CHANNELS, result ? "ALLOWED" : "REJECTED");
        
        if (!result) {
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_ERROR, "CHANNEL LIMIT REACHED! Current: %u, Max: %d", 
                           current, MAX_CONCURRENT_CHANNELS);
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_ERROR, "Consider increasing MAX_CONCURRENT_CHANNELS or investigating resource leaks");
            
            // Clean up any stale sessions
            {
                std::lock_guard<std::mutex> lock(g_channel_mutex);
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_INFO, "Active sessions in set: %zu", g_active_sessions.size());
                
                // If there's a mismatch between counter and set, fix it
                if (g_active_sessions.size() != current) {
                    DEBUG_LOG_GLOBAL(DEBUG_LEVEL_WARNING, "Mismatch detected: counter=%u, set_size=%zu. Synchronizing...", 
                                   current, g_active_sessions.size());
                    g_active_channels.store(static_cast<uint32_t>(g_active_sessions.size()));
                    current = g_active_channels.load();
                    result = (current < MAX_CONCURRENT_CHANNELS) ? SWITCH_TRUE : SWITCH_FALSE;
                    DEBUG_LOG_GLOBAL(DEBUG_LEVEL_INFO, "After sync: %u/%d - %s", 
                                   current, MAX_CONCURRENT_CHANNELS, result ? "ALLOWED" : "REJECTED");
                }
            }
            
            if (!result) {
                debug_log_channels_status();
            }
        }
        
        return result;
    }

    void increment_active_channels() {
        uint32_t new_count = g_active_channels.fetch_add(1) + 1;
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Incremented active channels to: %u", new_count);
        
        if (new_count > MAX_CONCURRENT_CHANNELS) {
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_ERROR, "WARNING: Active channels (%u) exceeded limit (%d)!", 
                           new_count, MAX_CONCURRENT_CHANNELS);
        }
    }

    void decrement_active_channels() {
        uint32_t prev = g_active_channels.fetch_sub(1);
        if (prev > 0) {
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Decremented active channels to: %u", prev - 1);
        } else {
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_ERROR, "ERROR: Attempted to decrement channels below zero!");
        }
    }

    uint32_t get_active_channel_count() {
        return g_active_channels.load();
    }

    switch_status_t init_audio_stream_module() {
        g_active_channels.store(0);
        g_active_sessions.clear();
        return SWITCH_STATUS_SUCCESS;
    }

    void cleanup_audio_stream_module() {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        g_active_sessions.clear();
        g_active_channels.store(0);
    }

    int validate_ws_uri(const char* url, char* wsUri) {
        const char* scheme = nullptr;
        const char* hostStart = nullptr;
        const char* hostEnd = nullptr;
        const char* portStart = nullptr;

        // Check scheme
        if (strncmp(url, "ws://", 5) == 0) {
            scheme = "ws";
            hostStart = url + 5;
        } else if (strncmp(url, "wss://", 6) == 0) {
            scheme = "wss";
            hostStart = url + 6;
        } else {
            return 0;
        }

        // Find host end or port start
        hostEnd = hostStart;
        while (*hostEnd && *hostEnd != ':' && *hostEnd != '/') {
            if (!std::isalnum(*hostEnd) && *hostEnd != '-' && *hostEnd != '.') {
                return 0;
            }
            ++hostEnd;
        }

        // Check if host is empty
        if (hostStart == hostEnd) {
            return 0;
        }

        // Check for port
        if (*hostEnd == ':') {
            portStart = hostEnd + 1;
            while (*portStart && *portStart != '/') {
                if (!std::isdigit(*portStart)) {
                    return 0;
                }
                ++portStart;
            }
        }

        // Copy valid URI to wsUri
        std::strncpy(wsUri, url, MAX_WS_URI);
        return 1;
    }

    switch_status_t is_valid_utf8(const char *str) {
        switch_status_t status = SWITCH_STATUS_FALSE;
        while (*str) {
            if ((*str & 0x80) == 0x00) {
                // 1-byte character
                str++;
            } else if ((*str & 0xE0) == 0xC0) {
                // 2-byte character
                if ((str[1] & 0xC0) != 0x80) {
                    return status;
                }
                str += 2;
            } else if ((*str & 0xF0) == 0xE0) {
                // 3-byte character
                if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80) {
                    return status;
                }
                str += 3;
            } else if ((*str & 0xF8) == 0xF0) {
                // 4-byte character
                if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 || (str[3] & 0xC0) != 0x80) {
                    return status;
                }
                str += 4;
            } else {
                // invalid character
                return status;
            }
        }
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_send_text(switch_core_session_t *session, char* text) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_send_text failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

        if (!tech_pvt) return SWITCH_STATUS_FALSE;
        auto *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
        if (pAudioStreamer && text) pAudioStreamer->writeText(text);

        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_pauseresume failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

        if (!tech_pvt) return SWITCH_STATUS_FALSE;

        switch_core_media_bug_flush(bug);
        tech_pvt->audio_paused = pause;
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_init(switch_core_session_t *session,
                                        responseHandler_t responseHandler,
                                        uint32_t samples_per_second,
                                        char *wsUri,
                                        int sampling,
                                        int channels,
                                        char* metadata,
                                        audio_mix_mode_t mix_mode,
                                        void **ppUserData)
    {
        if (!session || !wsUri || !ppUserData) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "stream_session_init: Invalid parameters\n");
            return SWITCH_STATUS_FALSE;
        }

        debug_log_session_info(session, "INIT START");
        DEBUG_LOG(DEBUG_LEVEL_DEBUG, session, "Parameters: samples_per_second=%u, sampling=%d, channels=%d, wsUri=%s", 
                samples_per_second, sampling, channels, wsUri ? wsUri : "NULL");

        // Check channel limit before proceeding
        if (check_channel_limit() != SWITCH_TRUE) {
            DEBUG_LOG(DEBUG_LEVEL_ERROR, session, "Session init FAILED - channel limit reached (%u/%d)", 
                     g_active_channels.load(), MAX_CONCURRENT_CHANNELS);
            debug_log_channels_status();
            return SWITCH_STATUS_FALSE;
        }

        // Increment counter early to reserve slot
        increment_active_channels();
        DEBUG_LOG(DEBUG_LEVEL_DEBUG, session, "Reserved channel slot successfully. Active: %u/%d", 
                 g_active_channels.load(), MAX_CONCURRENT_CHANNELS);

        // Initialize variables with default values
        int deflate = 0;
        int heart_beat = 0;
        bool globalTrace = false;
        bool suppressLog = false;
        const char* buffer_size = nullptr;
        const char* extra_headers = nullptr;
        int rtp_packets = 1;
        bool no_reconnect = false;

        switch_channel_t *channel = switch_core_session_get_channel(session);
        if (!channel) {
            DEBUG_LOG(DEBUG_LEVEL_ERROR, session, "Failed to get channel");
            decrement_active_channels();
            return SWITCH_STATUS_FALSE;
        }

        // Read channel variables safely
        if (switch_channel_var_true(channel, "STREAM_MESSAGE_DEFLATE")) {
            deflate = 1;
        }

        if (switch_channel_var_true(channel, "STREAM_GLOBAL_TRACE")) {
            globalTrace = true;
        }

        if (switch_channel_var_true(channel, "STREAM_SUPPRESS_LOG")) {
            suppressLog = true;
        }

        if (switch_channel_var_true(channel, "STREAM_NO_RECONNECT")) {
            no_reconnect = true;
        }

        const char* heartBeat = switch_channel_get_variable(channel, "STREAM_HEART_BEAT");
        if (heartBeat && *heartBeat) {
            char *endptr;
            long value = strtol(heartBeat, &endptr, 10);
            if (*endptr == '\0' && value > 0 && value <= INT_MAX) {
                heart_beat = (int) value;
                DEBUG_LOG(DEBUG_LEVEL_DEBUG, session, "Set heartbeat to %d seconds", heart_beat);
            }
        }

        if ((buffer_size = switch_channel_get_variable(channel, "STREAM_BUFFER_SIZE"))) {
            int bSize = atoi(buffer_size);
            if(bSize % 20 != 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, 
                                  "%s: Buffer size of %s is not a multiple of 20ms. Using default 20ms.\n",
                                  switch_channel_get_name(channel), buffer_size);
            } else if(bSize >= 20 && bSize <= 1000){  // Limit max buffer size
                rtp_packets = bSize/20;
                DEBUG_LOG(DEBUG_LEVEL_DEBUG, session, "Set buffer size to %dms (%d packets)", bSize, rtp_packets);
            }
        }

        extra_headers = switch_channel_get_variable(channel, "STREAM_EXTRA_HEADERS");

        // Allocate per-session tech_pvt with error checking
        auto* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));
        if (!tech_pvt) {
            DEBUG_LOG(DEBUG_LEVEL_ERROR, session, "Failed to allocate tech_pvt memory");
            decrement_active_channels();
            return SWITCH_STATUS_FALSE;
        }
        
        // Initialize tech_pvt to prevent garbage values
        memset(tech_pvt, 0, sizeof(private_t));
        DEBUG_LOG(DEBUG_LEVEL_DEBUG, session, "Allocated and initialized tech_pvt successfully");
        
        // Try to initialize stream data
        switch_status_t init_result = stream_data_init(tech_pvt, session, wsUri, samples_per_second, sampling, 
                                                      channels, metadata, responseHandler, deflate, heart_beat,
                                                      globalTrace, suppressLog, rtp_packets, extra_headers, no_reconnect, mix_mode);
        
        if (init_result != SWITCH_STATUS_SUCCESS) {
            DEBUG_LOG(DEBUG_LEVEL_ERROR, session, "stream_data_init failed with status %d", init_result);
            decrement_active_channels();
            return SWITCH_STATUS_FALSE;
        }

        *ppUserData = tech_pvt;
        DEBUG_LOG(DEBUG_LEVEL_INFO, session, "Session initialized successfully. Active channels: %u/%d", 
                 g_active_channels.load(), MAX_CONCURRENT_CHANNELS);
        
        if (g_debug_level.load() >= DEBUG_LEVEL_DEBUG) {
            debug_log_channels_status();
        }
        
        return SWITCH_STATUS_SUCCESS;
    }

    // Enhanced mixing helper function - mixes two audio samples with soft clipping prevention
    static inline int16_t mix_samples_enhanced(int16_t sample1, int16_t sample2) {
        // Use 32-bit arithmetic to prevent overflow
        int32_t mixed = static_cast<int32_t>(sample1) + static_cast<int32_t>(sample2);
        
        // Apply soft clipping with headroom to prevent distortion
        // This provides better audio quality than hard clipping
        if (mixed > 32767) {
            mixed = 32767;
        } else if (mixed < -32768) {
            mixed = -32768;
        }
        
        return static_cast<int16_t>(mixed);
    }

    // Alternative mixing with automatic gain control for better quality
    static inline int16_t mix_samples_agc(int16_t sample1, int16_t sample2, float gain) {
        int32_t mixed = static_cast<int32_t>(static_cast<float>(sample1) * gain) + 
                        static_cast<int32_t>(static_cast<float>(sample2) * gain);
        
        // Soft clip
        if (mixed > 32767) mixed = 32767;
        else if (mixed < -32768) mixed = -32768;
        
        return static_cast<int16_t>(mixed);
    }

    switch_bool_t stream_frame(switch_media_bug_t *bug)
    {
        auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt || tech_pvt->audio_paused || tech_pvt->close_requested) {
            return SWITCH_TRUE;
        }

        // Use trylock to prevent blocking
        if (switch_mutex_trylock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS) {
            return SWITCH_TRUE;
        }

        if (!tech_pvt->pAudioStreamer) {
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

        auto *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);

        if(!pAudioStreamer->isConnected()) {
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

        try {
            // Handle enhanced mixed mode separately for better quality mixing
            if (tech_pvt->mix_mode == MIX_MODE_ENHANCED_MIXED) {
                // In enhanced mixed mode, we capture both directions and mix them manually
                // Using switch_core_media_bug_read with SWITCH_TRUE gets the mixed audio,
                // but we want to apply our own mixing algorithm for better quality
                
                uint8_t read_data[SWITCH_RECOMMENDED_BUFFER_SIZE];
                switch_frame_t frame = {};
                frame.data = read_data;
                frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
                
                // Get the native read frame (caller's audio)
                switch_frame_t *native_read = switch_core_media_bug_get_native_read_frame(bug);
                // Get the native write frame (callee's audio / playback)
                switch_frame_t *native_write = switch_core_media_bug_get_native_write_frame(bug);
                
                if (native_read && native_read->datalen > 0) {
                    size_t sample_count = native_read->datalen / sizeof(int16_t);
                    int16_t *read_samples = static_cast<int16_t*>(native_read->data);
                    int16_t *write_samples = nullptr;
                    size_t write_sample_count = 0;
                    
                    if (native_write && native_write->datalen > 0) {
                        write_samples = static_cast<int16_t*>(native_write->data);
                        write_sample_count = native_write->datalen / sizeof(int16_t);
                    }
                    
                    // Create mixed output buffer
                    int16_t mixed_samples[sample_count];
                    
                    // Mix with enhanced algorithm - use 0.85 gain factor to prevent clipping 
                    // while preserving more dynamic range
                    const float mix_gain = 0.85f;
                    for (size_t i = 0; i < sample_count; i++) {
                        int16_t read_sample = read_samples[i];
                        int16_t write_sample = (write_samples && i < write_sample_count) ? write_samples[i] : 0;
                        mixed_samples[i] = mix_samples_agc(read_sample, write_sample, mix_gain);
                    }
                    
                    // Handle resampling if needed
                    if (tech_pvt->resampler) {
                        spx_uint32_t in_len = sample_count;
                        const size_t max_out_samples = sample_count * 2; // Allow for upsampling
                        int16_t resampled[max_out_samples];
                        spx_uint32_t out_len = max_out_samples;
                        
                        speex_resampler_process_int(tech_pvt->resampler,
                                        0,
                                        mixed_samples,
                                        &in_len,
                                        resampled,
                                        &out_len);
                        
                        if (out_len > 0) {
                            size_t bytes_written = out_len * sizeof(int16_t);
                            if (tech_pvt->rtp_packets == 1) {
                                pAudioStreamer->writeBinary(reinterpret_cast<uint8_t*>(resampled), bytes_written);
                            } else {
                                switch_buffer_write(tech_pvt->sbuffer, reinterpret_cast<uint8_t*>(resampled), bytes_written);
                                if (switch_buffer_freespace(tech_pvt->sbuffer) == 0) {
                                    const switch_size_t buf_len = switch_buffer_inuse(tech_pvt->sbuffer);
                                    uint8_t buf_ptr[buf_len];
                                    switch_buffer_read(tech_pvt->sbuffer, buf_ptr, buf_len);
                                    switch_buffer_zero(tech_pvt->sbuffer);
                                    pAudioStreamer->writeBinary(buf_ptr, buf_len);
                                }
                            }
                        }
                    } else {
                        // No resampling needed
                        size_t bytes_to_send = sample_count * sizeof(int16_t);
                        if (tech_pvt->rtp_packets == 1) {
                            pAudioStreamer->writeBinary(reinterpret_cast<uint8_t*>(mixed_samples), bytes_to_send);
                        } else {
                            // Use ring buffer for larger packets
                            size_t available = ringBufferFreeSpace(tech_pvt->buffer);
                            if (available >= bytes_to_send) {
                                ringBufferAppendMultiple(tech_pvt->buffer, reinterpret_cast<uint8_t*>(mixed_samples), bytes_to_send);
                            }
                            if (ringBufferFreeSpace(tech_pvt->buffer) == 0) {
                                size_t nBytes = ringBufferLen(tech_pvt->buffer);
                                uint8_t chunkPtr[nBytes];
                                ringBufferGetMultiple(tech_pvt->buffer, chunkPtr, nBytes);
                                pAudioStreamer->writeBinary(chunkPtr, nBytes);
                                ringBufferClear(tech_pvt->buffer);
                            }
                        }
                    }
                }
            }
            // Standard modes (mono, mixed, stereo) - existing code
            else if (nullptr == tech_pvt->resampler) {
                uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
                switch_frame_t frame = {};
                frame.data = data;
                frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
                size_t available = ringBufferFreeSpace(tech_pvt->buffer);
                while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !tech_pvt->close_requested) {
                    if(frame.datalen) {
                        if (1 == tech_pvt->rtp_packets) {
                            pAudioStreamer->writeBinary((uint8_t *) frame.data, frame.datalen);
                            continue;
                        }

                        size_t remaining = 0;
                        if(available >= frame.datalen) {
                            ringBufferAppendMultiple(tech_pvt->buffer, static_cast<uint8_t *>(frame.data), frame.datalen);
                        } else {
                            ringBufferAppendMultiple(tech_pvt->buffer, static_cast<uint8_t *>(frame.data), available);
                            remaining = frame.datalen - available;
                        }

                        if(0 == ringBufferFreeSpace(tech_pvt->buffer)) {
                            size_t nFrames = ringBufferLen(tech_pvt->buffer);
                            size_t nBytes = nFrames + remaining;
                            uint8_t chunkPtr[nBytes];
                            ringBufferGetMultiple(tech_pvt->buffer, &chunkPtr[0], nBytes);

                            if(remaining > 0) {
                                memcpy(&chunkPtr[nBytes - remaining], static_cast<uint8_t *>(frame.data) + frame.datalen - remaining, remaining);
                            }

                            pAudioStreamer->writeBinary(chunkPtr, nBytes);
                            ringBufferClear(tech_pvt->buffer);
                        }
                    }
                }
            } else {
                // ...existing resampling code...
                uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
                switch_frame_t frame = {};
                frame.data = data;
                frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
                const size_t available = switch_buffer_freespace(tech_pvt->sbuffer);

                while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !tech_pvt->close_requested) {
                    if(frame.datalen) {
                        spx_uint32_t in_len = frame.samples;
                        spx_uint32_t out_len = (available / (tech_pvt->channels * sizeof(spx_int16_t)));
                        spx_int16_t out[available / sizeof(spx_int16_t)];

                        if(tech_pvt->channels == 1) {
                            speex_resampler_process_int(tech_pvt->resampler,
                                            0,
                                            (const spx_int16_t *)frame.data,
                                            &in_len,
                                            &out[0],
                                            &out_len);
                        } else {
                            speex_resampler_process_interleaved_int(tech_pvt->resampler,
                                            (const spx_int16_t *)frame.data,
                                            &in_len,
                                            &out[0],
                                            &out_len);
                        }

                        if(out_len > 0) {
                            const size_t bytes_written = out_len * tech_pvt->channels * sizeof(spx_int16_t);
                            if (tech_pvt->rtp_packets == 1) {
                                pAudioStreamer->writeBinary((uint8_t *) out, bytes_written);
                                continue;
                            }
                            if (bytes_written <= available) {
                                switch_buffer_write(tech_pvt->sbuffer, (const uint8_t *)out, bytes_written);
                            }
                        }

                        if(switch_buffer_freespace(tech_pvt->sbuffer) == 0) {
                            const switch_size_t buf_len= switch_buffer_inuse(tech_pvt->sbuffer);
                            uint8_t buf_ptr[buf_len];
                            switch_buffer_read(tech_pvt->sbuffer, buf_ptr, buf_len);
                            switch_buffer_zero(tech_pvt->sbuffer);
                            pAudioStreamer->writeBinary(buf_ptr, buf_len);
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error in stream_frame: %s\n", e.what());
        }

        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
    }

    switch_status_t stream_session_cleanup(switch_core_session_t *session, char* text, int channelIsClosing) {
        debug_log_session_info(session, "CLEANUP START");
        
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        
        if (!bug) {
            DEBUG_LOG(DEBUG_LEVEL_DEBUG, session, "No bug found - connection already closed");
            return SWITCH_STATUS_FALSE;
        }

        // IMPORTANT: Clear private data FIRST to prevent "bug already attached" errors
        // on rapid stop/start sequences
        switch_channel_set_private(channel, MY_BUG_NAME, nullptr);

        auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt) {
            DEBUG_LOG(DEBUG_LEVEL_ERROR, session, "No tech_pvt found");
            // Remove the bug even without tech_pvt
            if (!channelIsClosing) {
                switch_core_media_bug_remove(session, &bug);
            }
            return SWITCH_STATUS_FALSE;
        }

        char sessionId[MAX_SESSION_ID];
        strncpy(sessionId, tech_pvt->sessionId, MAX_SESSION_ID - 1);
        sessionId[MAX_SESSION_ID - 1] = '\0';

        DEBUG_LOG(DEBUG_LEVEL_DEBUG, session, "Cleanup for session %s, channelIsClosing=%d", 
                sessionId, channelIsClosing);

        // Prevent multiple cleanup calls
        if (tech_pvt->close_requested) {
            DEBUG_LOG(DEBUG_LEVEL_DEBUG, session, "Cleanup already in progress for session %s", sessionId);
            // Still need to remove the bug if not closing
            if (!channelIsClosing) {
                switch_core_media_bug_remove(session, &bug);
            }
            return SWITCH_STATUS_SUCCESS;
        }
        
        tech_pvt->close_requested = 1;
        DEBUG_LOG(DEBUG_LEVEL_DEBUG, session, "Set close_requested flag for session %s", sessionId);

        if (tech_pvt->mutex) {
            switch_mutex_lock(tech_pvt->mutex);
        }

        // Remove the bug AFTER setting close_requested to avoid race in callback
        if (!channelIsClosing) {
            switch_core_media_bug_remove(session, &bug);
        }

        auto* audioStreamer = (AudioStreamer *) tech_pvt->pAudioStreamer;
        if (audioStreamer) {
            audioStreamer->deleteFiles();
            if (text) {
                audioStreamer->writeText(text);
            }
            finish(tech_pvt);
        }

        if (tech_pvt->mutex) {
            switch_mutex_unlock(tech_pvt->mutex);
        }

        destroy_tech_pvt(tech_pvt);

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%s) stream_session_cleanup: connection closed\n", sessionId);
        return SWITCH_STATUS_SUCCESS;
    }

    // Audio injection functions
    switch_status_t enable_audio_injection(switch_core_session_t *session, int enable) {
        if (!session) {
            return SWITCH_STATUS_FALSE;
        }
        
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if (!channel) {
            return SWITCH_STATUS_FALSE;
        }
        
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, 
                "enable_audio_injection: No active stream bug found\n");
            return SWITCH_STATUS_FALSE;
        }
        
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt) {
            return SWITCH_STATUS_FALSE;
        }
        
        tech_pvt->inject_audio_enabled = enable;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, 
            "(%s) Audio injection %s\n", tech_pvt->sessionId, enable ? "enabled" : "disabled");
        
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t inject_audio_data(switch_core_session_t *session, const uint8_t* audio_data, 
                                      size_t data_len, int sample_rate, int channels) {
        if (!session || !audio_data || data_len == 0) {
            return SWITCH_STATUS_FALSE;
        }
        
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if (!channel) {
            return SWITCH_STATUS_FALSE;
        }
        
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            return SWITCH_STATUS_FALSE;
        }
        
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt || !tech_pvt->inject_audio_enabled) {
            return SWITCH_STATUS_FALSE;
        }

        if (!tech_pvt->inject_buffer || !tech_pvt->inject_mutex) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                "(%s) inject_audio_data: injection not initialized\n", tech_pvt->sessionId);
            return SWITCH_STATUS_FALSE;
        }

        if (sample_rate <= 0) {
            return SWITCH_STATUS_FALSE;
        }

        const uint8_t* input_ptr = audio_data;
        size_t input_len = data_len;
        std::vector<int16_t> mono_samples;
        std::vector<int16_t> resampled_samples;

        if (channels > 1) {
            const size_t frames = data_len / (sizeof(int16_t) * channels);
            mono_samples.resize(frames);
            const int16_t *in = reinterpret_cast<const int16_t *>(audio_data);
            for (size_t i = 0; i < frames; ++i) {
                int32_t acc = 0;
                for (int c = 0; c < channels; ++c) {
                    acc += in[i * channels + c];
                }
                mono_samples[i] = static_cast<int16_t>(acc / channels);
            }
            input_ptr = reinterpret_cast<const uint8_t *>(mono_samples.data());
            input_len = mono_samples.size() * sizeof(int16_t);
            channels = 1;
        }

        const int out_rate = tech_pvt->sampling > 0 ? tech_pvt->sampling : sample_rate;
        if (sample_rate != out_rate) {
            int err = 0;
            if (!tech_pvt->inject_resampler || tech_pvt->inject_sample_rate != sample_rate) {
                if (tech_pvt->inject_resampler) {
                    speex_resampler_destroy(tech_pvt->inject_resampler);
                    tech_pvt->inject_resampler = nullptr;
                }
                tech_pvt->inject_resampler = speex_resampler_init(1,
                                                                   static_cast<spx_uint32_t>(sample_rate),
                                                                   static_cast<spx_uint32_t>(out_rate),
                                                                   SWITCH_RESAMPLE_QUALITY,
                                                                   &err);
                if (err != 0 || !tech_pvt->inject_resampler) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                        "(%s) inject_audio_data: failed to init inject resampler: %s\n",
                        tech_pvt->sessionId, speex_resampler_strerror(err));
                    return SWITCH_STATUS_FALSE;
                }
                tech_pvt->inject_sample_rate = sample_rate;
            }

            const spx_int16_t *in = reinterpret_cast<const spx_int16_t *>(input_ptr);
            spx_uint32_t in_len = static_cast<spx_uint32_t>(input_len / sizeof(int16_t));
            spx_uint32_t out_len = static_cast<spx_uint32_t>((static_cast<uint64_t>(in_len) * out_rate) / sample_rate + 16);
            resampled_samples.resize(out_len);

            speex_resampler_process_int(tech_pvt->inject_resampler,
                                        0,
                                        in,
                                        &in_len,
                                        reinterpret_cast<spx_int16_t *>(resampled_samples.data()),
                                        &out_len);
            resampled_samples.resize(out_len);
            input_ptr = reinterpret_cast<const uint8_t *>(resampled_samples.data());
            input_len = resampled_samples.size() * sizeof(int16_t);
        }

        switch_mutex_lock(tech_pvt->inject_mutex);
        switch_size_t in_use = switch_buffer_inuse(tech_pvt->inject_buffer);
        switch_size_t free_bytes = switch_buffer_freespace(tech_pvt->inject_buffer);
        const switch_size_t capacity = in_use + free_bytes;

        const uint8_t *write_ptr = input_ptr;
        switch_size_t write_len = static_cast<switch_size_t>(input_len);

        // If incoming chunk is larger than total buffer capacity, keep only newest tail.
        if (write_len > capacity) {
            write_ptr = input_ptr + (write_len - capacity);
            write_len = capacity;
        }

        if (write_len > free_bytes) {
            const switch_size_t bytes_to_drop = write_len - free_bytes;
            std::vector<uint8_t> drop_tmp(bytes_to_drop);
            const switch_size_t dropped = switch_buffer_read(tech_pvt->inject_buffer, drop_tmp.data(), bytes_to_drop);
            if (dropped < bytes_to_drop) {
                switch_buffer_zero(tech_pvt->inject_buffer);
                in_use = 0;
                free_bytes = switch_buffer_freespace(tech_pvt->inject_buffer);
            } else {
                in_use = switch_buffer_inuse(tech_pvt->inject_buffer);
                free_bytes = switch_buffer_freespace(tech_pvt->inject_buffer);
            }
        }

        const switch_size_t bytes_to_write = (write_len <= free_bytes) ? write_len : free_bytes;
        if (bytes_to_write > 0) {
            switch_buffer_write(tech_pvt->inject_buffer, write_ptr, bytes_to_write);
        }
        switch_mutex_unlock(tech_pvt->inject_mutex);

        if (bytes_to_write == 0) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                "(%s) inject_audio_data: no space after recovery, dropping %zu bytes\n",
                tech_pvt->sessionId, input_len);
            return SWITCH_STATUS_FALSE;
        }
        
        DEBUG_LOG(DEBUG_LEVEL_DEBUG, session, "inject_audio_data: queued %zu/%zu bytes at %d Hz", 
                 static_cast<size_t>(bytes_to_write), input_len, sample_rate);
        
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t process_injected_audio(switch_core_session_t *session, switch_frame_t *frame) {
        if (!session || !frame) {
            return SWITCH_STATUS_FALSE;
        }

        switch_channel_t *channel = switch_core_session_get_channel(session);
        if (!channel) {
            return SWITCH_STATUS_FALSE;
        }

        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            return SWITCH_STATUS_FALSE;
        }

        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt || !tech_pvt->inject_audio_enabled || !tech_pvt->inject_buffer || !tech_pvt->inject_mutex) {
            return SWITCH_STATUS_FALSE;
        }

        switch_mutex_lock(tech_pvt->inject_mutex);
        const switch_size_t in_use = switch_buffer_inuse(tech_pvt->inject_buffer);
        if (in_use == 0) {
            switch_mutex_unlock(tech_pvt->inject_mutex);
            return SWITCH_STATUS_FALSE;
        }

        const switch_size_t to_read = static_cast<switch_size_t>((frame->datalen <= in_use) ? frame->datalen : in_use);
        const switch_size_t read_bytes = switch_buffer_read(tech_pvt->inject_buffer,
                                                             reinterpret_cast<uint8_t *>(frame->data),
                                                             to_read);
        switch_mutex_unlock(tech_pvt->inject_mutex);

        if (read_bytes == 0) {
            return SWITCH_STATUS_FALSE;
        }

        // Reemplaza audio saliente; si falta audio inyectado completa con silencio.
        if (read_bytes < frame->datalen) {
            memset(reinterpret_cast<uint8_t *>(frame->data) + read_bytes, 0, frame->datalen - read_bytes);
        }
        
        return SWITCH_STATUS_SUCCESS;
    }
}
