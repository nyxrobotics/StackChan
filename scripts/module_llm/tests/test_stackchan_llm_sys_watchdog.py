import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).parents[1] / "stackchan_llm_sys_watchdog.py"
SPEC = importlib.util.spec_from_file_location("stackchan_llm_sys_watchdog", SCRIPT_PATH)
WATCHDOG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WATCHDOG)


class OpenJTalkActivityTest(unittest.TestCase):
    def write_command(self, root, pid, *arguments):
        process = Path(root) / str(pid)
        process.mkdir()
        (process / "cmdline").write_bytes(b"\0".join(arguments) + b"\0")

    def test_detects_openjtalk_wrapper(self):
        with tempfile.TemporaryDirectory() as proc_root:
            self.write_command(
                proc_root,
                101,
                b"bash",
                b"/opt/stackchan/openjtalk_tts.sh",
                b"--text",
                b"test",
            )
            self.assertTrue(WATCHDOG.openjtalk_tts_active(proc_root))

    def test_detects_openjtalk_playback(self):
        with tempfile.TemporaryDirectory() as proc_root:
            self.write_command(
                proc_root,
                102,
                b"aplay",
                b"/tmp/stackchan-openjtalk/audio.1234",
            )
            self.assertTrue(WATCHDOG.openjtalk_tts_active(proc_root))

    def test_ignores_unrelated_processes(self):
        with tempfile.TemporaryDirectory() as proc_root:
            self.write_command(proc_root, 103, b"/opt/m5stack/bin/llm_sys-1.6")
            self.assertFalse(WATCHDOG.openjtalk_tts_active(proc_root))


class LocalConversationActivityTest(unittest.TestCase):
    def test_detects_local_conversation_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            activity_marker = Path(directory) / "local-turn.active"
            activity_marker.write_text("active\n", encoding="ascii")
            self.assertTrue(
                WATCHDOG.local_conversation_active(str(activity_marker))
            )

    def test_ignores_missing_local_conversation_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            activity_marker = Path(directory) / "local-turn.active"
            self.assertFalse(
                WATCHDOG.local_conversation_active(str(activity_marker))
            )

    def test_reports_local_turn_as_uart_activity(self):
        with tempfile.TemporaryDirectory() as directory:
            proc_root = Path(directory) / "proc"
            proc_root.mkdir()
            activity_marker = Path(directory) / "local-turn.active"
            activity_marker.write_text("active\n", encoding="ascii")
            self.assertEqual(
                WATCHDOG.stackchan_uart_activity(
                    str(proc_root), str(activity_marker)
                ),
                "local conversation turn",
            )


if __name__ == "__main__":
    unittest.main()
