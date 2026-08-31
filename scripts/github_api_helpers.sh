#!/usr/bin/env bash

# Resolve an optional scalar without allowing a failed GitHub API response body
# to become a valid value. Returns 0 when found, 1 when absent, and 2 for an
# unexpected API failure. The destination is empty for both failure states.
gh_api_optional_scalar() {
  if [ "$#" -ne 3 ]; then
    printf 'gh_api_optional_scalar requires: destination endpoint jq-filter\n' >&2
    return 2
  fi

  local destination_name="$1"
  local endpoint="$2"
  local jq_filter="$3"
  local lookup_error
  local lookup_output=""
  local lookup_status=0

  if ! [[ "$destination_name" =~ ^[a-zA-Z_][a-zA-Z0-9_]*$ ]]; then
    printf 'invalid gh_api_optional_scalar destination: %s\n' "$destination_name" >&2
    return 2
  fi
  printf -v "$destination_name" '%s' ""

  lookup_error="$(mktemp)"
  lookup_output="$(gh api "$endpoint" --jq "$jq_filter" 2>"$lookup_error")" \
    || lookup_status=$?
  if [ "$lookup_status" -eq 0 ]; then
    rm -f "$lookup_error"
    printf -v "$destination_name" '%s' "$lookup_output"
    return 0
  fi
  if grep -Eq '404|Not Found' "$lookup_error"; then
    rm -f "$lookup_error"
    return 1
  fi

  cat "$lookup_error" >&2
  rm -f "$lookup_error"
  return 2
}
