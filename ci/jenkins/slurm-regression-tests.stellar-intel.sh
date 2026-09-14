#!/bin/bash -l
# All-C regression payload for ci/jenkins/Jenkinsfile.stellar-intel.
#
# Jenkins builds, installs, and compiles the PR and trusted baseline on the
# login node. This payload only executes their precompiled C tests, creates
# the baseline's outputs, checks the PR against them, and turns SQLite-recorded
# failures into a Slurm/Jenkins failure.
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

# The PR-side runner provides the compile/execute-only interface. It drives
# baseline tests through their own source directory and install prefix, so the
# tested baseline executable and libraries still come from agent_tools-jenkins.
cd "$CI_BASELINE_DIR"
"$pr_gkeyll" runregression run -c --execute-only create \
  --jobs "$CI_REGRESSION_JOBS" \
  --timeout "$CI_REGRESSION_TEST_TIMEOUT"

cd "$CI_WORKSPACE"
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

"$pr_gkeyll" runregression run -c --execute-only check \
  --jobs "$CI_REGRESSION_JOBS" \
  --timeout "$CI_REGRESSION_TEST_TIMEOUT"
"$pr_gkeyll" ci/jenkins/check_regression_results.lua \
  "$CI_PR_PREFIX/gkeyll-results" \
  ci/jenkins/expected_regression_diffs.txt
