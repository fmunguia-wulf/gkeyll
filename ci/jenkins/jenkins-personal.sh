#!/usr/bin/env bash
# Submit and inspect the local personal Jenkins Pipeline. Jenkins itself must
# already be running (for example as a Homebrew or systemd service).
set -euo pipefail

JENKINS_URL="${JENKINS_URL:-http://127.0.0.1:8080}"
JENKINS_JOB="${JENKINS_JOB:-gkeyll-ci-personal}"
JENKINS_CLI_AUTH_FILE="${JENKINS_CLI_AUTH_FILE:-}"
CURL_CONFIG=''
QUEUE_ID=''

die() { echo "ERROR: $*" >&2; exit 1; }
usage() {
    cat <<'EOF'
Usage: jenkins-personal.sh <command> [flags]

Commands:
  run --pr NUMBER [--follow]
  run --candidate-ref REF --baseline-ref REF [--follow]
  follow --build NUMBER
  status --build NUMBER
  active
  recent [--limit NUMBER]

Jenkins must already be running. Set JENKINS_CLI_AUTH_FILE to a protected
file containing one line: jenkins-user:api-token. JENKINS_URL and JENKINS_JOB
override the loopback URL and gkeyll-ci-personal defaults.
EOF
}
job_path() { local p='/job' n; IFS=/ read -ra n <<< "$JENKINS_JOB"; for x in "${n[@]}"; do p+="/$x/job"; done; printf '%s' "${p%/job}"; }
prepare_auth() {
    [[ -n "$JENKINS_CLI_AUTH_FILE" ]] || die 'Set JENKINS_CLI_AUTH_FILE to a mode-600 user:api-token file'
    [[ -O "$JENKINS_CLI_AUTH_FILE" ]] || die "credential file is not owned by $USER"
    [[ "$(stat -f '%Lp' "$JENKINS_CLI_AUTH_FILE" 2>/dev/null || stat -c '%a' "$JENKINS_CLI_AUTH_FILE")" == 600 ]] || die 'credential file must have mode 600'
    local credential; credential="$(<"$JENKINS_CLI_AUTH_FILE")"
    [[ "$credential" =~ ^[^[:space:]:]+:[^[:space:]:]+$ ]] || die 'credential file must contain user:api-token'
    CURL_CONFIG="$(mktemp "${TMPDIR:-/tmp}/gkeyll-jenkins.XXXXXX")"; chmod 600 "$CURL_CONFIG"
    printf 'user = "%s"\n' "$credential" > "$CURL_CONFIG"
    trap 'rm -f "$CURL_CONFIG"' EXIT
}
curl_auth() { curl --fail --silent --show-error --globoff --config "$CURL_CONFIG" "$@"; }
positive() { [[ "$2" =~ ^[1-9][0-9]*$ ]] || die "$1 must be a positive integer"; }
state() {
    local payload
    payload="$(curl_auth "${JENKINS_URL}$(job_path)/$1/api/json")" || return 75
    python3 -c 'import json,sys; x=json.load(sys.stdin); print("BUILDING" if x["building"] else (x.get("result") or "UNKNOWN"))' <<< "$payload"
}
status() {
    positive 'build number' "$1"
    local s
    s="$(state "$1")" || die "Build #$1 is not available yet. Retry shortly."
    echo "Build #$1: $s"
    [[ "$s" == SUCCESS ]]
}
follow() {
    positive 'build number' "$1"
    while :; do
        local s
        if ! s="$(state "$1")"; then
            echo "Build #$1 is not available yet; retrying." >&2
            sleep 2
            continue
        fi
        [[ "$s" == BUILDING ]] || { echo "Build #$1: $s"; [[ "$s" == SUCCESS ]]; return; }
        sleep 5
    done
}
submit() {
    local pr="$1" candidate="$2" baseline="$3" headers queue
    headers="$(mktemp "${TMPDIR:-/tmp}/gkeyll-jenkins-headers.XXXXXX")"
    curl_auth --dump-header "$headers" --output /dev/null --request POST \
        --data-urlencode "CANDIDATE_PR=$pr" --data-urlencode "CANDIDATE_REF=$candidate" --data-urlencode "BASELINE_REF=$baseline" \
        "${JENKINS_URL}$(job_path)/buildWithParameters" || { rm -f "$headers"; die 'Jenkins rejected the build'; }
    queue="$(awk 'BEGIN{IGNORECASE=1} /^Location:/{sub(/^[^:]*: /,""); sub(/\r$/,""); print; exit}' "$headers")"; rm -f "$headers"
    [[ "$queue" =~ /queue/item/([0-9]+)/ ]] || die 'Jenkins accepted the build but returned no queue ID'
    QUEUE_ID="${BASH_REMATCH[1]}"
    echo "Queued $JENKINS_JOB as queue item $QUEUE_ID"
}
queue_build() { curl_auth "${JENKINS_URL}/queue/item/$1/api/json" 2>/dev/null | python3 -c 'import json,sys; x=json.load(sys.stdin); print((x.get("executable") or {}).get("number", ""))' 2>/dev/null || true; }
run() {
    local pr='' candidate='' baseline='' want_follow=false
    while (($#)); do case "$1" in --pr) (($#>=2))||die '--pr requires a number'; pr="$2"; shift 2;; --candidate-ref) (($#>=2))||die '--candidate-ref requires a ref'; candidate="$2"; shift 2;; --baseline-ref) (($#>=2))||die '--baseline-ref requires a ref'; baseline="$2"; shift 2;; --follow) want_follow=true; shift;; *) die "unknown run option: $1";; esac; done
    if [[ -n "$pr" ]]; then positive '--pr' "$pr"; [[ -z "$candidate$baseline" ]] || die '--pr cannot be combined with refs'; else [[ -n "$candidate" && -n "$baseline" ]] || die 'provide --pr, or both --candidate-ref and --baseline-ref'; fi
    submit "$pr" "$candidate" "$baseline"
    if [[ "$want_follow" == true ]]; then
        local build=''
        while [[ -z "$build" ]]; do build="$(queue_build "$QUEUE_ID")"; [[ -n "$build" ]] || sleep 2; done
        follow "$build"
    fi
}
list_builds() { local limit="${1:-10}"; positive '--limit' "$limit"; curl_auth "${JENKINS_URL}$(job_path)/api/json?tree=builds[number,building,result,timestamp,actions[parameters[name,value]]]" | python3 -c 'import json,sys; limit=int(sys.argv[1]); x=json.load(sys.stdin); [print("#%s %s"%(b["number"], "BUILDING" if b["building"] else b.get("result","UNKNOWN"))) for b in x.get("builds",[])[:limit]]' "$limit"; }
main() {
    (($#)) || { usage; exit 2; }
    case "$1" in -h|--help|help) usage; return;; esac
    prepare_auth
    case "$1" in run) shift; run "$@";; follow) shift; [[ $# == 2 && $1 == --build ]] || die 'usage: follow --build NUMBER'; follow "$2";; status) shift; [[ $# == 2 && $1 == --build ]] || die 'usage: status --build NUMBER'; status "$2";; active) shift; [[ $# == 0 ]] || die 'usage: active'; list_builds 100;; recent) shift; [[ $# == 0 || ( $# == 2 && $1 == --limit ) ]] || die 'usage: recent [--limit NUMBER]'; list_builds "${2:-10}";; -h|--help|help) usage;; *) usage >&2; die "unknown command: $1";; esac
}
main "$@"
