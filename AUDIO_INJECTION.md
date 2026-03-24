# Audio Injection Feature - mod_audio_stream

## Overview

The audio injection feature allows you to inject audio from the WebSocket into the active call, enabling real-time audio playback during conversations.

## How it works

1. **Enable injection**: Use the API command to enable audio injection for a session
2. **Send audio via WebSocket**: Send audio data through the WebSocket connection
3. **Audio mixing**: The module automatically mixes the injected audio with the call audio
4. **Real-time playback**: The audio is played back in real-time during the call

## API Commands

### Enable Audio Injection
```
uuid_audio_stream <uuid> enable_inject
```

### Disable Audio Injection
```
uuid_audio_stream <uuid> disable_inject
```

## WebSocket Message Format for Audio Injection

Send a JSON message with the following structure:

```json
{
  "type": "injectAudio",
  "data": {
    "audioData": "<base64_encoded_audio_data>",
    "format": "raw",
    "sampleRate": 8000,
    "channels": 1
  }
}
```

### Parameters:

- **type**: Must be "injectAudio"
- **audioData**: Base64 encoded audio data
- **format**: Audio format - currently supports "raw", "wav", "mp3", "ogg"
- **sampleRate**: Sample rate of the audio (8000, 16000, 24000, 32000, 48000, 64000)
- **channels**: Number of audio channels (1 for mono, 2 for stereo)

## Usage Example

### 1. Start audio streaming
```bash
uuid_audio_stream <session_uuid> start wss://your-websocket-server.com/audio mono 8000
```

### 2. Enable audio injection
```bash
uuid_audio_stream <session_uuid> enable_inject
```

### 3. Send audio via WebSocket (JavaScript example)
```javascript
// Convert audio data to base64
const audioData = btoa(String.fromCharCode.apply(null, audioBuffer));

// Create injection message
const message = {
  type: "injectAudio",
  data: {
    audioData: audioData,
    format: "raw",
    sampleRate: 8000,
    channels: 1
  }
};

// Send via WebSocket
websocket.send(JSON.stringify(message));
```

### 4. Disable injection when done
```bash
uuid_audio_stream <session_uuid> disable_inject
```

## Supported Audio Formats

- **Raw PCM**: 16-bit signed PCM audio data
- **WAV**: Standard WAV format
- **MP3**: MPEG Audio Layer III
- **OGG**: Ogg Vorbis format

## Technical Details

### Audio Processing Pipeline

1. **Reception**: Audio data received via WebSocket
2. **Decoding**: Base64 decoded and format validation
3. **Resampling**: Automatic resampling to match call sample rate
4. **Buffering**: Audio buffered for smooth playback
5. **Mixing**: Mixed with call audio in real-time
6. **Playback**: Injected into the call stream

### Performance Considerations

- **Buffer Management**: The module uses circular buffers to minimize latency
- **Thread Safety**: All audio operations are thread-safe
- **Memory Usage**: Automatic cleanup of audio buffers and resources
- **Resampling**: Efficient resampling using Speex DSP library

### Sample Rates and Formats

The module automatically handles resampling between different sample rates:
- Input: Any supported rate (8kHz, 16kHz, 24kHz, 32kHz, 48kHz, 64kHz)
- Output: Matches the call's sample rate automatically

### Error Handling

- Invalid audio data is rejected with appropriate logging
- Buffer overflow protection prevents memory issues  
- Automatic fallback to disable injection on critical errors

## Debug and Monitoring

Enable debug logging to monitor audio injection:

```bash
# Set debug level (0=none, 5=verbose)
uuid_audio_stream debug_level 4
```

Monitor active sessions:
```bash
uuid_audio_stream debug_channels
```

## Integration Examples

### Python Example
```python
import websocket
import base64
import json
import wave

def inject_wav_file(ws, wav_file_path):
    with wave.open(wav_file_path, 'rb') as wav_file:
        audio_data = wav_file.readframes(wav_file.getnframes())
        audio_b64 = base64.b64encode(audio_data).decode('utf-8')
        
        message = {
            "type": "injectAudio",
            "data": {
                "audioData": audio_b64,
                "format": "raw",
                "sampleRate": wav_file.getframerate(),
                "channels": wav_file.getnchannels()
            }
        }
        
        ws.send(json.dumps(message))
```

### Node.js Example
```javascript
const fs = require('fs');
const WebSocket = require('ws');

function injectAudioFile(ws, audioFilePath) {
    const audioBuffer = fs.readFileSync(audioFilePath);
    const audioData = audioBuffer.toString('base64');
    
    const message = {
        type: "injectAudio",
        data: {
            audioData: audioData,
            format: "wav",  // Assuming WAV file
            sampleRate: 8000,
            channels: 1
        }
    };
    
    ws.send(JSON.stringify(message));
}
```

## Limitations

- Maximum buffer size: 8KB per injection
- Supported sample rates: 8kHz to 64kHz  
- Maximum concurrent injections: Limited by system resources
- Audio formats: Limited to PCM, WAV, MP3, OGG

## Troubleshooting

### Common Issues

1. **Audio not playing**: Check if injection is enabled and audio format is supported
2. **Poor audio quality**: Verify sample rate matches or enable automatic resampling
3. **Latency issues**: Reduce buffer sizes and use lower sample rates
4. **Connection drops**: Check WebSocket connection stability

### Debug Commands

```bash
# Check session status
uuid_audio_stream <uuid> status

# View active channels
uuid_audio_stream debug_channels

# Enable verbose logging
uuid_audio_stream debug_level 5
```
