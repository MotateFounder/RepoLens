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

install_linux_dependencies() {
    if ! prompt_yes_no "Install missing build dependencies with the detected package manager?"; then
        say "Install CMake 3.16+, a C++20 compiler, and a C compiler, then rerun this script."
        exit 1
    fi

    if command_exists apt-get; then
        sudo apt-get update
        sudo apt-get install -y cmake build-essential
    elif command_exists dnf; then
        sudo dnf install -y cmake gcc gcc-c++ make
    elif command_exists yum; then
        sudo yum install -y cmake gcc gcc-c++ make
    elif command_exists pacman; then
        sudo pacman -S --needed cmake base-devel
    elif command_exists zypper; then
        sudo zypper install -y cmake gcc gcc-c++ make
    else
        say "No supported package manager found. Install dependencies manually."
        exit 1
    fi
}

has_compilers() {
    local has_cxx=1
    local has_c=1

    if command_exists g++ || command_exists clang++; then
        has_cxx=0
    fi

    if command_exists gcc || command_exists clang || command_exists cc; then
        has_c=0
    fi

    [ "$has_cxx" -eq 0 ] && [ "$has_c" -eq 0 ]
}

ensure_dependencies() {
    if command_exists cmake && cmake_version_ok && has_compilers; then
        return 0
    fi

    say "Missing one or more build dependencies:"
    if ! command_exists cmake || ! cmake_version_ok; then
        say "- CMake 3.16+"
    fi
    has_compilers || say "- C++20 compiler and C compiler"
    install_linux_dependencies
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

say "RepoLens Linux installer/build script"
ensure_dependencies
build_release
