#!/usr/bin/env python3
"""
AI simulator over mod_audio_stream (unmixed injection path).

Flow:
1) Caller audio arrives from mod_audio_stream as binary frames.
2) This script does simple VAD (voice activity detection).
3) When caller finishes speaking (speech -> silence), script sends an "AI response"
   as streamAudio/raw PCM16 mono chunks.

Important:
- Start mod_audio_stream in mono mode to avoid mixed input stream.
- enable_inject must be enabled in FreeSWITCH.

Example FreeSWITCH commands:
  uuid_audio_stream <uuid> start ws://127.0.0.1:5012 mono 16000
  uuid_audio_stream <uuid> enable_inject
"""

import argparse
import asyncio
import base64
import json
import math
import struct
from collections import deque
from typing import Deque, Tuple

import websockets


def pcm16_rms(frame: bytes) -> float:
    if not frame:
        return 0.0
    samples = struct.unpack("<" + "h" * (len(frame) // 2), frame)
    if not samples:
        return 0.0
    acc = 0.0
    for s in samples:
        acc += float(s) * float(s)
    return math.sqrt(acc / len(samples))


def tone_pcm16(sample_rate: int, hz: float, sec: float, amp: float = 0.45) -> bytes:
    n = int(sample_rate * sec)
    out = bytearray()
    for i in range(n):
        t = i / sample_rate
        v = math.sin(2.0 * math.pi * hz * t) * amp
        out.extend(struct.pack("<h", int(v * 32767.0)))
    return bytes(out)


def silence_pcm16(sample_rate: int, sec: float) -> bytes:
    return b"\x00\x00" * int(sample_rate * sec)


def build_simulated_ai_response(sample_rate: int) -> bytes:
    # Two-tone synthetic response to emulate TTS latency/content.
    return (
        tone_pcm16(sample_rate, 520.0, 0.30)
        + silence_pcm16(sample_rate, 0.06)
        + tone_pcm16(sample_rate, 660.0, 0.35)
        + silence_pcm16(sample_rate, 0.06)
        + tone_pcm16(sample_rate, 740.0, 0.40)
    )


async def send_stream_audio_raw(websocket, pcm: bytes, sample_rate: int, chunk_ms: int):
    chunk_bytes = int(sample_rate * (chunk_ms / 1000.0) * 2)
    if chunk_bytes <= 0:
        raise ValueError("chunk_ms too small")

    for pos in range(0, len(pcm), chunk_bytes):
        chunk = pcm[pos : pos + chunk_bytes]
        msg = {
            "type": "streamAudio",
            "data": {
                "audioDataType": "raw",
                "sampleRate": sample_rate,
                "channels": 1,
                "audioData": base64.b64encode(chunk).decode("ascii"),
            },
        }
        await websocket.send(json.dumps(msg))
        await asyncio.sleep(chunk_ms / 1000.0)


class VadState:
    def __init__(self, threshold: float, start_frames: int, end_frames: int):
        self.threshold = threshold
        self.start_frames = start_frames
        self.end_frames = end_frames
        self.voice_run = 0
        self.silence_run = 0
        self.in_speech = False

    def update(self, rms: float) -> Tuple[bool, bool]:
        speech_started = False
        speech_ended = False

        if rms >= self.threshold:
            self.voice_run += 1
            self.silence_run = 0
        else:
            self.silence_run += 1
            self.voice_run = 0

        if not self.in_speech and self.voice_run >= self.start_frames:
            self.in_speech = True
            speech_started = True

        if self.in_speech and self.silence_run >= self.end_frames:
            self.in_speech = False
            speech_ended = True
            self.voice_run = 0
            self.silence_run = 0

        return speech_started, speech_ended


async def handle_call(websocket, args):
    print("[session] connected from mod_audio_stream")

    vad = VadState(
        threshold=args.vad_threshold,
        start_frames=args.vad_start_frames,
        end_frames=args.vad_end_frames,
    )

    # Keep a short rolling buffer for debug/telemetry if needed.
    recent_rms: Deque[float] = deque(maxlen=30)

    # Prevent overlapping responses.
    responding = False

    try:
        async for message in websocket:
            if isinstance(message, str):
                print(f"[from-fs] text={message[:150]}")
                continue

            # Binary caller audio frame.
            rms = pcm16_rms(message)
            recent_rms.append(rms)
            started, ended = vad.update(rms)

            if started:
                print(f"[vad] speech start (rms={rms:.1f})")

            if ended and not responding:
                responding = True
                print(f"[vad] speech end -> simulate AI response")
                response_pcm = build_simulated_ai_response(args.sample_rate)
                await send_stream_audio_raw(
                    websocket=websocket,
                    pcm=response_pcm,
                    sample_rate=args.sample_rate,
                    chunk_ms=args.chunk_ms,
                )
                secs = len(response_pcm) / (args.sample_rate * 2)
                print(f"[inject] sent {secs:.2f}s response (unmixed replace path)")
                responding = False

    except websockets.ConnectionClosed as ex:
        print(f"[session] closed code={ex.code}")


async def main():
    parser = argparse.ArgumentParser(description="AI simulator for mod_audio_stream (no mixed input)")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5012)
    parser.add_argument("--sample-rate", type=int, default=16000)
    parser.add_argument("--chunk-ms", type=int, default=20)

    parser.add_argument("--vad-threshold", type=float, default=900.0)
    parser.add_argument("--vad-start-frames", type=int, default=4)
    parser.add_argument("--vad-end-frames", type=int, default=8)

    args = parser.parse_args()

    print(f"[ws] listening ws://{args.host}:{args.port}")
    print("[hint] FreeSWITCH: start ... mono 16000 ; then enable_inject")

    async with websockets.serve(
        lambda ws: handle_call(ws, args),
        args.host,
        args.port,
        ping_interval=30,
        ping_timeout=10,
        compression=None,
        max_size=10 * 1024 * 1024,
    ):
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("stopped")
