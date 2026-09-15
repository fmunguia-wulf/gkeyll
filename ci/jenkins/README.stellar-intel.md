# Manual Jenkins CI on Princeton Stellar Intel

This is the Level-1, CPU-only Gkeyll CI setup for Princeton's Stellar Intel
cluster. A person authenticates to Stellar with SSH/Duo, then manually starts
a Jenkins build for either a GitHub pull-request number or an explicitly
selected candidate branch/commit. Jenkins itself neither logs in through Duo
nor accepts GitHub webhooks.

The job runs `ci/jenkins/Jenkinsfile.stellar-intel` from a reviewed, trusted
CI branch during bring-up. It fetches the requested `refs/pull/<number>/head`
commit, builds Gkeyll on the login node, then submits unit tests and all
supported C regression tests as separate CPU Slurm jobs. The C regressions use
a same-session `agent_tools-jenkins-stellar_intel-baseline` baseline. Lua
regression, MPI, and GPU testing are deliberately not part of this setup.

## 0. Publish these CI files first

Create the dedicated branch `agent_tools-jenkins-stellar_intel`, review, and
push the Stellar CI implementation files to it:

```text
ci/jenkins/Jenkinsfile.stellar-intel
ci/jenkins/jenkins-stellar-intel.sh
ci/jenkins/slurm-unit-tests.stellar-intel.sh
ci/jenkins/slurm-regression-tests.stellar-intel.sh
ci/jenkins/README.stellar-intel.md
machines/module_load.stellar-intel.sh
machines/mkdeps.stellar-intel.sh
machines/configure.stellar-intel.sh
```

Configure the initial Jenkins job to load its Pipeline script from this branch.
It is the trusted orchestration branch; do not point the job at an unreviewed
PR branch. After the pipeline has been validated and the change is merged,
switch its SCM branch specifier to `*/main`. No pipeline-code change is needed
at that point.

Create the fixed regression baseline from the same reviewed commit, then push
it. Keep this branch unchanged while candidate work is being compared against
it:

```sh
git branch agent_tools-jenkins-stellar_intel-baseline HEAD
git push -u origin agent_tools-jenkins-stellar_intel-baseline
```

## 1. Choose the CI root and Slurm settings

Keep the Jenkins controller, its installation, its workspaces, and all CI
artifacts under one shared, compute-node-visible directory:

```sh
export GKEYLL_CI_ROOT=/scratch/gpfs/$USER/gkeyll_ci
```

Stellar provides the username through `$USER`. The setup below creates:

```text
$GKEYLL_CI_ROOT/jenkins.war          Jenkins installation archive
$GKEYLL_CI_ROOT/jenkins_home/        JENKINS_HOME: controller configuration, plugins, jobs
$GKEYLL_CI_ROOT/jenkins_webroot/     unpacked Jenkins web application
$GKEYLL_CI_ROOT/tmp/                 controller temporary files
$GKEYLL_CI_ROOT/logs/                controller logs
$GKEYLL_CI_ROOT/workspaces/          Pipeline build workspaces and Slurm output
```

Do not use `/tmp`, `/home`, or a project directory for these CI files. The
pipeline creates an isolated workspace below `$GKEYLL_CI_ROOT/workspaces` for every
build. Since scratch storage can be purged, do not treat Jenkins build history
or credentials stored there as durable backups.

Record the Slurm values for your group:

```sh
qos
sshare
```

`STELLAR_SLURM_QOS` is required. `STELLAR_SLURM_ACCOUNT` is optional for PU
users but required for PPPL/CIMES users when their project policy requires
`--account`.

The initial job requests one task with four CPUs. `make unit-run` is launched
only once and does not distribute the full suite as a Slurm MPI job; the
four-CPU allocation is needed for its roughly 30 GB default memory allocation
(Stellar allocates 7.5 GB per core by default). In particular,
`test_dg_interpolate_3x2v_gk_ho` did not fit in a one-core allocation. Stellar
may place requests of 47 cores or fewer in its low-priority serial queue. Do
not request an entire 96-core node merely to bypass that queue; add a genuinely
parallel test profile first.

The separate C regression job requests one task with eight CPUs (roughly
60 GB by Stellar's 7.5 GB-per-core default) and runs four C cases at a time.
Its default four-hour allocation covers baseline creation and the PR check.
C executables are compiled before Slurm submission on the login node. Adjust
its settings only after measuring the complete first run.

## 2. Validate the cluster setup by hand

SSH to the Intel side and approve Duo:

```sh
ssh <NetID>@stellar.princeton.edu
```

Create a disposable checkout below the CI root. This branch contains the
Stellar pipeline files; do not use an unrelated checkout that might lack the
`machines/` configuration scripts or Slurm test payload.

```sh
export GKEYLL_CI_ROOT=/scratch/gpfs/$USER/gkeyll_ci
mkdir "$GKEYLL_CI_ROOT"
cd "$GKEYLL_CI_ROOT"
git clone --branch agent_tools-jenkins-stellar_intel --single-branch \
  https://github.com/gkeyllorg/gkeyll.git gkeyll
cd gkeyll
```

From this `gkeyll/` checkout, validate the same environment the job will use:
`PREFIX="$PWD/../gkylsoft"` places the dependencies at
`$GKEYLL_CI_ROOT/gkylsoft`, beside the disposable source checkout.

```sh
PREFIX="$PWD/../gkylsoft" ./machines/mkdeps.stellar-intel.sh
PREFIX="$PWD/../gkylsoft" ./machines/configure.stellar-intel.sh
. ./machines/module_load.stellar-intel.sh
make -j32 unit
make -j32 install
sbatch --wait --qos pppl-short --nodes 1 --ntasks 1 --cpus-per-task 4 \
  --time 00:30:00 --chdir "$PWD" \
  --export=ALL,CI_WORKSPACE="$PWD" \
  ci/jenkins/slurm-unit-tests.stellar-intel.sh
```

For PPPL/CIMES, add `--account <your-account>` to the `sbatch` command. This
must complete successfully before introducing Jenkins. `CI_WORKSPACE` is the
shared checkout path that the batch payload uses after it starts on a compute
node; Jenkins supplies the same variable when it submits this job.

The machine configuration scripts run in child shells, so their environment
does not remain active for the later `make` command. Source
`machines/module_load.stellar-intel.sh` before `make`; it is also sourced by the
machine scripts, Jenkins build stage, and Slurm payload. This keeps all
Stellar Intel module versions in one file.

### Validate the all-C regression comparison by hand

The regression job compares the current checkout with the fixed
`agent_tools-jenkins-stellar_intel-baseline` baseline. Build and install that
baseline beside the candidate checkout, using its own dependency prefix:

```sh
cd "$GKEYLL_CI_ROOT"
git clone --branch agent_tools-jenkins-stellar_intel-baseline --single-branch \
  https://github.com/gkeyllorg/gkeyll.git gkeyll-baseline
cd gkeyll-baseline
PREFIX="$PWD/gkylsoft" ./machines/mkdeps.stellar-intel.sh
PREFIX="$PWD/gkylsoft" ./machines/configure.stellar-intel.sh
. ../gkeyll/machines/module_load.stellar-intel.sh
make -j32 install

"$PWD/gkylsoft/gkeyll/bin/gkeyll" runregression configure \
  --source-dir "$PWD" --prefix "$PWD/gkylsoft"
"$PWD/gkylsoft/gkeyll/bin/gkeyll" runregression run -c compile
```

Return to the candidate checkout, install it if the earlier unit-test
validation did not already do so, then configure and compile its C regressions
on the login node. This honors each layer's `ignore_c_tests.lua`:

```sh
cd "$GKEYLL_CI_ROOT/gkeyll"
. machines/module_load.stellar-intel.sh
make -j32 install
"$PWD/gkylsoft/gkeyll/bin/gkeyll" runregression configure \
  --source-dir "$PWD" --prefix "$PWD/gkylsoft"
"$PWD/gkylsoft/gkeyll/bin/gkeyll" runregression run -c compile
```

Then submit the execution-only baseline/check workflow. It runs four C cases
concurrently and limits each case to 900 seconds:

```sh
cd "$GKEYLL_CI_ROOT/gkeyll"
sbatch --wait --qos pppl-short --nodes 1 --ntasks 1 --cpus-per-task 8 \
  --time 04:00:00 --chdir "$PWD" \
  --export=ALL,CI_WORKSPACE="$PWD",CI_BASELINE_DIR="$GKEYLL_CI_ROOT/gkeyll-baseline",CI_BASELINE_PREFIX="$GKEYLL_CI_ROOT/gkeyll-baseline/gkylsoft",CI_CANDIDATE_PREFIX="$GKEYLL_CI_ROOT/gkeyll/gkylsoft",CI_REGRESSION_JOBS=4,CI_REGRESSION_TEST_TIMEOUT=900 \
  ci/jenkins/slurm-regression-tests.stellar-intel.sh
```

The baseline's `creg-accepted` output is moved into the candidate results tree and
is therefore disposable. Do not use a persistent accepted-output cache for
this workflow.

## 3. Install and run Jenkins privately

Stellar Intel is Red Hat Enterprise Linux 8.10. Because this is an
unprivileged personal setup, use the Jenkins WAR directly rather than a
system RPM/service. This procedure requires no root access: do not use
`sudo`, `dnf`, `rpm`, or `systemctl`. The login node's default Java is Java
17; current Jenkins requires Java 21 or later. Stellar's Java 21 location is
`/usr/lib/jvm/java-21-openjdk-21.0.12.1.1-1.1.el8.x86_64`. Do not use the
default `java` command unless `java -version` confirms it is Java 21 or newer.

After logging in through Duo, set up the scratch-only controller directory:

```sh
export GKEYLL_CI_ROOT=/scratch/gpfs/$USER/gkeyll_ci
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-21.0.12.1.1-1.1.el8.x86_64
export PATH="$JAVA_HOME/bin:$PATH"
export JENKINS_HOME="$GKEYLL_CI_ROOT/jenkins_home"
export JENKINS_WEBROOT="$GKEYLL_CI_ROOT/jenkins_webroot"
export TMPDIR="$GKEYLL_CI_ROOT/tmp"

mkdir -p "$JENKINS_HOME" "$JENKINS_WEBROOT" "$TMPDIR" \
  "$GKEYLL_CI_ROOT/logs" "$GKEYLL_CI_ROOT/workspaces"
java -version
```

The final command must report Java 21 or newer. Download the Jenkins LTS WAR
into the same directory; if Stellar cannot reach the download URL, download it
on the laptop and copy it into `$GKEYLL_CI_ROOT` through the authenticated SSH
connection instead:

```sh
cd "$GKEYLL_CI_ROOT"
curl -fL -o jenkins.war https://get.jenkins.io/war-stable/latest/jenkins.war
```

Start Jenkins in a named tmux session. This is the initial operational mode:
it keeps the controller available after SSH disconnects without installing a
system service. Stop it after a test session if it is not needed.

```sh
tmux new -s gkeyll_ci

export GKEYLL_CI_ROOT=/scratch/gpfs/$USER/gkeyll_ci
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-21.0.12.1.1-1.1.el8.x86_64
export PATH="$JAVA_HOME/bin:$PATH"
export JENKINS_HOME="$GKEYLL_CI_ROOT/jenkins_home"
export JENKINS_WEBROOT="$GKEYLL_CI_ROOT/jenkins_webroot"
export TMPDIR="$GKEYLL_CI_ROOT/tmp"

java -Djava.io.tmpdir="$TMPDIR" -jar "$GKEYLL_CI_ROOT/jenkins.war" \
  --webroot="$JENKINS_WEBROOT" \
  --httpListenAddress=127.0.0.1 \
  --httpPort=8080 \
  2>&1 | tee -a "$GKEYLL_CI_ROOT/logs/jenkins.log"
```

In another Stellar shell, use these commands to manage that controller:

```sh
tmux attach -t gkeyll_ci   # return to the Jenkins console
tmux ls                    # verify the session exists
tmux kill-session -t gkeyll_ci # stop Jenkins after testing
```

The controller must run as the same Unix account that owns the Slurm
allocation. It binds only to `127.0.0.1`; do not expose a public port or
configure a GitHub webhook for this Level-1 setup.

### Optional browser access

The command-line launcher below is the normal way to submit a build. The UI is
still available for one-time configuration, interactive inspection, or manual
**Build with Parameters** submissions. After SSH/Duo authentication from the
laptop, open a tunnel. The first port is
on the laptop and can be any unused port; the second is Jenkins' listening
port on Stellar. If Jenkins uses its default remote port 8080 and the Mac's
local Jenkins already owns local port 8080, use 8081 locally:

```sh
ssh -N -L 8081:127.0.0.1:8080 <NetID>@stellar.princeton.edu
```

Then open `http://localhost:8081` in the laptop browser. The Mac Jenkins
remains at `http://localhost:8080`; both interfaces can be open at once. If
you configure the Stellar Jenkins service itself to listen on a non-default
remote port, replace only the final `8080` in the tunnel command.

On its first start, get the unlock password with:

```sh
cat "$JENKINS_HOME/secrets/initialAdminPassword"
```

Complete Jenkins initial setup and create an administrator account.

Install these Jenkins plugins if they are not already present: Pipeline, Git,
Credentials Binding, and the Git client plugin. The Pipeline and Git plugins
are normally part of Jenkins' suggested-plugin installation.

### Create a Jenkins API token for the launcher

While signed in to the Jenkins UI as the user who will launch builds, create a
user API token in that user's security configuration. Store the Jenkins user
name and token in a file readable only by that Unix user:

```sh
umask 077
printf '%s:%s\n' '<jenkins-user>' '<jenkins-api-token>' \
  > "$JENKINS_HOME/jenkins-cli.auth"
chmod 600 "$JENKINS_HOME/jenkins-cli.auth"
```

This token is separate from the GitHub credential below. The launcher uses it
only against Jenkins at `127.0.0.1`; it neither changes browser login nor
disables Jenkins CSRF protection.

## 4. Create the GitHub credential

Create a GitHub fine-grained token restricted to the `gkeyllorg/gkeyll`
repository. Grant `Contents: Read`, `Pull requests: Read`, and `Commit
statuses: Read and write`. In Jenkins add it as a Username-with-password
credential, using the GitHub username and token, and give it an ID such as
`gkeyll-github-stellar`. The Pipeline uses this one credential both to fetch
the PR and to publish its commit status.

The token never receives repository-content write permission, and the
Pipeline binds it only for Git checkout and short, trusted GitHub API calls;
it is not present while a command from the PR checkout runs. Do not put Duo
secrets, a personal SSH key, or unrelated credentials in the Jenkins account.

## 5. Configure the Jenkins node and global environment

Use the controller's built-in node or a local agent. Give it the label
`stellar-intel` (or select another label and set
`STELLAR_INTEL_NODE_LABEL` below). The agent must run on the Stellar Intel
login side and as the same Unix account that can submit Slurm jobs.

In **Manage Jenkins → System → Global properties → Environment variables**,
set:

| Name | Required value |
| --- | --- |
| `GKEYLL_CI_ROOT` | Output of `printf '/scratch/gpfs/%s/gkeyll_ci' "$USER"`; paste the expanded result, not a literal `$USER` |
| `STELLAR_GITHUB_CREDENTIAL_ID` | Jenkins credential ID, e.g. `gkeyll-github-stellar` |
| `STELLAR_SLURM_QOS` | Your valid CPU QoS, e.g. `pppl-short` |
| `STELLAR_SLURM_ACCOUNT` | Project account, if required; otherwise omit it |
| `STELLAR_SLURM_TIME` | Optional time limit; defaults to `00:30:00` |
| `STELLAR_BUILD_JOBS` | Optional login-node compile parallelism; defaults to `3` |
| `STELLAR_REGRESSION_TIME` | Optional C-regression allocation limit; defaults to `04:00:00` |
| `STELLAR_REGRESSION_JOBS` | Optional concurrent C test runs; defaults to `4` |
| `STELLAR_REGRESSION_TEST_TIMEOUT` | Optional per-C-test limit in seconds; defaults to `900` |
| `STELLAR_INTEL_NODE_LABEL` | Optional agent label; defaults to `stellar-intel` |

Do not set a broad global `PATH` to an interactive shell configuration. The
pipeline explicitly initializes the Stellar modules for every build and Slurm
job. `GKEYLL_CI_ROOT` is the Pipeline's only root setting; it must exactly
match the value used to set `JENKINS_HOME` when the controller was launched.
The Pipeline derives its workspace root as `$GKEYLL_CI_ROOT/workspaces`; do
not define a separate workspace-root variable.

## 6. Create the one parameterized Pipeline job

Create **New Item → Pipeline** named `gkeyll-ci-stellar-intel`.

Configure its pipeline definition as **Pipeline script from SCM**:

- SCM: Git
- Repository: `https://github.com/gkeyllorg/gkeyll.git`
- Credentials: the read-only GitHub credential
- Branch specifier: `*/agent_tools-jenkins-stellar_intel`
- Script path: `ci/jenkins/Jenkinsfile.stellar-intel`

The CI-branch selection is intentional during bring-up. Do not point this job
at a PR branch or let the requested PR select its own Jenkinsfile. Once this
pipeline is validated and merged, change this branch specifier to `*/main`.

Leave **This project is parameterized** unchecked. The trusted Pipeline file
declares and owns its three parameters; do not add them manually in the
Jenkins UI. On a newly created job, click **Build Now** once. That initial
build stops immediately because neither candidate selector is set, but it
registers the Pipeline-declared parameters with Jenkins. Thereafter Jenkins
displays **Build with Parameters**. The initial empty build is expected and
does not submit a Slurm job.

The selectors are:

| Parameter | Use |
| --- | --- |
| `CANDIDATE_PR` | A positive GitHub PR number. Leave both reference fields empty. Jenkins obtains the PR's current head commit and asks GitHub which base branch that PR targets; that base branch is the baseline. |
| `CANDIDATE_REF` | A candidate branch name or a full 40-character commit SHA. Set this instead of `CANDIDATE_PR`. |
| `BASELINE_REF` | A baseline branch name or full 40-character commit SHA. It is required with `CANDIDATE_REF`, and must be empty with `CANDIDATE_PR`. |

Tags are deliberately not accepted. This keeps a run's input unambiguous and
avoids testing an unexpectedly retargeted tag.

## 7. Run and inspect a build

### Single-session command-line launch

After SSH/Duo authentication, run the launcher from the reviewed CI checkout.
It defaults `GKEYLL_CI_ROOT` to `/scratch/gpfs/$USER/gkeyll_ci`, starts the
controller in a detached `gkeyll_ci` tmux session when necessary, and submits
the existing `gkeyll-ci-stellar-intel` job through its loopback-only API:

```sh
# Queue a PR build and return after Jenkins accepts it.
ci/jenkins/jenkins-stellar-intel.sh run --pr 1104

# Queue a direct-ref build and follow it through its final result.
ci/jenkins/jenkins-stellar-intel.sh run \
  --candidate-ref feature/new-solver --baseline-ref main --follow
```

The default command prints a Jenkins queue ID and returns so the SSH terminal
is immediately available. `follow --queue` resolves a queue item that has
already started through the retained Jenkins build metadata; use the assigned
build number directly when it is known:

```sh
ci/jenkins/jenkins-stellar-intel.sh follow --queue 42
ci/jenkins/jenkins-stellar-intel.sh follow --build 187
ci/jenkins/jenkins-stellar-intel.sh status --build 187

# Recover build and queue identifiers after reconnecting.
ci/jenkins/jenkins-stellar-intel.sh active
ci/jenkins/jenkins-stellar-intel.sh recent
ci/jenkins/jenkins-stellar-intel.sh recent --limit 5
```

`active` lists queued and running builds for this Jenkins job. `recent` lists
the most recent 10 retained builds (or the requested positive `--limit`), with
their queue IDs, selectors, state, time, and Jenkins URL. Both commands include
builds submitted through the UI as well as the launcher.

`--follow` waits for Jenkins to assign the queue item a build number, streams
the Pipeline console (including Slurm state), prints its terminal result, and
returns zero only for `SUCCESS`. It does not move the controller or Pipeline
into the SSH shell: an SSH disconnect or interrupt stops only the local
monitor, while Jenkins in tmux and submitted Slurm jobs continue. Attach to
the controller console for diagnosis with `tmux attach -t gkeyll_ci`.
Press `Ctrl-C` while following to stop monitoring and return to the SSH shell;
it does not abort the Jenkins build.

Internally, the follow operation uses the controller-matched Jenkins CLI JAR
with an API-token credential file; this is useful for direct diagnosis but the
launcher should be used for ordinary runs:

```sh
java -jar "$GKEYLL_CI_ROOT/jenkins-cli.jar" \
  -s http://127.0.0.1:8080 \
  -http \
  -auth @"$JENKINS_HOME/jenkins-cli.auth" \
  console gkeyll-ci-stellar-intel 187 -f
```

The UI remains a fully supported alternative. A UI-triggered build and a
launcher-triggered build submit the same job and parameters; the existing
`disableConcurrentBuilds()` setting makes the later request wait in Jenkins'
queue.

### Browser launch

After reaching Jenkins through the SSH tunnel, select **Build with
Parameters** and choose one selector form. For a PR run, set
`CANDIDATE_PR` (for example `1104`) and leave `CANDIDATE_REF` and
`BASELINE_REF` empty. For a branch or exact-commit run, set both
`CANDIDATE_REF` and `BASELINE_REF`. Jenkins records the requested selectors
and exact checked-out commits, immediately posts the GitHub commit status
`continuous-integration/jenkins/stellar-intel` as pending, builds unit tests
and both installs on the login node, then submits separate unit and C
regression payloads to Slurm. The C job builds accepted outputs from the
selected baseline and compares all
non-ignored C regressions from the candidate against them. Both C suites are
compiled on the login node before
the regression allocation is submitted; the allocation executes only the
precompiled tests.

The final regression check uses the existing
`ci/jenkins/expected_regression_diffs.txt` acknowledgement policy. A real,
reviewed change in numerical output must be added there deliberately; an
unlisted regression difference fails the build and is reported in the Jenkins
console and result database.

The final GitHub-status description contains the candidate C
`passed/acknowledged/unacknowledged` counts, candidate/baseline C compilation
seconds, and unit/regression Slurm execution seconds. Missing metrics from an
early failure are shown as `not-recorded`. The full key/value record is always
printed to the console and archived as `ci-timing-summary.txt`.

At the end of the Pipeline, the same status becomes `success` for a passing
build, `failure` for a build/test/Slurm failure, or `error` for an aborted
build or controller-side cleanup problem. The status is attached to the exact
candidate commit. A PR run therefore appears on its PR; a direct-ref run
appears on that commit's GitHub commit page/history and will also appear in a
PR only if that exact commit later becomes its head. The Stellar Jenkins URL
is private behind SSH/Duo, so GitHub does not receive a build URL.

Failure to publish either the pending or final GitHub status fails the Jenkins
build. This prevents a green Jenkins result from silently lacking its GitHub
report.

While the Slurm job is pending or running, the Jenkins console prints its
state from `squeue`. Once it leaves the queue, Jenkins obtains its final state,
exit code, and `ElapsedRaw` execution time from `sacct`; only `COMPLETED` with
exit code `0:0` is a passing build. Queue time is not included in that elapsed
time. The archived artifacts include:

```text
ci-selection.txt                    requested candidate and baseline selectors
ci-candidate-commit.txt             exact tested candidate commit
ci-baseline-commit.txt              exact Stellar Intel baseline commit
candidate-c-compile-seconds.txt     candidate C-regression compile duration
baseline-c-compile-seconds.txt      baseline C-regression compile duration
ci-regression-summary.txt           candidate C pass/acknowledged/failure counts
ci-timing-summary.txt               aggregate compile and Slurm execution timings
slurm-unit-job-id.txt               submitted unit-job ID
slurm-unit-job-status.txt           unit-job terminal state, exit code, elapsed seconds
slurm-regression-job-id.txt         submitted regression-job ID
slurm-regression-job-status.txt     regression terminal state, exit code, elapsed seconds
slurm-unit-<jobid>.out              unit-job stdout/stderr
slurm-regression-<jobid>.out        regression-job stdout/stderr
`gkylsoft/gkeyll-results/**/regressiondb`  candidate C-regression results
```

To stop a queued or running build, use **Abort** in Jenkins. The submission
shell cancels its recorded Slurm job and waits until it disappears from
`squeue`. If Jenkins terminates that shell before its trap completes, the
Pipeline's finalizer performs the same cancellation and writes the relevant
`slurm-*-job-status.txt` before artifact archival. Do not manually remove the
workspace while the job appears in `squeue`.

If the Jenkins controller itself crashes, its shell trap cannot run. Find the
job ID in the build workspace or Jenkins console, then check and, if needed,
cancel it manually:

```sh
squeue --me
scancel <jobid>
```

After Slurm has stopped, it is safe to remove the abandoned workspace. Restart
Jenkins in the `gkeyll_ci` tmux session as described above; its job
configuration and retained build records live under `$JENKINS_HOME`.

The Pipeline archives artifacts before deleting every completed, failed, or
aborted build workspace. Jenkins retains the 20 most recent build records and
their artifacts; older records are discarded automatically.

## What this does not yet do

- automatic GitHub polling or webhooks;
- Lua regression, MPI regression, MOAT-only regression, or GPU tests;
- automatic recovery of a Slurm job after a Jenkins-controller crash.

Those are future stages, after this manually-triggered CPU unit-and-C-
regression path is reliable.
