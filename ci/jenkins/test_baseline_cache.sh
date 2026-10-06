#!/usr/bin/env bash
# Focused acceptance test for persistent CI cache lifecycle helpers.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cache_tool="$repo_root/ci/jenkins/baseline_cache.sh"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/gkeyll-baseline-cache.XXXXXX")"
trap 'rm -rf "$work_dir"' EXIT

root="$work_dir/root"
platform=personal
sha=0123456789abcdef0123456789abcdef01234567

stage="$($cache_tool prepare-baseline "$root" "$platform" "$sha")"
git -C "$stage" init -q
git -C "$stage" config user.email ci@example.invalid
git -C "$stage" config user.name CI
touch "$stage/placeholder"
git -C "$stage" add placeholder
GIT_AUTHOR_DATE='2000-01-01T00:00:00Z' GIT_COMMITTER_DATE='2000-01-01T00:00:00Z' git -C "$stage" commit -qm fixture
actual_sha="$(git -C "$stage" rev-parse HEAD)"
mkdir -p "$stage/gkylsoft/gkeyll/bin"
for layer in moments vlasov gyrokinetic pkpm; do
  mkdir -p "$stage/gkylsoft/gkeyll-results/$layer/creg-accepted"
  mkdir -p "$stage/gkylsoft/gkeyll-results/parallel-c-4/$layer/creg-accepted"
done
touch "$stage/gkylsoft/gkeyll/bin/gkeyll"
chmod +x "$stage/gkylsoft/gkeyll/bin/gkeyll"
cat > "$stage/cache-manifest.txt" <<EOF
format_version=1
platform=$platform
baseline_commit=$actual_sha
EOF

entry="$($cache_tool publish-baseline "$root" "$platform" "$actual_sha" "$stage")"
$cache_tool valid "$root" "$platform" "$actual_sha"
test "$entry" = "$root/baseline-cache/$platform/$actual_sha"

mkdir -p "$work_dir/results/moments/creg-runs"
touch "$work_dir/results/moments/creg-runs/output.gkyl"
$cache_tool clear-candidate "$root" "$platform"
$cache_tool retain-candidate "$root" "$platform" "$actual_sha" "$work_dir/results" success done
test -f "$root/candidate-cache/$platform/$actual_sha/gkeyll-results/moments/creg-runs/output.gkyl"
$cache_tool clear-candidate "$root" "$platform"
test ! -e "$root/candidate-cache/$platform/$actual_sha"

echo 'baseline cache helper test passed'
