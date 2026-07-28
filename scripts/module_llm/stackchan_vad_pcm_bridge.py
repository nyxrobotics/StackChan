#!/usr/bin/env python3
"""Send only VAD-delimited microphone segments to StackFlow Whisper."""

import argparse
import base64
import ctypes
import errno
import fcntl
import glob
import json
import logging
import os
import queue
import signal
import socket
import stat
import threading
import time
import uuid
import wave
from collections import deque


LIBZMQ_PATH = "/opt/m5stack/lib/libzmq.so.5"
PCM_SOCKET_PATH = "/tmp/llm/pcm.cap.socket"
VAD_SOCKET_GLOB = "/tmp/llm/*.sock.vad.output_url"
UART_URL = "ipc:///tmp/llm/5556.sock"
STACKFLOW_HOST = "127.0.0.1"
STACKFLOW_PORT = 10001

PAUSE_FILE = "/run/stackchan-vad-pcm-bridge.paused"
READY_FILE = "/run/stackchan-vad-pcm-bridge.ready"
LOCK_FILE = "/run/stackchan-vad-pcm-bridge.lock"
DEBUG_ENABLE_FILE = "/run/stackchan-vad-pcm-bridge.debug"
DEBUG_SEGMENT_DIR = "/tmp/stackchan-vad-pcm-bridge"

SAMPLE_RATE = 16000
SAMPLE_WIDTH = 2
PRE_ROLL_SECONDS = 2.0
TAIL_SECONDS = 0.1
MAX_SEGMENT_SECONDS = 29.0

ZMQ_SUB = 2
ZMQ_PUSH = 8
ZMQ_DONTWAIT = 1
ZMQ_SUBSCRIBE = 6
ZMQ_LINGER = 17
ZMQ_RCVTIMEO = 27
ZMQ_SNDTIMEO = 28


class ZmqRuntime:
    def __init__(self):
        self.lib = ctypes.CDLL(LIBZMQ_PATH, use_errno=True)
        self.lib.zmq_ctx_new.restype = ctypes.c_void_p
        self.lib.zmq_socket.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.zmq_socket.restype = ctypes.c_void_p
        self.lib.zmq_connect.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self.lib.zmq_setsockopt.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_size_t,
        ]
        self.lib.zmq_recv.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_int,
        ]
        self.lib.zmq_send.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_int,
        ]
        self.lib.zmq_close.argtypes = [ctypes.c_void_p]
        self.lib.zmq_ctx_term.argtypes = [ctypes.c_void_p]

        self.context = self.lib.zmq_ctx_new()
        if not self.context:
            raise RuntimeError("zmq_ctx_new failed")

    def close(self):
        if self.context:
            self.lib.zmq_ctx_term(self.context)
            self.context = None


class ZmqSocket:
    def __init__(self, runtime, socket_type, url, timeout_ms):
        self.runtime = runtime
        self.socket = runtime.lib.zmq_socket(runtime.context, socket_type)
        if not self.socket:
            raise RuntimeError("zmq_socket failed")

        linger = ctypes.c_int(0)
        timeout = ctypes.c_int(timeout_ms)
        self._set_option(ZMQ_LINGER, linger)
        if socket_type == ZMQ_SUB:
            if runtime.lib.zmq_setsockopt(self.socket, ZMQ_SUBSCRIBE, None, 0) != 0:
                self.close()
                raise RuntimeError("failed to subscribe")
            self._set_option(ZMQ_RCVTIMEO, timeout)
        else:
            self._set_option(ZMQ_SNDTIMEO, timeout)

        if runtime.lib.zmq_connect(self.socket, url.encode()) != 0:
            self.close()
            raise RuntimeError(f"failed to connect to {url}")

    def _set_option(self, option, value):
        if self.runtime.lib.zmq_setsockopt(
            self.socket,
            option,
            ctypes.byref(value),
            ctypes.sizeof(value),
        ) != 0:
            raise RuntimeError(f"zmq_setsockopt({option}) failed")

    def receive(self, nonblocking=False):
        frame = ctypes.create_string_buffer(65536)
        flags = ZMQ_DONTWAIT if nonblocking else 0
        size = self.runtime.lib.zmq_recv(self.socket, frame, len(frame), flags)
        if size < 0:
            error_number = ctypes.get_errno()
            if error_number in (errno.EAGAIN, errno.EINTR):
                return None
            raise OSError(error_number, os.strerror(error_number))
        if size > len(frame):
            raise RuntimeError(f"oversized ZeroMQ frame: {size} bytes")
        return bytes(frame.raw[:size])

    def send(self, payload):
        buffer = ctypes.create_string_buffer(payload, len(payload))
        size = self.runtime.lib.zmq_send(self.socket, buffer, len(payload), 0)
        if size != len(payload):
            error_number = ctypes.get_errno()
            raise OSError(error_number, os.strerror(error_number))

    def close(self):
        if self.socket:
            self.runtime.lib.zmq_close(self.socket)
            self.socket = None


class StackFlowClient:
    def __init__(self):
        self.socket = None
        self.receive_buffer = b""

    def close(self):
        if self.socket:
            self.socket.close()
            self.socket = None
        self.receive_buffer = b""

    def connect(self):
        self.close()
        self.socket = socket.create_connection(
            (STACKFLOW_HOST, STACKFLOW_PORT), timeout=5.0
        )
        self.socket.settimeout(45.0)

    def request(self, body, predicate):
        if not self.socket:
            self.connect()
        encoded = (
            json.dumps(body, ensure_ascii=False, separators=(",", ":")) + "\n"
        ).encode("utf-8")
        try:
            self.socket.sendall(encoded)
            deadline = time.monotonic() + 45.0
            while time.monotonic() < deadline:
                while b"\n" in self.receive_buffer:
                    line, self.receive_buffer = self.receive_buffer.split(b"\n", 1)
                    if not line:
                        continue
                    response = json.loads(line.decode("utf-8"))
                    if predicate(response):
                        return response
                chunk = self.socket.recv(65536)
                if not chunk:
                    raise ConnectionError("StackFlow TCP connection closed")
                self.receive_buffer += chunk
        except Exception:
            self.close()
            raise
        raise TimeoutError("StackFlow response timed out")

    def whisper_work_id(self):
        request_id = f"vad-bridge-list-{uuid.uuid4().hex}"
        response = self.request(
            {
                "request_id": request_id,
                "work_id": "whisper",
                "action": "taskinfo",
            },
            lambda item: item.get("request_id") == request_id,
        )
        error = response.get("error", {})
        if error.get("code") != 0:
            raise RuntimeError(f"Whisper taskinfo failed: {error}")
        work_ids = response.get("data")
        if not isinstance(work_ids, list):
            raise RuntimeError(f"invalid Whisper task list: {work_ids!r}")
        candidates = [
            item
            for item in work_ids
            if isinstance(item, str) and item.startswith("whisper.")
        ]
        if not candidates:
            raise RuntimeError("no Whisper task is active")
        return max(candidates, key=lambda item: int(item.split(".", 1)[1]))

    def transcribe(self, pcm):
        work_id = self.whisper_work_id()
        request_id = f"vad-segment-{uuid.uuid4().hex}"
        response = self.request(
            {
                "request_id": request_id,
                "work_id": work_id,
                "action": "inference",
                "object": "whisper.vad.pcm.base64",
                "data": base64.b64encode(pcm).decode("ascii"),
            },
            lambda item: (
                item.get("request_id") == request_id
                and item.get("work_id") == work_id
            ),
        )
        return response


class VadPcmBridge:
    def __init__(self, runtime):
        self.runtime = runtime
        self.stop_event = threading.Event()
        self.segment_queue = queue.Queue(maxsize=1)
        self.pre_roll = deque()
        self.pre_roll_size = 0
        self.segment = []
        self.segment_size = 0
        self.speech_active = False
        self.finalize_at = None
        self.segment_truncated = False
        self.generation = 0
        self.debug_enabled = os.path.exists(DEBUG_ENABLE_FILE)
        if not self.debug_enabled:
            self._clear_debug_segments()
        self.pcm_socket = None
        self.pcm_socket_signature = None
        self.vad_socket = None
        self.vad_socket_path = None
        self.vad_socket_signature = None
        self.paused = os.path.exists(PAUSE_FILE)
        self.max_pre_roll_bytes = int(
            SAMPLE_RATE * SAMPLE_WIDTH * PRE_ROLL_SECONDS
        )
        self.max_segment_bytes = int(
            SAMPLE_RATE * SAMPLE_WIDTH * MAX_SEGMENT_SECONDS
        )
        self.worker = threading.Thread(
            target=self._inference_loop,
            name="vad-pcm-inference",
            daemon=True,
        )

    def stop(self):
        self.stop_event.set()

    def _reset_segment(self):
        self.segment = []
        self.segment_size = 0
        self.speech_active = False
        self.finalize_at = None
        self.segment_truncated = False

    def _invalidate_segments(self):
        self.generation += 1
        self._reset_segment()
        self.pre_roll.clear()
        self.pre_roll_size = 0
        while True:
            try:
                self.segment_queue.get_nowait()
            except queue.Empty:
                break

    def _append_pre_roll(self, frame):
        self.pre_roll.append(frame)
        self.pre_roll_size += len(frame)
        while self.pre_roll and self.pre_roll_size > self.max_pre_roll_bytes:
            removed = self.pre_roll.popleft()
            self.pre_roll_size -= len(removed)

    def _handle_pcm(self, frame):
        if self.paused:
            return
        self._append_pre_roll(frame)
        if not self.speech_active or self.segment_truncated:
            return
        room = self.max_segment_bytes - self.segment_size
        if room <= 0:
            self.segment_truncated = True
            logging.warning("VAD segment reached %.1f second limit", MAX_SEGMENT_SECONDS)
            return
        chunk = frame[:room]
        self.segment.append(chunk)
        self.segment_size += len(chunk)
        if len(chunk) != len(frame):
            self.segment_truncated = True
            logging.warning("VAD segment reached %.1f second limit", MAX_SEGMENT_SECONDS)

    def _handle_vad(self, raw, uart):
        try:
            body = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            logging.debug("Ignoring invalid VAD frame: %r", raw[:200])
            return

        if isinstance(body, bool):
            speech = body
        elif isinstance(body, dict) and isinstance(body.get("data"), bool):
            speech = body["data"]
        else:
            return

        if self.paused:
            self._reset_segment()
            logging.debug("Dropping VAD state while bridge is paused")
            return

        if isinstance(body, dict):
            response = body
        else:
            response = {
                "request_id": "vad-bridge",
                "work_id": "vad",
                "created": int(time.time()),
                "object": "vad.bool",
                "data": speech,
                "error": {"code": 0, "message": ""},
            }
        self._forward_to_uart(uart, response)
        logging.info("Forwarded VAD state: %s", "speech" if speech else "silence")

        if speech:
            if self.speech_active:
                self.finalize_at = None
                return
            self.segment = list(self.pre_roll)
            self.segment_size = self.pre_roll_size
            self.speech_active = True
            self.finalize_at = None
            self.segment_truncated = False
            logging.info("VAD speech started (pre_roll_bytes=%d)", self.segment_size)
            return

        if self.speech_active and self.finalize_at is None:
            self.finalize_at = time.monotonic() + TAIL_SECONDS
            logging.info("VAD speech ended; capturing %.0f ms tail", TAIL_SECONDS * 1000)

    def _finalize_if_ready(self):
        if self.finalize_at is None or time.monotonic() < self.finalize_at:
            return
        pcm = b"".join(self.segment)
        generation = self.generation
        duration = len(pcm) / (SAMPLE_RATE * SAMPLE_WIDTH)
        truncated = self.segment_truncated
        self._reset_segment()
        if self.paused:
            logging.info("Dropping VAD segment while bridge is paused")
            return
        self._save_debug_segment(pcm)
        try:
            self.segment_queue.put_nowait((generation, pcm))
            logging.info(
                "Queued VAD PCM segment: bytes=%d duration=%.2fs truncated=%s",
                len(pcm),
                duration,
                truncated,
            )
        except queue.Full:
            logging.warning("Dropping VAD segment because Whisper is still busy")

    def _save_debug_segment(self, pcm):
        if not self.debug_enabled:
            return
        temporary_path = None
        try:
            os.makedirs(DEBUG_SEGMENT_DIR, mode=0o700, exist_ok=True)
            path = os.path.join(DEBUG_SEGMENT_DIR, "segment-latest.wav")
            temporary_path = f"{path}.{os.getpid()}.tmp"
            with wave.open(temporary_path, "wb") as output:
                output.setnchannels(1)
                output.setsampwidth(SAMPLE_WIDTH)
                output.setframerate(SAMPLE_RATE)
                output.writeframes(pcm)
            os.replace(temporary_path, path)
            temporary_path = None
            logging.info("Saved VAD debug segment: %s", path)
        except (OSError, wave.Error):
            logging.exception("Failed to save VAD debug segment")
        finally:
            if temporary_path:
                try:
                    os.unlink(temporary_path)
                except FileNotFoundError:
                    pass

    def _clear_debug_segments(self):
        for path in glob.glob(os.path.join(DEBUG_SEGMENT_DIR, "*")):
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass
        try:
            os.rmdir(DEBUG_SEGMENT_DIR)
        except (FileNotFoundError, OSError):
            pass

    def _refresh_debug_state(self):
        enabled = os.path.exists(DEBUG_ENABLE_FILE)
        if enabled == self.debug_enabled:
            return
        self.debug_enabled = enabled
        if not enabled:
            self._clear_debug_segments()
            logging.info("Cleared VAD debug segments")

    def _set_vad_socket(self, signature):
        if self.vad_socket:
            self.vad_socket.close()
        self.vad_socket = None
        self.vad_socket_path = None
        self.vad_socket_signature = None
        self._invalidate_segments()
        if not signature:
            return
        path = signature[0]
        self.vad_socket = ZmqSocket(
            self.runtime, ZMQ_SUB, f"ipc://{path}", timeout_ms=0
        )
        self.vad_socket_path = path
        self.vad_socket_signature = signature
        logging.info("Following VAD output: %s", path)

    def _set_pcm_socket(self, signature):
        if self.pcm_socket:
            self.pcm_socket.close()
        self.pcm_socket = None
        self.pcm_socket_signature = None
        self.pre_roll.clear()
        self.pre_roll_size = 0
        self._invalidate_segments()
        if not signature:
            return
        path = signature[0]
        self.pcm_socket = ZmqSocket(
            self.runtime, ZMQ_SUB, f"ipc://{path}", timeout_ms=100
        )
        self.pcm_socket_signature = signature
        logging.info("Following PCM capture: %s", path)

    @staticmethod
    def _socket_signature(path):
        try:
            info = os.stat(path)
        except FileNotFoundError:
            return None
        if not stat.S_ISSOCK(info.st_mode):
            return None
        return (path, info.st_dev, info.st_ino)

    def _discover_vad_socket(self):
        candidates = []
        for path in glob.glob(VAD_SOCKET_GLOB):
            try:
                info = os.stat(path)
            except FileNotFoundError:
                continue
            if stat.S_ISSOCK(info.st_mode):
                signature = (path, info.st_dev, info.st_ino)
                candidates.append((info.st_mtime_ns, signature))
        return max(candidates)[1] if candidates else None

    def _refresh_pause_state(self):
        paused = os.path.exists(PAUSE_FILE)
        if paused != self.paused:
            self.paused = paused
            self._invalidate_segments()
            logging.info("Bridge %s", "paused" if paused else "resumed")

    def _refresh_state(self):
        self._refresh_pause_state()
        self._refresh_debug_state()

        signature = self._discover_vad_socket()
        if signature != self.vad_socket_signature:
            self._set_vad_socket(signature)

        pcm_signature = self._socket_signature(PCM_SOCKET_PATH)
        if pcm_signature != self.pcm_socket_signature:
            self._set_pcm_socket(pcm_signature)

    def _forward_to_uart(self, uart, response):
        payload = (
            json.dumps(response, ensure_ascii=False, separators=(",", ":")) + "\n"
        ).encode("utf-8")
        uart.send(payload)

    def _inference_loop(self):
        client = StackFlowClient()
        uart = None
        try:
            uart = ZmqSocket(self.runtime, ZMQ_PUSH, UART_URL, timeout_ms=5000)
            while not self.stop_event.is_set():
                try:
                    generation, pcm = self.segment_queue.get(timeout=0.2)
                except queue.Empty:
                    continue
                if (
                    generation != self.generation
                    or self.paused
                    or os.path.exists(PAUSE_FILE)
                ):
                    logging.info("Dropping invalidated VAD segment")
                    continue
                try:
                    response = client.transcribe(pcm)
                    error = response.get("error", {})
                    if error.get("code") != 0:
                        raise RuntimeError(f"Whisper inference failed: {error}")
                    if response.get("object") != "asr.utf-8":
                        raise RuntimeError(f"unexpected Whisper response: {response!r}")
                    if (
                        generation != self.generation
                        or self.paused
                        or os.path.exists(PAUSE_FILE)
                    ):
                        logging.info("Dropping invalidated Whisper result")
                        continue
                    logging.info("Whisper result: %s", response.get("data", ""))
                    self._forward_to_uart(uart, response)
                except Exception:
                    logging.exception("Failed to transcribe VAD segment")
                    client.close()
        finally:
            client.close()
            if uart:
                uart.close()

    def run(self):
        self.worker.start()
        next_refresh = 0.0
        vad_uart = None
        try:
            vad_uart = ZmqSocket(self.runtime, ZMQ_PUSH, UART_URL, timeout_ms=5000)
            with open(READY_FILE, "w", encoding="ascii") as ready:
                ready.write(f"{os.getpid()}\n")
            logging.info("VAD PCM bridge ready")

            while not self.stop_event.is_set():
                # Pause is part of the audio data contract, so observe it at
                # frame cadence instead of waiting for socket rediscovery.
                self._refresh_pause_state()
                now = time.monotonic()
                if now >= next_refresh:
                    self._refresh_state()
                    next_refresh = now + 0.5

                frame = self.pcm_socket.receive() if self.pcm_socket else None
                if frame:
                    self._handle_pcm(frame)
                elif not self.pcm_socket:
                    time.sleep(0.05)

                if self.vad_socket:
                    while True:
                        vad_frame = self.vad_socket.receive(nonblocking=True)
                        if vad_frame is None:
                            break
                        self._handle_vad(vad_frame, vad_uart)
                self._finalize_if_ready()
        finally:
            try:
                os.unlink(READY_FILE)
            except FileNotFoundError:
                pass
            if self.pcm_socket:
                self.pcm_socket.close()
            if self.vad_socket:
                self.vad_socket.close()
            if vad_uart:
                vad_uart.close()
            self.stop_event.set()
            self.worker.join(timeout=2.0)


def set_paused(paused):
    if paused:
        with open(PAUSE_FILE, "w", encoding="ascii") as marker:
            marker.write("paused\n")
    else:
        try:
            os.unlink(PAUSE_FILE)
        except FileNotFoundError:
            pass


def acquire_lock():
    lock = open(LOCK_FILE, "w", encoding="ascii")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        lock.close()
        raise RuntimeError("another VAD PCM bridge is already running") from exc
    lock.write(f"{os.getpid()}\n")
    lock.flush()
    return lock


def main():
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--pause", action="store_true")
    mode.add_argument("--resume", action="store_true")
    args = parser.parse_args()

    if args.pause:
        set_paused(True)
        return 0
    if args.resume:
        set_paused(False)
        return 0

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    runtime = ZmqRuntime()
    if args.check:
        runtime.close()
        print(f"STACKCHAN_VAD_PCM_BRIDGE_READY library={LIBZMQ_PATH}")
        return 0

    lock = acquire_lock()
    bridge = VadPcmBridge(runtime)

    def stop_bridge(_signum, _frame):
        bridge.stop()

    signal.signal(signal.SIGTERM, stop_bridge)
    signal.signal(signal.SIGINT, stop_bridge)
    try:
        bridge.run()
    finally:
        runtime.close()
        lock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
