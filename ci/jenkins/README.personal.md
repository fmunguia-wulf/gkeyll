# Personal-computer Jenkins CI

This setup is for a computer whose owner explicitly chooses which Gkeyll PR or
candidate/baseline comparison to execute. It does not poll GitHub and does not
run unselected contributor code.

## Install and configure Jenkins

Install Jenkins LTS and Java 21 or newer using the normal local service
mechanism (for example, `brew install jenkins-lts` and `brew services start
jenkins-lts` on macOS). Install Pipeline, Git, Credentials Binding, Git client,
and GitHub Branch Source plugins.

Create a fine-grained GitHub token for `gkeyllorg/gkeyll` with `Contents: Read`,
`Pull requests: Read`, and `Commit statuses: Read and write`. Add it to Jenkins
as a Username-with-password credential and record its credential ID.

Label the build node and set its node PATH so it contains compilers, `cmake`,
and Python with NumPy. In Jenkins global environment variables set:

| Name | Meaning |
| --- | --- |
| `PERSONAL_NODE_LABEL` | Label of the local Jenkins build node. |
| `PERSONAL_MKDEPS_SCRIPT` | Filename below `machines/`, e.g. `mkdeps.macos.sh`. |
| `PERSONAL_CONFIGURE_SCRIPT` | Filename below `machines/`, e.g. `configure.macos.sh`. |
| `PERSONAL_GITHUB_CREDENTIAL_ID` | GitHub credential ID. |
| `PERSONAL_BUILD_JOBS` | Optional build parallelism; default `3`. |
| `PERSONAL_REGRESSION_JOBS` | Optional C-regression parallelism; default `1`. |

Create a Pipeline job named `gkeyll-ci-personal`. Choose **Pipeline script from
SCM**, use the repository's trusted `main` branch, and set Script Path to
`ci/jenkins/jenkinsfile.personal`. Do not let a selected PR supply the Pipeline
script. Run it once without parameters so Jenkins registers the parameters.

The job publishes `continuous-integration/jenkins/personal` to the exact tested
candidate commit. It first posts `pending`, then posts `success`, `failure`, or
`error` after cleanup.

## Run a build

Sign in to the local Jenkins UI as the account that will run the client. Open
the account's security page (**Manage Jenkins** → **Users** → your user →
**Configure**), use **API Token** → **Add new Token**, give it a descriptive
name such as `gkeyll-personal-cli`, and copy the token when Jenkins displays
it. Jenkins shows a newly generated token only once.

Store the Jenkins username and token in a file owned by the Unix account that
will use the client. Do not use the GitHub token from the previous setup step:
this is a Jenkins API token and is used only to authenticate to Jenkins.

```sh
mkdir -p "$HOME/.config/gkeyll"
umask 077
printf '%s:%s\n' '<jenkins-user>' '<jenkins-api-token>' \
  > "$HOME/.config/gkeyll/jenkins-cli.auth"
chmod 600 "$HOME/.config/gkeyll/jenkins-cli.auth"
```

The file must contain exactly one `jenkins-user:api-token` line, have mode
`600`, and must not be committed to the repository.

```sh
export JENKINS_CLI_AUTH_FILE="$HOME/.config/gkeyll/jenkins-cli.auth"
ci/jenkins/jenkins-personal.sh run --pr 1234 --follow
ci/jenkins/jenkins-personal.sh run \
  --candidate-ref feature/new-solver --baseline-ref main
ci/jenkins/jenkins-personal.sh active
ci/jenkins/jenkins-personal.sh recent --limit 5
```

`JENKINS_URL` defaults to `http://127.0.0.1:8080`; `JENKINS_JOB` defaults to
`gkeyll-ci-personal`. The `run` command accepts either `--pr NUMBER`, or both
`--candidate-ref REF` and `--baseline-ref REF`. Branch names and full
40-character commit SHAs are accepted; tags are not. `--follow` waits for the
queued build to complete. Use the Jenkins UI as an equivalent alternative.

Each run builds the candidate and its baseline in isolated workspace prefixes,
runs unit tests, compiles regressions, compares non-ignored C regressions, and
archives logs and regression databases. Expected numerical changes must be
listed in `expected_regression_diffs.txt` with a reason.
