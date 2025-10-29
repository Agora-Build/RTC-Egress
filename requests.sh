#!/usr/bin/env bash
set -euo pipefail

DEFAULT_BASE_URL="http://localhost:8091"
DEFAULT_APP_ID="2655d20a82fc47cebcff82d5bd5d53ef"

usage() {
  cat <<USAGE
Usage:
  $(basename "$0") start [--base_url URL] [--app_id ID] --access_token TOKEN [--cmd CMD1|CMD2]
  $(basename "$0") stop [--base_url URL] [--app_id ID] --task_id ID1|ID2|...
  $(basename "$0") status [--base_url URL] [--app_id ID] --task_id ID1|ID2|...

Defaults:
  base_url: ${DEFAULT_BASE_URL}
  app_id:   ${DEFAULT_APP_ID}
USAGE
}

random_request_id() {
  hexdump -n6 -v -e '/1 "%02x"' /dev/urandom
}

print_response() {
  local response=$1
  printf 'Response:\n'
  if command -v jq >/dev/null 2>&1; then
    if ! printf '%s' "$response" | jq .; then
      printf '%s\n' "$response"
    fi
  else
    printf '%s\n' "$response"
  fi
}

print_payload() {
  local payload=$1
  printf 'Payload:\n'
  if command -v jq >/dev/null 2>&1; then
    printf '%s' "$payload" | jq .
  else
    printf '%s\n' "$payload"
  fi
}

extract_task_id() {
  local response=$1
  if command -v jq >/dev/null 2>&1; then
    printf '%s' "$response" | jq -r '.task_id // empty'
  else
    printf '%s\n' "$response" | sed -n 's/.*"task_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p'
  fi
}

parse_pipe_list() {
  local value=$1
  local -a items=()
  IFS='|' read -r -a parts <<< "$value"
  for part in "${parts[@]}"; do
    local trimmed
    trimmed=$(printf '%s' "$part" | xargs)
    if [[ -n "$trimmed" ]]; then
      items+=("$trimmed")
    fi
  done
  printf '%s\n' "${items[@]}"
}

join_with_delim() {
  local delim=$1
  shift
  local IFS=$delim
  printf '%s' "$*"
}

LAST_TASK_ID=""

start_request() {
  local base_url=$1
  local app_id=$2
  local access_token=$3
  local cmd=$4
  LAST_TASK_ID=""
  local request_id
  request_id=$(random_request_id)
  local payload
  payload=$(cat <<JSON
{
  "request_id": "${request_id}",
  "cmd": "${cmd}",
  "payload": {
    "layout": "flat",
    "channel": "egress_test",
    "access_token": "${access_token}",
    "workerUid": 42,
    "users": ["803231", "322353"],
    "filename_pattern": "file_output",
    "format": "hls",
    "video": {
      "width": 1280,
      "height": 720,
      "frameRate": 30,
      "bitrate": 1000
    },
    "audio": {
      "sampleRate": 48000,
      "channels": 2,
      "bitrate": 128
    }
  }
}
JSON
  )

  print_payload "$payload"

  local response
  response=$(curl -sS -X POST "${base_url}/egress/v1/${app_id}/tasks" \
    -H "Content-Type: application/json" \
    -d "${payload}")

  print_response "$response"

  LAST_TASK_ID=$(extract_task_id "$response")
}

stop_request() {
  local base_url=$1
  local app_id=$2
  local task_id=$3
  local request_id
  request_id=$(random_request_id)

  local payload
  payload=$(cat <<JSON
{
  "request_id": "${request_id}"
}
JSON
  )

  local response
  response=$(curl -sS -X POST "${base_url}/egress/v1/${app_id}/tasks/${task_id}/stop" \
    -H "Content-Type: application/json" \
    -d "${payload}")

  print_response "$response"
}

status_request() {
  local base_url=$1
  local app_id=$2
  local task_id=$3
  local request_id
  request_id=$(random_request_id)

  local payload
  payload=$(cat <<JSON
{
  "request_id": "${request_id}"
}
JSON
  )

  local response
  response=$(curl -sS -X POST "${base_url}/egress/v1/${app_id}/tasks/${task_id}/status" \
    -H "Content-Type: application/json" \
    -d "${payload}")

  print_response "$response"
}

start_handler() {
  local base_url=$DEFAULT_BASE_URL
  local app_id=$DEFAULT_APP_ID
  local access_token=""
  local -a cmds=()
  local -a created_tasks=()

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --base_url)
        [[ $# -lt 2 ]] && { echo "Missing value for --base_url" >&2; exit 1; }
        base_url=$2
        shift 2
        ;;
      --app_id)
        [[ $# -lt 2 ]] && { echo "Missing value for --app_id" >&2; exit 1; }
        app_id=$2
        shift 2
        ;;
      --access_token)
        [[ $# -lt 2 ]] && { echo "Missing value for --access_token" >&2; exit 1; }
        access_token=$2
        shift 2
        ;;
      --cmd)
        [[ $# -lt 2 ]] && { echo "Missing value for --cmd" >&2; exit 1; }
        mapfile -t cmds < <(parse_pipe_list "$2")
        if [[ ${#cmds[@]} -eq 0 ]]; then
          echo "--cmd list cannot be empty" >&2
          exit 1
        fi
        shift 2
        ;;
      --*)
        echo "Unknown option: $1" >&2
        exit 1
        ;;
      *)
        echo "Unexpected positional argument: $1" >&2
        exit 1
        ;;
    esac
  done

  if [[ -z "$access_token" ]]; then
    echo "Missing --access_token for start command." >&2
    exit 1
  fi

  if [[ ${#cmds[@]} -eq 0 ]]; then
    cmds=("record")
  fi

  for cmd in "${cmds[@]}"; do
    printf '=== Executing cmd: %s ===\n' "$cmd"
    start_request "$base_url" "$app_id" "$access_token" "$cmd"
    if [[ -n "$LAST_TASK_ID" ]]; then
      created_tasks+=("$LAST_TASK_ID")
    fi
    printf '\n'
  done

  if [[ ${#created_tasks[@]} -gt 0 ]]; then
    local stop_cmd="$(basename "$0") stop"
    if [[ "$base_url" != "$DEFAULT_BASE_URL" ]]; then
      stop_cmd+=" --base_url $base_url"
    fi
    if [[ "$app_id" != "$DEFAULT_APP_ID" ]]; then
      stop_cmd+=" --app_id $app_id"
    fi
    local joined_tasks
    joined_tasks=$(join_with_delim '|' "${created_tasks[@]}")
    stop_cmd+=" --task_id \"$joined_tasks\""
    printf 'Stop command: bash %s\n' "$stop_cmd"
  fi
}

stop_handler() {
  local base_url=$DEFAULT_BASE_URL
  local app_id=$DEFAULT_APP_ID
  local -a tasks=()

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --base_url)
        [[ $# -lt 2 ]] && { echo "Missing value for --base_url" >&2; exit 1; }
        base_url=$2
        shift 2
        ;;
      --app_id)
        [[ $# -lt 2 ]] && { echo "Missing value for --app_id" >&2; exit 1; }
        app_id=$2
        shift 2
        ;;
      --task_id)
        [[ $# -lt 2 ]] && { echo "Missing value for --task_id" >&2; exit 1; }
        mapfile -t new_tasks < <(parse_pipe_list "$2")
        if [[ ${#new_tasks[@]} -eq 0 ]]; then
          echo "--task_id list cannot be empty" >&2
          exit 1
        fi
        tasks+=("${new_tasks[@]}")
        shift 2
        ;;
      --*)
        echo "Unknown option: $1" >&2
        exit 1
        ;;
      *)
        echo "Unexpected positional argument: $1" >&2
        exit 1
        ;;
    esac
  done

  if [[ ${#tasks[@]} -eq 0 ]]; then
    echo "At least one --task_id is required." >&2
    exit 1
  fi

  local joined_display
  joined_display=$(join_with_delim '|' "${tasks[@]}")
  printf 'Stopping tasks: %s\n' "$joined_display"
  for task_id in "${tasks[@]}"; do
    stop_request "$base_url" "$app_id" "$task_id"
    printf '\n'
  done
}

status_handler() {
  local base_url=$DEFAULT_BASE_URL
  local app_id=$DEFAULT_APP_ID
  local -a tasks=()

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --base_url)
        [[ $# -lt 2 ]] && { echo "Missing value for --base_url" >&2; exit 1; }
        base_url=$2
        shift 2
        ;;
      --app_id)
        [[ $# -lt 2 ]] && { echo "Missing value for --app_id" >&2; exit 1; }
        app_id=$2
        shift 2
        ;;
      --task_id)
        [[ $# -lt 2 ]] && { echo "Missing value for --task_id" >&2; exit 1; }
        mapfile -t new_tasks < <(parse_pipe_list "$2")
        if [[ ${#new_tasks[@]} -eq 0 ]]; then
          echo "--task_id list cannot be empty" >&2
          exit 1
        fi
        tasks+=("${new_tasks[@]}")
        shift 2
        ;;
      --*)
        echo "Unknown option: $1" >&2
        exit 1
        ;;
      *)
        echo "Unexpected positional argument: $1" >&2
        exit 1
        ;;
    esac
  done

  if [[ ${#tasks[@]} -eq 0 ]]; then
    echo "At least one --task_id is required." >&2
    exit 1
  fi

  local joined_display
  joined_display=$(join_with_delim '|' "${tasks[@]}")
  printf 'Status for tasks: %s\n' "$joined_display"
  for task_id in "${tasks[@]}"; do
    status_request "$base_url" "$app_id" "$task_id"
    printf '\n'
  done
}

main() {
  if [[ $# -lt 1 ]]; then
    usage
    exit 1
  fi

  local command=$1
  shift

  case $command in
    start)
      start_handler "$@"
      ;;
    stop)
      stop_handler "$@"
      ;;
    status)
      status_handler "$@"
      ;;
    *)
      usage
      exit 1
      ;;
  esac
}

main "$@"
