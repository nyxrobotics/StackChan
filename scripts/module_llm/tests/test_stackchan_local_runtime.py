import os
import subprocess
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).parents[1] / "stackchan_local_runtime.sh"
LOCAL_SERVICES = (
    "llm-audio.service",
    "llm-vad.service",
    "llm-whisper.service",
    "llm-llm.service",
    "llm-melotts.service",
    "stackchan-vad-pcm-bridge.service",
)


class LocalRuntimeScriptTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self.state = self.root / "systemctl-state"
        self.state.mkdir()
        self.marker = self.root / "local-runtime.active"
        self.pause = self.root / "vad-bridge.paused"
        self.local_turn = self.root / "local-turn.active"
        self.systemctl = self.root / "systemctl"
        self.systemctl.write_text(
            """#!/bin/sh
set -eu
state=$STACKCHAN_TEST_SYSTEMCTL_STATE
command=$1
shift
case "$command" in
    cat)
        exit 0
        ;;
    is-active)
        if [ "${1:-}" = "--quiet" ]; then shift; fi
        test -f "$state/$1.active"
        ;;
    start)
        for service in "$@"; do touch "$state/$service.active"; done
        ;;
    stop)
        for service in "$@"; do rm -f "$state/$service.active"; done
        ;;
    *)
        echo "unsupported systemctl command: $command" >&2
        exit 2
        ;;
esac
""",
            encoding="ascii",
        )
        self.systemctl.chmod(0o755)
        (self.state / "llm-sys.service.active").touch()

        self.environment = os.environ.copy()
        self.environment.update(
            {
                "STACKCHAN_SYSTEMCTL": str(self.systemctl),
                "STACKCHAN_TEST_SYSTEMCTL_STATE": str(self.state),
                "STACKCHAN_LOCAL_RUNTIME_ACTIVE_FILE": str(self.marker),
                "STACKCHAN_VAD_PCM_BRIDGE_PAUSE_FILE": str(self.pause),
                "STACKCHAN_LOCAL_TURN_FILE": str(self.local_turn),
                "STACKCHAN_SKIP_PROCESS_KILL": "1",
            }
        )

    def tearDown(self):
        self.temporary_directory.cleanup()

    def run_script(self, command):
        return subprocess.run(
            ["bash", str(SCRIPT_PATH), command],
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
        )

    def test_s3_can_start_and_stop_all_inference_services(self):
        started = self.run_script("start")
        self.assertEqual(started.returncode, 0, started.stderr)
        self.assertIn("STACKCHAN_LOCAL_RUNTIME_STARTED", started.stdout)
        self.assertIn("STACKCHAN_LOCAL_RUNTIME_RUNNING", started.stdout)
        self.assertTrue(self.marker.exists())
        self.assertTrue(self.pause.exists())
        for service in LOCAL_SERVICES:
            self.assertTrue((self.state / f"{service}.active").exists())

        started_again = self.run_script("start")
        self.assertEqual(started_again.returncode, 0, started_again.stderr)
        self.assertNotIn("STACKCHAN_LOCAL_RUNTIME_STARTED", started_again.stdout)
        self.assertIn("STACKCHAN_LOCAL_RUNTIME_RUNNING", started_again.stdout)

        status = self.run_script("status")
        self.assertEqual(status.returncode, 0, status.stderr)
        self.assertIn("STACKCHAN_LOCAL_RUNTIME_RUNNING", status.stdout)

        self.local_turn.touch()
        stopped = self.run_script("stop")
        self.assertEqual(stopped.returncode, 0, stopped.stderr)
        self.assertIn("STACKCHAN_LOCAL_RUNTIME_STOPPED", stopped.stdout)
        self.assertFalse(self.marker.exists())
        self.assertFalse(self.local_turn.exists())
        self.assertTrue(self.pause.exists())
        for service in LOCAL_SERVICES:
            self.assertFalse((self.state / f"{service}.active").exists())

    def test_status_rejects_partial_runtime(self):
        (self.state / "llm-audio.service.active").touch()
        status = self.run_script("status")
        self.assertNotEqual(status.returncode, 0)
        self.assertIn("STACKCHAN_LOCAL_RUNTIME_PARTIAL", status.stderr)

    def test_start_requires_control_service(self):
        (self.state / "llm-sys.service.active").unlink()
        started = self.run_script("start")
        self.assertNotEqual(started.returncode, 0)
        self.assertIn("llm-sys is not active", started.stderr)


if __name__ == "__main__":
    unittest.main()
