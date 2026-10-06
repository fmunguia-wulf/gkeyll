#!/usr/bin/env bash
# Helpers for the persistent Jenkins baseline and candidate-result caches.
set -euo pipefail

die() { echo "baseline-cache: $*" >&2; exit 2; }

cache_root() {
  local root="$1"
  [[ -n "$root" && "$root" = /* ]] || die 'GKEYLL_CI_ROOT must be an absolute path'
  printf '%s' "${root%/}"
}

platform_dir() {
  local root
  root="$(cache_root "$1")"
  printf '%s/%s/%s' "$root" "$2" "$3"
}

baseline_path() { platform_dir "$1" baseline-cache "$2"; }
candidate_path() { platform_dir "$1" candidate-cache "$2"; }

cache_tree_valid() {
  local entry="$1" platform="$2" sha="$3" manifest head layer
  [[ "$sha" =~ ^[0-9a-f]{40}$ ]] || return 1
  manifest="$entry/cache-manifest.txt"
  [[ -x "$entry/gkylsoft/gkeyll/bin/gkeyll" && -f "$manifest" && -d "$entry/gkylsoft/gkeyll-results" ]] || return 1
  [[ "$(awk -F= '$1 == "format_version" {print $2}' "$manifest")" == 1 ]] || return 1
  [[ "$(awk -F= '$1 == "platform" {print $2}' "$manifest")" == "$platform" ]] || return 1
  [[ "$(awk -F= '$1 == "baseline_commit" {print $2}' "$manifest")" == "$sha" ]] || return 1
  head="$(git -C "$entry" rev-parse HEAD 2>/dev/null || true)"
  [[ "$head" == "$sha" ]] || return 1
  for layer in moments vlasov gyrokinetic pkpm; do
    [[ -d "$entry/gkylsoft/gkeyll-results/$layer/creg-accepted" ]] || return 1
    [[ -d "$entry/gkylsoft/gkeyll-results/parallel-c-4/$layer/creg-accepted" ]] || return 1
  done
}

manifest_valid() {
  local root="$1" platform="$2" sha="$3"
  cache_tree_valid "$(baseline_path "$root" "$platform")/$sha" "$platform" "$sha"
}

prepare_baseline_stage() {
  local root platform sha parent stage
  root="$1"; platform="$2"; sha="$3"
  parent="$(baseline_path "$root" "$platform")"
  mkdir -p "$parent"
  stage="$parent/.staging-${sha}-${BUILD_TAG:-manual}"
  rm -rf "$stage"
  mkdir -p "$stage"
  printf '%s' "$stage"
}

publish_baseline() {
  local root="$1" platform="$2" sha="$3" stage="$4" parent target old
  parent="$(baseline_path "$root" "$platform")"
  target="$parent/$sha"
  [[ -d "$stage" ]] || die "staging directory is missing: $stage"
  [[ "$stage" == "$parent"/.staging-* ]] || die "unsafe staging directory: $stage"
  cache_tree_valid "$stage" "$platform" "$sha" || die "staging cache is incomplete or invalid: $stage"
  old="$parent/.previous-${BUILD_TAG:-manual}"
  rm -rf "$old"
  if [[ -e "$target" ]]; then mv "$target" "$old"; fi
  mv "$stage" "$target"
  find "$parent" -mindepth 1 -maxdepth 1 -type d ! -name "$sha" -exec rm -rf {} +
  printf '%s' "$target"
}

clear_candidate() {
  local root="$1" platform="$2" parent
  parent="$(candidate_path "$root" "$platform")"
  rm -rf "$parent"
  mkdir -p "$parent"
}

retain_candidate() {
  local root="$1" platform="$2" sha="$3" results="$4" result="$5" stage="$6" parent target temporary
  [[ "$sha" =~ ^[0-9a-f]{40}$ ]] || return 0
  parent="$(candidate_path "$root" "$platform")"
  target="$parent/$sha"
  temporary="$parent/.staging-${sha}-${BUILD_TAG:-manual}"
  rm -rf "$temporary" "$target"
  mkdir -p "$temporary"
  if [[ -d "$results" ]]; then cp -aL "$results" "$temporary/gkeyll-results"; fi
  printf 'candidate_commit=%s\nresult=%s\nterminal_stage=%s\n' "$sha" "$result" "$stage" > "$temporary/candidate-manifest.txt"
  mv "$temporary" "$target"
}

case "${1:-}" in
  valid) manifest_valid "$2" "$3" "$4" ;;
  baseline-path) printf '%s\n' "$(baseline_path "$2" "$3")/$4" ;;
  prepare-baseline) prepare_baseline_stage "$2" "$3" "$4"; echo ;;
  publish-baseline) publish_baseline "$2" "$3" "$4" "$5"; echo ;;
  clear-candidate) clear_candidate "$2" "$3" ;;
  retain-candidate) retain_candidate "$2" "$3" "$4" "$5" "$6" "$7" ;;
  *) die 'usage: baseline_cache.sh {valid|baseline-path|prepare-baseline|publish-baseline|clear-candidate|retain-candidate} ...' ;;
esac
