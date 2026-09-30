#!/bin/sh
# Shared configure/prebuild/trace phases and complete candidate analysis roles.
set -eu
exec python3 "$(dirname "$0")/qualify_ci_analysis.py" inside "$@"
