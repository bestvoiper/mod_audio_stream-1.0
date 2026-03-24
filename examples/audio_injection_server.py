#!/usr/bin/env python3
"""
Audio Injection Server Example for mod_audio_stream

This script demonstrates how to create a WebSocket server that can inject
audio into FreeSWITCH calls using the mod_audio_stream module.
"""

import asyncio
import websockets
import json
import base64
import wave
import struct
import math
import argparse
import logging
from pathlib import Path
import aiofiles

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

class AudioInjectionServer:
    def __init__(self, host='localhost', port=8080):
        self.host = host
        self.port = port
        self.clients = set()
        
    async def register_client(self, websocket, path):
        """Register a new WebSocket client"""
        self.clients.add(websocket)
        client_addr = websocket.remote_address
        logger.info(f"Client connected from {client_addr}")
        
        try:
            # Send welcome message
            welcome_msg = {
                "type": "connected",
                "message": "Audio injection server ready"
            }
            await websocket.send(json.dumps(welcome_msg))
            
            # Handle incoming messages
            async for message in websocket:
                await self.handle_message(websocket, message)
                
        except websockets.exceptions.ConnectionClosed:
            logger.info(f"Client {client_addr} disconnected")
        except Exception as e:
            logger.error(f"Error handling client {client_addr}: {e}")
        finally:
            self.clients.discard(websocket)
    
    async def handle_message(self, websocket, message):
        """Handle incoming WebSocket messages"""
        try:
            data = json.loads(message)
            msg_type = data.get('type')
            
            if msg_type == 'requestAudio':
                await self.handle_audio_request(websocket, data)
            elif msg_type == 'getStatus':
                await self.send_status(websocket)
            else:
                logger.warning(f"Unknown message type: {msg_type}")
                
        except json.JSONDecodeError:
            logger.error("Invalid JSON received")
        except Exception as e:
            logger.error(f"Error handling message: {e}")
    
    async def handle_audio_request(self, websocket, data):
        """Handle requests for audio injection"""
        request_type = data.get('requestType')
        
        if request_type == 'tone':
            await self.inject_tone(websocket, data.get('params', {}))
        elif request_type == 'file':
            await self.inject_file(websocket, data.get('params', {}))
        elif request_type == 'tts':
            await self.inject_tts(websocket, data.get('params', {}))
        elif request_type == 'silence':
            await self.inject_silence(websocket, data.get('params', {}))
        else:
            logger.warning(f"Unknown audio request type: {request_type}")
    
    async def inject_tone(self, websocket, params):
        """Generate and inject a tone"""
        frequency = params.get('frequency', 440)
        duration = params.get('duration', 1.0)
        sample_rate = params.get('sampleRate', 8000)
        amplitude = params.get('amplitude', 0.5)
        
        logger.info(f"Generating {frequency}Hz tone for {duration}s")
        
        # Generate tone
        num_samples = int(sample_rate * duration)
        audio_data = bytearray()
        
        for i in range(num_samples):
            t = i / sample_rate
            sample = amplitude * math.sin(2 * math.pi * frequency * t)
            # Convert to 16-bit signed integer
            int_sample = int(sample * 32767)
            audio_data.extend(struct.pack('<h', int_sample))
        
        # Send injection message
        injection_msg = {
            "type": "injectAudio",
            "data": {
                "audioData": base64.b64encode(audio_data).decode('utf-8'),
                "format": "raw",
                "sampleRate": sample_rate,
                "channels": 1
            }
        }
        
        await websocket.send(json.dumps(injection_msg))
        logger.info(f"Injected {len(audio_data)} bytes of tone audio")
    
    async def inject_file(self, websocket, params):
        """Inject audio from a file"""
        file_path = params.get('path')
        if not file_path:
            logger.error("No file path provided")
            return
        
        path = Path(file_path)
        if not path.exists():
            logger.error(f"File not found: {file_path}")
            return
        
        try:
            # Read file
            async with aiofiles.open(path, 'rb') as f:
                audio_data = await f.read()
            
            # Determine format
            format_type = self.get_audio_format(path)
            sample_rate = 8000  # Default, could be detected from file
            
            # For WAV files, try to extract sample rate
            if format_type == 'wav':
                sample_rate = self.get_wav_sample_rate(audio_data)
            
            # Send injection message
            injection_msg = {
                "type": "injectAudio",
                "data": {
                    "audioData": base64.b64encode(audio_data).decode('utf-8'),
                    "format": format_type,
                    "sampleRate": sample_rate,
                    "channels": 1
                }
            }
            
            await websocket.send(json.dumps(injection_msg))
            logger.info(f"Injected audio file: {file_path} ({len(audio_data)} bytes)")
            
        except Exception as e:
            logger.error(f"Error injecting file {file_path}: {e}")
    
    async def inject_tts(self, websocket, params):
        """Inject text-to-speech audio (placeholder implementation)"""
        text = params.get('text', '')
        if not text:
            logger.error("No text provided for TTS")
            return
        
        logger.info(f"TTS request for: '{text}'")
        
        # This is a placeholder - in a real implementation, you would:
        # 1. Call a TTS service (Google TTS, Amazon Polly, etc.)
        # 2. Get the audio data back
        # 3. Inject it as shown below
        
        # For now, just inject a beep as a placeholder
        await self.inject_tone(websocket, {'frequency': 800, 'duration': 0.5})
        
        logger.warning("TTS not implemented - injected beep instead")
    
    async def inject_silence(self, websocket, params):
        """Inject silence (useful for creating pauses)"""
        duration = params.get('duration', 1.0)
        sample_rate = params.get('sampleRate', 8000)
        
        logger.info(f"Injecting {duration}s of silence")
        
        # Generate silence
        num_samples = int(sample_rate * duration)
        audio_data = bytearray(num_samples * 2)  # 16-bit samples (2 bytes each) filled with zeros
        
        # Send injection message
        injection_msg = {
            "type": "injectAudio",
            "data": {
                "audioData": base64.b64encode(audio_data).decode('utf-8'),
                "format": "raw",
                "sampleRate": sample_rate,
                "channels": 1
            }
        }
        
        await websocket.send(json.dumps(injection_msg))
        logger.info(f"Injected {duration}s of silence")
    
    def get_audio_format(self, path):
        """Determine audio format from file extension"""
        extension = path.suffix.lower()
        format_map = {
            '.wav': 'wav',
            '.mp3': 'mp3',
            '.ogg': 'ogg',
            '.raw': 'raw'
        }
        return format_map.get(extension, 'raw')
    
    def get_wav_sample_rate(self, wav_data):
        """Extract sample rate from WAV file header"""
        try:
            # WAV sample rate is at bytes 24-27 (little-endian)
            if len(wav_data) >= 28 and wav_data[:4] == b'RIFF':
                sample_rate = struct.unpack('<I', wav_data[24:28])[0]
                return sample_rate
        except:
            pass
        return 8000  # Default fallback
    
    async def send_status(self, websocket):
        """Send server status"""
        status_msg = {
            "type": "status",
            "data": {
                "connectedClients": len(self.clients),
                "serverHost": self.host,
                "serverPort": self.port
            }
        }
        await websocket.send(json.dumps(status_msg))
    
    async def broadcast_message(self, message):
        """Broadcast a message to all connected clients"""
        if self.clients:
            await asyncio.gather(
                *[client.send(json.dumps(message)) for client in self.clients],
                return_exceptions=True
            )
    
    def start_server(self):
        """Start the WebSocket server"""
        logger.info(f"Starting audio injection server on {self.host}:{self.port}")
        return websockets.serve(self.register_client, self.host, self.port)

class AudioInjectionDemo:
    """Demo class showing various audio injection scenarios"""
    
    def __init__(self, websocket_url):
        self.websocket_url = websocket_url
    
    async def run_demo(self):
        """Run various audio injection demonstrations"""
        try:
            async with websockets.connect(self.websocket_url) as websocket:
                logger.info(f"Connected to {self.websocket_url}")
                
                # Demo 1: Inject welcome tone
                await self.demo_welcome_tone(websocket)
                await asyncio.sleep(2)
                
                # Demo 2: Inject file (if exists)
                await self.demo_file_injection(websocket)
                await asyncio.sleep(2)
                
                # Demo 3: DTMF sequence
                await self.demo_dtmf_sequence(websocket)
                await asyncio.sleep(2)
                
                # Demo 4: Silence injection
                await self.demo_silence(websocket)
                
                logger.info("Demo completed")
                
        except Exception as e:
            logger.error(f"Demo error: {e}")
    
    async def demo_welcome_tone(self, websocket):
        """Demo: Welcome tone sequence"""
        logger.info("Demo: Playing welcome tone sequence")
        
        # Play ascending tones
        frequencies = [440, 523, 659, 784]  # A, C, E, G
        for freq in frequencies:
            await self.inject_tone(websocket, freq, 0.3)
            await asyncio.sleep(0.1)
    
    async def demo_file_injection(self, websocket):
        """Demo: File injection"""
        test_file = Path("test_audio.wav")
        if test_file.exists():
            logger.info(f"Demo: Injecting audio file {test_file}")
            await self.inject_file(websocket, str(test_file))
        else:
            logger.info("Demo: No test audio file found, skipping file injection demo")
    
    async def demo_dtmf_sequence(self, websocket):
        """Demo: DTMF digit sequence"""
        logger.info("Demo: Playing DTMF sequence: 1-2-3-4")
        
        # DTMF frequencies (simplified to single tone)
        dtmf_tones = {
            '1': 697, '2': 770, '3': 852,
            '4': 941, '5': 697, '6': 770,
            '7': 852, '8': 941, '9': 697,
            '0': 941
        }
        
        for digit in "1234":
            frequency = dtmf_tones[digit]
            await self.inject_tone(websocket, frequency, 0.2)
            await asyncio.sleep(0.1)
    
    async def demo_silence(self, websocket):
        """Demo: Silence injection"""
        logger.info("Demo: Injecting 1 second of silence")
        await self.inject_silence(websocket, 1.0)
    
    async def inject_tone(self, websocket, frequency, duration, sample_rate=8000):
        """Inject a tone"""
        num_samples = int(sample_rate * duration)
        audio_data = bytearray()
        
        for i in range(num_samples):
            t = i / sample_rate
            sample = 0.5 * math.sin(2 * math.pi * frequency * t)
            int_sample = int(sample * 32767)
            audio_data.extend(struct.pack('<h', int_sample))
        
        injection_msg = {
            "type": "injectAudio",
            "data": {
                "audioData": base64.b64encode(audio_data).decode('utf-8'),
                "format": "raw",
                "sampleRate": sample_rate,
                "channels": 1
            }
        }
        
        await websocket.send(json.dumps(injection_msg))
    
    async def inject_file(self, websocket, file_path):
        """Inject audio from file"""
        path = Path(file_path)
        if not path.exists():
            logger.error(f"File not found: {file_path}")
            return
        
        with open(path, 'rb') as f:
            audio_data = f.read()
        
        injection_msg = {
            "type": "injectAudio",
            "data": {
                "audioData": base64.b64encode(audio_data).decode('utf-8'),
                "format": "wav",
                "sampleRate": 8000,
                "channels": 1
            }
        }
        
        await websocket.send(json.dumps(injection_msg))
    
    async def inject_silence(self, websocket, duration, sample_rate=8000):
        """Inject silence"""
        num_samples = int(sample_rate * duration)
        audio_data = bytearray(num_samples * 2)
        
        injection_msg = {
            "type": "injectAudio",
            "data": {
                "audioData": base64.b64encode(audio_data).decode('utf-8'),
                "format": "raw",
                "sampleRate": sample_rate,
                "channels": 1
            }
        }
        
        await websocket.send(json.dumps(injection_msg))

def main():
    parser = argparse.ArgumentParser(description='Audio Injection Server/Demo for mod_audio_stream')
    parser.add_argument('--mode', choices=['server', 'demo'], default='server',
                       help='Run as server or demo client')
    parser.add_argument('--host', default='localhost', help='Server host')
    parser.add_argument('--port', type=int, default=8080, help='Server port')
    parser.add_argument('--url', help='WebSocket URL for demo mode')
    
    args = parser.parse_args()
    
    if args.mode == 'server':
        # Run server
        server = AudioInjectionServer(args.host, args.port)
        start_server = server.start_server()
        
        logger.info("Audio injection server starting...")
        logger.info("Available injection types:")
        logger.info("  - tone: Generate tones")
        logger.info("  - file: Inject audio files")
        logger.info("  - tts: Text-to-speech (placeholder)")
        logger.info("  - silence: Inject silence")
        logger.info("Press Ctrl+C to stop")
        
        try:
            asyncio.get_event_loop().run_until_complete(start_server)
            asyncio.get_event_loop().run_forever()
        except KeyboardInterrupt:
            logger.info("Server stopped")
    
    elif args.mode == 'demo':
        # Run demo
        if not args.url:
            args.url = f"ws://{args.host}:{args.port}"
        
        demo = AudioInjectionDemo(args.url)
        logger.info(f"Running audio injection demo against {args.url}")
        
        try:
            asyncio.get_event_loop().run_until_complete(demo.run_demo())
        except KeyboardInterrupt:
            logger.info("Demo stopped")

if __name__ == '__main__':
    main()
