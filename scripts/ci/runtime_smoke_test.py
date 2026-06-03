#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import queue
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = REPO_ROOT / "bin"
RUNTIME_NAME = "Index-Runtime.exe" if os.name == "nt" else "Index-Runtime"
DEFAULT_TIMEOUT_SECONDS = 45.0
TERMINATE_GRACE_SECONDS = 5.0
REQUIRED_MARKERS = (
    "Loaded project:",
    "Loaded scene: SampleScene",
)
FORBIDDEN_MARKERS = (
    "Failed to load default texture",
    "IndexAssets/Textures not found",
    "Texture 'Default/",
    "Failed to start scene 'SampleScene'",
    "Scene destroy failed for 'SampleScene'",
)


def fail(message: str, log: str | None = None) -> int:
    print(message, file=sys.stderr)
    if log:
        print("---- runtime smoke log ----", file=sys.stderr)
        print(log, file=sys.stderr)
        print("---------------------------", file=sys.stderr)
    return 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the Index runtime smoke test.")
    parser.add_argument(
        "--binary",
        type=Path,
        help="Exact Index-Runtime binary to test. Falls back to bin/ auto-discovery when omitted.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help=f"Seconds to wait for the smoke-test markers (default: {DEFAULT_TIMEOUT_SECONDS:g}).",
    )
    return parser.parse_args()


def find_runtime_binary() -> Path:
    if not BIN_DIR.is_dir():
        raise FileNotFoundError(f"Expected bin directory at: {BIN_DIR}")

    candidates = [
        path for path in BIN_DIR.rglob(RUNTIME_NAME)
        if path.is_file() and path.parent.name == "Index-Runtime"
    ]
    if not candidates:
        raise FileNotFoundError(f"{RUNTIME_NAME} was not found under {BIN_DIR}")

    def rank(path: Path) -> tuple[int, float]:
        runtime_dir = path.parent
        build_name = runtime_dir.parent.name.lower()

        score = 0
        if (runtime_dir / "IndexAssets").is_dir():
            score += 100
        if build_name.startswith("release-"):
            score += 50
        elif build_name.startswith("debug-"):
            score += 40
        elif build_name.startswith("dist-"):
            score += 30

        return score, path.stat().st_mtime

    return max(candidates, key=rank)


def stage_smoke_project(runtime_dir: Path) -> None:
    shutil.rmtree(runtime_dir / "Assets", ignore_errors=True)
    project_file = runtime_dir / "index-project.json"
    if project_file.exists():
        project_file.unlink()

    scenes_dir = runtime_dir / "Assets" / "Scenes"
    scenes_dir.mkdir(parents=True, exist_ok=True)

    (runtime_dir / "index-project.json").write_text(
        json.dumps(
            {
                "name": "SmokeProject",
                "engineVersion": "ci",
                "startupScene": "SampleScene",
                "lastOpenedScene": "SampleScene",
                "buildScenes": ["SampleScene"],
            },
            indent=2,
        ),
        encoding="utf-8",
    )

    scene_definition = {
        "version": 1,
        "name": "SampleScene",
        "entities": [
            {
                "name": "Main Camera",
                "Transform2D": {
                    "posX": 0.0,
                    "posY": 0.0,
                    "rotation": 0.0,
                    "scaleX": 1.0,
                    "scaleY": 1.0,
                },
                "Camera2D": {
                    "orthoSize": 5.0,
                    "zoom": 1.0,
                    "clearR": 0.1,
                    "clearG": 0.1,
                    "clearB": 0.1,
                    "clearA": 1.0,
                },
            },
            {
                "name": "Pixel Sprite",
                "Transform2D": {
                    "posX": 0.0,
                    "posY": 0.0,
                    "rotation": 0.0,
                    "scaleX": 1.0,
                    "scaleY": 1.0,
                },
                "SpriteRenderer": {
                    "r": 1.0,
                    "g": 1.0,
                    "b": 1.0,
                    "a": 1.0,
                    "sortOrder": 0,
                    "sortLayer": 0,
                    "texture": "Default/Pixel.png",
                },
            },
            {
                "name": "Physics Crate",
                "Transform2D": {
                    "posX": 0.0,
                    "posY": 1.0,
                    "rotation": 0.0,
                    "scaleX": 1.0,
                    "scaleY": 1.0,
                },
                "Rigidbody2D": {
                    "bodyType": 2,
                    "gravityScale": 1.0,
                    "mass": 1.0,
                },
                "BoxCollider2D": {
                    "scaleX": 1.0,
                    "scaleY": 1.0,
                    "centerX": 0.0,
                    "centerY": 0.0,
                    "friction": 0.3,
                    "bounciness": 0.0,
                    "layer": 0,
                    "registerContacts": False,
                    "sensor": False,
                },
            },
        ],
    }

    (scenes_dir / "SampleScene.scene").write_text(
        json.dumps(scene_definition, indent=2),
        encoding="utf-8",
    )


def build_command(runtime_binary: Path) -> list[str]:
    if os.name != "nt":
        xvfb_run = shutil.which("xvfb-run")
        if xvfb_run:
            return [xvfb_run, "-a", str(runtime_binary)]

    return [str(runtime_binary)]


def terminate_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return

    if os.name == "nt":
        process.terminate()
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass

    try:
        process.wait(timeout=TERMINATE_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        if os.name == "nt":
            process.kill()
        else:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait(timeout=TERMINATE_GRACE_SECONDS)


def enqueue_output(stream, output_queue: queue.Queue[str | None]) -> None:
    try:
        for line in stream:
            output_queue.put(line)
    finally:
        output_queue.put(None)


def run_runtime(runtime_binary: Path, timeout_seconds: float) -> tuple[int, str]:
    command = build_command(runtime_binary)
    creation_flags = 0
    if os.name == "nt":
        creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    popen_kwargs = {
        "cwd": runtime_binary.parent,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.STDOUT,
        "text": True,
        "encoding": "utf-8",
        "errors": "replace",
        "creationflags": creation_flags,
    }
    if os.name != "nt":
        popen_kwargs["start_new_session"] = True

    process = subprocess.Popen(command, **popen_kwargs)
    output_queue: queue.Queue[str | None] = queue.Queue()
    reader = threading.Thread(
        target=enqueue_output,
        args=(process.stdout, output_queue),
        daemon=True,
    )
    reader.start()

    log_lines: list[str] = []
    seen_markers: set[str] = set()
    deadline = time.monotonic() + max(timeout_seconds, 1.0)
    reader_done = False

    while not reader_done:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            terminate_process(process)
            while not output_queue.empty():
                item = output_queue.get_nowait()
                if item is not None:
                    log_lines.append(item)
            return 124, "".join(log_lines)

        try:
            item = output_queue.get(timeout=min(0.1, remaining))
        except queue.Empty:
            continue

        if item is None:
            reader_done = True
            continue

        log_lines.append(item)
        for marker in REQUIRED_MARKERS:
            if marker in item:
                seen_markers.add(marker)

        if all(marker in seen_markers for marker in REQUIRED_MARKERS):
            terminate_process(process)
            while not output_queue.empty():
                extra = output_queue.get_nowait()
                if extra is not None:
                    log_lines.append(extra)
            return 0, "".join(log_lines)

    return process.returncode or 0, "".join(log_lines)


def main() -> int:
    args = parse_args()
    if args.binary:
        runtime_binary = args.binary.resolve()
        if not runtime_binary.is_file():
            return fail(f"Runtime binary was not found: {runtime_binary}")
    else:
        try:
            runtime_binary = find_runtime_binary()
        except FileNotFoundError as error:
            return fail(str(error))

    runtime_dir = runtime_binary.parent
    index_assets_dir = runtime_dir / "IndexAssets"
    if not index_assets_dir.is_dir():
        return fail(f"Expected IndexAssets next to the runtime binary at {index_assets_dir}")

    stage_smoke_project(runtime_dir)
    exit_code, log_output = run_runtime(runtime_binary, args.timeout)

    if exit_code not in (0, 124):
        return fail(f"Smoke test process exited with status {exit_code}", log_output)

    for marker in REQUIRED_MARKERS:
        if marker not in log_output:
            return fail(f"Runtime did not emit expected marker: {marker}", log_output)

    for marker in FORBIDDEN_MARKERS:
        if marker in log_output:
            return fail(f"Runtime log contained failure marker: {marker}", log_output)

    print(f"Runtime smoke test passed with binary: {runtime_binary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
