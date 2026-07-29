#!/usr/bin/env python3
"""Recover llm-sys when its TCP control path or CoreS3 UART path stalls."""

import argparse
import glob
import json
import logging
import os
import re
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Optional, Tuple


READY_MARKER = "STACKCHAN_LLM_SYS_WATCHDOG_READY"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 10001
DEFAULT_UART_INDEX = 1
MAX_RESPONSE_BYTES = 64 * 1024
MIN_UNANSWERED_UART_BYTES = 32
DEFAULT_LOCAL_TURN_GRACE = 60.0
DEFAULT_OPENJTALK_GRACE = 300.0
OPENJTALK_PROCESS_MARKERS = (
    b"/opt/stackchan/openjtalk_tts.sh",
    b"/tmp/stackchan-openjtalk/",
)
LOCAL_CONVERSATION_MARKER = "/run/stackchan-local-turn.active"
LOCAL_RUNTIME_MARKER = "/run/stackchan-local-runtime.active"
LOCAL_RUNTIME_HELPER = "/opt/stackchan/stackchan_local_runtime.sh"


@dataclass(frozen=True)
class SerialCounters:
    tx: int
    rx: int


def stackflow_ping(host: str, port: int, timeout: float) -> Tuple[bool, str]:
    request_id = f"stackchan-watchdog-{time.monotonic_ns()}"
    request = {
        "request_id": request_id,
        "work_id": "sys",
        "action": "ping",
    }

    try:
        with socket.create_connection((host, port), timeout=timeout) as connection:
            connection.settimeout(timeout)
            payload = json.dumps(request, separators=(",", ":")).encode("utf-8") + b"\n"
            connection.sendall(payload)

            response = bytearray()
            while b"\n" not in response:
                chunk = connection.recv(4096)
                if not chunk:
                    return False, "connection closed before a response"
                response.extend(chunk)
                if len(response) > MAX_RESPONSE_BYTES:
                    return False, "response exceeded size limit"
    except (OSError, TimeoutError) as error:
        return False, str(error)

    try:
        line = bytes(response).split(b"\n", 1)[0]
        body = json.loads(line.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        return False, f"invalid response: {error}"

    if body.get("request_id") != request_id:
        return False, "response request_id did not match"
    error = body.get("error")
    if not isinstance(error, dict) or error.get("code") != 0:
        return False, f"StackFlow error: {error!r}"
    return True, "ok"


def read_serial_counters(index: int) -> SerialCounters:
    pattern = re.compile(rf"^{index}:.*\btx:(\d+)\s+rx:(\d+)(?:\s|$)")
    with open("/proc/tty/driver/serial", encoding="ascii") as serial_info:
        for line in serial_info:
            match = pattern.search(line)
            if match:
                return SerialCounters(tx=int(match.group(1)), rx=int(match.group(2)))
    raise RuntimeError(f"UART {index} counters were not found")


def openjtalk_tts_active(proc_root: str = "/proc") -> bool:
    """Return true while the synchronous OpenJTalk bashexec is expected to be quiet."""
    pattern = os.path.join(proc_root, "[0-9]*", "cmdline")
    for path in glob.iglob(pattern):
        try:
            with open(path, "rb") as command_line:
                value = command_line.read(MAX_RESPONSE_BYTES)
        except OSError:
            continue
        if any(marker in value for marker in OPENJTALK_PROCESS_MARKERS):
            return True
    return False


def local_conversation_active(
    activity_marker: str = LOCAL_CONVERSATION_MARKER,
) -> bool:
    """Return true while CoreS3 has deliberately gated microphone input."""
    return os.path.exists(activity_marker)


def stackchan_uart_activity(
    proc_root: str = "/proc",
    activity_marker: str = LOCAL_CONVERSATION_MARKER,
) -> Optional[str]:
    """Describe legitimate work that can delay a CoreS3 UART response."""
    if openjtalk_tts_active(proc_root):
        return "OpenJTalk"
    if local_conversation_active(activity_marker):
        return "local conversation turn"
    return None


def run_systemctl(*arguments: str, timeout: float = 20.0) -> bool:
    try:
        result = subprocess.run(
            ["systemctl", *arguments],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        logging.error("systemctl %s failed: %s", " ".join(arguments), error)
        return False

    if result.returncode != 0:
        logging.error(
            "systemctl %s failed: %s",
            " ".join(arguments),
            result.stderr.strip() or f"exit {result.returncode}",
        )
        return False
    return True


def restore_local_runtime_if_requested(
    activity_marker: str = LOCAL_RUNTIME_MARKER,
    runtime_helper: str = LOCAL_RUNTIME_HELPER,
) -> bool:
    """Restore the inference services only when CoreS3 requested them."""
    if not os.path.exists(activity_marker):
        logging.info("local inference remains stopped after llm-sys recovery")
        return True

    if not os.access(runtime_helper, os.X_OK):
        logging.error("local runtime helper is unavailable: %s", runtime_helper)
        return False

    try:
        result = subprocess.run(
            [runtime_helper, "start"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=60,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        logging.error("local runtime restore failed: %s", error)
        return False

    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        logging.error(
            "local runtime restore failed: %s",
            detail or f"exit {result.returncode}",
        )
        return False

    logging.info("local inference restored after llm-sys recovery")
    return True


class LlmSysWatchdog:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.running = True

    def stop(self, _signum: int, _frame: object) -> None:
        self.running = False

    def wait_until_healthy(self, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while self.running and time.monotonic() < deadline:
            active = run_systemctl("is-active", "--quiet", "llm-sys.service", timeout=5)
            healthy, detail = stackflow_ping(
                self.args.host, self.args.port, self.args.ping_timeout
            )
            if active and healthy:
                return True
            logging.warning("llm-sys recovery check pending: %s", detail)
            time.sleep(1)
        return False

    def recover(self, reason: str) -> bool:
        logging.error("llm-sys recovery started: %s", reason)
        restarted = run_systemctl("restart", "llm-sys.service", timeout=20)
        if not restarted:
            logging.error("normal restart failed; escalating to SIGKILL")
            run_systemctl(
                "kill", "--kill-who=all", "--signal=KILL", "llm-sys.service", timeout=10
            )
            restarted = run_systemctl("start", "llm-sys.service", timeout=20)

        if not restarted or not self.wait_until_healthy(30):
            logging.error("llm-sys did not recover")
            return False

        if not restore_local_runtime_if_requested():
            logging.warning("llm-sys recovered, but local inference did not")
            return False

        logging.info("llm-sys recovery completed")
        return True

    def run(self) -> int:
        previous: Optional[SerialCounters] = None
        unanswered_since: Optional[float] = None
        unanswered_bytes = 0
        ping_failures = 0
        next_ping = 0.0
        grace_until = time.monotonic() + self.args.startup_grace
        last_recovery = float("-inf")
        recovery_deferred_for_activity = False
        deferred_activity: Optional[str] = None
        activity_deferred_since: Optional[float] = None

        logging.info(
            "watching llm-sys: ping=%ss/%s failures, UART%d timeout=%ss",
            self.args.interval,
            self.args.ping_failures,
            self.args.uart_index,
            self.args.uart_timeout,
        )

        while self.running:
            now = time.monotonic()
            reason: Optional[str] = None
            reason_is_uart_stall = False

            try:
                current = read_serial_counters(self.args.uart_index)
            except (OSError, RuntimeError) as error:
                logging.warning("UART counter check failed: %s", error)
                current = None

            if current is not None and previous is not None:
                counters_reset = current.tx < previous.tx or current.rx < previous.rx
                if counters_reset or current.tx > previous.tx:
                    unanswered_since = None
                    unanswered_bytes = 0
                    if recovery_deferred_for_activity:
                        logging.info("UART response resumed after deferred StackChan activity")
                        recovery_deferred_for_activity = False
                        deferred_activity = None
                        activity_deferred_since = None
                elif current.rx > previous.rx:
                    if unanswered_since is None:
                        unanswered_since = now
                    unanswered_bytes += current.rx - previous.rx

                if (
                    now >= grace_until
                    and unanswered_since is not None
                    and unanswered_bytes >= MIN_UNANSWERED_UART_BYTES
                    and now - unanswered_since >= self.args.uart_timeout
                ):
                    reason = (
                        f"UART{self.args.uart_index} received {unanswered_bytes} bytes "
                        f"without TX progress for {now - unanswered_since:.1f}s"
                    )
                    reason_is_uart_stall = True
            previous = current

            if now >= next_ping:
                healthy, detail = stackflow_ping(
                    self.args.host, self.args.port, self.args.ping_timeout
                )
                next_ping = now + self.args.interval
                if healthy:
                    if ping_failures:
                        logging.info("local StackFlow ping recovered")
                    ping_failures = 0
                else:
                    ping_failures += 1
                    logging.warning(
                        "local StackFlow ping failed (%d/%d): %s",
                        ping_failures,
                        self.args.ping_failures,
                        detail,
                    )
                    if now >= grace_until and ping_failures >= self.args.ping_failures:
                        reason = f"local StackFlow ping failed {ping_failures} times: {detail}"
                        reason_is_uart_stall = False

            activity = stackchan_uart_activity() if reason_is_uart_stall else None
            if reason is not None and activity is not None:
                if activity != deferred_activity or activity_deferred_since is None:
                    deferred_activity = activity
                    activity_deferred_since = now
                grace = (
                    self.args.openjtalk_grace
                    if activity == "OpenJTalk"
                    else self.args.local_turn_grace
                )
                if now - activity_deferred_since < grace:
                    if not recovery_deferred_for_activity:
                        logging.info(
                            "llm-sys UART recovery deferred during %s: %s",
                            activity,
                            reason,
                        )
                    recovery_deferred_for_activity = True
                    reason = None
                    # Long-running inference and synchronous sys.bashexec
                    # commands may not return a UART frame until the current
                    # turn finishes. Restart the timeout window so a genuine
                    # failure is recovered after the gate reopens.
                    unanswered_since = now
                    ping_failures = 0
                else:
                    logging.info(
                        "llm-sys UART deferral expired during %s after %.1fs",
                        activity,
                        now - activity_deferred_since,
                    )
                    recovery_deferred_for_activity = False

            if reason is not None and now - last_recovery >= self.args.cooldown:
                self.recover(reason)
                last_recovery = time.monotonic()
                grace_until = last_recovery + self.args.startup_grace
                next_ping = last_recovery + self.args.interval
                previous = None
                unanswered_since = None
                unanswered_bytes = 0
                ping_failures = 0
                recovery_deferred_for_activity = False
                deferred_activity = None
                activity_deferred_since = None

            time.sleep(1)

        logging.info("watchdog stopped")
        return 0


def positive_number(value: str) -> float:
    number = float(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return number


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="run one health check and exit")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--uart-index", type=int, default=DEFAULT_UART_INDEX)
    parser.add_argument("--interval", type=positive_number, default=5.0)
    parser.add_argument("--ping-timeout", type=positive_number, default=2.0)
    parser.add_argument("--ping-failures", type=int, default=4)
    parser.add_argument("--uart-timeout", type=positive_number, default=30.0)
    parser.add_argument("--startup-grace", type=positive_number, default=30.0)
    parser.add_argument("--cooldown", type=positive_number, default=60.0)
    parser.add_argument(
        "--local-turn-grace",
        type=positive_number,
        default=DEFAULT_LOCAL_TURN_GRACE,
    )
    parser.add_argument(
        "--openjtalk-grace",
        type=positive_number,
        default=DEFAULT_OPENJTALK_GRACE,
    )
    args = parser.parse_args()
    if args.port <= 0 or args.port > 65535:
        parser.error("--port must be between 1 and 65535")
    if args.uart_index < 0:
        parser.error("--uart-index must be non-negative")
    if args.ping_failures <= 0:
        parser.error("--ping-failures must be greater than zero")
    return args


def main() -> int:
    args = parse_args()
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    try:
        counters = read_serial_counters(args.uart_index)
    except (OSError, RuntimeError) as error:
        logging.error("UART counter check failed: %s", error)
        return 1

    healthy, detail = stackflow_ping(args.host, args.port, args.ping_timeout)
    if args.check:
        if not healthy:
            logging.error("local StackFlow ping failed: %s", detail)
            return 1
        print(f"{READY_MARKER} uart={args.uart_index} tx={counters.tx} rx={counters.rx}")
        return 0

    watchdog = LlmSysWatchdog(args)
    signal.signal(signal.SIGTERM, watchdog.stop)
    signal.signal(signal.SIGINT, watchdog.stop)
    return watchdog.run()


if __name__ == "__main__":
    sys.exit(main())
