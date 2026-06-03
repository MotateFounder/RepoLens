#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"
RELEASE_DIR="$ROOT_DIR/release"

say() {
    printf '%s\n' "$*"
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

cmake_version_ok() {
    local version
    version="$(cmake --version | head -n 1 | sed 's/^cmake version //')"
    local major="${version%%.*}"
    local rest="${version#*.}"
    local minor="${rest%%.*}"
    [ "${major:-0}" -gt 3 ] || { [ "${major:-0}" -eq 3 ] && [ "${minor:-0}" -ge 16 ]; }
}

prompt_yes_no() {
    local message="$1"
    read -r -p "$message [y/N] " answer
    case "$answer" in
        y|Y|yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

ensure_xcode_tools() {
    if xcode-select -p >/dev/null 2>&1; then
        return 0
    fi

    say "Apple Command Line Tools were not found."
    if prompt_yes_no "Start Apple's Command Line Tools installer?"; then
        xcode-select --install || true
        say "Finish the Apple installer, then rerun this script."
        exit 1
    fi

    say "Install Apple Command Line Tools and rerun this script."
    exit 1
}

ensure_cmake() {
    if command_exists cmake && cmake_version_ok; then
        return 0
    fi

    say "CMake 3.16+ was not found."
    if command_exists brew; then
        if prompt_yes_no "Install CMake with Homebrew?"; then
            brew install cmake
            return 0
        fi
    else
        say "Homebrew was not found."
        if prompt_yes_no "Install Homebrew from https://brew.sh/?"; then
            /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
            if command_exists brew; then
                brew install cmake
                return 0
            fi
        fi
    fi

    say "Install CMake 3.16+ and rerun this script."
    exit 1
}

build_release() {
    rm -rf "$BUILD_DIR"
    mkdir -p "$RELEASE_DIR"

    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DREPOLENS_BUILD_STANDALONE=ON -DREPOLENS_USE_SYSTEM_SQLITE=OFF
    cmake --build "$BUILD_DIR"

    if [ ! -f "$BUILD_DIR/repolens" ]; then
        say "Build finished, but repolens was not found."
        exit 1
    fi

    cp -f "$BUILD_DIR/repolens" "$RELEASE_DIR/repolens"
    chmod +x "$RELEASE_DIR/repolens"
    rm -rf "$BUILD_DIR"

    say "RepoLens release binary:"
    say "$RELEASE_DIR/repolens"
    "$RELEASE_DIR/repolens" --version
}

say "RepoLens macOS installer/build script"
ensure_xcode_tools
ensure_cmake
build_release
