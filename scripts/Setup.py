#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

MINIMUM_PYTHON = (3, 10)


def ensure_supported_python() -> None:
    if sys.version_info < MINIMUM_PYTHON:
        required = ".".join(str(part) for part in MINIMUM_PYTHON)
        found = ".".join(str(part) for part in sys.version_info[:3])
        raise SystemExit(
            f"[Index Setup] Python {required}+ is required. "
            f"Found Python {found}. Please install a newer Python version and try again."
        )


def run_step(cmd: list[str], cwd: Path, label: str, allow_failure: bool = False) -> bool:
    print(f"[Index Setup] {label}...")
    print(f"[Index Setup] > {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=cwd)
    if result.returncode != 0 and not allow_failure:
        raise RuntimeError(f"Step failed ({label}) with exit code {result.returncode}.")
    return result.returncode == 0


def check_tool(name: str, cmd: str | None = None) -> bool:
    """Return True if *name* is reachable on PATH."""
    exe = shutil.which(cmd or name)
    if exe:
        print(f"  [OK] {name} found: {exe}")
    else:
        print(f"  [!!] {name} NOT found on PATH")
    return exe is not None


# ── Auto-install support ──────────────────────────────────────────────────────
# winget package IDs (Windows) for prerequisites we can install on the user's
# behalf. premake is handled separately (download into vendor/bin); Python is
# installed by the shell wrapper before Setup.py runs.
WINGET_PACKAGE_IDS = {
    "git": "Git.Git",
    "dotnet": "Microsoft.DotNet.SDK.9",
}
# Linux package names per tool, with per-manager overrides keyed by the manager
# executable (see detect_linux_package_manager); "default" covers apt/dnf/zypper.
LINUX_PACKAGES = {
    "git": {"default": ["git"]},
    "dotnet": {"default": ["dotnet-sdk-9.0"], "pacman": ["dotnet-sdk"]},
}
# Fallback premake build, downloaded only when vendor/bin/premake5 is absent AND
# premake5 is not on PATH. Bump if the vendored toolchain moves on.
PREMAKE_FALLBACK_VERSION = "5.0.0-beta2"


def is_interactive() -> bool:
    return bool(sys.stdin) and sys.stdin.isatty()


def prompt_yes_no(question: str, assume_yes: bool) -> bool:
    """Ask a [Y/n] question (default yes). Returns False non-interactively
    unless *assume_yes* short-circuits the prompt."""
    if assume_yes:
        print(f"{question} [Y/n] y")
        return True
    if not is_interactive():
        return False
    try:
        answer = input(f"{question} [Y/n] ").strip().lower()
    except EOFError:
        return False
    return answer in ("", "y", "yes")


def refresh_windows_path() -> None:
    """Reload PATH from the registry so a tool installed during this run becomes
    resolvable (and inheritable by child processes) without reopening the shell."""
    if platform.system() != "Windows":
        return
    import winreg

    collected: list[str] = []
    for hive, subkey in (
        (winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"),
        (winreg.HKEY_CURRENT_USER, "Environment"),
    ):
        try:
            with winreg.OpenKey(hive, subkey) as key:
                value, _ = winreg.QueryValueEx(key, "Path")
        except OSError:
            continue
        if value:
            collected.append(os.path.expandvars(value))

    merged = os.pathsep.join(collected + [os.environ.get("PATH", "")])
    seen: set[str] = set()
    ordered: list[str] = []
    for part in merged.split(os.pathsep):
        if part and part not in seen:
            seen.add(part)
            ordered.append(part)
    if ordered:
        os.environ["PATH"] = os.pathsep.join(ordered)


def detect_linux_package_manager() -> tuple[str, list[str]] | None:
    """Return (name, install-command-prefix) for the host package manager."""
    for exe, install_cmd in (
        ("apt-get", ["apt-get", "install", "-y"]),
        ("dnf", ["dnf", "install", "-y"]),
        ("pacman", ["pacman", "-S", "--noconfirm"]),
        ("zypper", ["zypper", "install", "-y"]),
    ):
        if shutil.which(exe):
            return exe, install_cmd
    return None


def winget_install(package_id: str) -> bool:
    if not shutil.which("winget"):
        print("  [!!] winget (App Installer) was not found, so auto-install is unavailable.")
        print("       Install 'App Installer' from the Microsoft Store, then re-run setup.")
        return False
    cmd = [
        "winget", "install", "--exact", "--id", package_id,
        "--accept-package-agreements", "--accept-source-agreements",
        "--disable-interactivity",
    ]
    print(f"  [..] winget install {package_id}")
    return subprocess.run(cmd).returncode == 0


def linux_package_install(tool: str) -> bool:
    manager = detect_linux_package_manager()
    if not manager:
        print("  [!!] No supported package manager (apt/dnf/pacman/zypper) was found.")
        return False
    name, install_cmd = manager
    spec = LINUX_PACKAGES.get(tool)
    packages = spec.get(name, spec["default"]) if spec else None
    if not packages:
        print(f"  [!!] No known {name} package for {tool}.")
        return False
    cmd = list(install_cmd) + packages
    if hasattr(os, "geteuid") and os.geteuid() != 0:
        if shutil.which("sudo"):
            cmd = ["sudo", *cmd]
        else:
            print("  [!!] Root privileges are required but 'sudo' is unavailable.")
            return False
    print(f"  [..] {name} install {' '.join(packages)}")
    return subprocess.run(cmd).returncode == 0


def install_premake(repo_root: Path) -> str | None:
    """Download a fallback premake build into vendor/bin and return its path."""
    import tarfile
    import tempfile
    import urllib.request
    import zipfile

    system = platform.system()
    version = PREMAKE_FALLBACK_VERSION
    if system == "Windows":
        asset, exe_name = f"premake-{version}-windows.zip", "premake5.exe"
    elif system == "Darwin":
        asset, exe_name = f"premake-{version}-macosx.tar.gz", "premake5"
    else:
        asset, exe_name = f"premake-{version}-linux.tar.gz", "premake5"
    url = f"https://github.com/premake/premake-core/releases/download/v{version}/{asset}"

    dest_dir = repo_root / "vendor" / "bin"
    dest_dir.mkdir(parents=True, exist_ok=True)
    print(f"  [..] Downloading premake {version}: {url}")
    try:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            archive = tmp_path / asset
            urllib.request.urlretrieve(url, archive)
            if asset.endswith(".zip"):
                with zipfile.ZipFile(archive) as zf:
                    zf.extractall(tmp_path)
            else:
                with tarfile.open(archive) as tf:
                    tf.extractall(tmp_path)
            sources = [p for p in tmp_path.rglob(exe_name) if p.is_file()]
            if not sources:
                print("  [!!] premake executable missing from the downloaded archive.")
                return None
            final = dest_dir / exe_name
            shutil.copy2(sources[0], final)
            if system != "Windows":
                final.chmod(0o755)
    except Exception as exc:  # network, extraction, permissions
        print(f"  [!!] premake download failed: {exc}")
        return None
    print(f"  [OK] premake installed: {final}")
    return str(final)


def offer_install(tool: str, install_enabled: bool, assume_yes: bool) -> bool:
    """Offer to install *tool* (git/dotnet) via the platform package manager.
    Returns True only if the tool is reachable afterwards."""
    if not install_enabled:
        return False
    if not (assume_yes or is_interactive()):
        return False
    if not prompt_yes_no(f"[Index Setup] {tool} is missing. Install it automatically?", assume_yes):
        return False

    if platform.system() == "Windows":
        package = WINGET_PACKAGE_IDS.get(tool)
        installed = winget_install(package) if package else False
        if installed:
            refresh_windows_path()
    else:
        installed = linux_package_install(tool)

    if not installed:
        print(f"  [!!] Could not install {tool} automatically; please install it manually.")
        return False

    resolved = shutil.which(tool)
    if resolved:
        print(f"  [OK] {tool} ready: {resolved}")
        return True
    print(f"  [!!] {tool} installed but not yet visible on PATH; please re-run setup.")
    return False


def submodules_need_update(repo_root: Path) -> bool:
    """Return True if any registered submodule is missing, conflicted, or stale."""
    gitmodules = repo_root / ".gitmodules"
    if not gitmodules.exists():
        return False
    result = subprocess.run(
        ["git", "submodule", "status", "--recursive"],
        cwd=repo_root,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return True
    for raw_line in result.stdout.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        # "-" = missing, "+" = wrong commit checked out, "U" = conflict
        if line[0] in {"-", "+", "U"}:
            return True
    return False


def dotnet_files_present(repo_root: Path) -> bool:
    """Return True if External/dotnet already has all required files."""
    dotnet_dir = repo_root / "External" / "dotnet"
    required = [
        dotnet_dir / "nethost.h",
        dotnet_dir / "hostfxr.h",
        dotnet_dir / "coreclr_delegates.h",
        dotnet_dir / "lib" / "nethost.lib",
        dotnet_dir / "lib" / "nethost.dll",
    ]
    return all(f.is_file() for f in required)


def resolve_premake_executable(repo_root: Path) -> str | None:
    if platform.system() == "Windows":
        vendored = repo_root / "vendor" / "bin" / "premake5.exe"
    else:
        vendored = repo_root / "vendor" / "bin" / "premake5"
    if vendored.is_file():
        return str(vendored)
    return shutil.which("premake5")


def describe_premake_expectation(repo_root: Path) -> str:
    if platform.system() == "Windows":
        vendored = repo_root / "vendor" / "bin" / "premake5.exe"
    else:
        vendored = repo_root / "vendor" / "bin" / "premake5"
    return (
        f"Premake 5 was not found. Expected vendored executable at '{vendored}' "
        "or a 'premake5' executable on PATH."
    )


def has_lfs_patterns(repo_root: Path) -> bool:
    """Return True if tracked attributes declare Git LFS filters."""
    gitattributes = repo_root / ".gitattributes"
    if not gitattributes.is_file():
        return False
    try:
        text = gitattributes.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return "filter=lfs" in text


def normalize_host_architecture(raw_arch: str | None) -> str:
    arch = (raw_arch or "").strip().lower()
    mapping = {
        "amd64": "x64",
        "x86_64": "x64",
        "x64": "x64",
        "arm64": "arm64",
        "aarch64": "arm64",
    }
    return mapping.get(arch, "")


def detect_dotnet_version(dotnet_root: Path, host_arch: str) -> str | None:
    """Find the latest installed .NET host pack version."""
    host_packs = dotnet_root / "packs" / f"Microsoft.NETCore.App.Host.win-{host_arch}"
    if not host_packs.is_dir():
        return None
    versions = sorted(
        [d.name for d in host_packs.iterdir() if d.is_dir()],
        reverse=True,
    )
    return versions[0] if versions else None


def detect_msbuild(script_dir: Path) -> str | None:
    """Locate MSBuild.exe via vswhere on Windows. Writes the resolved path to
    scripts/index-build-env.bat for downstream tools (e.g. RunTests.bat).

    Returns the resolved MSBuild path, or None if vswhere/MSBuild was not
    located. Non-fatal: failures are logged and the function returns None.
    """
    if platform.system() != "Windows":
        return None

    program_files_x86 = os.environ.get("ProgramFiles(x86)") or r"C:\Program Files (x86)"
    vswhere = Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        print(f"  [!!] vswhere not found at '{vswhere}' - skipping MSBuild detection")
        return None

    try:
        result = subprocess.run(
            [
                str(vswhere),
                "-latest",
                "-products", "*",
                "-requires", "Microsoft.Component.MSBuild",
                "-find", r"MSBuild\**\Bin\MSBuild.exe",
            ],
            capture_output=True,
            text=True,
        )
    except OSError as exc:
        print(f"  [!!] vswhere failed to launch: {exc}")
        return None

    if result.returncode != 0:
        print(f"  [!!] vswhere exited with code {result.returncode}; skipping MSBuild detection")
        return None

    candidates = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if not candidates:
        print("  [!!] vswhere did not return any MSBuild paths; skipping")
        return None

    msbuild_path = candidates[0]
    print(f"  [OK] MSBuild detected: {msbuild_path}")

    env_file = script_dir / "index-build-env.bat"
    try:
        env_file.write_text(
            "@echo off\r\n"
            "REM Auto-generated by scripts/Setup.py - do not edit by hand.\r\n"
            f"set \"MSBUILD_PATH={msbuild_path}\"\r\n",
            encoding="utf-8",
        )
        print(f"  [OK] Wrote build environment to '{env_file}'")
    except OSError as exc:
        print(f"  [!!] Failed to write '{env_file}': {exc}")

    return msbuild_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Set up Index dependencies and generate project files.")
    parser.add_argument(
        "--generator",
        default=os.environ.get("INDEX_PREMAKE_ACTION"),
        help="Premake action to generate (defaults to vs2022 on Windows, gmake2 elsewhere).",
    )
    parser.add_argument(
        "--dotnet-arch",
        default=os.environ.get("INDEX_DOTNET_ARCH"),
        help="Windows .NET host-pack architecture to copy (for example: x64 or arm64).",
    )
    parser.add_argument(
        "--skip-lfs",
        action="store_true",
        help="Skip Git LFS pull even if this checkout declares LFS attributes.",
    )
    parser.add_argument(
        "--require-lfs",
        action="store_true",
        help="Fail setup if Git LFS assets are declared and git lfs pull fails.",
    )
    parser.add_argument(
        "--module-profile",
        choices=("full", "core", "custom"),
        help="Premake Index module profile to generate.",
    )
    parser.add_argument(
        "--with-render",
        action="store_true",
        help="Forward --with-render to Premake when using a custom module profile.",
    )
    parser.add_argument(
        "--with-audio",
        action="store_true",
        help="Forward --with-audio to Premake when using a custom module profile.",
    )
    parser.add_argument(
        "--with-physics",
        action="store_true",
        help="Forward --with-physics to Premake when using a custom module profile.",
    )
    parser.add_argument(
        "--with-scripting",
        action="store_true",
        help="Forward --with-scripting to Premake when using a custom module profile.",
    )
    parser.add_argument(
        "--with-editor",
        action="store_true",
        help="Forward --with-editor to Premake. The editor profile enables its required runtime modules.",
    )
    parser.add_argument(
        "--with-imgui-demo",
        action="store_true",
        help="Forward --with-imgui-demo to Premake.",
    )
    parser.add_argument(
        "--premake-arg",
        action="append",
        default=[],
        help="Extra raw argument to pass to Premake. May be supplied more than once.",
    )
    parser.add_argument(
        "--ignore-prereqs",
        action="store_true",
        help=(
            "Treat missing prerequisites (git, python<3.10, premake, dotnet when "
            "scripting is wanted) as warnings instead of hard failures. By default, "
            "missing prerequisites abort setup so first-time contributors get a "
            "fast, actionable error."
        ),
    )
    parser.add_argument(
        "--assume-yes",
        "-y",
        action="store_true",
        help="Auto-confirm install prompts for missing prerequisites (for scripted/CI runs).",
    )
    parser.add_argument(
        "--no-install",
        action="store_true",
        help="Never offer to install missing prerequisites; only report them.",
    )
    return parser.parse_args()


def build_premake_args(args: argparse.Namespace) -> list[str]:
    premake_args: list[str] = []
    if args.module_profile:
        premake_args.append(f"--module-profile={args.module_profile}")
    if args.with_render:
        premake_args.append("--with-render")
    if args.with_audio:
        premake_args.append("--with-audio")
    if args.with_physics:
        premake_args.append("--with-physics")
    if args.with_scripting:
        premake_args.append("--with-scripting")
    if args.with_editor:
        premake_args.append("--with-editor")
    if args.with_imgui_demo:
        premake_args.append("--with-imgui-demo")
    premake_args.extend(args.premake_arg or [])
    return premake_args


def main() -> int:
    ensure_supported_python()
    args = parse_args()
    install_enabled = not args.no_install
    assume_yes = args.assume_yes
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent

    os.chdir(repo_root)
    print(f"[Index Setup] Repository root: {repo_root}")
    print(f"[Index Setup] Platform: {platform.system()}")
    print()

    os.environ["INDEX_DIR"] = str(repo_root)

    print("[Index Setup] Checking prerequisites...")
    # "required" prereqs hard-fail by default (override with --ignore-prereqs).
    # "soft" prereqs only warn -- they cover downstream resources that have
    # their own fallback paths (e.g. setup-dotnet.ps1 copies the host pack).
    required_missing: list[str] = []
    soft_missing: list[str] = []

    # The user opts out of scripting by selecting --module-profile=core, or by
    # selecting --module-profile=custom without --with-scripting. Otherwise
    # (default profile, --module-profile=full, --module-profile=custom
    # --with-scripting) scripting is part of the build and dotnet is required.
    scripting_wanted = not (
        args.module_profile == "core"
        or (args.module_profile == "custom" and not args.with_scripting)
    )

    if not check_tool("git"):
        if not offer_install("git", install_enabled, assume_yes):
            required_missing.append("git")

    if not check_tool("python", sys.executable):
        required_missing.append("python")

    premake_path = resolve_premake_executable(repo_root)
    if premake_path:
        print(f"  [OK] premake5 found: {premake_path}")
    else:
        print(f"  [!!] {describe_premake_expectation(repo_root)}")
        if (
            install_enabled
            and (assume_yes or is_interactive())
            and prompt_yes_no(
                "[Index Setup] premake5 is missing. Download it into vendor/bin now?",
                assume_yes,
            )
        ):
            premake_path = install_premake(repo_root)
        if not premake_path:
            required_missing.append("premake5")

    is_windows = platform.system() == "Windows"
    dotnet_exe = shutil.which("dotnet")

    if is_windows:
        dotnet_root = Path(os.environ.get("DOTNET_ROOT", r"C:\Program Files\dotnet"))
        dotnet_arch = normalize_host_architecture(args.dotnet_arch or platform.machine())
        if dotnet_exe:
            print(f"  [OK] dotnet found: {dotnet_exe}")
        else:
            print("  [!!] dotnet NOT found on PATH")
            if scripting_wanted:
                if offer_install("dotnet", install_enabled, assume_yes):
                    dotnet_exe = shutil.which("dotnet")
                else:
                    required_missing.append("dotnet")

        detected_ver = None
        if not dotnet_arch:
            print(f"  [!!] Unsupported Windows host architecture: {platform.machine()}")
            if not dotnet_files_present(repo_root):
                soft_missing.append("dotnet-host-pack")
        else:
            print(f"  [OK] Using .NET host architecture: {dotnet_arch}")
            os.environ["INDEX_DOTNET_ARCH"] = dotnet_arch
            detected_ver = detect_dotnet_version(dotnet_root, dotnet_arch)
        if detected_ver:
            print(f"  [OK] .NET host pack found: {detected_ver}")
        else:
            print("  [!!] No .NET host pack found - setup-dotnet will fail")
            if not dotnet_files_present(repo_root):
                soft_missing.append("dotnet-host-pack")
    else:
        # Non-Windows: dotnet is still required for scripting/Index-ScriptCore.
        if dotnet_exe:
            print(f"  [OK] dotnet found: {dotnet_exe}")
        else:
            print("  [!!] dotnet NOT found on PATH")
            if scripting_wanted:
                if offer_install("dotnet", install_enabled, assume_yes):
                    dotnet_exe = shutil.which("dotnet")
                else:
                    required_missing.append("dotnet")

    # E12: detect MSBuild via vswhere on Windows (non-fatal). Writes
    # scripts/index-build-env.bat with MSBUILD_PATH for downstream tools.
    if is_windows:
        detect_msbuild(script_dir)

    print()
    if soft_missing:
        # Deduplicate while preserving order.
        seen: set[str] = set()
        unique_soft = [m for m in soft_missing if not (m in seen or seen.add(m))]
        print(f"[Index Setup] WARNING: Optional resources missing: {', '.join(unique_soft)}")
        print("[Index Setup] Some downstream steps may fall back or fail; continuing.")
        print()

    if required_missing:
        if args.ignore_prereqs:
            print(f"[Index Setup] WARNING: Missing prerequisites: {', '.join(required_missing)}")
            print("[Index Setup] --ignore-prereqs supplied; continuing anyway. Some steps may fail.")
            print()
        else:
            print(f"[Index Setup] ERROR: Missing required prerequisites: {', '.join(required_missing)}")
            print("[Index Setup] Install the listed tools, or re-run with --ignore-prereqs to")
            print("[Index Setup] continue anyway (steps depending on the missing tool will fail).")
            return 1

    run_step(
        ["git", "submodule", "sync", "--recursive"],
        repo_root,
        "Syncing git submodule URLs",
    )
    if submodules_need_update(repo_root):
        run_step(
            ["git", "submodule", "update", "--init", "--recursive", "--jobs", "8"],
            repo_root,
            "Updating git submodules",
        )
    else:
        print("[Index Setup] Submodules already match the recorded revisions.")

    if args.skip_lfs:
        print("[Index Setup] Git LFS skipped by --skip-lfs.")
    elif has_lfs_patterns(repo_root):
        lfs_ok = run_step(
            ["git", "lfs", "pull"],
            repo_root,
            "Pulling Git LFS assets",
            allow_failure=not args.require_lfs,
        )
        if not lfs_ok:
            print("[Index Setup] Warning: git lfs pull failed or Git LFS is unavailable. Continuing...")
    else:
        print("[Index Setup] No Git LFS attributes found - skipping git lfs pull.")

    if is_windows:
        if dotnet_files_present(repo_root):
            print("[Index Setup] .NET hosting files already present - skipping.")
        else:
            print("[Index Setup] Setting up .NET hosting files...")
            ps_script = script_dir / "setup-dotnet.ps1"
            if not ps_script.is_file():
                print("[Index Setup] WARNING: setup-dotnet.ps1 not found, skipping .NET setup.")
            else:
                ps_args = ["powershell", "-ExecutionPolicy", "Bypass", "-File", str(ps_script)]
                if detected_ver:
                    ps_args += ["-DotNetVersion", detected_ver]
                if dotnet_arch:
                    ps_args += ["-DotNetArch", dotnet_arch]
                run_step(ps_args, script_dir, "Copying .NET hosting files", allow_failure=True)

    if not premake_path:
        raise RuntimeError(describe_premake_expectation(repo_root))

    action = args.generator or ("vs2022" if is_windows else "gmake2")
    premake_args = build_premake_args(args)
    run_step([premake_path, *premake_args, action], repo_root, f"Generating {action} files via Premake")

    # E13: when scripting is on the table and dotnet is reachable, restore the
    # generated solution so the .NET projects are ready to build immediately.
    if scripting_wanted and dotnet_exe:
        sln = repo_root / "Index.sln"
        if sln.is_file():
            run_step(
                [dotnet_exe, "restore", str(sln)],
                repo_root,
                "Restoring .NET dependencies (dotnet restore Index.sln)",
                allow_failure=True,
            )
        else:
            print("[Index Setup] Index.sln not produced by Premake; skipping dotnet restore.")
    elif scripting_wanted and not dotnet_exe:
        print("[Index Setup] Skipping dotnet restore - dotnet not on PATH.")

    print()
    print("[Index Setup] Setup complete.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"[Index Setup] ERROR: {exc}")
        raise SystemExit(1)
