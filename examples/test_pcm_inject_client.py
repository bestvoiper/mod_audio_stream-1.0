#!/usr/bin/env python3
"""
Simple PCM injection test endpoint for mod_audio_stream.

This script runs a WebSocket server that accepts a connection from mod_audio_stream
and sends streamAudio/raw messages with PCM16 mono chunks.

Usage examples:
  python test_pcm_inject_client.py --port 5012 --tone-seconds 3
  python test_pcm_inject_client.py --wav ./prompt.wav --sample-rate 16000

FreeSWITCH example:
  uuid_audio_stream <uuid> start ws://127.0.0.1:5012 mono 16000
  uuid_audio_stream <uuid> enable_inject
"""

import argparse
import asyncio
import base64
import json
import math
import struct
import wave
from pathlib import Path
from typing import Tuple

import websockets


def generate_tone_pcm16(sample_rate: int, seconds: float, frequency: float, amplitude: float) -> bytes:
    total_samples = int(sample_rate * seconds)
    amp = max(0.0, min(1.0, amplitude))
    out = bytearray()
    for i in range(total_samples):
        t = i / sample_rate
        v = math.sin(2.0 * math.pi * frequency * t) * amp
        out.extend(struct.pack("<h", int(v * 32767.0)))
    return bytes(out)


def _pcm16_from_wav(path: Path) -> Tuple[bytes, int, int]:
    with wave.open(str(path), "rb") as wf:
        channels = wf.getnchannels()
        sample_width = wf.getsampwidth()
        sample_rate = wf.getframerate()
        frames = wf.getnframes()
        raw = wf.readframes(frames)

    if sample_width != 2:
        raise ValueError("Only 16-bit WAV is supported")

    if channels == 1:
        return raw, sample_rate, 1

    # Downmix multi-channel to mono by averaging channels per frame.
    ints = struct.unpack("<" + "h" * (len(raw) // 2), raw)
    mono = bytearray()
    for i in range(0, len(ints), channels):
        acc = 0
        for ch in range(channels):
            acc += ints[i + ch]
        mono.extend(struct.pack("<h", int(acc / channels)))
    return bytes(mono), sample_rate, 1


def _resample_linear_pcm16_mono(data: bytes, src_rate: int, dst_rate: int) -> bytes:
    if src_rate == dst_rate:
        return data

    src = struct.unpack("<" + "h" * (len(data) // 2), data)
    if not src:
        return b""

    src_len = len(src)
    dst_len = int(src_len * dst_rate / src_rate)
    dst = [0] * dst_len

    for i in range(dst_len):
        pos = i * (src_len - 1) / max(1, dst_len - 1)
        idx = int(pos)
        frac = pos - idx
        a = src[idx]
        b = src[min(idx + 1, src_len - 1)]
        dst[i] = int(a + (b - a) * frac)

    return struct.pack("<" + "h" * len(dst), *dst)


async def send_pcm_stream(
    websocket,
    pcm_data: bytes,
    sample_rate: int,
    chunk_ms: int,
    repeat: int,
):
    chunk_bytes = int(sample_rate * (chunk_ms / 1000.0) * 2)
    if chunk_bytes <= 0:
        raise ValueError("chunk size must be > 0")

    for cycle in range(repeat):
        sent = 0
        for pos in range(0, len(pcm_data), chunk_bytes):
            chunk = pcm_data[pos : pos + chunk_bytes]
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
            sent += len(chunk)
            await asyncio.sleep(chunk_ms / 1000.0)

        seconds = sent / (sample_rate * 2)
        print(f"[inject] cycle={cycle + 1}/{repeat} sent={sent} bytes ({seconds:.2f}s)")


async def receive_from_fs(websocket):
    total_bytes = 0
    try:
        async for message in websocket:
            if isinstance(message, bytes):
                total_bytes += len(message)
                if total_bytes % 32000 < len(message):
                    print(f"[from-fs] received ~{total_bytes / 32000:.1f}s audio")
            else:
                print(f"[from-fs] text={message[:160]}")
    except websockets.ConnectionClosed:
        pass


async def handler(websocket, args, pcm_data: bytes, pcm_rate: int):
    print("[ws] mod_audio_stream connected")

    rx_task = asyncio.create_task(receive_from_fs(websocket))
    try:
        if args.start_delay > 0:
            await asyncio.sleep(args.start_delay)

        await send_pcm_stream(
            websocket=websocket,
            pcm_data=pcm_data,
            sample_rate=pcm_rate,
            chunk_ms=args.chunk_ms,
            repeat=args.repeat,
        )

        if args.keep_open > 0:
            await asyncio.sleep(args.keep_open)
    finally:
        rx_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await rx_task
        print("[ws] session done")


def prepare_audio(args) -> Tuple[bytes, int]:
    if args.wav:
        wav_path = Path(args.wav)
        if not wav_path.exists():
            raise FileNotFoundError(f"WAV file not found: {wav_path}")

        pcm, src_rate, _ = _pcm16_from_wav(wav_path)
        pcm = _resample_linear_pcm16_mono(pcm, src_rate, args.sample_rate)
        print(
            f"[audio] wav={wav_path} src_rate={src_rate}Hz out_rate={args.sample_rate}Hz bytes={len(pcm)}"
        )
        return pcm, args.sample_rate

    pcm = generate_tone_pcm16(
        sample_rate=args.sample_rate,
        seconds=args.tone_seconds,
        frequency=args.tone_hz,
        amplitude=args.tone_amp,
    )
    print(
        f"[audio] tone rate={args.sample_rate}Hz hz={args.tone_hz} sec={args.tone_seconds} bytes={len(pcm)}"
    )
    return pcm, args.sample_rate


async def main():
    parser = argparse.ArgumentParser(description="Simple PCM injection test for mod_audio_stream")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5012)
    parser.add_argument("--sample-rate", type=int, default=16000)
    parser.add_argument("--chunk-ms", type=int, default=20)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--start-delay", type=float, default=1.0)
    parser.add_argument("--keep-open", type=float, default=2.0)

    parser.add_argument("--wav", default="")
    parser.add_argument("--tone-seconds", type=float, default=3.0)
    parser.add_argument("--tone-hz", type=float, default=440.0)
    parser.add_argument("--tone-amp", type=float, default=0.5)

    args = parser.parse_args()

    pcm_data, pcm_rate = prepare_audio(args)

    print(f"[ws] listening on ws://{args.host}:{args.port}")
    print("[hint] start FreeSWITCH with mix mode mono and enable_inject")

    async with websockets.serve(
        lambda ws: handler(ws, args, pcm_data, pcm_rate),
        args.host,
        args.port,
        ping_interval=30,
        ping_timeout=10,
        compression=None,
        max_size=10 * 1024 * 1024,
    ):
        await asyncio.Future()


if __name__ == "__main__":
    import contextlib

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("stopped")
