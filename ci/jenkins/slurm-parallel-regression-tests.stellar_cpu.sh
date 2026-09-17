#!/bin/bash -l
set -euo pipefail

: "${CI_WORKSPACE:?}"
: "${CI_BASELINE_DIR:?}"
: "${CI_BASELINE_PREFIX:?}"
: "${CI_CANDIDATE_PREFIX:?}"

cd "$CI_WORKSPACE"
. machines/module_load.stellar-intel.sh

baseline_gkeyll="$CI_BASELINE_PREFIX/gkeyll/bin/gkeyll"
candidate_gkeyll="$CI_CANDIDATE_PREFIX/gkeyll/bin/gkeyll"

cd "$CI_BASELINE_DIR"
"$baseline_gkeyll" runregression run -c --parallel --execute-only create

cd "$CI_WORKSPACE"
for layer in moments vlasov gyrokinetic pkpm; do
  src="$CI_BASELINE_PREFIX/gkeyll-results/parallel-c-4/$layer/creg-accepted"
  dst="$CI_CANDIDATE_PREFIX/gkeyll-results/parallel-c-4/$layer/creg-accepted"
  rm -rf "$dst"
  if [[ -d "$src" ]]; then mv "$src" "$dst"; fi
done
"$candidate_gkeyll" runregression run -c --parallel --execute-only check
"$candidate_gkeyll" ci/jenkins/check_regression_results.lua \
  "$CI_CANDIDATE_PREFIX/gkeyll-results/parallel-c-4" \
  ci/jenkins/expected_regression_diffs.txt ci-parallel-regression-summary.txt
