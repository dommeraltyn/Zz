#!/usr/bin/env python3
"""
BestClient voice packet sender for ZZClient-10.8.7 protocol.

Protocol match source:
  src/game/client/components/bestclient/voice/protocol.h
  src/game/client/components/bestclient/voice/voice.cpp

What it does:
  1) Sends PACKET_HELLO
  2) Waits for PACKET_HELLO_ACK (voice id)
  3) Encodes audio (wav/mp3/anything ffmpeg can read) -> Opus 20ms frames
  4) Sends PACKET_VOICE frames with sequence counter
  5) Optionally sends PACKET_GOODBYE
  6) Supports multiple fake players with random IDs (200-500)

Requirements:
  - ffmpeg in PATH
  - libopus available (e.g. libopus.dll on Windows)
"""

from __future__ import annotations

import argparse
import asyncio
import ctypes
import random
import shutil
import socket
import struct
import subprocess
import sys
from pathlib import Path
from typing import Optional


PROTOCOL_MAGIC = 0x42564331  # BVC1
PROTOCOL_VERSION = 3

SAMPLE_RATE = 48000
CHANNELS = 1
FRAME_SIZE = 960  # 20 ms @ 48 kHz
FRAME_BYTES = FRAME_SIZE * 2  # s16 mono
MAX_OPUS_PACKET_SIZE = 400
DEFAULT_PORT = 8777
MAX_ROOM_KEY_LENGTH = 128

PACKET_HELLO = 1
PACKET_HELLO_ACK = 2
PACKET_VOICE = 3
PACKET_VOICE_RELAY = 4
PACKET_PING = 6
PACKET_PONG = 7
PACKET_GOODBYE = 9

OPUS_APPLICATION_VOIP = 2048
OPUS_SET_BITRATE_REQUEST = 4002
OPUS_SET_VBR_REQUEST = 4006
OPUS_SET_VBR_CONSTRAINT_REQUEST = 4020
OPUS_SET_COMPLEXITY_REQUEST = 4010
OPUS_SET_SIGNAL_REQUEST = 4024
OPUS_SET_DTX_REQUEST = 4016
OPUS_SIGNAL_VOICE = 3001


def parse_host_port(text: str, default_port: int = DEFAULT_PORT) -> tuple[str, int]:
    if text.count(":") == 0:
        return text, default_port
    if text.startswith("["):
        right = text.rfind("]")
        if right == -1:
            raise ValueError(f"invalid address: {text}")
        host = text[1:right]
        rest = text[right + 1 :]
        if not rest.startswith(":"):
            return host, default_port
        return host, int(rest[1:])
    host, port = text.rsplit(":", 1)
    return host, int(port)


def packet_header(packet_type: int) -> bytes:
    return struct.pack(">IBB", PROTOCOL_MAGIC, packet_type, PROTOCOL_VERSION)


def pack_hello(room_key: str, game_client_id: int, team: int) -> bytes:
    key_bytes = room_key.encode("utf-8")[:MAX_ROOM_KEY_LENGTH]
    payload = bytearray()
    payload += struct.pack(">H", 1)  # same as client (reserved/caps)
    payload += struct.pack(">H", len(key_bytes))
    payload += key_bytes
    payload += struct.pack(">h", int(game_client_id))
    payload += struct.pack(">h", int(team))
    return packet_header(PACKET_HELLO) + payload


def pack_goodbye() -> bytes:
    return packet_header(PACKET_GOODBYE)


def pack_voice(team: int, pos_x: int, pos_y: int, seq: int, opus_payload: bytes) -> bytes:
    if not (1 <= len(opus_payload) <= MAX_OPUS_PACKET_SIZE):
        raise ValueError(f"opus payload size out of range: {len(opus_payload)}")
    payload = bytearray()
    payload += struct.pack(">h", int(team))
    payload += struct.pack(">i", int(pos_x))
    payload += struct.pack(">i", int(pos_y))
    payload += struct.pack(">H", seq & 0xFFFF)
    payload += struct.pack(">H", len(opus_payload))
    payload += opus_payload
    return packet_header(PACKET_VOICE) + payload


def decode_audio_to_pcm16_mono_48k(path: Path) -> bytes:
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("ffmpeg not found in PATH")
    cmd = [
        "ffmpeg",
        "-v",
        "error",
        "-i",
        str(path),
        "-f",
        "s16le",
        "-acodec",
        "pcm_s16le",
        "-ac",
        "1",
        "-ar",
        str(SAMPLE_RATE),
        "-",
    ]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg failed: {proc.stderr.decode('utf-8', 'replace').strip()}")
    return proc.stdout


def pcm_frames(pcm_s16le: bytes):
    total = len(pcm_s16le)
    offset = 0
    while offset < total:
        chunk = pcm_s16le[offset : offset + FRAME_BYTES]
        offset += FRAME_BYTES
        if len(chunk) < FRAME_BYTES:
            chunk = chunk + (b"\x00" * (FRAME_BYTES - len(chunk)))
        yield chunk


class OpusEncoderWrapper:
    def __init__(self, opus_lib_path: Optional[str], bitrate_kbps: int):
        self.lib = self._load_lib(opus_lib_path)
        self._setup_symbols()

        err = ctypes.c_int(0)
        self.enc = self.lib.opus_encoder_create(
            SAMPLE_RATE,
            CHANNELS,
            OPUS_APPLICATION_VOIP,
            ctypes.byref(err),
        )
        if not self.enc or err.value != 0:
            raise RuntimeError(f"opus_encoder_create failed, err={err.value}")

        self._ctl(OPUS_SET_BITRATE_REQUEST, int(max(6, min(128, bitrate_kbps)) * 1000))
        self._ctl(OPUS_SET_VBR_REQUEST, 1)
        self._ctl(OPUS_SET_VBR_CONSTRAINT_REQUEST, 0)
        self._ctl(OPUS_SET_COMPLEXITY_REQUEST, 10)
        self._ctl(OPUS_SET_SIGNAL_REQUEST, OPUS_SIGNAL_VOICE)
        self._ctl(OPUS_SET_DTX_REQUEST, 0)

    def _load_lib(self, opus_lib_path: Optional[str]):
        candidates = []
        if opus_lib_path:
            candidates.append(opus_lib_path)
        candidates += [
            "libopus.dll",
            "opus.dll",
            str(Path(__file__).resolve().parents[1] / "build" / "libopus.dll"),
        ]
        last_err: Optional[Exception] = None
        for c in candidates:
            try:
                return ctypes.CDLL(c)
            except Exception as e:  # noqa: BLE001
                last_err = e
        raise RuntimeError(f"failed to load libopus ({candidates}): {last_err}")

    def _setup_symbols(self):
        self.lib.opus_encoder_create.restype = ctypes.c_void_p
        self.lib.opus_encoder_create.argtypes = [
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_int),
        ]

        self.lib.opus_encode.restype = ctypes.c_int
        self.lib.opus_encode.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int16),
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_ubyte),
            ctypes.c_int32,
        ]

        self.lib.opus_encoder_destroy.restype = None
        self.lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]

        self.lib.opus_encoder_ctl.restype = ctypes.c_int
        self.lib.opus_encoder_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]

    def _ctl(self, request: int, value: int):
        ret = self.lib.opus_encoder_ctl(self.enc, request, value)
        if ret != 0:
            raise RuntimeError(f"opus_encoder_ctl({request}) failed: {ret}")

    def encode_frame(self, frame_s16le: bytes) -> bytes:
        if len(frame_s16le) != FRAME_BYTES:
            raise ValueError(f"bad frame size: {len(frame_s16le)}")
        pcm_ptr = (ctypes.c_int16 * FRAME_SIZE).from_buffer_copy(frame_s16le)
        out_buf = (ctypes.c_ubyte * MAX_OPUS_PACKET_SIZE)()
        n = self.lib.opus_encode(self.enc, pcm_ptr, FRAME_SIZE, out_buf, MAX_OPUS_PACKET_SIZE)
        if n <= 0:
            raise RuntimeError(f"opus_encode failed: {n}")
        return bytes(out_buf[:n])

    def close(self):
        if getattr(self, "enc", None):
            self.lib.opus_encoder_destroy(self.enc)
            self.enc = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()


async def wait_for_hello_ack(
    sock: socket.socket,
    timeout_sec: float,
) -> Optional[int]:
    loop = asyncio.get_running_loop()
    end = loop.time() + timeout_sec
    while True:
        remaining = end - loop.time()
        if remaining <= 0:
            return None
        try:
            data = await asyncio.wait_for(loop.sock_recv(sock, 2048), timeout=remaining)
        except asyncio.TimeoutError:
            return None
        if len(data) < 8:
            continue
        magic, packet_type, version = struct.unpack(">IBB", data[:6])
        if magic != PROTOCOL_MAGIC or version != PROTOCOL_VERSION:
            continue
        if packet_type == PACKET_HELLO_ACK and len(data) >= 8:
            (voice_id,) = struct.unpack(">H", data[6:8])
            return int(voice_id)


class AsyncFakePlayer:
    def __init__(
        self,
        player_id: int,
        server: tuple[str, int],
        room_key: str,
        team: int,
        opus_frames: list[bytes],
        pos_x: int,
        pos_y: int,
        realtime: bool,
        speed: float,
        hello_retries: int,
        hello_timeout: float,
        send_goodbye: bool,
    ):
        self.player_id = player_id
        self.server = server
        self.room_key = room_key
        self.team = team
        self.opus_frames = opus_frames
        self.pos_x = pos_x
        self.pos_y = pos_y
        self.realtime = realtime
        self.speed = speed
        self.hello_retries = hello_retries
        self.hello_timeout = hello_timeout
        self.send_goodbye = send_goodbye
        self.success = False
        self.error: Optional[str] = None

    async def run(self):
        try:
            loop = asyncio.get_running_loop()
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setblocking(False)
            sock.connect(self.server)
            try:
                hello = pack_hello(self.room_key, self.player_id, self.team)
                voice_id: Optional[int] = None

                for _attempt in range(1, self.hello_retries + 1):
                    await loop.sock_sendall(sock, hello)
                    voice_id = await wait_for_hello_ack(sock, self.hello_timeout)
                    if voice_id is not None:
                        break

                if voice_id is None:
                    self.error = f"Player {self.player_id}: no HELLO_ACK"
                    return

                print(f"[+] Player {self.player_id}: HELLO_ACK voice_id={voice_id}")

                seq = 0
                frame_sec = FRAME_SIZE / SAMPLE_RATE
                next_send = loop.time()

                for opus_payload in self.opus_frames:
                    pkt = pack_voice(
                        team=self.team,
                        pos_x=self.pos_x,
                        pos_y=self.pos_y,
                        seq=seq,
                        opus_payload=opus_payload,
                    )
                    await loop.sock_sendall(sock, pkt)
                    seq = (seq + 1) & 0xFFFF

                    if self.realtime:
                        next_send += frame_sec / max(self.speed, 0.001)
                        sleep_for = next_send - loop.time()
                        if sleep_for > 0:
                            await asyncio.sleep(sleep_for)

                if self.send_goodbye:
                    await loop.sock_sendall(sock, pack_goodbye())

                self.success = True
                print(f"[+] Player {self.player_id}: sent {len(self.opus_frames)} frames")
            finally:
                sock.close()
        except Exception as e:  # noqa: BLE001
            self.error = f"Player {self.player_id}: {e}"


async def send_file_multi(
    server: tuple[str, int],
    room_key: str,
    team: int,
    audio_path: Path,
    pos_x: int,
    pos_y: int,
    bitrate_kbps: int,
    realtime: bool,
    speed: float,
    hello_retries: int,
    hello_timeout: float,
    send_goodbye: bool,
    opus_lib: Optional[str],
    num_fake_players: int,
):
    pcm = decode_audio_to_pcm16_mono_48k(audio_path)
    frame_count = (len(pcm) + FRAME_BYTES - 1) // FRAME_BYTES
    print(f"[i] input={audio_path} pcm_bytes={len(pcm)} frames={frame_count}")

    print(f"[i] encoding {frame_count} frames to Opus...")
    opus_frames: list[bytes] = []
    with OpusEncoderWrapper(opus_lib, bitrate_kbps) as enc:
        for frame in pcm_frames(pcm):
            opus_frames.append(enc.encode_frame(frame))
    print(f"[i] encoded {len(opus_frames)} Opus frames")

    player_ids = random.sample(range(0, 101), min(num_fake_players, 101))
    print(
        f"[i] spawning {len(player_ids)} fake players with IDs: "
        f"{player_ids[:10]}{'...' if len(player_ids) > 10 else ''}"
    )

    players = [
        AsyncFakePlayer(
            player_id=pid,
            server=server,
            room_key=room_key,
            team=team,
            opus_frames=opus_frames,
            pos_x=pos_x,
            pos_y=pos_y,
            realtime=realtime,
            speed=speed,
            hello_retries=hello_retries,
            hello_timeout=hello_timeout,
            send_goodbye=send_goodbye,
        )
        for pid in player_ids
    ]

    tasks = []
    for player in players:
        tasks.append(asyncio.create_task(player.run()))
        await asyncio.sleep(0.01)

    await asyncio.gather(*tasks)

    success_count = sum(1 for p in players if p.success)
    failed_count = len(players) - success_count
    print(f"\n[=] Results: {success_count} successful, {failed_count} failed")

    if failed_count > 0:
        print("[!] Failed players:")
        for player in players:
            if player.error:
                print(f"    {player.error}")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Send voice packets compatible with BestClient voice protocol")
    p.add_argument("--server", required=True, help="voice server host[:port], default port 8777")
    p.add_argument("--room-key", required=True, help="room key used by client (<=128 bytes utf-8)")
    p.add_argument("--game-client-id", type=int, help="DDNet client id (s16) - ignored if --num-fake-players is set")
    p.add_argument("--team", type=int, required=True, help="voice team id (s16)")
    p.add_argument("--audio", required=True, help="path to wav/mp3/any ffmpeg-readable file")
    p.add_argument("--pos-x", type=int, default=0, help="sender position X (s32)")
    p.add_argument("--pos-y", type=int, default=0, help="sender position Y (s32)")
    p.add_argument("--bitrate-kbps", type=int, default=64, help="Opus bitrate kbps (6..128)")
    p.add_argument("--no-realtime", action="store_true", help="send as fast as possible")
    p.add_argument("--speed", type=float, default=1.0, help="realtime speed multiplier (e.g. 1.0, 2.0)")
    p.add_argument("--hello-retries", type=int, default=5, help="hello retry count")
    p.add_argument("--hello-timeout", type=float, default=1.0, help="seconds to wait for each HELLO_ACK")
    p.add_argument("--no-goodbye", action="store_true", help="do not send PACKET_GOODBYE at end")
    p.add_argument("--opus-lib", default=None, help="path to libopus (e.g. C:\\...\\libopus.dll)")
    p.add_argument(
        "--num-fake-players",
        type=int,
        default=0,
        help="number of fake players (0 = single player mode, 1-301 = multi-player mode with random IDs 200-500)",
    )
    return p


async def main_async() -> int:
    args = build_parser().parse_args()
    try:
        host, port = parse_host_port(args.server, DEFAULT_PORT)
        server = (host, port)

        if args.num_fake_players > 0:
            if args.num_fake_players > 101:
                print("[!] Warning: num-fake-players capped at 101 (range 0-100)", file=sys.stderr)
                args.num_fake_players = 101

            await send_file_multi(
                server=server,
                room_key=args.room_key,
                team=args.team,
                audio_path=Path(args.audio),
                pos_x=args.pos_x,
                pos_y=args.pos_y,
                bitrate_kbps=args.bitrate_kbps,
                realtime=not args.no_realtime,
                speed=args.speed,
                hello_retries=max(1, args.hello_retries),
                hello_timeout=max(0.05, args.hello_timeout),
                send_goodbye=not args.no_goodbye,
                opus_lib=args.opus_lib,
                num_fake_players=args.num_fake_players,
            )
        else:
            if args.game_client_id is None:
                print("[x] --game-client-id is required in single player mode", file=sys.stderr)
                return 1

            pcm = decode_audio_to_pcm16_mono_48k(Path(args.audio))
            frame_count = (len(pcm) + FRAME_BYTES - 1) // FRAME_BYTES
            print(f"[i] input={args.audio} pcm_bytes={len(pcm)} frames={frame_count}")

            loop = asyncio.get_running_loop()
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock, OpusEncoderWrapper(args.opus_lib, args.bitrate_kbps) as enc:
                sock.setblocking(False)
                sock.connect(server)
                hello = pack_hello(args.room_key, args.game_client_id, args.team)
                voice_id: Optional[int] = None

                for attempt in range(1, max(1, args.hello_retries) + 1):
                    await loop.sock_sendall(sock, hello)
                    voice_id = await wait_for_hello_ack(sock, max(0.05, args.hello_timeout))
                    if voice_id is not None:
                        break
                    print(f"[!] no HELLO_ACK on attempt {attempt}/{args.hello_retries}")

                if voice_id is None:
                    raise RuntimeError("server did not return HELLO_ACK")
                print(f"[+] HELLO_ACK voice_id={voice_id}")

                seq = 0
                frame_sec = FRAME_SIZE / SAMPLE_RATE
                next_send = loop.time()
                sent = 0

                for frame in pcm_frames(pcm):
                    opus_payload = enc.encode_frame(frame)
                    pkt = pack_voice(
                        team=args.team,
                        pos_x=args.pos_x,
                        pos_y=args.pos_y,
                        seq=seq,
                        opus_payload=opus_payload,
                    )
                    await loop.sock_sendall(sock, pkt)
                    seq = (seq + 1) & 0xFFFF
                    sent += 1

                    if not args.no_realtime:
                        next_send += frame_sec / max(args.speed, 0.001)
                        sleep_for = next_send - loop.time()
                        if sleep_for > 0:
                            await asyncio.sleep(sleep_for)

                print(f"[+] sent voice frames: {sent}")
                if not args.no_goodbye:
                    await loop.sock_sendall(sock, pack_goodbye())
                    print("[+] sent GOODBYE")

        return 0
    except Exception as e:  # noqa: BLE001
        print(f"[x] {e}", file=sys.stderr)
        return 1


def main() -> int:
    return asyncio.run(main_async())


if __name__ == "__main__":
    raise SystemExit(main())
