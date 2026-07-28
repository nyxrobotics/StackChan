#!/usr/bin/env python3
"""Wait until StackFlow's sys.pcm publisher emits a captured audio frame."""

import argparse
import ctypes
import time


LIBZMQ_PATH = "/opt/m5stack/lib/libzmq.so.5"
PCM_URL = "ipc:///tmp/llm/pcm.cap.socket"

ZMQ_SUB = 2
ZMQ_SUBSCRIBE = 6
ZMQ_RCVTIMEO = 27


def load_libzmq():
    lib = ctypes.CDLL(LIBZMQ_PATH, use_errno=True)
    lib.zmq_ctx_new.restype = ctypes.c_void_p
    lib.zmq_socket.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.zmq_socket.restype = ctypes.c_void_p
    lib.zmq_connect.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.zmq_setsockopt.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_size_t,
    ]
    lib.zmq_recv.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_int,
    ]
    lib.zmq_close.argtypes = [ctypes.c_void_p]
    lib.zmq_ctx_term.argtypes = [ctypes.c_void_p]
    return lib


def wait_for_pcm(timeout_seconds):
    lib = load_libzmq()
    context = lib.zmq_ctx_new()
    if not context:
        raise RuntimeError("zmq_ctx_new failed")

    subscriber = lib.zmq_socket(context, ZMQ_SUB)
    if not subscriber:
        lib.zmq_ctx_term(context)
        raise RuntimeError("zmq_socket failed")

    receive_timeout = ctypes.c_int(200)
    try:
        if lib.zmq_setsockopt(subscriber, ZMQ_SUBSCRIBE, None, 0) != 0:
            raise RuntimeError("failed to subscribe to sys.pcm")
        if lib.zmq_setsockopt(
            subscriber,
            ZMQ_RCVTIMEO,
            ctypes.byref(receive_timeout),
            ctypes.sizeof(receive_timeout),
        ) != 0:
            raise RuntimeError("failed to set receive timeout")
        if lib.zmq_connect(subscriber, PCM_URL.encode()) != 0:
            raise RuntimeError("failed to connect to sys.pcm")

        deadline = time.monotonic() + timeout_seconds
        frame = ctypes.create_string_buffer(65536)
        while time.monotonic() < deadline:
            size = lib.zmq_recv(subscriber, frame, len(frame), 0)
            if size > 0:
                print(f"STACKCHAN_PCM_READY bytes={size}", flush=True)
                return True
        return False
    finally:
        lib.zmq_close(subscriber)
        lib.zmq_ctx_term(context)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    if args.check:
        load_libzmq()
        print(f"STACKCHAN_PCM_PROBE_READY library={LIBZMQ_PATH}")
        return 0
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    if wait_for_pcm(args.timeout):
        return 0
    print("STACKCHAN_PCM_TIMEOUT", flush=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
