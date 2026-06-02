#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Pre-approve installs via -y / --assume-yes / --yes, or INDEX_SETUP_ASSUME_YES=1.
ASSUME_YES="${INDEX_SETUP_ASSUME_YES:-}"
for arg in "$@"; do
    case "$arg" in
        -y | --yes | --assume-yes) ASSUME_YES=1 ;;
    esac
done

# True when python3 exists and satisfies the 3.10+ minimum.
have_python() {
    command -v python3 >/dev/null 2>&1 || return 1
    python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1
}

# Echo the package-manager command that installs Python, or return 1.
detect_python_install_cmd() {
    if command -v apt-get >/dev/null 2>&1; then echo "apt-get install -y python3"; return 0; fi
    if command -v dnf >/dev/null 2>&1; then echo "dnf install -y python3"; return 0; fi
    if command -v pacman >/dev/null 2>&1; then echo "pacman -S --noconfirm python"; return 0; fi
    if command -v zypper >/dev/null 2>&1; then echo "zypper install -y python3"; return 0; fi
    return 1
}

install_python() {
    local cmd
    if ! cmd="$(detect_python_install_cmd)"; then
        echo "[Index Setup] No supported package manager (apt/dnf/pacman/zypper) was found."
        echo "[Index Setup] Install Python 3.10+ manually, then re-run setup."
        return 1
    fi
    if [ "$(id -u)" -ne 0 ]; then
        if command -v sudo >/dev/null 2>&1; then
            cmd="sudo $cmd"
        else
            echo "[Index Setup] Root privileges are required but 'sudo' is unavailable."
            return 1
        fi
    fi
    echo "[Index Setup] Installing Python via: $cmd"
    # shellcheck disable=SC2086
    $cmd
}

if ! have_python; then
    echo "[Index Setup] Python 3.10+ was not found."
    do_install=0
    if [ -n "$ASSUME_YES" ]; then
        do_install=1
    elif [ -t 0 ]; then
        read -r -p "[Index Setup] Install Python now? [Y/n] " answer
        case "${answer:-y}" in
            "" | y | Y | yes | Yes) do_install=1 ;;
        esac
    fi
    if [ "$do_install" -eq 1 ]; then
        install_python || true
    fi
    if ! have_python; then
        echo "[Index Setup] ERROR: Python 3.10+ is still unavailable. Aborting."
        exit 1
    fi
fi

python3 "$SCRIPT_DIR/Setup.py" "$@"
