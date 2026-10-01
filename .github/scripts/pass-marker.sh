#!/usr/bin/env bash
# Pass markers: an empty Actions cache entry a workflow saves when a check passes, named for the
# content it checked. The same content isn't checked twice: a merged pull request, a branch
# promoted to another, a scheduled run whose inputs didn't move.
#
#   pass-marker.sh key <prefix> <path>...   prints <prefix>-<digest of those paths at HEAD>
#   pass-marker.sh check <key>...           prints each key that has a trusted marker
#   pass-marker.sh save                     writes the (empty) directory to save as a marker
#
# Save a marker with actions/cache/save (path .pass-marker) in the job's last step, after
# everything it vouches for passed. A marker counts only from this repository's own branches and
# pull requests: a fork's pull request runs its own copy of the workflow, which could save any key.
# Checking needs GH_TOKEN with actions: read.
set -euo pipefail

usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

cmd="${1:-}"
shift || true
case "$cmd" in
  key)
    [[ $# -ge 2 ]] || usage
    prefix=$1
    shift
    # Each path's tree or blob id at HEAD: they change exactly when the content does.
    ids="$(for path in "$@"; do
      printf '%s %s\n' "$(git rev-parse -q --verify "HEAD:$path" || echo missing)" "$path"
    done)"
    echo "$prefix-$(sha256sum <<<"$ids" | cut -c1-24)"
    ;;
  check)
    : "${GITHUB_REPOSITORY:?}"
    declare -A pr_trusted=()
    for key in "$@"; do
      [[ "$key" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "bad marker key: $key" >&2; exit 2; }
      refs="$(gh api -X GET "repos/$GITHUB_REPOSITORY/actions/caches" -f key="$key" -f per_page=100 \
        --jq ".actions_caches[] | select(.key == \"$key\") | .ref")" || {
        echo "::warning::Couldn't list pass markers; checking everything." >&2
        exit 0
      }
      for ref in $refs; do
        if [[ "$ref" == refs/heads/* ]]; then
          echo "$key"
          break
        fi
        if [[ "$ref" =~ ^refs/pull/([0-9]+)/merge$ ]]; then
          pr="${BASH_REMATCH[1]}"
          if [[ -z "${pr_trusted[$pr]:-}" ]]; then
            head_repo="$(gh api "repos/$GITHUB_REPOSITORY/pulls/$pr" --jq '.head.repo.full_name' 2>/dev/null || true)"
            pr_trusted[$pr]="$([[ "$head_repo" == "$GITHUB_REPOSITORY" ]] && echo yes || echo no)"
          fi
          if [[ "${pr_trusted[$pr]}" == yes ]]; then
            echo "$key"
            break
          fi
        fi
      done
    done
    ;;
  save)
    mkdir -p .pass-marker
    : > .pass-marker/passed
    ;;
  *) usage ;;
esac
