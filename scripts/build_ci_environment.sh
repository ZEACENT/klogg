#!/bin/sh
# Keep the shell entry point transparent: validation and execution live in Python.
set -eu
exec python3 "$(dirname "$0")/build_ci_environment.py" "$@"
