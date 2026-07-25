import os
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_repos


class GitCommandTest(unittest.TestCase):
    @mock.patch("fetch_repos.subprocess.run")
    def test_git_commands_are_bounded_and_noninteractive(self, run):
        run.return_value = subprocess.CompletedProcess(["git", "status"], 0)

        fetch_repos._run_git(
            ["status"], repo_name="test-repo", operation="status", check=True
        )

        command = run.call_args.args[0]
        options = run.call_args.kwargs
        self.assertEqual(command, ["git", "status"])
        self.assertEqual(options["timeout"], fetch_repos.DEFAULT_GIT_TIMEOUT_SECONDS)
        self.assertEqual(options["env"]["GIT_TERMINAL_PROMPT"], "0")
        self.assertEqual(options["env"]["GCM_INTERACTIVE"], "Never")

    @mock.patch("fetch_repos.subprocess.run")
    def test_timeout_identifies_repository_and_operation(self, run):
        run.side_effect = subprocess.TimeoutExpired(["git", "fetch"], 7)

        with mock.patch.dict(
            os.environ, {fetch_repos.GIT_TIMEOUT_ENV: "7"}, clear=False
        ):
            with self.assertRaisesRegex(
                RuntimeError, "Git fetch timed out after 7 seconds for esp-now"
            ):
                fetch_repos._run_git(
                    ["fetch"],
                    repo_name="esp-now",
                    operation="fetch",
                    check=True,
                )

    def test_timeout_must_be_a_positive_integer(self):
        for invalid_timeout in ("0", "-1", "forever"):
            with self.subTest(invalid_timeout=invalid_timeout):
                with mock.patch.dict(
                    os.environ,
                    {fetch_repos.GIT_TIMEOUT_ENV: invalid_timeout},
                    clear=False,
                ):
                    with self.assertRaises(RuntimeError):
                        fetch_repos._git_timeout_seconds()


if __name__ == "__main__":
    unittest.main()
