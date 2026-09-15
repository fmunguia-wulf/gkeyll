# Team-workstation Jenkins CI

This setup uses a shared workstation to poll GitHub for pull requests into
`main` from every author. It also allows a maintainer to request a selected PR
or candidate/baseline comparison from the command line.

## Install and configure Jenkins

Install Jenkins LTS and Java 21 or newer as a local service. Install Pipeline,
Git, Credentials Binding, Git client, and GitHub Branch Source plugins. Create
a GitHub credential with repository contents/pull-request read access and
commit-status write access.

Label the workstation build node, set its non-interactive PATH to include the
toolchain, `cmake`, and Python with NumPy, then define these Jenkins global
environment variables:

| Name | Meaning |
| --- | --- |
| `TEAM_WORKSTATION_NODE_LABEL` | Workstation Jenkins node label. |
| `TEAM_WORKSTATION_MKDEPS_SCRIPT` | Filename below `machines/`. |
| `TEAM_WORKSTATION_CONFIGURE_SCRIPT` | Filename below `machines/`. |
| `TEAM_WORKSTATION_GITHUB_CREDENTIAL_ID` | GitHub credential ID. |
| `TEAM_WORKSTATION_BUILD_JOBS` | Optional build parallelism; default `3`. |
| `TEAM_WORKSTATION_REGRESSION_JOBS` | Optional C-regression parallelism; default `1`. |

Create a Multibranch Pipeline named `gkeyll-ci-team-workstation` with the
GitHub source `gkeyllorg/gkeyll`. Discover `main` and pull requests, configure
the branch-source behavior to exclude ordinary branches that are also PRs, and
set Script Path to `ci/jenkins/jenkinsfile.team_workstation`. Enable periodic
multibranch scans at a two-minute interval. The Pipeline skips plain branches
and PRs whose target is not `main`.

The `main` child is the trusted entry point for command-line selected runs. The
team client therefore defaults `JENKINS_JOB` to
`gkeyll-ci-team-workstation/main`; run an initial multibranch scan before using
the client. Automated PR builds use the PR child job. The Pipeline publishes
`continuous-integration/jenkins/team-workstation` for the exact tested commit.

## Use the client

Store a Jenkins API token as a user-owned mode-600 `jenkins-user:api-token`
file and set `JENKINS_CLI_AUTH_FILE` to its path.

```sh
export JENKINS_CLI_AUTH_FILE="$HOME/.config/gkeyll/jenkins-cli.auth"
ci/jenkins/jenkins-team-workstation.sh scan
ci/jenkins/jenkins-team-workstation.sh run --pr 1234 --follow
ci/jenkins/jenkins-team-workstation.sh run \
  --candidate-ref feature/new-solver --baseline-ref main
ci/jenkins/jenkins-team-workstation.sh active
```

`JENKINS_URL` defaults to loopback. Override `JENKINS_JOB` to select another
trusted child and `JENKINS_ROOT_JOB` to select another multibranch root. The
client never starts Jenkins or tmux. `scan` requests an immediate multibranch
index; periodic scans remain the normal trigger.

Automatic and selected runs build isolated candidate/baseline prefixes, run
unit and C regression checks, archive diagnostics, and fail on unacknowledged
entries from `expected_regression_diffs.txt`.
