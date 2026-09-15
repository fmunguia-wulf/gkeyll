#!/usr/bin/env bash
#
# Start and use the private, loopback-only Jenkins controller on Stellar Intel.
# This is intentionally a controller launcher, not a replacement for the
# Jenkins UI: both clients submit the same parameterized Pipeline job.

set -euo pipefail

readonly SCRIPT_PATH="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/$(basename -- "${BASH_SOURCE[0]}")"
readonly DEFAULT_JAVA_HOME=/usr/lib/jvm/java-21-openjdk-21.0.12.1.1-1.1.el8.x86_64

CI_ROOT="${GKEYLL_CI_ROOT:-/scratch/gpfs/${USER:?USER must be set}/gkeyll_ci}"
JENKINS_HOME="${JENKINS_HOME:-$CI_ROOT/jenkins_home}"
JENKINS_WEBROOT="${JENKINS_WEBROOT:-$CI_ROOT/jenkins_webroot}"
JENKINS_TMPDIR="${TMPDIR:-$CI_ROOT/tmp}"
JENKINS_PORT="${JENKINS_PORT:-8080}"
JENKINS_URL="${JENKINS_URL:-http://127.0.0.1:$JENKINS_PORT}"
JENKINS_JOB="${JENKINS_JOB:-gkeyll-ci-stellar-intel}"
JENKINS_SESSION="${JENKINS_SESSION:-gkeyll_ci}"
JENKINS_CLI_AUTH_FILE="${JENKINS_CLI_AUTH_FILE:-$JENKINS_HOME/jenkins-cli.auth}"
JAVA_HOME="${JAVA_HOME:-$DEFAULT_JAVA_HOME}"
CLI_JAR="$CI_ROOT/jenkins-cli.jar"

CURL_CONFIG=''
SUBMITTED_QUEUE_ID=''
RESOLVED_BUILD_NUMBER=''

die() {
    echo "ERROR: $*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage:
  jenkins-stellar-intel.sh start
  jenkins-stellar-intel.sh run --pr NUMBER [--follow]
  jenkins-stellar-intel.sh run --candidate-ref REF --baseline-ref REF [--follow]
  jenkins-stellar-intel.sh follow --queue ID
  jenkins-stellar-intel.sh follow --build NUMBER
  jenkins-stellar-intel.sh status --queue ID
  jenkins-stellar-intel.sh status --build NUMBER

The run command submits gkeyll-ci-stellar-intel and normally returns as soon
as Jenkins accepts the request. --follow waits for the queue item to become a
build, streams its console, and returns that build's final result.

GKEYLL_CI_ROOT defaults to /scratch/gpfs/$USER/gkeyll_ci. The Jenkins API
credential file defaults to $JENKINS_HOME/jenkins-cli.auth and must contain
one line in the form: jenkins-user:api-token
EOF
}

prepare_paths() {
    mkdir -p "$JENKINS_HOME" "$JENKINS_WEBROOT" "$JENKINS_TMPDIR" \
        "$CI_ROOT/logs" "$CI_ROOT/workspaces"
}

controller_running() {
    tmux has-session -t "$JENKINS_SESSION" 2>/dev/null
}

wait_for_controller() {
    local attempt
    for attempt in {1..30}; do
        if curl --fail --silent --show-error --max-time 5 \
            --output /dev/null "$JENKINS_URL/login"; then
            return 0
        fi
        sleep 2
    done
    die "Jenkins did not become ready at $JENKINS_URL. Inspect: tmux attach -t $JENKINS_SESSION"
}

start_controller() {
    prepare_paths
    [[ -x "$JAVA_HOME/bin/java" ]] || die "Java 21 was not found at $JAVA_HOME/bin/java"
    [[ -f "$CI_ROOT/jenkins.war" ]] || die "Jenkins WAR is missing: $CI_ROOT/jenkins.war"

    if controller_running; then
        echo "Jenkins tmux session already exists: $JENKINS_SESSION"
    else
        tmux new-session -d -s "$JENKINS_SESSION" "$SCRIPT_PATH" __controller
        echo "Started Jenkins in tmux session: $JENKINS_SESSION"
    fi
    wait_for_controller
    echo "Jenkins is ready at $JENKINS_URL"
}

run_controller() {
    prepare_paths
    [[ -x "$JAVA_HOME/bin/java" ]] || die "Java 21 was not found at $JAVA_HOME/bin/java"
    [[ -f "$CI_ROOT/jenkins.war" ]] || die "Jenkins WAR is missing: $CI_ROOT/jenkins.war"

    export JENKINS_HOME JENKINS_WEBROOT
    export TMPDIR="$JENKINS_TMPDIR"
    export PATH="$JAVA_HOME/bin:$PATH"
    "$JAVA_HOME/bin/java" -Djava.io.tmpdir="$TMPDIR" -jar "$CI_ROOT/jenkins.war" \
        --webroot="$JENKINS_WEBROOT" \
        --httpListenAddress=127.0.0.1 \
        --httpPort="$JENKINS_PORT" \
        2>&1 | tee -a "$CI_ROOT/logs/jenkins.log"
}

cleanup_curl_config() {
    if [[ -n "$CURL_CONFIG" ]]; then
        rm -f "$CURL_CONFIG"
    fi
}

prepare_auth() {
    [[ -f "$JENKINS_CLI_AUTH_FILE" ]] || die "Jenkins API credential file is missing: $JENKINS_CLI_AUTH_FILE"
    [[ -O "$JENKINS_CLI_AUTH_FILE" ]] || die "Jenkins API credential file is not owned by $USER: $JENKINS_CLI_AUTH_FILE"

    local mode credential
    mode="$(stat -c '%a' "$JENKINS_CLI_AUTH_FILE")"
    [[ "$mode" == 600 ]] || die "Jenkins API credential file must have mode 600: $JENKINS_CLI_AUTH_FILE"
    credential="$(<"$JENKINS_CLI_AUTH_FILE")"
    [[ "$credential" =~ ^[^[:space:]:]+:[^[:space:]:]+$ ]] || die "Jenkins API credential file must contain exactly user:api-token"
    [[ "$credential" != *'"'* && "$credential" != *'\\'* ]] || die "Jenkins API credential file contains an unsupported character"

    CURL_CONFIG="$(mktemp "$JENKINS_TMPDIR/jenkins-curl.XXXXXX")"
    chmod 600 "$CURL_CONFIG"
    printf 'user = "%s"\n' "$credential" > "$CURL_CONFIG"
    trap cleanup_curl_config EXIT
}

curl_auth() {
    curl --fail --silent --show-error --globoff --config "$CURL_CONFIG" "$@"
}

download_cli() {
    if [[ ! -s "$CLI_JAR" ]]; then
        local temporary_jar
        temporary_jar="$(mktemp "$JENKINS_TMPDIR/jenkins-cli.XXXXXX")"
        curl --fail --silent --show-error --output "$temporary_jar" \
            "$JENKINS_URL/jnlpJars/jenkins-cli.jar"
        mv "$temporary_jar" "$CLI_JAR"
    fi
}

require_positive_integer() {
    local name="$1" value="$2"
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || die "$name must be a positive integer, got '$value'"
}

queue_state() {
    local queue_id="$1" payload
    payload="$(curl_auth "$JENKINS_URL/queue/item/$queue_id/api/json")" || return 1
    python3 -c '
import json
import sys
item = json.load(sys.stdin)
executable = item.get("executable") or {}
number = executable.get("number", "")
cancelled = "true" if item.get("cancelled", False) else "false"
why = " ".join((item.get("why") or "").split())
print(f"{number}\t{cancelled}\t{why}")
' <<< "$payload"
}

build_for_queue() {
    local queue_id="$1" payload
    payload="$(curl_auth "$JENKINS_URL/job/$JENKINS_JOB/api/json?tree=builds[number,queueId]")" || return 1
    python3 -c '
import json
import sys

queue_id = int(sys.argv[1])
for build in json.load(sys.stdin).get("builds", []):
    if build.get("queueId") == queue_id:
        print(build["number"])
        break
' "$queue_id" <<< "$payload"
}

build_state() {
    local build_number="$1" payload
    payload="$(curl_auth "$JENKINS_URL/job/$JENKINS_JOB/$build_number/api/json")" || return 1
    python3 -c '
import json
import sys
build = json.load(sys.stdin)
print("true" if build.get("building", False) else "false")
print(build.get("result") or "")
' <<< "$payload"
}

wait_for_build_number() {
    local queue_id="$1" state number cancelled why previous='' attempt
    while :; do
        if ! state="$(queue_state "$queue_id")"; then
            # Jenkins normally removes a queue item as soon as it starts its
            # build. Build records retain queueId, so recover the assignment
            # from the job rather than requiring the caller to discover it.
            for attempt in {1..6}; do
                if number="$(build_for_queue "$queue_id")" && [[ -n "$number" ]]; then
                    RESOLVED_BUILD_NUMBER="$number"
                    return 0
                fi
                sleep 2
            done
            die "Queue item $queue_id is unavailable and no matching Jenkins build was found"
        fi
        IFS=$'\t' read -r number cancelled why <<< "$state"
        [[ "$cancelled" == false ]] || die "Queue item $queue_id was cancelled"
        if [[ -n "$number" ]]; then
            RESOLVED_BUILD_NUMBER="$number"
            return 0
        fi
        if [[ "$why" != "$previous" ]]; then
            echo "Queue item $queue_id: ${why:-waiting}" >&2
            previous="$why"
        fi
        sleep 5
    done
}

follow_build() {
    local build_number="$1" state building result
    require_positive_integer 'build number' "$build_number"
    download_cli
    echo "Following $JENKINS_JOB #$build_number"
    echo "Build URL: $JENKINS_URL/job/$JENKINS_JOB/$build_number/"

    # -f follows console output without propagating a client interruption to
    # the Jenkins build. The controller and Slurm work remain independent of
    # the SSH terminal.
    if ! "$JAVA_HOME/bin/java" -jar "$CLI_JAR" -s "$JENKINS_URL" \
        -auth "@$JENKINS_CLI_AUTH_FILE" console "$JENKINS_JOB" "$build_number" -f; then
        echo "Console follower ended before Jenkins reported a terminal result; checking build status." >&2
    fi

    while :; do
        mapfile -t state < <(build_state "$build_number")
        building="${state[0]:-}"
        result="${state[1]:-}"
        [[ "$building" == false ]] && break
        sleep 5
    done

    echo "Build #$build_number result: ${result:-UNKNOWN}"
    [[ "$result" == SUCCESS ]]
}

submit_build() {
    local candidate_pr="$1" candidate_ref="$2" baseline_ref="$3"
    local headers queue_url queue_id
    headers="$(mktemp "$JENKINS_TMPDIR/jenkins-headers.XXXXXX")"

    if ! curl_auth --dump-header "$headers" --output /dev/null --request POST \
        --data-urlencode "CANDIDATE_PR=$candidate_pr" \
        --data-urlencode "CANDIDATE_REF=$candidate_ref" \
        --data-urlencode "BASELINE_REF=$baseline_ref" \
        "$JENKINS_URL/job/$JENKINS_JOB/buildWithParameters"; then
        rm -f "$headers"
        die 'Jenkins rejected the build submission'
    fi
    queue_url="$(awk 'BEGIN { IGNORECASE=1 } /^Location:/ { sub(/^[^:]*:[[:space:]]*/, ""); sub(/\r$/, ""); print; exit }' "$headers")"
    rm -f "$headers"
    [[ "$queue_url" =~ /queue/item/([0-9]+)/ ]] || die "Jenkins accepted the request but did not return a queue location"
    queue_id="${BASH_REMATCH[1]}"

    echo "Queued $JENKINS_JOB as queue item $queue_id" >&2
    echo "Queue URL: $queue_url" >&2
    echo "Monitor it with: $SCRIPT_PATH follow --queue $queue_id" >&2
    SUBMITTED_QUEUE_ID="$queue_id"
}

run_command() {
    local candidate_pr='' candidate_ref='' baseline_ref='' follow=false queue_id
    while (($#)); do
        case "$1" in
            --pr)
                (($# >= 2)) || die '--pr requires a number'
                candidate_pr="$2"
                shift 2
                ;;
            --candidate-ref)
                (($# >= 2)) || die '--candidate-ref requires a ref'
                candidate_ref="$2"
                shift 2
                ;;
            --baseline-ref)
                (($# >= 2)) || die '--baseline-ref requires a ref'
                baseline_ref="$2"
                shift 2
                ;;
            --follow)
                follow=true
                shift
                ;;
            *) die "unknown run option: $1" ;;
        esac
    done

    if [[ -n "$candidate_pr" ]]; then
        require_positive_integer '--pr' "$candidate_pr"
        [[ -z "$candidate_ref$baseline_ref" ]] || die '--pr cannot be combined with candidate/baseline refs'
    else
        [[ -n "$candidate_ref" && -n "$baseline_ref" ]] || die 'provide --pr, or both --candidate-ref and --baseline-ref'
    fi

    start_controller
    prepare_auth
    submit_build "$candidate_pr" "$candidate_ref" "$baseline_ref"
    queue_id="$SUBMITTED_QUEUE_ID"
    if [[ "$follow" == true ]]; then
        local build_number
        wait_for_build_number "$queue_id"
        build_number="$RESOLVED_BUILD_NUMBER"
        follow_build "$build_number"
    fi
}

follow_command() {
    [[ $# -eq 2 ]] || die 'usage: follow --queue ID | follow --build NUMBER'
    start_controller
    prepare_auth
    case "$1" in
        --queue)
            require_positive_integer 'queue ID' "$2"
            wait_for_build_number "$2"
            follow_build "$RESOLVED_BUILD_NUMBER"
            ;;
        --build)
            follow_build "$2"
            ;;
        *) die 'usage: follow --queue ID | follow --build NUMBER' ;;
    esac
}

status_command() {
    [[ $# -eq 2 ]] || die 'usage: status --queue ID | status --build NUMBER'
    start_controller
    prepare_auth
    case "$1" in
        --queue)
            require_positive_integer 'queue ID' "$2"
            local state number cancelled why
            state="$(queue_state "$2")" || die "Queue item $2 is unavailable"
            IFS=$'\t' read -r number cancelled why <<< "$state"
            if [[ -n "$number" ]]; then
                echo "Queue item $2 is build #$number"
                status_command --build "$number"
            elif [[ "$cancelled" == true ]]; then
                echo "Queue item $2: CANCELLED"
                return 1
            else
                echo "Queue item $2: ${why:-waiting}"
            fi
            ;;
        --build)
            require_positive_integer 'build number' "$2"
            local build
            mapfile -t build < <(build_state "$2")
            if [[ "${build[0]:-}" == true ]]; then
                echo "Build #$2: BUILDING"
            else
                echo "Build #$2: ${build[1]:-UNKNOWN}"
                [[ "${build[1]:-}" == SUCCESS ]]
            fi
            ;;
        *) die 'usage: status --queue ID | status --build NUMBER' ;;
    esac
}

main() {
    (($# >= 1)) || { usage; exit 2; }
    case "$1" in
        __controller) shift; (($# == 0)) || die '__controller takes no arguments'; run_controller ;;
        start) shift; (($# == 0)) || die 'start takes no arguments'; start_controller ;;
        run) shift; run_command "$@" ;;
        follow) shift; follow_command "$@" ;;
        status) shift; status_command "$@" ;;
        -h|--help|help) usage ;;
        *) usage >&2; die "unknown command: $1" ;;
    esac
}

main "$@"
