# Manual Jenkins CI on NERSC Perlmutter GPU

This Level-1 CUDA CI lane is manually triggered after SSH/MFA authentication.
It runs a trusted pipeline, rather than pipeline code from the candidate pull
request. The pipeline builds candidate and baseline CUDA/NCCL installations on
the login node, then submits the unit and C-regression phases to Perlmutter GPU
nodes.

The GPU build makes `make unit-run` execute CUDA unit tests automatically.
For C regressions, the baseline `create` run deliberately forces CPU mode;
the candidate `check` run executes CPU and GPU variants for GPU-capable
layers, compares them with the CPU baseline, and records GPU results.

## Naming migration

This GPU lane supersedes the former `perlmutter-gpu` Jenkins identity. Create
or reconfigure the job as `gkeyll-ci-perlmutter_gpu`, use the
`continuous-integration/jenkins/perlmutter_gpu` GitHub status, and replace its
node label and global `PERLMUTTER_*` settings with the `perlmutter_gpu` and
`PERLMUTTER_GPU_*` values documented below. Retire the old job only after the
new job has completed a successful build.

## CI files

Keep these files on a reviewed trusted branch, initially named
`agent_tools-jenkins-perlmutter_gpu`:

```text
ci/jenkins/Jenkinsfile.perlmutter_gpu
ci/jenkins/jenkins-perlmutter_gpu.sh
ci/jenkins/slurm-unit-tests.perlmutter_gpu.sh
ci/jenkins/slurm-regression-tests.perlmutter_gpu.sh
ci/jenkins/README.perlmutter_gpu.md
machines/module_load.perlmutter-gpu.sh
machines/mkdeps.perlmutter.gpu.sh
machines/configure.perlmutter.gpu.sh
```

Create an immutable comparison baseline from that reviewed commit:

```sh
git branch agent_tools-jenkins-perlmutter_gpu-baseline HEAD
git push -u origin agent_tools-jenkins-perlmutter_gpu-baseline
```

Once validated and merged, change the Jenkins job's SCM branch to `main`.
The pipeline still resolves each PR's target branch as its baseline.

## Perlmutter resources and environment

Set `GKEYLL_CI_ROOT` to a project scratch directory visible to compute nodes,
for example:

```sh
export GKEYLL_CI_ROOT=/pscratch/sd/<first-letter>/<username>/gkeyll_ci
```

Do not use `/tmp`; Jenkins workspaces, Slurm output, and both installations
must be visible from the submitted node. Scratch may be purged, so retain no
durable credentials or only copies of build records there.

Each test job requests one task, 32 CPUs, one GPU, `--constraint=gpu`, and
the `shared` QoS by default. NERSC requires both the GPU constraint and an
explicit GPU request. The shared QoS is appropriate for one GPU. See
[NERSC's Perlmutter job guide](https://docs.nersc.gov/systems/perlmutter/running-jobs/).

The shared module file loads the same compiler, CUDA, MPI, NCCL, and LibSci
versions used by the existing Perlmutter GPU machine scripts. It also sets the
runtime environment needed for NCCL communication.

## Validate manually

On Perlmutter, clone the trusted branch and build in a disposable directory:

```sh
git clone --branch agent_tools-jenkins-perlmutter_gpu --single-branch \
  https://github.com/gkeyllorg/gkeyll.git gkeyll
cd gkeyll
PREFIX="$PWD/../gkylsoft" ./machines/mkdeps.perlmutter.gpu.sh
PREFIX="$PWD/../gkylsoft" ./machines/configure.perlmutter.gpu.sh
. machines/module_load.perlmutter-gpu.sh
make -j3 unit
make -j3 install
```

Run the GPU unit payload with a valid project account:

```sh
sbatch --wait --account <project> --qos shared --constraint gpu \
  --nodes 1 --ntasks 1 --cpus-per-task 32 --gpus-per-task 1 \
  --time 00:30:00 --chdir "$PWD" --export=ALL,CI_WORKSPACE="$PWD" \
  ci/jenkins/slurm-unit-tests.perlmutter_gpu.sh
```

Before enabling Jenkins, also create a checked-out baseline, build and install
it with its own prefix, run `runregression configure` plus
`runregression run -c compile` in both trees, then submit
`slurm-regression-tests.perlmutter_gpu.sh` with `CI_BASELINE_DIR`,
`CI_BASELINE_PREFIX`, `CI_CANDIDATE_PREFIX`, `CI_REGRESSION_JOBS=4`, and
`CI_REGRESSION_TEST_TIMEOUT=900`. The Jenkins pipeline supplies these
variables automatically.

## Jenkins configuration

Run a Jenkins controller or agent in an authenticated Perlmutter session only
as permitted by local NERSC policy. Configure its workspace root under
`$GKEYLL_CI_ROOT/workspaces`, on shared storage. Install the Pipeline, Git,
Credentials Binding, and GitHub plugins. Add a GitHub credential with
repository read access and commit-status write access.

Create a Pipeline job named `gkeyll-ci-perlmutter_gpu`:

- Definition: Pipeline script from SCM.
- Repository: `https://github.com/gkeyllorg/gkeyll.git`.
- Branch: `*/agent_tools-jenkins-perlmutter_gpu` during bring-up.
- Script path: `ci/jenkins/Jenkinsfile.perlmutter_gpu`.
- Node label: `perlmutter_gpu` (or set the matching override below).

Set these Jenkins global environment variables:

| Variable | Value |
| --- | --- |
| `GKEYLL_CI_ROOT` | Absolute shared CI root |
| `PERLMUTTER_GPU_GITHUB_CREDENTIAL_ID` | GitHub credential ID |
| `PERLMUTTER_GPU_SLURM_ACCOUNT` | Required NERSC GPU project/account |
| `PERLMUTTER_GPU_NODE_LABEL` | Optional; defaults to `perlmutter_gpu` |
| `PERLMUTTER_GPU_SLURM_QOS` | Optional; defaults to `shared` |
| `PERLMUTTER_GPU_BUILD_JOBS` | Optional login-node build parallelism; defaults to `3` |
| `PERLMUTTER_GPU_UNIT_TIME` | Optional unit-test allocation limit; defaults to `00:30:00` |
| `PERLMUTTER_GPU_REGRESSION_TIME` | Optional regression limit; defaults to `04:00:00` |
| `PERLMUTTER_GPU_REGRESSION_JOBS` | Optional CPU-phase concurrency; defaults to `4` |
| `PERLMUTTER_GPU_REGRESSION_TEST_TIMEOUT` | Optional per-test limit in seconds; defaults to `900` |

The provided launcher uses the loopback Jenkins API:

```sh
ci/jenkins/jenkins-perlmutter_gpu.sh run --pr 1234
ci/jenkins/jenkins-perlmutter_gpu.sh run --candidate-ref feature --baseline-ref main
ci/jenkins/jenkins-perlmutter_gpu.sh follow --queue <id>
```

Use `jenkins-perlmutter_gpu.sh abort --queue ID` or
`jenkins-perlmutter_gpu.sh abort --build NUMBER` to cancel a build; Jenkins
**Abort** is an equivalent UI action. The pipeline cancels a submitted Slurm
job, records its final state, archives Slurm output and regression databases,
publishes the final GitHub status, and removes its isolated workspace.
