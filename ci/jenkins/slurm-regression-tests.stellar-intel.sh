#!/bin/bash -l
# All-C regression payload for ci/jenkins/Jenkinsfile.stellar-intel.
#
# Jenkins builds and installs the PR and trusted baseline on the login node.
# This payload only creates the baseline's C outputs, checks the PR against
# them, and turns SQLite-recorded failures into a Slurm/Jenkins failure.
set -euo pipefail

: "${CI_WORKSPACE:?CI_WORKSPACE must name the shared Jenkins workspace}"
: "${CI_BASELINE_DIR:?CI_BASELINE_DIR must name the trusted baseline checkout}"
: "${CI_BASELINE_PREFIX:?CI_BASELINE_PREFIX must name the baseline install prefix}"
: "${CI_PR_PREFIX:?CI_PR_PREFIX must name the PR install prefix}"
: "${CI_REGRESSION_JOBS:?CI_REGRESSION_JOBS must be set}"
: "${CI_REGRESSION_TEST_TIMEOUT:?CI_REGRESSION_TEST_TIMEOUT must be set}"

cd "$CI_WORKSPACE"
. machines/module_load.stellar-intel.sh

baseline_gkeyll="$CI_BASELINE_PREFIX/gkeyll/bin/gkeyll"
pr_gkeyll="$CI_PR_PREFIX/gkeyll/bin/gkeyll"

test -x "$baseline_gkeyll"
test -x "$pr_gkeyll"

cd "$CI_BASELINE_DIR"
"$baseline_gkeyll" runregression configure \
  --source-dir "$CI_BASELINE_DIR" \
  --prefix "$CI_BASELINE_PREFIX"
"$baseline_gkeyll" runregression run -c create \
  --jobs "$CI_REGRESSION_JOBS" \
  --timeout "$CI_REGRESSION_TEST_TIMEOUT"

cd "$CI_WORKSPACE"
"$pr_gkeyll" runregression configure \
  --source-dir "$CI_WORKSPACE" \
  --prefix "$CI_PR_PREFIX"

# Use only C accepted output from the fixed baseline. Do not carry Lua
# baselines into this CPU C-regression job.
for layer in moments vlasov gyrokinetic pkpm; do
  baseline_accepted="$CI_BASELINE_PREFIX/gkeyll-results/$layer/creg-accepted"
  pr_accepted="$CI_PR_PREFIX/gkeyll-results/$layer/creg-accepted"
  rm -rf "$pr_accepted"
  if [[ -d "$baseline_accepted" ]]; then
    mv "$baseline_accepted" "$pr_accepted"
  fi
done

"$pr_gkeyll" runregression run -c check \
  --jobs "$CI_REGRESSION_JOBS" \
  --timeout "$CI_REGRESSION_TEST_TIMEOUT"
"$pr_gkeyll" ci/jenkins/check_regression_results.lua \
  "$CI_PR_PREFIX/gkeyll-results" \
  ci/jenkins/expected_regression_diffs.txt
