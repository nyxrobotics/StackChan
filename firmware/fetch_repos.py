import os
import subprocess
import json

GIT_TIMEOUT_ENV = "STACKCHAN_GIT_TIMEOUT_SECONDS"
DEFAULT_GIT_TIMEOUT_SECONDS = 300


def _git_timeout_seconds():
    raw_timeout = os.environ.get(
        GIT_TIMEOUT_ENV, str(DEFAULT_GIT_TIMEOUT_SECONDS)
    )
    try:
        timeout = int(raw_timeout)
    except ValueError as error:
        raise RuntimeError(
            f"{GIT_TIMEOUT_ENV} must be a positive integer, got {raw_timeout!r}"
        ) from error
    if timeout <= 0:
        raise RuntimeError(
            f"{GIT_TIMEOUT_ENV} must be greater than zero, got {timeout}"
        )
    return timeout


def _run_git(args, *, repo_name, operation, **kwargs):
    env = os.environ.copy()
    env["GIT_TERMINAL_PROMPT"] = "0"
    env["GCM_INTERACTIVE"] = "Never"
    timeout = _git_timeout_seconds()
    try:
        return subprocess.run(
            ["git", *args],
            env=env,
            timeout=timeout,
            **kwargs,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"Git {operation} timed out after {timeout} seconds for {repo_name}"
        ) from error


def clone_or_update_repo(
    repo_url, path, ref=None, with_submodules=False, patch_path=None
):
    repo_name = os.path.basename(os.path.normpath(path)) or path

    if not os.path.exists(path):
        _run_git(
            ["clone", repo_url, path],
            repo_name=repo_name,
            operation="clone",
            check=True,
        )
    else:
        _run_git(
            ["-C", path, "fetch"],
            repo_name=repo_name,
            operation="fetch",
            check=True,
        )

    if ref:
        _run_git(
            ["-C", path, "checkout", ref],
            repo_name=repo_name,
            operation="checkout",
            check=True,
        )

    if with_submodules:
        _run_git(
            ["-C", path, "submodule", "update", "--init", "--recursive"],
            repo_name=repo_name,
            operation="submodule update",
            check=True,
        )

    # 应用 patch
    if patch_path:
        patch_full_path = (
            patch_path
            if os.path.isabs(patch_path)
            else os.path.join(os.getcwd(), patch_path)
        )
        # 使用 git apply --check 先检测补丁是否能应用，避免报错
        forward_check = _run_git(
            ["-C", path, "apply", "--check", patch_full_path],
            repo_name=repo_name,
            operation="patch check",
            capture_output=True,
            text=True,
        )
        if forward_check.returncode == 0:
            _run_git(
                ["-C", path, "apply", patch_full_path],
                repo_name=repo_name,
                operation="patch apply",
                check=True,
            )
            print(f"Applied patch {patch_path} to {path}")
            return

        reverse_check = _run_git(
            ["-C", path, "apply", "--reverse", "--check", patch_full_path],
            repo_name=repo_name,
            operation="reverse patch check",
            capture_output=True,
            text=True,
        )
        if reverse_check.returncode == 0:
            print(f"Patch {patch_path} is already applied to {path}")
            return

        raise RuntimeError(
            f"Patch {patch_path} matches neither the clean nor patched state in {path}.\n"
            f"Forward check: {forward_check.stderr.strip()}\n"
            f"Reverse check: {reverse_check.stderr.strip()}"
        )


def fetch_dependencies():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    config_path = os.path.join(script_dir, "repos.json")

    with open(config_path) as f:
        repos = json.load(f)

    for repo in repos:
        repo_path = os.path.join(script_dir, repo["path"])
        branch = repo.get("branch")
        with_submodules = repo.get("with_submodules", False)
        patch = repo.get("patch")
        if patch and not os.path.isabs(patch):
            patch = os.path.join(script_dir, patch)
        clone_or_update_repo(repo["url"], repo_path, branch, with_submodules, patch)


if __name__ == "__main__":
    fetch_dependencies()
