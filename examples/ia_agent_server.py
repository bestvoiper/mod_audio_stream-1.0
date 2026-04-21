#!/usr/bin/env python3
"""
Servidor WebSocket para Agente IA con mod_audio_stream
Características:
- Recibe audio del caller en tiempo real
- Inyecta audio TTS al caller
- Soporta barge-in (interrumpir TTS cuando caller habla)
- Flujo bidireccional continuo

Uso:
    pip install websockets numpy aiohttp
    python ia_agent_server.py

FreeSWITCH dialplan:
    <action application="set" data="stream_result=${uuid_audio_stream(${uuid} start ws://IP:5012 mono 16000)}"/>
    <action application="set" data="inject_result=${uuid_audio_stream(${uuid} enable_inject)}"/>
    <action application="park"/>
"""

import asyncio
import websockets
import json
import base64
import wave
import numpy as np
from datetime import datetime
from pathlib import Path
from collections import deque
from typing import Optional, Dict
import struct
from aiohttp import web

# Configuración
HOST = "0.0.0.0"
PORT = 5012
HTTP_HOST = "0.0.0.0"
HTTP_PORT = 5013
SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2  # 16-bit

# Umbral de energía para detectar voz (ajustar según necesidad)
VOICE_ENERGY_THRESHOLD = 500
# Número de frames consecutivos con voz para confirmar barge-in
VOICE_FRAMES_FOR_BARGEIN = 3

# Sesiones activas para permitir inyección PCM externa por HTTP
ACTIVE_SESSIONS: Dict[str, "IAAgentSession"] = {}
SESSIONS_LOCK = asyncio.Lock()


class AudioBuffer:
    """Buffer circular para audio con detección de voz"""
    def __init__(self, max_seconds: float = 30.0):
        self.max_samples = int(SAMPLE_RATE * max_seconds)
        self.buffer = deque(maxlen=self.max_samples)
        self.voice_frame_count = 0
        
    def add_samples(self, samples: np.ndarray) -> bool:
        """Agrega samples y retorna True si detecta voz"""
        self.buffer.extend(samples.tolist())
        
        # Calcular energía RMS para detectar voz
        if len(samples) > 0:
            energy = np.sqrt(np.mean(samples.astype(np.float32) ** 2))
            if energy > VOICE_ENERGY_THRESHOLD:
                self.voice_frame_count += 1
            else:
                self.voice_frame_count = max(0, self.voice_frame_count - 1)
        
        return self.voice_frame_count >= VOICE_FRAMES_FOR_BARGEIN
    
    def get_audio(self) -> np.ndarray:
        """Obtiene todo el audio acumulado"""
        return np.array(list(self.buffer), dtype=np.int16)
    
    def clear(self):
        """Limpia el buffer"""
        self.buffer.clear()
        self.voice_frame_count = 0


class TTSQueue:
    """Cola de audio TTS para reproducir al caller"""
    def __init__(self):
        self.queue: deque = deque()
        self.is_playing = False
        self.current_audio: Optional[np.ndarray] = None
        self.current_position = 0
        
    def add_audio(self, audio_data: bytes, sample_rate: int = SAMPLE_RATE):
        """Agrega audio TTS a la cola"""
        samples = np.frombuffer(audio_data, dtype=np.int16)
        self.queue.append({
            'samples': samples,
            'sample_rate': sample_rate
        })
        
    def get_next_chunk(self, chunk_size: int = 320) -> Optional[bytes]:
        """Obtiene el siguiente chunk de audio TTS (20ms = 320 samples a 16k)"""
        if not self.is_playing and self.queue:
            item = self.queue.popleft()
            self.current_audio = item['samples']
            self.current_position = 0
            self.is_playing = True
            
        if self.is_playing and self.current_audio is not None:
            remaining = len(self.current_audio) - self.current_position
            if remaining > 0:
                end_pos = min(self.current_position + chunk_size, len(self.current_audio))
                chunk = self.current_audio[self.current_position:end_pos]
                self.current_position = end_pos
                
                # Padding si el chunk es más pequeño
                if len(chunk) < chunk_size:
                    chunk = np.pad(chunk, (0, chunk_size - len(chunk)))
                    
                return chunk.tobytes()
            else:
                # Audio actual terminado
                self.is_playing = False
                self.current_audio = None
                self.current_position = 0
                # Intentar siguiente en cola
                return self.get_next_chunk(chunk_size)
                
        return None
    
    def stop(self):
        """Detiene reproducción actual (barge-in)"""
        self.is_playing = False
        self.current_audio = None
        self.current_position = 0
        self.queue.clear()
        
    def is_active(self) -> bool:
        return self.is_playing or len(self.queue) > 0


class IAAgentSession:
    """Sesión de agente IA para una llamada"""
    
    def __init__(self, websocket, call_id: str):
        self.websocket = websocket
        self.call_id = call_id
        self.audio_buffer = AudioBuffer()
        self.tts_queue = TTSQueue()
        self.is_listening = True
        self.send_lock = asyncio.Lock()
        self.recording_file = None
        self.total_bytes_received = 0
        
        # Crear archivo de grabación para debug
        Path("recordings").mkdir(exist_ok=True)
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.recording_path = f"recordings/{timestamp}_{call_id}.wav"
        self.recording_file = wave.open(self.recording_path, 'wb')
        self.recording_file.setnchannels(CHANNELS)
        self.recording_file.setsampwidth(SAMPLE_WIDTH)
        self.recording_file.setframerate(SAMPLE_RATE)
        
    async def process_audio(self, audio_data: bytes):
        """Procesa audio recibido del caller"""
        self.total_bytes_received += len(audio_data)
        
        # Guardar en grabación
        if self.recording_file:
            self.recording_file.writeframes(audio_data)
        
        # Convertir a samples
        samples = np.frombuffer(audio_data, dtype=np.int16)
        
        # Detectar voz (para barge-in)
        voice_detected = self.audio_buffer.add_samples(samples)
        
        # Si hay TTS reproduciéndose y el caller habla, hacer barge-in
        if voice_detected and self.tts_queue.is_active():
            print(f"   🔇 BARGE-IN detectado - deteniendo TTS")
            self.tts_queue.stop()
            # Aquí podrías notificar al LLM que el usuario interrumpió
            
        # Aquí enviarías el audio al ASR/LLM
        # Por ahora solo mostramos que llega audio
        if self.total_bytes_received % (SAMPLE_RATE * SAMPLE_WIDTH) < len(audio_data):
            duration = self.total_bytes_received / (SAMPLE_RATE * SAMPLE_WIDTH)
            print(f"   ⏱️  {duration:.1f}s recibidos del caller")
    
    async def send_tts_audio(self, audio_data: bytes, sample_rate: int = SAMPLE_RATE):
        """Envía audio TTS al caller via WebSocket"""
        # Formato esperado por mod_audio_stream
        message = {
            "type": "streamAudio",
            "data": {
                "audioDataType": "raw",
                "sampleRate": sample_rate,
                "audioData": base64.b64encode(audio_data).decode('utf-8')
            }
        }
        
        try:
            async with self.send_lock:
                await self.websocket.send(json.dumps(message))
            duration = len(audio_data) / (sample_rate * SAMPLE_WIDTH)
            print(f"   🔊 Enviando {duration:.2f}s de TTS al caller")
        except Exception as e:
            print(f"   ❌ Error enviando TTS: {e}")
    
    async def send_tts_file(self, wav_path: str):
        """Envía un archivo WAV como TTS"""
        try:
            with wave.open(wav_path, 'rb') as wav:
                sample_rate = wav.getframerate()
                audio_data = wav.readframes(wav.getnframes())
                await self.send_tts_audio(audio_data, sample_rate)
        except Exception as e:
            print(f"   ❌ Error leyendo WAV: {e}")
    
    async def send_test_tone(self, frequency: int = 440, duration: float = 1.0):
        """Envía un tono de prueba (útil para verificar que funciona)"""
        t = np.linspace(0, duration, int(SAMPLE_RATE * duration), dtype=np.float32)
        tone = (np.sin(2 * np.pi * frequency * t) * 16000).astype(np.int16)
        await self.send_tts_audio(tone.tobytes())
        
    def close(self):
        """Cierra la sesión"""
        if self.recording_file:
            self.recording_file.close()
            duration = self.total_bytes_received / (SAMPLE_RATE * SAMPLE_WIDTH)
            print(f"✅ Grabación guardada: {self.recording_path}")
            print(f"   Duración: {duration:.2f}s")


async def resolve_target_session(call_id: Optional[str]):
    """Resuelve la sesión destino para inyección HTTP."""
    async with SESSIONS_LOCK:
        if call_id:
            return ACTIVE_SESSIONS.get(call_id), None

        if len(ACTIVE_SESSIONS) == 1:
            return next(iter(ACTIVE_SESSIONS.values())), None

        if not ACTIVE_SESSIONS:
            return None, "No hay llamadas activas"

        return None, "Hay múltiples llamadas activas; especifica call_id"


def parse_sample_rate(raw_value) -> int:
    """Valida sample_rate recibido por HTTP."""
    try:
        sample_rate = int(raw_value)
    except (TypeError, ValueError):
        raise ValueError("sample_rate debe ser un entero")

    if sample_rate <= 0:
        raise ValueError("sample_rate debe ser mayor que 0")

    return sample_rate


async def http_health(request):
    return web.json_response({"ok": True, "service": "ia-agent-server"})


async def http_calls(request):
    async with SESSIONS_LOCK:
        call_ids = list(ACTIVE_SESSIONS.keys())

    return web.json_response({
        "active_calls": call_ids,
        "count": len(call_ids)
    })


async def http_inject_pcm(request):
    """POST /inject-pcm: inyecta PCM16 mono al caller sin mezclar en servidor."""
    content_type = request.content_type or ""
    call_id = None
    sample_rate = SAMPLE_RATE
    pcm_data = b""

    if content_type == "application/octet-stream":
        call_id = request.query.get("call_id")
        try:
            sample_rate = parse_sample_rate(request.query.get("sample_rate", SAMPLE_RATE))
        except ValueError as ex:
            return web.json_response({"ok": False, "error": str(ex)}, status=400)
        pcm_data = await request.read()
    else:
        try:
            payload = await request.json()
        except Exception:
            return web.json_response({
                "ok": False,
                "error": "Body inválido. Usa JSON o application/octet-stream"
            }, status=400)

        call_id = payload.get("call_id")

        try:
            sample_rate = parse_sample_rate(payload.get("sample_rate", SAMPLE_RATE))
        except ValueError as ex:
            return web.json_response({"ok": False, "error": str(ex)}, status=400)

        audio_b64 = payload.get("audio_base64") or payload.get("audioData")
        if not audio_b64:
            return web.json_response({
                "ok": False,
                "error": "Falta audio_base64/audioData en el body JSON"
            }, status=400)

        try:
            pcm_data = base64.b64decode(audio_b64, validate=True)
        except Exception:
            return web.json_response({"ok": False, "error": "audio_base64 inválido"}, status=400)

    if not pcm_data:
        return web.json_response({"ok": False, "error": "No se recibió audio PCM"}, status=400)

    if len(pcm_data) % SAMPLE_WIDTH != 0:
        return web.json_response({
            "ok": False,
            "error": "El audio PCM16 debe tener tamaño par de bytes"
        }, status=400)

    session, resolve_error = await resolve_target_session(call_id)
    if resolve_error:
        return web.json_response({"ok": False, "error": resolve_error}, status=400)

    if not session:
        return web.json_response({"ok": False, "error": f"call_id no encontrado: {call_id}"}, status=404)

    await session.send_tts_audio(pcm_data, sample_rate=sample_rate)

    duration = len(pcm_data) / (sample_rate * SAMPLE_WIDTH)
    return web.json_response({
        "ok": True,
        "call_id": session.call_id,
        "sample_rate": sample_rate,
        "bytes": len(pcm_data),
        "duration_sec": round(duration, 3),
        "mode_hint": "Use uuid_audio_stream start ... mono ... para evitar mezcla en el stream de entrada"
    })


async def start_http_server():
    app = web.Application()
    app.add_routes([
        web.get("/health", http_health),
        web.get("/calls", http_calls),
        web.post("/inject-pcm", http_inject_pcm),
    ])

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, HTTP_HOST, HTTP_PORT)
    await site.start()
    return runner


async def handle_connection(websocket):
    """Maneja conexión WebSocket de una llamada"""
    try:
        client_ip = websocket.remote_address[0] if websocket.remote_address else "unknown"
    except:
        client_ip = "unknown"
    
    call_id = datetime.now().strftime("%H%M%S%f")[:10]
    
    print(f"\n{'='*60}")
    print(f"🤖 Nueva sesión de Agente IA")
    print(f"   Call ID: {call_id}")
    print(f"   Cliente: {client_ip}")
    print(f"{'='*60}")
    
    session = IAAgentSession(websocket, call_id)
    async with SESSIONS_LOCK:
        ACTIVE_SESSIONS[call_id] = session
    
    # Enviar tono de prueba al inicio (opcional)
    # await asyncio.sleep(0.5)
    # await session.send_test_tone(440, 0.5)
    
    try:
        async for message in websocket:
            if isinstance(message, bytes):
                # Audio del caller
                await session.process_audio(message)
                
            elif isinstance(message, str):
                # Mensaje de texto (metadata, comandos)
                print(f"📨 Mensaje: {message[:200]}")
                
                try:
                    data = json.loads(message)
                    
                    # Puedes manejar comandos especiales aquí
                    if data.get('type') == 'test_tts':
                        # Ejemplo: enviar audio de prueba
                        await session.send_test_tone(440, 1.0)
                        
                except json.JSONDecodeError:
                    pass
                    
    except websockets.exceptions.ConnectionClosedOK:
        print(f"🔌 Conexión cerrada normalmente")
    except websockets.exceptions.ConnectionClosedError as e:
        print(f"🔌 Conexión cerrada: {e.code}")
    except Exception as e:
        print(f"❌ Error: {e}")
    finally:
        async with SESSIONS_LOCK:
            ACTIVE_SESSIONS.pop(call_id, None)
        session.close()
        print(f"{'='*60}\n")


async def main():
    print("=" * 60)
    print("🤖 Servidor de Agente IA - mod_audio_stream")
    print("=" * 60)
    print(f"📡 WebSocket: ws://{HOST}:{PORT}")
    print(f"🌐 HTTP API: http://{HTTP_HOST}:{HTTP_PORT}")
    print(f"🎵 Audio: {SAMPLE_RATE}Hz, {CHANNELS}ch, 16-bit PCM")
    print("-" * 60)
    print("Características:")
    print("  ✅ Recepción de audio del caller")
    print("  ✅ Inyección de TTS al caller")
    print("  ✅ Endpoint HTTP para inyección PCM")
    print("  ✅ Sugerencia de modo mono para evitar mezcla")
    print("  ✅ Barge-in (interrupción automática)")
    print("-" * 60)
    print("FreeSWITCH dialplan:")
    print(f"""
<action application="answer"/>
<action application="set" data="stream_result=${{uuid_audio_stream(${{uuid}} start ws://TU_IP:{PORT} mono 16000)}}"/>
<action application="set" data="inject_result=${{uuid_audio_stream(${{uuid}} enable_inject)}}"/>
<action application="park"/>
""")
    print("=" * 60)
    print("Esperando conexiones...\n")

    print("Endpoint POST PCM:")
    print("  POST /inject-pcm (JSON: call_id, sample_rate, audio_base64)")
    print("  POST /inject-pcm?call_id=...&sample_rate=16000 (application/octet-stream)")
    print("Endpoints de monitoreo:")
    print("  GET /calls")
    print("  GET /health")

    http_runner = await start_http_server()
    ws_server = await websockets.serve(
        handle_connection,
        HOST,
        PORT,
        ping_interval=30,
        ping_timeout=10,
        close_timeout=5,
        max_size=10 * 1024 * 1024,
        compression=None
    )

    try:
        await asyncio.Future()
    finally:
        ws_server.close()
        await ws_server.wait_closed()
        await http_runner.cleanup()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n👋 Servidor detenido")
