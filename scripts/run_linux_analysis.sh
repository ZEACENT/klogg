#!/bin/sh
# Shared CI/qualification analysis phases and complete candidate analysis roles.
set -eu
exec python3 "$(dirname "$0")/qualify_ci_analysis.py" inside "$@"
