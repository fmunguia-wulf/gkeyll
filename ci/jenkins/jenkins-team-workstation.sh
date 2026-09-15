#!/usr/bin/env bash
# Submit, rescan, and inspect the local team-workstation Jenkins job. Jenkins
# must already be running; this script never starts a controller or tmux.
set -euo pipefail
JENKINS_URL="${JENKINS_URL:-http://127.0.0.1:8080}"
JENKINS_JOB="${JENKINS_JOB:-gkeyll-ci-team-workstation/main}"
JENKINS_ROOT_JOB="${JENKINS_ROOT_JOB:-gkeyll-ci-team-workstation}"
JENKINS_CLI_AUTH_FILE="${JENKINS_CLI_AUTH_FILE:-}"
CURL_CONFIG=''; QUEUE_ID=''
die() { echo "ERROR: $*" >&2; exit 1; }
usage() { cat <<'EOF'
Usage: jenkins-team-workstation.sh <command> [flags]

Commands:
  scan
  run --pr NUMBER [--follow]
  run --candidate-ref REF --baseline-ref REF [--follow]
  follow --build NUMBER
  status --build NUMBER
  active
  recent [--limit NUMBER]

Jenkins must already be running. JENKINS_JOB defaults to the trusted main
child of gkeyll-ci-team-workstation; JENKINS_ROOT_JOB is used by scan.
EOF
}
path_for() { local job="$1" p='/job' x; IFS=/ read -ra part <<< "$job"; for x in "${part[@]}"; do p+="/$x/job"; done; printf '%s' "${p%/job}"; }
prepare() { [[ -n "$JENKINS_CLI_AUTH_FILE" ]] || die 'Set JENKINS_CLI_AUTH_FILE to a mode-600 user:api-token file'; [[ -O "$JENKINS_CLI_AUTH_FILE" ]] || die "credential file is not owned by $USER"; [[ "$(stat -f '%Lp' "$JENKINS_CLI_AUTH_FILE" 2>/dev/null || stat -c '%a' "$JENKINS_CLI_AUTH_FILE")" == 600 ]] || die 'credential file must have mode 600'; local c="$(<"$JENKINS_CLI_AUTH_FILE")"; [[ "$c" =~ ^[^[:space:]:]+:[^[:space:]:]+$ ]] || die 'credential file must contain user:api-token'; CURL_CONFIG="$(mktemp "${TMPDIR:-/tmp}/gkeyll-jenkins.XXXXXX")"; chmod 600 "$CURL_CONFIG"; printf 'user = "%s"\n' "$c" > "$CURL_CONFIG"; trap 'rm -f "$CURL_CONFIG"' EXIT; }
curl_auth() { curl --fail --silent --show-error --globoff --config "$CURL_CONFIG" "$@"; }
positive() { [[ "$2" =~ ^[1-9][0-9]*$ ]] || die "$1 must be a positive integer"; }
state() { curl_auth "${JENKINS_URL}$(path_for "$JENKINS_JOB")/$1/api/json" | python3 -c 'import json,sys; x=json.load(sys.stdin); print("BUILDING" if x["building"] else (x.get("result") or "UNKNOWN"))'; }
status() { positive 'build number' "$1"; local s="$(state "$1")"; echo "Build #$1: $s"; [[ "$s" == SUCCESS ]]; }
follow() { positive 'build number' "$1"; while :; do local s="$(state "$1")"; [[ "$s" == BUILDING ]] || { echo "Build #$1: $s"; [[ "$s" == SUCCESS ]]; return; }; sleep 5; done; }
submit() { local pr="$1" c="$2" b="$3" h q; h="$(mktemp "${TMPDIR:-/tmp}/gkeyll-jenkins-headers.XXXXXX")"; curl_auth --dump-header "$h" --output /dev/null --request POST --data-urlencode "CANDIDATE_PR=$pr" --data-urlencode "CANDIDATE_REF=$c" --data-urlencode "BASELINE_REF=$b" "${JENKINS_URL}$(path_for "$JENKINS_JOB")/buildWithParameters" || { rm -f "$h"; die 'Jenkins rejected the build'; }; q="$(awk 'BEGIN{IGNORECASE=1} /^Location:/{sub(/^[^:]*: /,"");sub(/\r$/,"");print;exit}' "$h")"; rm -f "$h"; [[ "$q" =~ /queue/item/([0-9]+)/ ]] || die 'Jenkins accepted the build but returned no queue ID'; QUEUE_ID="${BASH_REMATCH[1]}"; echo "Queued $JENKINS_JOB as queue item $QUEUE_ID"; }
queue_build() { curl_auth "${JENKINS_URL}/queue/item/$1/api/json" 2>/dev/null | python3 -c 'import json,sys; print((json.load(sys.stdin).get("executable") or {}).get("number", ""))' 2>/dev/null || true; }
run() { local p='' c='' b='' f=false; while (($#)); do case "$1" in --pr) (($#>=2))||die '--pr requires a number';p="$2";shift 2;;--candidate-ref)(($#>=2))||die '--candidate-ref requires a ref';c="$2";shift 2;;--baseline-ref)(($#>=2))||die '--baseline-ref requires a ref';b="$2";shift 2;;--follow)f=true;shift;;*)die "unknown run option: $1";;esac;done; if [[ -n "$p" ]];then positive '--pr' "$p";[[ -z "$c$b" ]]||die '--pr cannot be combined with refs';else [[ -n "$c" && -n "$b" ]]||die 'provide --pr, or both --candidate-ref and --baseline-ref';fi; submit "$p" "$c" "$b"; if [[ "$f" == true ]];then local n='';while [[ -z "$n" ]];do n="$(queue_build "$QUEUE_ID")";[[ -n "$n" ]]||sleep 2;done;follow "$n";fi; }
list() { local n="${1:-10}"; positive '--limit' "$n"; curl_auth "${JENKINS_URL}$(path_for "$JENKINS_JOB")/api/json?tree=builds[number,building,result]" | python3 -c 'import json,sys; [print("#%s %s"%(b["number"],"BUILDING" if b["building"] else b.get("result","UNKNOWN"))) for b in json.load(sys.stdin).get("builds",[])[:int(sys.argv[1])]]' "$n"; }
scan() { curl_auth --output /dev/null --request POST "${JENKINS_URL}$(path_for "$JENKINS_ROOT_JOB")/build?delay=0"; echo "Requested multibranch scan for $JENKINS_ROOT_JOB"; }
main() { (($#))||{ usage;exit 2;};case "$1" in -h|--help|help)usage;return;;esac;prepare;case "$1" in scan)shift;[[ $# == 0 ]]||die 'usage: scan';scan;;run)shift;run "$@";;follow)shift;[[ $# == 2 && $1 == --build ]]||die 'usage: follow --build NUMBER';follow "$2";;status)shift;[[ $# == 2 && $1 == --build ]]||die 'usage: status --build NUMBER';status "$2";;active)shift;[[ $# == 0 ]]||die 'usage: active';list 100;;recent)shift;[[ $# == 0 || ( $# == 2 && $1 == --limit ) ]]||die 'usage: recent [--limit NUMBER]';list "${2:-10}";;*)usage >&2;die "unknown command: $1";;esac; }
main "$@"
