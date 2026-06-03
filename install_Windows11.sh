#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"
RELEASE_DIR="$ROOT_DIR/release"

say() {
    printf '%s\n' "$*" >&2
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

cmake_version_ok() {
    local cmake_bin="$1"
    local version
    version="$("$cmake_bin" --version | head -n 1 | sed 's/^cmake version //')"
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

add_user_path_if_exists() {
    local candidate="$1"
    if [ -d "$candidate" ]; then
        export PATH="$candidate:$PATH"
        local windows_candidate="$candidate"
        if command_exists cygpath; then
            windows_candidate="$(cygpath -w "$candidate")"
        fi
        powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "\$p = [Environment]::GetEnvironmentVariable('Path', 'User'); if ((\$p -split ';') -notcontains '$windows_candidate') { [Environment]::SetEnvironmentVariable('Path', (\$p.TrimEnd(';') + ';$windows_candidate').TrimStart(';'), 'User') }" >/dev/null 2>&1 || true
    fi
}

has_windows_compiler() {
    if command_exists cl.exe || command_exists gcc || command_exists clang; then
        return 0
    fi

    local vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    if [ -x "$vswhere" ]; then
        "$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | grep -q .
        return $?
    fi

    return 1
}

find_cmake() {
    if command_exists cmake; then
        local path
        path="$(command -v cmake)"
        if cmake_version_ok "$path"; then
            printf '%s\n' "$path"
            return 0
        fi
    fi

    local candidates=(
        "/c/Program Files/CMake/bin"
        "/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
        "/c/Program Files/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
    )

    for candidate in "${candidates[@]}"; do
        add_user_path_if_exists "$candidate"
        if [ -x "$candidate/cmake.exe" ] && cmake_version_ok "$candidate/cmake.exe"; then
            printf '%s\n' "$candidate/cmake.exe"
            return 0
        fi
    done

    return 1
}

ensure_windows_dependencies() {
    local cmake_path=""
    if ! cmake_path="$(find_cmake)"; then
        say "CMake 3.16+ was not found."
        if command_exists winget.exe && prompt_yes_no "Install CMake with winget?"; then
            winget.exe install --id Kitware.CMake --source winget --accept-package-agreements --accept-source-agreements
            add_user_path_if_exists "/c/Program Files/CMake/bin"
            cmake_path="$(find_cmake)"
        else
            say "Install CMake 3.16+ and rerun this script."
            exit 1
        fi
    fi

    if ! has_windows_compiler; then
        say "No C/C++ compiler was found in PATH."
        if command_exists winget.exe && prompt_yes_no "Install Visual Studio 2022 Build Tools with C++ support?"; then
            winget.exe install --id Microsoft.VisualStudio.2022.BuildTools --source winget --accept-package-agreements --accept-source-agreements --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
        else
            say "Install a C++20 compiler, such as Visual Studio Build Tools, and rerun this script."
            exit 1
        fi
    fi

    printf '%s\n' "$cmake_path"
}

build_release() {
    local cmake_path="$1"
    rm -rf "$BUILD_DIR"
    mkdir -p "$RELEASE_DIR"

    "$cmake_path" -S "$ROOT_DIR" -B "$BUILD_DIR" -DREPOLENS_BUILD_STANDALONE=ON -DREPOLENS_USE_SYSTEM_SQLITE=OFF
    "$cmake_path" --build "$BUILD_DIR" --config Release

    local built_exe=""
    if [ -f "$BUILD_DIR/Release/repolens.exe" ]; then
        built_exe="$BUILD_DIR/Release/repolens.exe"
    elif [ -f "$BUILD_DIR/repolens.exe" ]; then
        built_exe="$BUILD_DIR/repolens.exe"
    else
        say "Build finished, but repolens.exe was not found."
        exit 1
    fi

    cp -f "$built_exe" "$RELEASE_DIR/repolens.exe"
    rm -rf "$BUILD_DIR"

    say "RepoLens release binary:"
    say "$RELEASE_DIR/repolens.exe"
    "$RELEASE_DIR/repolens.exe" --version
}

say "RepoLens ${REPOLENS_WINDOWS_LABEL:-Windows 11} installer/build script"
CMAKE_BIN="$(ensure_windows_dependencies)"
build_release "$CMAKE_BIN"
