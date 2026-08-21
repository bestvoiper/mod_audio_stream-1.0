/*
 *  WebSocketClient.h
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#include <event2/bufferevent.h>
#ifdef USE_TLS
    #include <event2/bufferevent_ssl.h>
#endif
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/dns.h>
#include <event2/thread.h>
#include <arpa/inet.h>
#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <mutex>
#include <functional>
#include <atomic>
#include <random>
#include <zlib.h>
#include <iomanip>
#include <condition_variable>

#ifdef USE_TLS
    #include <openssl/ssl.h>
    #include <openssl/err.h>
    #include <openssl/rand.h>
    #include <openssl/x509.h>
    #include <openssl/sha.h>
#endif

#include "WebSocketTLSOptions.h"
#include "WebSocketHeaders.h"
#include "Utf8Validator.h"

#include <array>
#include <deque>
#if !USE_TLS
    #include "sha1.hpp"
#endif

#include "base64.h"

// Byte order conversion helpers
#define htonll(x) ((1==htonl(1)) ? (x) : ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#define ntohll(x) htonll(x)

static constexpr char WS_MAGIC[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

class WebSocketClient {
public:

    enum class MessageType {
        TEXT,
        BINARY,
        PING,
        PONG,
        CLOSE
    };

    enum class ConnectionState {
        DISCONNECTED,
        DISCONNECTING,
        CONNECTING,
        CONNECTED,
        FAILED
    };

    enum class ErrorCode {
        IO = 1,
        INVALID_HEADER,
        SERVER_MASKED,
        NOT_SUPPORTED,
        PING_TIMEOUT,
        CONNECT_FAILED,
        TLS_INIT_FAILED,
        SSL_HANDSHAKE_FAILED,
        SSL_ERROR,
    };

    enum class CloseCode {
        NORMAL = 1000,
        GOING_AWAY = 1001,
        PROTOCOL_ERROR = 1002,
        UNSUPPORTED = 1003,
        NO_STATUS = 1005,
        ABNORMAL = 1006,
        INVALID_PAYLOAD = 1007,
        POLICY_VIOLATION = 1008,
        MESSAGE_TOO_BIG = 1009,
        MANDATORY_EXTENSION = 1010,
        INTERNAL_ERROR = 1011,
        SERVICE_RESTART = 1012,
        TRY_AGAIN_LATER = 1013,
        TLS_HANDSHAKE = 1015,
    
        UNKNOWN = 0  // fallback/default
    };

    // Define a message callback type
    using MessageCallback = std::function<void(const std::string&)>;
    using BinaryCallback = std::function<void(const void*, size_t)>;
    using CloseCallback = std::function<void(int code, const std::string& reason)>;
    using ErrorCallback = std::function<void(int error_code, const std::string& error_message)>;
    using OpenCallback = std::function<void()>;

    WebSocketClient();
    virtual ~WebSocketClient();

    // non-copyable
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;
    WebSocketClient(WebSocketClient&&) noexcept = default;
    WebSocketClient& operator=(WebSocketClient&&) noexcept = default;

    void connect();
    void disconnect();
    bool isConnected();
    void setUrl(const std::string& url);
    bool sendMessage(const std::string& message);
    bool sendMessage(const char* msg, size_t len);
    bool sendData(const void* data, size_t length, MessageType type);
    bool sendBinary(const void* data, size_t length);
    /* Bytes waiting in the TCP/TLS output buffer (not yet on the wire). */
    size_t outboundQueuedBytes();
    void setMessageCallback(MessageCallback callback);
    void setBinaryCallback(BinaryCallback callback);
    void setCloseCallback(CloseCallback callback);
    void setErrorCallback(ErrorCallback callback);
    void setOpenCallback(OpenCallback callback);
    bool close(int code = 1000, const std::string& reason = "Normal closure");
    bool close(CloseCode code, const std::string& reason);
    void enableCompression(bool enable = true);
    void setTLSOptions(const WebSocketTLSOptions& options);
    void setHeaders(const WebSocketHeaders& headers);
    void setPingInterval(int interval);
    void setConnectionTimeout(int timeout);

private:

    static const size_t MAX_QUEUE_SIZE = 1024;

    // Pending queue
    struct Pending {
        enum Type { Text, Binary } type;
        std::string            text;
        std::vector<uint8_t>   bin;

        Pending(std::string&&  t) : type(Text),   text(std::move(t)) {}
        Pending(std::vector<uint8_t>&& b) : type(Binary), bin(std::move(b)) {}
    };
    std::deque<Pending> send_queue;
    std::mutex          send_queue_mutex;
    void flushSendQueue();

    // Connection properties
    std::string host;
    unsigned short port;
    std::string uri;
    bool secure;
    std::string key;
    std::string accept;

    // Connection state
    std::atomic<bool> upgraded;
    std::atomic<bool> running;

    // libevent objects
    event_base* base;
    evdns_base* dns_base;
    bufferevent* m_bev;

    // Thread for event loop
    std::thread* event_thread;
    
    // Mutexes for different purposes
    std::mutex callback_mutex;      // Protects message callback
    std::mutex state_mutex;         // Protects connection state

    // Message handling callback
    MessageCallback message_callback;
    BinaryCallback binary_callback;
    CloseCallback close_callback;
    ErrorCallback error_callback;
    OpenCallback open_callback;

    void sendHandshakeRequest();
    void cleanup();
    void send(evbuffer* buf, const void* data, size_t len, MessageType type = MessageType::TEXT);
    void receive(evbuffer* buf);
    bool isValidUtf8(const char *str, size_t len);

    // Static callbacks - these will be called by libevent
    static void readCallback(bufferevent* bev, void* ctx);
    static void writeCallback(bufferevent* bev, void* ctx);
    static void eventCallback(bufferevent* bev, short events, void* ctx);
    static void pingCallback(evutil_socket_t fd, short event, void *arg);
    static void timeoutCallback(evutil_socket_t fd, short event, void *arg);

    void sendError(int error_code, const std::string& error_message);
    void sendError(ErrorCode code, const std::string& message);
    void sendPing();

    // Member callback implementations
    void handleRead(bufferevent* bev);
    void handleWrite(bufferevent* bev);
    void handleEvent(bufferevent* bev, short events);

    void handleContinuationFrame(unsigned char* data, size_t header_len, uint64_t payload_len, int fin);
    void handleDataFrame(unsigned char* data, size_t header_len, uint64_t payload_len, int fin, int opcode, int rsv1);
    void handleCloseFrame(unsigned char* data, size_t header_len, uint64_t payload_len);
    void handlePingFrame(unsigned char* data, size_t header_len, uint64_t payload_len);
    
    // WebSocket key
    std::array<uint8_t,20> hexToBytes(const std::string &hex);
    std::string getWebSocketKey();
    std::string computeAccept(const std::string &key);

    // Handling fragmented messages
    bool message_in_progress = false;
    bool compressed_message_in_progress = false;
    std::vector<uint8_t> fragmented_message;
    int fragmented_opcode = 0; // Original opcode of the first fragment
    bool decompressMessage(const uint8_t* input, size_t input_len, std::vector<uint8_t>& output);

    // Per-message Deflate
    bool compression_requested = true;
    bool use_compression = false;
    int compression_level = 6; //Z_BEST_SPEED;
    z_stream inflate_stream;
    z_stream deflate_stream;
    bool inflate_initialized = false;
    bool deflate_initialized = false;
    bool initializeCompression();
    bool server_no_context_takeover = false;
    bool client_no_context_takeover = false;
    int  client_max_window_bits = 15;
    int  server_max_window_bits = 15;

    // Connection states
    std::atomic<ConnectionState> connection_state{ConnectionState::DISCONNECTED};
    std::condition_variable state_cv;

#ifdef USE_TLS
    SSL_CTX *ctx = nullptr;
    std::string getOpenSSLError();
    bool configureCiphers();
    bool configureCertificates();
    bool initTLS();
#endif
    WebSocketTLSOptions tlsOptions;
    WebSocketHeaders extraHeaders;
    std::string formatSocketError(int error_code);
    // Ping
    struct event *ping_event = nullptr;
    unsigned int ping_interval = 0;
    // Timeout
    unsigned int connection_timeout = 2;
    struct event *timeout_event = nullptr;
    // Flags
    std::atomic<bool> cleanup_complete{false};
    // Close
    std::atomic<bool> sent_close{false};
    std::atomic<bool> got_remote_close{false};

    Utf8Validator utf8Validator;
};