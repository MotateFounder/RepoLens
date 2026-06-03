#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

printf '%s\n' "RepoLens Windows 10 installer/build script"
printf '%s\n' "Using the same dependency checks and standalone build flow as Windows 11."

REPOLENS_WINDOWS_LABEL="Windows 10" bash "$ROOT_DIR/install_Windows11.sh"
