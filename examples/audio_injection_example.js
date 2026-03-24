/**
 * Audio Injection Example for mod_audio_stream
 * 
 * This example demonstrates how to inject audio into a FreeSWITCH call
 * using the WebSocket connection from mod_audio_stream.
 */

class AudioInjector {
    constructor(websocketUrl) {
        this.websocket = null;
        this.isConnected = false;
        this.websocketUrl = websocketUrl;
        this.audioContext = null;
        this.setupWebSocket();
    }

    setupWebSocket() {
        this.websocket = new WebSocket(this.websocketUrl);
        
        this.websocket.onopen = () => {
            console.log('WebSocket connected');
            this.isConnected = true;
        };
        
        this.websocket.onclose = () => {
            console.log('WebSocket disconnected');
            this.isConnected = false;
        };
        
        this.websocket.onerror = (error) => {
            console.error('WebSocket error:', error);
        };
        
        this.websocket.onmessage = (event) => {
            try {
                const message = JSON.parse(event.data);
                this.handleMessage(message);
            } catch (e) {
                console.error('Failed to parse message:', e);
            }
        };
    }

    handleMessage(message) {
        console.log('Received message:', message);
        
        // Handle different message types
        switch (message.type) {
            case 'connected':
                console.log('Audio stream connected');
                break;
            case 'error':
                console.error('Audio stream error:', message.data);
                break;
            default:
                console.log('Unknown message type:', message.type);
        }
    }

    /**
     * Inject raw PCM audio data
     * @param {ArrayBuffer} audioBuffer - Raw PCM audio data
     * @param {number} sampleRate - Sample rate (8000, 16000, etc.)
     * @param {number} channels - Number of channels (1 or 2)
     */
    injectRawAudio(audioBuffer, sampleRate = 8000, channels = 1) {
        if (!this.isConnected) {
            console.error('WebSocket not connected');
            return;
        }

        // Convert ArrayBuffer to base64
        const uint8Array = new Uint8Array(audioBuffer);
        const binaryString = String.fromCharCode.apply(null, uint8Array);
        const audioData = btoa(binaryString);

        const message = {
            type: "injectAudio",
            data: {
                audioData: audioData,
                format: "raw",
                sampleRate: sampleRate,
                channels: channels
            }
        };

        this.websocket.send(JSON.stringify(message));
        console.log(`Injected ${audioBuffer.byteLength} bytes of raw audio`);
    }

    /**
     * Inject audio from a file
     * @param {File} file - Audio file (WAV, MP3, OGG)
     */
    async injectAudioFile(file) {
        if (!this.isConnected) {
            console.error('WebSocket not connected');
            return;
        }

        try {
            const arrayBuffer = await file.arrayBuffer();
            const base64Audio = btoa(String.fromCharCode.apply(null, new Uint8Array(arrayBuffer)));
            
            // Determine format from file extension
            const format = this.getFormatFromFilename(file.name);
            
            const message = {
                type: "injectAudio",
                data: {
                    audioData: base64Audio,
                    format: format,
                    sampleRate: 8000, // Will be auto-detected for some formats
                    channels: 1
                }
            };

            this.websocket.send(JSON.stringify(message));
            console.log(`Injected audio file: ${file.name} (${arrayBuffer.byteLength} bytes)`);
            
        } catch (error) {
            console.error('Error injecting audio file:', error);
        }
    }

    /**
     * Inject audio from a URL
     * @param {string} audioUrl - URL to audio file
     */
    async injectAudioFromUrl(audioUrl) {
        if (!this.isConnected) {
            console.error('WebSocket not connected');
            return;
        }

        try {
            const response = await fetch(audioUrl);
            const arrayBuffer = await response.arrayBuffer();
            const base64Audio = btoa(String.fromCharCode.apply(null, new Uint8Array(arrayBuffer)));
            
            const format = this.getFormatFromUrl(audioUrl);
            
            const message = {
                type: "injectAudio",
                data: {
                    audioData: base64Audio,
                    format: format,
                    sampleRate: 8000,
                    channels: 1
                }
            };

            this.websocket.send(JSON.stringify(message));
            console.log(`Injected audio from URL: ${audioUrl}`);
            
        } catch (error) {
            console.error('Error injecting audio from URL:', error);
        }
    }

    /**
     * Record and inject audio from microphone
     * @param {number} duration - Recording duration in milliseconds
     */
    async recordAndInject(duration = 5000) {
        if (!this.isConnected) {
            console.error('WebSocket not connected');
            return;
        }

        try {
            // Initialize audio context if needed
            if (!this.audioContext) {
                this.audioContext = new (window.AudioContext || window.webkitAudioContext)();
            }

            // Get microphone access
            const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
            const mediaRecorder = new MediaRecorder(stream);
            const audioChunks = [];

            mediaRecorder.ondataavailable = (event) => {
                audioChunks.push(event.data);
            };

            mediaRecorder.onstop = async () => {
                const audioBlob = new Blob(audioChunks, { type: 'audio/wav' });
                const arrayBuffer = await audioBlob.arrayBuffer();
                
                // Convert to raw PCM if needed or send as WAV
                await this.injectRawAudio(arrayBuffer, 44100, 1);
                
                // Stop microphone
                stream.getTracks().forEach(track => track.stop());
            };

            // Start recording
            mediaRecorder.start();
            console.log(`Recording for ${duration}ms...`);

            // Stop after duration
            setTimeout(() => {
                mediaRecorder.stop();
                console.log('Recording stopped');
            }, duration);

        } catch (error) {
            console.error('Error recording audio:', error);
        }
    }

    /**
     * Generate and inject a tone
     * @param {number} frequency - Tone frequency in Hz
     * @param {number} duration - Duration in seconds
     * @param {number} sampleRate - Sample rate
     */
    injectTone(frequency = 440, duration = 1, sampleRate = 8000) {
        if (!this.isConnected) {
            console.error('WebSocket not connected');
            return;
        }

        const samples = sampleRate * duration;
        const audioBuffer = new ArrayBuffer(samples * 2); // 16-bit samples
        const dataView = new DataView(audioBuffer);
        
        for (let i = 0; i < samples; i++) {
            const sample = Math.sin(2 * Math.PI * frequency * i / sampleRate) * 0.5;
            const intSample = Math.round(sample * 32767);
            dataView.setInt16(i * 2, intSample, true); // little-endian
        }

        this.injectRawAudio(audioBuffer, sampleRate, 1);
        console.log(`Injected ${frequency}Hz tone for ${duration}s`);
    }

    /**
     * Inject text-to-speech audio (requires external TTS service)
     * @param {string} text - Text to convert to speech
     * @param {string} ttsUrl - TTS service URL
     */
    async injectTTS(text, ttsUrl) {
        if (!this.isConnected) {
            console.error('WebSocket not connected');
            return;
        }

        try {
            const response = await fetch(ttsUrl, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify({
                    text: text,
                    format: 'wav',
                    sampleRate: 8000
                })
            });

            if (!response.ok) {
                throw new Error(`TTS request failed: ${response.status}`);
            }

            const arrayBuffer = await response.arrayBuffer();
            const base64Audio = btoa(String.fromCharCode.apply(null, new Uint8Array(arrayBuffer)));
            
            const message = {
                type: "injectAudio",
                data: {
                    audioData: base64Audio,
                    format: "wav",
                    sampleRate: 8000,
                    channels: 1
                }
            };

            this.websocket.send(JSON.stringify(message));
            console.log(`Injected TTS audio for text: "${text}"`);
            
        } catch (error) {
            console.error('Error with TTS injection:', error);
        }
    }

    getFormatFromFilename(filename) {
        const extension = filename.split('.').pop().toLowerCase();
        switch (extension) {
            case 'wav': return 'wav';
            case 'mp3': return 'mp3';
            case 'ogg': return 'ogg';
            default: return 'raw';
        }
    }

    getFormatFromUrl(url) {
        return this.getFormatFromFilename(url);
    }

    disconnect() {
        if (this.websocket) {
            this.websocket.close();
        }
    }
}

// Usage Examples
const injector = new AudioInjector('wss://your-freeswitch-server.com/audio');

// Example 1: Inject a tone
document.getElementById('injectTone').addEventListener('click', () => {
    injector.injectTone(440, 2); // 440Hz tone for 2 seconds
});

// Example 2: Inject file from input
document.getElementById('audioFile').addEventListener('change', (event) => {
    const file = event.target.files[0];
    if (file) {
        injector.injectAudioFile(file);
    }
});

// Example 3: Record and inject
document.getElementById('recordAndInject').addEventListener('click', () => {
    injector.recordAndInject(3000); // Record 3 seconds
});

// Example 4: Inject from URL
document.getElementById('injectFromUrl').addEventListener('click', () => {
    const url = prompt('Enter audio file URL:');
    if (url) {
        injector.injectAudioFromUrl(url);
    }
});

// Example 5: TTS injection (requires TTS service)
document.getElementById('injectTTS').addEventListener('click', () => {
    const text = prompt('Enter text to speak:');
    if (text) {
        injector.injectTTS(text, 'https://your-tts-service.com/synthesize');
    }
});

// Export for use in other modules
if (typeof module !== 'undefined' && module.exports) {
    module.exports = AudioInjector;
}
