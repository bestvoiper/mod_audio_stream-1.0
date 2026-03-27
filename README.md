# mod_audio_stream

A FreeSWITCH module that streams L16 audio from a channel to a websocket endpoint. If websocket sends back responses (eg. JSON) it can be effectively used with ASR engines such as IBM Watson etc., or any other purpose you find applicable.

#### About

- The purpose of `mod_audio_stream` was to make a simple, less dependent but yet effective module to stream audio and receive responses from websocket server. It uses [ixwebsocket](https://machinezone.github.io/IXWebSocket/), c++ library for websocket protocol which is compiled as a static library.
- This module was inspired by [mod_audio_fork](https://github.com/drachtio/drachtio-freeswitch-modules/tree/main/modules/mod_audio_fork).

## Installation

### Dependencies
It requires `libfreeswitch-dev`, `libssl-dev`, `zlib1g-dev` and `libspeexdsp-dev` on Debian/Ubuntu which are regular packages for Freeswitch installation.
### Building
After cloning please execute: **git submodule init** and **git submodule update** to initialize the submodule.
#### Custom path
If you built FreeSWITCH from source, eq. install dir is /usr/local/freeswitch, add path to pkgconfig:
```
export PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig
```
To build the module, from the cloned repository directory:
```
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
sudo make install
```

## Scripted Build & Installation

```
sudo apt-get -y install git \
    && cd /usr/src/ \
    && git clone https://github.com/bestvoiper/mod_audio_stream-1.0.git \
    && cd mod_audio_stream \
    && sudo bash ./build-mod-audio-stream.sh
```

### Channel variables
The following channel variables can be used to fine tune websocket connection and also configure mod_audio_stream logging:

| Variable               | Description                                         | Default |
|------------------------|-----------------------------------------------------|---------|
| STREAM_MESSAGE_DEFLATE | true or 1, disables per message deflate             | off     |
| STREAM_HEART_BEAT      | number of seconds, interval to send the heart beat  | off     |
| STREAM_SUPPRESS_LOG    | true or 1, suppresses printing to log               | off     |
| STREAM_BUFFER_SIZE     | buffer duration in milliseconds, divisible by 20    | 20      |
| STREAM_EXTRA_HEADERS   | JSON object for additional headers in string format | none    |
| STREAM_NO_RECONNECT    | true or 1, disables automatic websocket reconnection| off     |

- Per message deflate compression option is enabled by default. It can lead to a very nice bandwidth savings. To disable it set the channel var to `true|1`.
- Heart beat, sent every xx seconds when there is no traffic to make sure that load balancers do not kill an idle connection.
- Suppress parameter is omitted by default(false). All the responses from websocket server will be printed to the log. Not to flood the log you can suppress it by setting the value to `true|1`. Events are fired still, it only affects printing to the log.
- `Buffer Size` actually represents a duration of audio chunk sent to websocket. If you want to send e.g. 100ms audio packets to your ws endpoint
you would set this variable to 100. If ommited, default packet size of 20ms will be sent as grabbed from the audio channel (which is default FreeSWITCH frame size)
- Extra headers should be a JSON object with key-value pairs representing additional HTTP headers. Each key should be a header name, and its corresponding value should be a string.
  ```json
  {
      "Header1": "Value1",
      "Header2": "Value2",
      "Header3": "Value3"
  }
- Websocket automatic reconnection is on by default. To disable it set this channel variable to true or 1.

## API

### Commands
The freeswitch module exposes the following API commands:

```
uuid_audio_stream <uuid> start <wss-url> <mix-type> <sampling-rate> <metadata>
```
Attaches a media bug and starts streaming audio (in L16 format) to the websocket server. FS default is 8k. If sampling-rate is other than 8k it will be resampled.
- `uuid` - Freeswitch channel unique id
- `wss-url` - websocket url `ws://` or `wss://`
- `mix-type` - See [Audio Mix Modes](#audio-mix-modes) section below
- `sampling-rate` - choice of
  - "8k" = 8000 Hz sample rate will be generated
  - "16k" = 16000 Hz sample rate will be generated
- `metadata` - (optional) a valid `utf-8` text to send. It will be sent the first before audio streaming starts.

```
uuid_audio_stream <uuid> send_text <metadata>
```
Sends a text to the websocket server. Requires a valid `utf-8` text.

```
uuid_audio_stream <uuid> stop <metadata>
```
Stops audio stream and closes websocket connection. If _metadata_ is provided it will be sent before the connection is closed.

```
uuid_audio_stream <uuid> pause
```
Pauses audio stream

```
uuid_audio_stream <uuid> resume
```
Resumes audio stream

```
uuid_audio_stream <uuid> enable_inject
```
Enables audio injection - allows receiving audio from WebSocket to play in the call

```
uuid_audio_stream <uuid> disable_inject
```
Disables audio injection

## Usage Examples

### fs_cli Commands

```bash
# Start streaming with mono (only caller audio)
fs_cli -x "uuid_audio_stream abc123-uuid start wss://server.com/audio mono 16000"

# Start streaming with enhanced mixed (both parties, better quality)
fs_cli -x "uuid_audio_stream abc123-uuid start wss://server.com/audio enhanced_mixed 16000"

# Start with initial metadata (JSON)
fs_cli -x "uuid_audio_stream abc123-uuid start wss://server.com/audio enhanced_mixed 16000 '{\"caller\":\"5551234567\",\"agent\":\"John\"}'"

# Pause streaming
fs_cli -x "uuid_audio_stream abc123-uuid pause"

# Resume streaming
fs_cli -x "uuid_audio_stream abc123-uuid resume"

# Send text message to WebSocket
fs_cli -x "uuid_audio_stream abc123-uuid send_text '{\"action\":\"end_conversation\"}'"

# Enable audio injection
fs_cli -x "uuid_audio_stream abc123-uuid enable_inject"

# Disable audio injection
fs_cli -x "uuid_audio_stream abc123-uuid disable_inject"

# Stop with final message
fs_cli -x "uuid_audio_stream abc123-uuid stop '{\"reason\":\"call_ended\"}'"
```

### Dialplan XML

```xml
<!-- Basic streaming on answer -->
<extension name="audio_stream_example">
  <condition field="destination_number" expression="^1234$">
    <action application="answer"/>
    <action application="set" data="STREAM_BUFFER_SIZE=100"/>
    <action application="set" data="STREAM_HEART_BEAT=30"/>
    
    <!-- Start streaming with enhanced mixed audio -->
    <action application="system" data="fs_cli -x 'uuid_audio_stream ${uuid} start wss://your-server.com/audio enhanced_mixed 16000'"/>
    
    <action application="playback" data="ivr/ivr-welcome.wav"/>
    <action application="park"/>
  </condition>
</extension>

<!-- Streaming during a bridged call -->
<extension name="stream_bridge_call">
  <condition field="destination_number" expression="^(\d{10})$">
    <action application="answer"/>
    <action application="set" data="STREAM_BUFFER_SIZE=100"/>
    
    <!-- Stop streaming after bridge ends -->
    <action application="set" data="EXEC_AFTER_BRIDGE_APP=system"/>
    <action application="set" data="EXEC_AFTER_BRIDGE_ARG=fs_cli -x 'uuid_audio_stream ${uuid} stop'"/>
    
    <!-- Start streaming before bridge -->
    <action application="system" data="fs_cli -x 'uuid_audio_stream ${uuid} start wss://asr.server.com/ws enhanced_mixed 16000'"/>
    
    <action application="bridge" data="sofia/gateway/my_gateway/$1"/>
  </condition>
</extension>

<!-- With authentication headers -->
<extension name="stream_with_auth">
  <condition field="destination_number" expression="^5678$">
    <action application="answer"/>
    <action application="set" data="STREAM_EXTRA_HEADERS={\"Authorization\": \"Bearer your_token_here\"}"/>
    <action application="system" data="fs_cli -x 'uuid_audio_stream ${uuid} start wss://secure-server.com/audio enhanced_mixed 16000'"/>
    <action application="park"/>
  </condition>
</extension>
```

### Lua Script

```lua
-- /usr/share/freeswitch/scripts/audio_stream.lua

local uuid = session:getVariable("uuid")
local wss_url = "wss://your-server.com/audio"

-- Configure channel variables
session:setVariable("STREAM_BUFFER_SIZE", "100")
session:setVariable("STREAM_HEART_BEAT", "30")
session:setVariable("STREAM_SUPPRESS_LOG", "false")

-- Answer the call
session:answer()

-- Start streaming with enhanced mixed audio
local api = freeswitch.API()
api:execute("uuid_audio_stream", uuid .. " start " .. wss_url .. " enhanced_mixed 16000")

-- Play welcome message
session:streamFile("ivr/ivr-welcome.wav")

-- Example: Pause streaming
api:execute("uuid_audio_stream", uuid .. " pause")

-- Example: Resume streaming
api:execute("uuid_audio_stream", uuid .. " resume")

-- Example: Send text to WebSocket
api:execute("uuid_audio_stream", uuid .. " send_text {\"action\":\"transcript_ready\"}")

-- Example: Enable audio injection
api:execute("uuid_audio_stream", uuid .. " enable_inject")

-- When finished, stop streaming
api:execute("uuid_audio_stream", uuid .. " stop")
```

### Python ESL

```python
#!/usr/bin/env python3
import ESL

# Connect to FreeSWITCH
con = ESL.ESLconnection("127.0.0.1", "8021", "ClueCon")

if con.connected():
    uuid = "abc123-def456-789"  # Call UUID
    wss_url = "wss://your-server.com/audio"
    
    # Start streaming with enhanced_mixed
    result = con.api("uuid_audio_stream", f"{uuid} start {wss_url} enhanced_mixed 16000")
    print(f"Start: {result.getBody()}")
    
    # Pause
    result = con.api("uuid_audio_stream", f"{uuid} pause")
    print(f"Pause: {result.getBody()}")
    
    # Resume
    result = con.api("uuid_audio_stream", f"{uuid} resume")
    print(f"Resume: {result.getBody()}")
    
    # Send text
    result = con.api("uuid_audio_stream", f'{uuid} send_text {{"event":"user_speaking"}}')
    print(f"Send text: {result.getBody()}")
    
    # Enable injection
    result = con.api("uuid_audio_stream", f"{uuid} enable_inject")
    print(f"Enable inject: {result.getBody()}")
    
    # Stop
    result = con.api("uuid_audio_stream", f"{uuid} stop")
    print(f"Stop: {result.getBody()}")
```

## Audio Mix Modes

The module supports 4 different audio mixing modes. Choose the one that best fits your use case:

### 1. MONO Mode

**Command:**
```bash
uuid_audio_stream ${uuid} start wss://server.com/audio mono 16000
```

**Description:**
Captures only the **caller's audio** (incoming audio from the person who initiated the call).

**Audio Flow:**
```
┌─────────────┐                    ┌─────────────┐
│   Caller    │ ──── Audio ────▶   │  WebSocket  │
│  (Customer) │                    │   Server    │
└─────────────┘                    └─────────────┘
      ▲
      │ (callee audio NOT captured)
      │
┌─────────────┐
│   Callee    │
│   (Agent)   │
└─────────────┘
```

**Output Format:**
- Channels: 1 (mono)
- Format: L16 (16-bit signed PCM)

**Use Cases:**
- ASR/Speech-to-Text for customer only
- Voice biometrics on caller
- IVR voice commands
- When you only need one side of conversation

---

### 2. MIXED Mode

**Command:**
```bash
uuid_audio_stream ${uuid} start wss://server.com/audio mixed 16000
```

**Description:**
Captures **both caller and callee audio** mixed together into a single channel using FreeSWITCH's standard mixing algorithm.

**Audio Flow:**
```
┌─────────────┐
│   Caller    │ ──┐
│  (Customer) │   │
└─────────────┘   │    ┌──────────┐    ┌─────────────┐
                  ├──▶ │  MIXER   │──▶ │  WebSocket  │
┌─────────────┐   │    │(standard)│    │   Server    │
│   Callee    │ ──┘    └──────────┘    └─────────────┘
│   (Agent)   │
└─────────────┘
```

**Output Format:**
- Channels: 1 (mono - both parties combined)
- Format: L16 (16-bit signed PCM)

**Use Cases:**
- Basic call recording
- Conversation transcription
- Quality assurance monitoring
- When you need full conversation but don't need parties separated

**Note:** Standard mixing may cause slight quality loss when both parties speak simultaneously.

---

### 3. STEREO Mode

**Command:**
```bash
uuid_audio_stream ${uuid} start wss://server.com/audio stereo 16000
```

**Description:**
Captures **both parties in separate channels** - caller in left channel, callee in right channel.

**Audio Flow:**
```
┌─────────────┐                         ┌─────────────┐
│   Caller    │ ──▶ LEFT CHANNEL  ──┐   │  WebSocket  │
│  (Customer) │                     ├──▶│   Server    │
└─────────────┘                     │   │             │
                                    │   │ [L] [R]     │
┌─────────────┐                     │   │  ▲   ▲      │
│   Callee    │ ──▶ RIGHT CHANNEL ──┘   │  │   │      │
│   (Agent)   │                         └──┼───┼──────┘
└─────────────┘                            │   │
                                      Caller  Callee
```

**Output Format:**
- Channels: 2 (stereo - interleaved L/R)
- Format: L16 (16-bit signed PCM)
- Sample order: [Left, Right, Left, Right, ...]

**Use Cases:**
- Separate ASR for each party
- Speaker diarization
- Individual voice analysis
- When you need to process each party independently
- Training ML models with labeled speaker data

**Processing Example (Python):**
```python
import numpy as np

# Received stereo audio (16-bit PCM)
stereo_audio = np.frombuffer(audio_data, dtype=np.int16)

# Split channels
caller_audio = stereo_audio[0::2]   # Left channel (even samples)
callee_audio = stereo_audio[1::2]   # Right channel (odd samples)
```

---

### 4. ENHANCED_MIXED Mode

**Command:**
```bash
uuid_audio_stream ${uuid} start wss://server.com/audio enhanced_mixed 16000
```

**Description:**
Captures **both parties with an improved mixing algorithm** that includes Automatic Gain Control (AGC) and soft clipping prevention for better audio quality.

**Audio Flow:**
```
┌─────────────┐
│   Caller    │ ──┐
│  (Customer) │   │
└─────────────┘   │    ┌──────────────┐    ┌─────────────┐
                  ├──▶ │ ENHANCED     │──▶ │  WebSocket  │
┌─────────────┐   │    │ MIXER + AGC  │    │   Server    │
│   Callee    │ ──┘    │ (0.85 gain)  │    └─────────────┘
│   (Agent)   │        └──────────────┘
└─────────────┘
```

**Output Format:**
- Channels: 1 (mono - both parties combined)
- Format: L16 (16-bit signed PCM)

**Features:**
- **AGC (Automatic Gain Control):** 0.85 gain factor prevents distortion
- **Soft Clipping:** Prevents audio saturation when both speak loudly
- **Better Dynamic Range:** Preserves audio quality during overlapping speech
- **No Distortion:** Eliminates crackling/popping when audio peaks

**Use Cases:**
- **Best choice for:** High-quality call recording
- Real-time transcription services (Google, AWS, Azure)
- AI conversation analysis
- Customer service quality monitoring
- When audio quality is critical

**Comparison with standard `mixed`:**
| Aspect | mixed | enhanced_mixed |
|--------|-------|----------------|
| Quality during overlap | May clip | No clipping |
| Volume consistency | Variable | Normalized |
| CPU usage | Lower | Slightly higher |
| Recommended for ASR | Good | **Best** |

---

### Quick Reference Table

| Mode | Parties | Channels | Quality | Best For |
|------|---------|----------|---------|----------|
| `mono` | Caller only | 1 | ★★★★★ | Single-party ASR, voice auth |
| `mixed` | Both | 1 | ★★★☆☆ | Basic recording |
| `stereo` | Both (separate) | 2 | ★★★★★ | Speaker separation, ML training |
| `enhanced_mixed` | Both | 1 | ★★★★★ | **Best for ASR, quality recording** |

### Choosing the Right Mode

```
Need only caller audio?
  └─▶ Use: mono

Need both parties?
  ├─▶ Need them separated? 
  │     └─▶ Use: stereo
  │
  └─▶ Need them mixed?
        ├─▶ Quality is critical?
        │     └─▶ Use: enhanced_mixed ⭐
        │
        └─▶ Basic recording?
              └─▶ Use: mixed
```

## Events
Module will generate the following event types:
- `mod_audio_stream::json`
- `mod_audio_stream::connect`
- `mod_audio_stream::disconnect`
- `mod_audio_stream::error`
- `mod_audio_stream::play`

### response
Message received from websocket endpoint. Json expected, but it contains whatever the websocket server's response is.
#### Freeswitch event generated
**Name**: mod_audio_stream::json
**Body**: WebSocket server response

### connect
Successfully connected to websocket server.
#### Freeswitch event generated
**Name**: mod_audio_stream::connect
**Body**: JSON
```json
{
	"status": "connected"
}
```

### disconnect
Disconnected from websocket server.
#### Freeswitch event generated
**Name**: mod_audio_stream::disconnect
**Body**: JSON
```json
{
	"status": "disconnected",
	"message": {
		"code": 1000,
		"reason": "Normal closure"
	}
}
```
- code: `<int>`
- reason: `<string>`

### error
There is an error with the connection. Multiple fields will be available on the event to describe the error.
#### Freeswitch event generated
**Name**: mod_audio_stream::error
**Body**: JSON
```json
{
	"status": "error",
	"message": {
		"retries": 1,
		"error": "Expecting status 101 (Switching Protocol), got 403 status connecting to wss://localhost, HTTP Status line: HTTP/1.1 403 Forbidden\r\n",
		"wait_time": 100,
		"http_status": 403
	}
}
```
- retries: `<int>`, error: `<string>`, wait_time: `<int>`, http_status: `<int>`

### play
**Name**: mod_audio_stream::play
**Body**: JSON

Websocket server may return JSON object containing base64 encoded audio to be played by the user. To use this feature, response must follow the format:
```json
{
  "type": "streamAudio",
  "data": {
    "audioDataType": "raw",
    "sampleRate": 8000,
    "audioData": "base64 encoded audio"
  }
}
```
- audioDataType: `<raw|wav|mp3|ogg>`

Event generated by the module (subclass: _mod_audio_stream::play_) will be the same as the `data` element with the **file** added to it representing filePath:
```json
{
  "audioDataType": "raw",
  "sampleRate": 8000,
  "file": "/path/to/the/file"
}
```
If printing to the log is not suppressed, `response` printed to the console will look the same as the event. The original response containing base64 encoded audio is replaced because it can be quite huge.

All the files generated by this feature will reside at the temp directory and will be deleted when the session is closed.
