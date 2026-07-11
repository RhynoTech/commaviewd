#!/usr/bin/env bash
set -euo pipefail

# Backwards-compatible raw installer shim for app versions that still fetch
# /comma4/install.sh. Keep this file small so old apps can reach the generic
# comma-device installer after the companion tree moved to /comma.
shim_ref="${COMMAVIEWD_INSTALLER_REF:-${COMMAVIEWD_REF:-}}"
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
  case "${args[$i]}" in
    --tag)
      if (( i + 1 < ${#args[@]} )); then
        shim_ref="${args[$((i + 1))]}"
      fi
      ;;
    --tag=*)
      shim_ref="${args[$i]#--tag=}"
      ;;
  esac
done

SCRIPT_URL="${COMMAVIEWD_COMMA_INSTALL_URL:-}"
if [[ -z "$SCRIPT_URL" ]]; then
  if [[ -n "${COMMAVIEWD_INSTALLER_RAW_BASE:-}" ]]; then
    SCRIPT_URL="${COMMAVIEWD_INSTALLER_RAW_BASE%/}/install.sh"
  else
    INSTALLER_REF="${shim_ref:-master}"
    GITHUB_REPO="${COMMAVIEWD_GITHUB_REPO:-RhynoTech/commaviewd}"
    SCRIPT_URL="https://raw.githubusercontent.com/${GITHUB_REPO}/${INSTALLER_REF}/comma/install.sh"
  fi
fi

curl -fsSL "$SCRIPT_URL" | bash -s -- "$@"
