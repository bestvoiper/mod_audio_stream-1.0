#include <string>
#include <cstring>
#include <vector>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <algorithm>
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
#include "base64.h"

#define FRAME_SIZE_8000  320 /* 1000x0.02 (20ms)= 160 x(16bit= 2 bytes) 320 frame size*/
/* If Vosk/proxy stall, keep ~2.5s instead of dropping to 400ms (that skipped late greetings). */
#define STREAM_WS_BACKLOG_MS 2500

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
}

// Forward declarations for functions used in cleanup
extern "C" {
    void decrement_active_channels();
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
            webSocket.setConnectionTimeout(15);
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Set websocket connect timeout to 15s for session %s", uuid);
            
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
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                                  "mod_audio_stream: WebSocket opened for session %s (queued %zu bytes before open)\n",
                                  m_sessionId.c_str(), m_send_used);
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
                /* Metadata goes first (CONNECT_SUCCESS), then audio captured during handshake. */
                flushPendingAudio();
            });

            webSocket.setErrorCallback([this](int error_code, const std::string& error_message){
                m_connected = false;
                m_audio_ready = false;
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
                m_audio_ready = false;
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

            /* Connect after the media bug is attached so early-media frames
               are queued from the first RTP packet, not dropped during TLS/DNS. */
            startSender();
            DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "AudioStreamer ready for session %s (websocket deferred)", uuid);
            
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
        auto *bug = get_media_bug(session);
        if(bug) {
            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            tech_pvt->close_requested = 1;
            switch_core_media_bug_close(&bug, SWITCH_FALSE);
        }
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
                int sampleRate;
                if (0 == strcmp(jsAudioDataType, "raw")) {
                    cJSON* jsonSampleRate = cJSON_GetObjectItem(jsonData, "sampleRate");
                    sampleRate = jsonSampleRate && jsonSampleRate->valueint ? jsonSampleRate->valueint : 0;
                    std::unordered_map<int, const char*> sampleRateMap = {
                            {8000, ".r8"},
                            {16000, ".r16"},
                            {24000, ".r24"},
                            {32000, ".r32"},
                            {48000, ".r48"},
                            {64000, ".r64"}
                    };
                    auto it = sampleRateMap.find(sampleRate);
                    fileType = (it != sampleRateMap.end()) ? it->second : "";
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

                if(jsonAudio && jsonAudio->valuestring != nullptr && !fileType.empty()) {
                    char filePath[256];
                    std::string rawAudio;
                    try {
                        rawAudio = base64_decode(jsonAudio->valuestring);
                    } catch (const std::exception& e) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%s) processMessage - base64 decode error: %s\n",
                                          m_sessionId.c_str(), e.what());
                        cJSON_Delete(jsonAudio); cJSON_Delete(json);
                        return status;
                    }
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
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%s) processMessage - failed to create file: %s\n",
                                          m_sessionId.c_str(), filePath);
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
        stopSender();
        
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
        m_audio_ready = false;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "disconnecting session %s...\n", m_sessionId.c_str());
        stopSender();
        webSocket.disconnect();
    }

    void start() {
        DEBUG_LOG_GLOBAL(DEBUG_LEVEL_DEBUG, "Starting WebSocket for session %s", m_sessionId.c_str());
        webSocket.connect();
    }

    bool isConnected() {
        return m_connected && webSocket.isConnected();
    }

    void setPcmFormat(int rate, int channels) {
        if (rate <= 0) {
            rate = 8000;
        }
        if (channels <= 0) {
            channels = 1;
        }
        /* ~100ms of live audio. A larger backlog never drains if WSS is realtime. */
        m_bytes_per_ms = (size_t) rate * (size_t) channels * sizeof(int16_t) / 1000;
        if (!m_bytes_per_ms) {
            m_bytes_per_ms = 16;
        }
        m_live_cap = m_bytes_per_ms * 100;
        if (m_live_cap < 640) {
            m_live_cap = 640;
        }
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                          "mod_audio_stream: live send cap %zu bytes (~100ms) rate=%dHz ch=%d session %s\n",
                          m_live_cap, rate, channels, m_sessionId.c_str());
    }

    /* Media thread: memcpy only. A sender thread talks to the websocket so a
       slow WSS cannot stall RTP and overflow the media-bug buffer (cuts). */
    void writeBinary(uint8_t* buffer, size_t len) {
        if (!buffer || !len || m_send_stop.load()) {
            return;
        }
        enqueuePcm(buffer, len);
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

    void flushPendingAudio() {
        size_t queued = 0;
        {
            std::lock_guard<std::mutex> lock(m_send_mutex);
            queued = m_send_used;
            /* Do not trim pre-connect silence/ring to 100ms. That discarded the
               leading CNG and shifted the voicemail (19s FS → 15s AMD). Vosk
               consumes a burst immediately; the WAV stays aligned with record_session. */
            m_audio_ready = true;
        }
        if (queued) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                              "mod_audio_stream: releasing %zu queued bytes (~%zu ms) for session %s\n",
                              queued, m_bytes_per_ms ? queued / m_bytes_per_ms : 0, m_sessionId.c_str());
        }
        m_send_cv.notify_one();
    }

    void startSender() {
        if (m_sender_started.exchange(true)) {
            return;
        }
        m_send_stop = false;
        m_send_ring.assign(SEND_RING_BYTES, 0);
        m_send_cap = SEND_RING_BYTES;
        m_send_head = m_send_tail = m_send_used = 0;
        m_send_thread = std::thread([this] { senderLoop(); });
    }

    void stopSender() {
        if (!m_sender_started.load()) {
            return;
        }
        m_send_stop = true;
        m_send_cv.notify_all();
        if (m_send_thread.joinable()) {
            m_send_thread.join();
        }
        m_sender_started = false;
    }

    void enqueuePcm(const uint8_t *data, size_t len) {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        if (!m_send_cap || m_send_ring.empty()) {
            return;
        }
        if (len >= m_send_cap) {
            data += (len - (m_send_cap - 1));
            len = m_send_cap - 1;
        }
        while (m_send_used + len > m_send_cap && m_send_used > 0) {
            size_t drop = std::min(m_send_used, std::max(len, (size_t) 320));
            m_send_tail = (m_send_tail + drop) % m_send_cap;
            m_send_used -= drop;
            m_dropped_bytes += drop;
            if (m_dropped_bytes == drop || (m_dropped_bytes / 16000) != ((m_dropped_bytes - drop) / 16000)) {
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_WARNING,
                               "Send queue overflow for session %s; dropped oldest audio (total dropped %zu bytes)",
                               m_sessionId.c_str(), m_dropped_bytes);
            }
        }
        size_t first = std::min(len, m_send_cap - m_send_head);
        memcpy(&m_send_ring[m_send_head], data, first);
        if (len > first) {
            memcpy(&m_send_ring[0], data + first, len - first);
        }
        m_send_head = (m_send_head + len) % m_send_cap;
        m_send_used += len;
        /* Before the socket is up, keep up to 5s (ring/CNG during TLS). After
           connect, keep STREAM_WS_BACKLOG_MS so a slow AMD does not skip the greeting. */
        if (!m_audio_ready.load()) {
            const size_t preconnect = m_bytes_per_ms ? (m_bytes_per_ms * 5000) : 80000;
            if (m_send_used > preconnect) {
                size_t discarded = m_send_used - preconnect;
                m_send_tail = (m_send_tail + discarded) % m_send_cap;
                m_send_used = preconnect;
                m_dropped_bytes += discarded;
            }
        } else {
            const size_t jitter = m_bytes_per_ms ? (m_bytes_per_ms * STREAM_WS_BACKLOG_MS) : 40000;
            if (m_send_used > jitter) {
                size_t discarded = m_send_used - jitter;
                m_send_tail = (m_send_tail + discarded) % m_send_cap;
                m_send_used = jitter;
                m_dropped_bytes += discarded;
            }
        }
        m_send_cv.notify_one();
    }

    size_t dropToLiveCapLocked() {
        size_t discarded = 0;
        if (!m_live_cap || m_send_used <= m_live_cap) {
            return 0;
        }
        discarded = m_send_used - m_live_cap;
        m_send_tail = (m_send_tail + discarded) % m_send_cap;
        m_send_used = m_live_cap;
        m_dropped_bytes += discarded;
        return discarded;
    }

    void senderLoop() {
        uint8_t chunk[1280];
        while (!m_send_stop.load()) {
            std::unique_lock<std::mutex> lock(m_send_mutex);
            m_send_cv.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return m_send_stop.load() ||
                       (m_audio_ready.load() && m_connected.load() && m_send_used > 0);
            });
            if (m_send_stop.load()) {
                break;
            }
            if (!m_audio_ready.load() || !m_connected.load() || m_send_used == 0) {
                continue;
            }
            /* Do not hold the send mutex while inspecting the WS socket. */
            lock.unlock();

            const size_t pending = webSocket.outboundQueuedBytes();
            const size_t watermark = m_bytes_per_ms ? (m_bytes_per_ms * STREAM_WS_BACKLOG_MS) : 40000;

            lock.lock();
            if (pending > watermark) {
                size_t discarded = 0;
                if (m_send_used > watermark) {
                    discarded = m_send_used - watermark;
                    m_send_tail = (m_send_tail + discarded) % m_send_cap;
                    m_send_used = watermark;
                    m_dropped_bytes += discarded;
                }
                lock.unlock();
                auto now = std::chrono::steady_clock::now();
                if (discarded && (m_last_lag_log.time_since_epoch().count() == 0 ||
                                  now - m_last_lag_log > std::chrono::seconds(2))) {
                    m_last_lag_log = now;
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                                      "mod_audio_stream: WSS output buffer %zu bytes (~%zu ms); dropped %zu bytes to stay live for session %s\n",
                                      pending,
                                      m_bytes_per_ms ? pending / m_bytes_per_ms : 0,
                                      discarded, m_sessionId.c_str());
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            /* One 20ms frame. Dumping 80ms+ bursts fills the TCP buffer and recreates the lag. */
            size_t frame_bytes = m_bytes_per_ms ? (m_bytes_per_ms * 20) : 320;
            if (frame_bytes < 320) {
                frame_bytes = 320;
            }
            if (frame_bytes > sizeof(chunk)) {
                frame_bytes = sizeof(chunk);
            }
            size_t n = std::min(m_send_used, frame_bytes);
            size_t first = std::min(n, m_send_cap - m_send_tail);
            memcpy(chunk, &m_send_ring[m_send_tail], first);
            if (n > first) {
                memcpy(chunk + first, &m_send_ring[0], n - first);
            }
            m_send_tail = (m_send_tail + n) % m_send_cap;
            m_send_used -= n;
            lock.unlock();

            try {
                if (!webSocket.sendBinary(chunk, n)) {
                    m_connected = false;
                    m_audio_ready = false;
                }
            } catch (const std::exception &e) {
                DEBUG_LOG_GLOBAL(DEBUG_LEVEL_ERROR, "Sender thread error for session %s: %s",
                               m_sessionId.c_str(), e.what());
                m_connected = false;
                m_audio_ready = false;
            }
        }
    }

private:
    static const size_t SEND_RING_BYTES = 16000 * 2 * 2 * 4; /* 4s @ 16kHz stereo */

    std::string m_sessionId;
    responseHandler_t m_notify;
    WebSocketClient webSocket;
    bool m_suppress_log;
    bool m_global_trace;
    const char* m_extra_headers;
    int m_playFile;
    std::unordered_set<std::string> m_Files;
    std::atomic<bool> m_connected;
    std::atomic<bool> m_audio_ready{false};
    std::mutex m_send_mutex;
    std::condition_variable m_send_cv;
    std::vector<uint8_t> m_send_ring;
    size_t m_send_cap = 0;
    size_t m_send_head = 0;
    size_t m_send_tail = 0;
    size_t m_send_used = 0;
    size_t m_dropped_bytes = 0;
    size_t m_live_cap = 1600; /* 100ms @ 8kHz mono until setPcmFormat */
    size_t m_bytes_per_ms = 16;
    std::thread m_send_thread;
    std::atomic<bool> m_send_stop{false};
    std::atomic<bool> m_sender_started{false};
    std::chrono::steady_clock::time_point m_last_lag_log{};
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
                                     bool no_reconnect)
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

        if (metadata) strncpy(tech_pvt->initialMetadata, metadata, MAX_METADATA_LEN);

        //size_t buflen = (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * 1000 / RTP_PERIOD * BUFFERED_SEC);
        const size_t buflen = (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * rtp_packets);

        auto* as = new AudioStreamer(tech_pvt->sessionId, wsUri, responseHandler, deflate, heart_beat, globalTrace, suppressLog, extra_headers, no_reconnect);
        as->setPcmFormat(desiredSampling, channels);
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

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_data_init\n", tech_pvt->sessionId);

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
        int heart_beat = 10; /* keep WSS alive during long ring / delayed voicemail */
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
                                                      globalTrace, suppressLog, rtp_packets, extra_headers, no_reconnect);
        
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

    void stream_session_start(void *pUserData)
    {
        if (!pUserData) {
            return;
        }
        auto *tech_pvt = static_cast<private_t *>(pUserData);
        if (!tech_pvt->pAudioStreamer) {
            return;
        }
        auto *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
        pAudioStreamer->start();
    }

    static void send_pcm_bytes(private_t *tech_pvt, AudioStreamer *pAudioStreamer,
                               const uint8_t *data, uint32_t datalen, uint32_t samples)
    {
        if (!data || !datalen) {
            return;
        }

        if (nullptr == tech_pvt->resampler) {
            if (1 == tech_pvt->rtp_packets) {
                pAudioStreamer->writeBinary(const_cast<uint8_t *>(data), datalen);
                return;
            }

            size_t available = ringBufferFreeSpace(tech_pvt->buffer);
            size_t remaining = 0;
            if (available >= datalen) {
                ringBufferAppendMultiple(tech_pvt->buffer, data, datalen);
            } else {
                ringBufferAppendMultiple(tech_pvt->buffer, data, available);
                remaining = datalen - available;
            }

            if (0 == ringBufferFreeSpace(tech_pvt->buffer)) {
                size_t nFrames = ringBufferLen(tech_pvt->buffer);
                size_t nBytes = nFrames + remaining;
                uint8_t chunkPtr[nBytes];
                ringBufferGetMultiple(tech_pvt->buffer, &chunkPtr[0], nFrames);

                if (remaining > 0) {
                    memcpy(&chunkPtr[nFrames], data + datalen - remaining, remaining);
                }

                pAudioStreamer->writeBinary(chunkPtr, nBytes);
                ringBufferClear(tech_pvt->buffer);
            }
            return;
        }

        const size_t available = switch_buffer_freespace(tech_pvt->sbuffer);
        spx_uint32_t in_len = samples ? samples : (datalen / (tech_pvt->channels * sizeof(spx_int16_t)));
        spx_uint32_t out_len = (spx_uint32_t) (available / (tech_pvt->channels * sizeof(spx_int16_t)));
        if (!out_len) {
            return;
        }
        spx_int16_t out[available / sizeof(spx_int16_t)];

        if (tech_pvt->channels == 1) {
            speex_resampler_process_int(tech_pvt->resampler,
                            0,
                            (const spx_int16_t *) data,
                            &in_len,
                            &out[0],
                            &out_len);
        } else {
            speex_resampler_process_interleaved_int(tech_pvt->resampler,
                            (const spx_int16_t *) data,
                            &in_len,
                            &out[0],
                            &out_len);
        }

        if (out_len > 0) {
            const size_t bytes_written = out_len * tech_pvt->channels * sizeof(spx_int16_t);
            if (tech_pvt->rtp_packets == 1) {
                pAudioStreamer->writeBinary((uint8_t *) out, bytes_written);
                return;
            }
            if (bytes_written <= available) {
                switch_buffer_write(tech_pvt->sbuffer, (const uint8_t *) out, bytes_written);
            }
        }

        if (switch_buffer_freespace(tech_pvt->sbuffer) == 0) {
            const switch_size_t buf_len = switch_buffer_inuse(tech_pvt->sbuffer);
            uint8_t buf_ptr[buf_len];
            switch_buffer_read(tech_pvt->sbuffer, buf_ptr, buf_len);
            switch_buffer_zero(tech_pvt->sbuffer);
            pAudioStreamer->writeBinary(buf_ptr, buf_len);
        }
    }

    static uint32_t clamp_pcm_bytes(const switch_frame_t *frame, int channels, uint32_t rate)
    {
        /* record_session writes real samples. The media path often hands us a
           30ms slot for 20ms RTP (zeros / CNG tail). Sending the slot as-is
           stretches 19s of ring into ~25s and the voicemail "starts late". */
        if (!frame || !frame->data || !frame->datalen) {
            return 0;
        }

        uint32_t ch = channels > 0 ? (uint32_t) channels : (frame->channels ? frame->channels : 1);
        uint32_t datalen = frame->datalen;
        if (datalen % 2) {
            datalen--;
        }

        if (frame->samples && ch) {
            uint32_t from_samples = frame->samples * (uint32_t) sizeof(int16_t) * ch;
            if (from_samples && from_samples < datalen) {
                datalen = from_samples;
            }
        }

        if (frame->codec && frame->codec->implementation && ch) {
            uint32_t native = frame->codec->implementation->samples_per_packet;
            uint32_t native_bytes = native * (uint32_t) sizeof(int16_t) * ch;
            if (native_bytes && datalen > native_bytes) {
                datalen = native_bytes;
            }
        }

        if (!rate) {
            rate = 8000;
        }
        const uint32_t bytes_20ms = (rate / 50) * (uint32_t) sizeof(int16_t) * ch;
        if (bytes_20ms && datalen > bytes_20ms) {
            /* Always 20ms, even if the extra 10ms has ring energy. Sending
               30ms per 20ms callback is what moved the greeting from 13s to 21s. */
            datalen = bytes_20ms;
        }
        return datalen;
    }

    static int pcm_peak(const uint8_t *data, uint32_t len)
    {
        if (!data || len < 2) {
            return 0;
        }
        const int16_t *s = reinterpret_cast<const int16_t *>(data);
        uint32_t n = len / 2;
        int peak = 0;
        for (uint32_t i = 0; i < n; i++) {
            int a = s[i] < 0 ? -s[i] : s[i];
            if (a > peak) {
                peak = a;
            }
        }
        return peak;
    }

    /* media_bug_read fill uses 0xFF (int16 -1). Real decoded silence is ~0. */
    static bool pcm_is_bug_padding(const uint8_t *data, uint32_t len)
    {
        if (!data || len < 2) {
            return true;
        }
        const int16_t *s = reinterpret_cast<const int16_t *>(data);
        uint32_t n = len / 2;
        uint32_t pad = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (s[i] == -1) {
                pad++;
            }
        }
        return pad * 2 >= n;
    }

    static void note_pcm_sent(private_t *tech_pvt, const uint8_t *data, uint32_t in_len, uint32_t out_len)
    {
        tech_pvt->dbg_frames++;
        tech_pvt->dbg_bytes += out_len;
        tech_pvt->dbg_last_in = in_len;
        tech_pvt->dbg_last_out = out_len;
        if (tech_pvt->dbg_frames <= 8 || (tech_pvt->dbg_frames % 250) == 0) {
            uint32_t rate = tech_pvt->sampling > 0 ? (uint32_t) tech_pvt->sampling : 8000;
            uint32_t ch = tech_pvt->channels > 0 ? (uint32_t) tech_pvt->channels : 1;
            uint64_t ms = 0;
            if (rate && ch) {
                ms = (tech_pvt->dbg_bytes * 1000) / (rate * ch * sizeof(int16_t));
            }
            int peak = pcm_peak(data, out_len);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                              "mod_audio_stream: pcm frame #%u in=%u out=%u peak=%d (~%lums sent, cng_as_silence=%u) session %s\n",
                              tech_pvt->dbg_frames, in_len, out_len, peak,
                              (unsigned long) ms, tech_pvt->dbg_cng_skip, tech_pvt->sessionId);
        }
    }

    static void emit_clock_silence(private_t *tech_pvt, AudioStreamer *pAudioStreamer, uint32_t rate)
    {
        uint32_t ch = tech_pvt->channels > 0 ? (uint32_t) tech_pvt->channels : 1;
        if (!rate) {
            rate = tech_pvt->sampling > 0 ? (uint32_t) tech_pvt->sampling : 8000;
        }
        uint32_t sil = (rate / 50) * (uint32_t) sizeof(int16_t) * ch;
        if (!sil || sil > 2048) {
            sil = 320;
        }
        static const uint8_t zeros[2048] = {0};
        uint32_t samples = sil / (sizeof(int16_t) * ch);
        tech_pvt->dbg_cng_skip++;
        note_pcm_sent(tech_pvt, zeros, sil, sil);
        send_pcm_bytes(tech_pvt, pAudioStreamer, zeros, sil, samples);
    }

    switch_bool_t stream_frame(switch_media_bug_t *bug)
    {
        auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt || tech_pvt->audio_paused || tech_pvt->close_requested) {
            return SWITCH_TRUE;
        }

        if (switch_mutex_lock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS) {
            return SWITCH_TRUE;
        }

        if (!tech_pvt->pAudioStreamer) {
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

        auto *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);

        try {
            /* fill=TRUE on read-only: empty buffer → FALSE (stops the loop).
               fill=FALSE on mixed: allow one-sided early media without blocking. */
            const switch_bool_t fill = switch_core_media_bug_test_flag(bug, SMBF_WRITE_STREAM)
                ? SWITCH_FALSE : SWITCH_TRUE;

            uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
            switch_frame_t frame = {};
            frame.data = data;
            frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

            int reads = 0;
            int sent_audio = 0;
            int saw_placeholder = 0;
            uint32_t last_rate = 8000;
            while (reads < 8 &&
                   switch_core_media_bug_read(bug, &frame, fill) == SWITCH_STATUS_SUCCESS &&
                   !tech_pvt->close_requested) {
                reads++;
                uint32_t rate = frame.rate ? frame.rate : (uint32_t) tech_pvt->sampling;
                last_rate = rate;
                if (!frame.datalen || switch_test_flag(&frame, SFF_CNG) ||
                    pcm_is_bug_padding(static_cast<const uint8_t *>(frame.data), frame.datalen)) {
                    saw_placeholder = 1;
                    continue;
                }

                uint32_t datalen = clamp_pcm_bytes(&frame, tech_pvt->channels, rate);
                uint32_t samples = 0;
                if (datalen && tech_pvt->channels > 0) {
                    samples = datalen / (sizeof(int16_t) * (uint32_t) tech_pvt->channels);
                }
                if (datalen) {
                    note_pcm_sent(tech_pvt, static_cast<const uint8_t *>(frame.data),
                                   frame.datalen, datalen);
                    send_pcm_bytes(tech_pvt, pAudioStreamer, static_cast<const uint8_t *>(frame.data),
                                   datalen, samples);
                    sent_audio++;
                }
            }
            /* CNG/0xFF is the early-media "silence" record_session writes (peak 1–2).
               Send one 20ms zero frame per callback — not one per extra fill read,
               or the greeting stretches (13s → 21s). */
            if (sent_audio == 0 && saw_placeholder) {
                emit_clock_silence(tech_pvt, pAudioStreamer, last_rate);
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

        auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt) {
            DEBUG_LOG(DEBUG_LEVEL_ERROR, session, "No tech_pvt found");
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
            return SWITCH_STATUS_SUCCESS;
        }
        
        tech_pvt->close_requested = 1;
        DEBUG_LOG(DEBUG_LEVEL_DEBUG, session, "Set close_requested flag for session %s", sessionId);

        if (tech_pvt->mutex) {
            switch_mutex_lock(tech_pvt->mutex);
        }

        switch_channel_set_private(channel, MY_BUG_NAME, nullptr);
        
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
}

