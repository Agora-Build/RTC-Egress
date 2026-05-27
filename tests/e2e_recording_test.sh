#!/bin/bash
#
# E2E Recording Test Suite
#
# Tests all video decode modes (auto/passthrough/ffmpeg/sdk) with both
# single-user (individual) and multi-user (composite) recording layouts.
# Verifies video, audio, and file playability for each combination.
#
# Prerequisites:
#   - atem CLI (for token generation)
#   - stream-to-agora (for streaming test video to Agora channel)
#   - Redis running on localhost:6379
#   - Built binaries in ./bin/ (run ./build.sh local first)
#   - Test video file (auto-downloaded if missing)
#
# Usage:
#   ./tests/e2e_recording_test.sh              # Run all tests
#   ./tests/e2e_recording_test.sh --quick      # Skip sdk decode (known crash)
#   ./tests/e2e_recording_test.sh --keep       # Keep output files after test
#   ./tests/e2e_recording_test.sh --channel X  # Use custom channel name
#
# Environment variables:
#   STREAM_TOOL    - Path to stream-to-agora binary
#   TEST_VIDEO     - Path to test video file (H264)
#   APP_ID         - Agora App ID (defaults to test app)
#   RECORD_SECONDS - How long to record each test (default: 15)
#   TOKEN_EXPIRE   - Token expiry in seconds (default: 3600)

set -uo pipefail

# ── Configuration ──────────────────────────────────────────────────────────────

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="$PROJECT_DIR/bin"
CONFIG_DIR="$PROJECT_DIR/config"
RECORD_DIR="$PROJECT_DIR/recordings"
LOG_DIR="/tmp/e2e_recording_test"

APP_ID="${APP_ID:-2655d20a82fc47cebcff82d5bd5d53ef}"
CHANNEL="${CHANNEL:-e2e_rec_test_$(date +%s)}"
RECORD_SECONDS="${RECORD_SECONDS:-15}"
TOKEN_EXPIRE="${TOKEN_EXPIRE:-3600}"
STREAM_TOOL="${STREAM_TOOL:-}"
TEST_VIDEO="${TEST_VIDEO:-}"

SKIP_SDK=false
KEEP_FILES=false

API_PORT=8091
EGRESS_HEALTH_PORT=8192
API_URL="http://localhost:${API_PORT}/egress/v1/${APP_ID}"

# PIDs to clean up
STREAM_PIDS=()
SERVICE_PIDS=()

# Test results
PASS=0
FAIL=0
SKIP=0
RESULTS=()

# ── Parse arguments ───────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)     SKIP_SDK=true; shift ;;
        --keep)      KEEP_FILES=true; shift ;;
        --channel)   CHANNEL="$2"; shift 2 ;;
        --seconds)   RECORD_SECONDS="$2"; shift 2 ;;
        --help|-h)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Helpers ───────────────────────────────────────────────────────────────────

log()  { echo "[$(date +%H:%M:%S)] $*"; }
pass() { PASS=$((PASS+1)); RESULTS+=("PASS  $1"); log "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); RESULTS+=("FAIL  $1: $2"); log "FAIL: $1 - $2"; }
skip() { SKIP=$((SKIP+1)); RESULTS+=("SKIP  $1: $2"); log "SKIP: $1 - $2"; }

cleanup() {
    log "Cleaning up..."
    for pid in "${STREAM_PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${SERVICE_PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    # Wait briefly for graceful shutdown
    sleep 2
    for pid in "${STREAM_PIDS[@]}" "${SERVICE_PIDS[@]}"; do
        kill -9 "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true

    if [ "$KEEP_FILES" = false ]; then
        rm -f "$RECORD_DIR"/recording_*e2e_rec_test*.mp4 2>/dev/null || true
    fi
}
trap cleanup EXIT

generate_token() {
    local uid="$1"
    local token
    token=$(atem token rtc create \
        --channel "$CHANNEL" \
        --rtc-user-id "$uid" \
        --expire "$TOKEN_EXPIRE" 2>/dev/null | sed -n '2p')
    if [ -z "$token" ]; then
        echo ""
        return 1
    fi
    echo "$token"
}

wait_for_health() {
    local url="$1"
    local name="$2"
    local max_wait=15
    local i=0
    while [ $i -lt $max_wait ]; do
        if curl -sf "$url" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
        i=$((i+1))
    done
    log "ERROR: $name did not become healthy within ${max_wait}s"
    return 1
}

# Verify a recording file has valid video + audio
verify_recording() {
    local file="$1"
    local label="$2"
    local expect_video_codec="${3:-h264}"

    if [ ! -f "$file" ]; then
        fail "$label" "Output file not found"
        return 1
    fi

    local size
    size=$(stat --printf="%s" "$file" 2>/dev/null || echo "0")
    if [ "$size" -lt 10000 ]; then
        fail "$label" "File too small (${size} bytes)"
        return 1
    fi

    # Extract stream info
    local probe
    probe=$(ffprobe -v quiet -show_streams -show_format "$file" 2>/dev/null)

    local video_codec audio_codec width height sample_rate channels duration
    video_codec=$(echo "$probe" | grep "^codec_name=" | head -1 | cut -d= -f2)
    audio_codec=$(echo "$probe" | grep "^codec_name=" | sed -n '2p' | cut -d= -f2)
    width=$(echo "$probe" | grep "^width=" | head -1 | cut -d= -f2)
    height=$(echo "$probe" | grep "^height=" | head -1 | cut -d= -f2)
    sample_rate=$(echo "$probe" | grep "^sample_rate=" | head -1 | cut -d= -f2)
    channels=$(echo "$probe" | grep "^channels=" | head -1 | cut -d= -f2)
    duration=$(echo "$probe" | grep "^duration=" | tail -1 | cut -d= -f2)
    local has_video has_audio
    has_video=$(echo "$probe" | grep -c "codec_type=video")
    has_audio=$(echo "$probe" | grep -c "codec_type=audio")

    local details="video=${video_codec}/${width}x${height} audio=${audio_codec}/${sample_rate}Hz/${channels}ch dur=${duration}s size=$(numfmt --to=iec "$size")"

    # Check video stream
    if [ "$has_video" -lt 1 ]; then
        fail "$label" "No video stream ($details)"
        return 1
    fi

    # Check audio stream
    if [ "$has_audio" -lt 1 ]; then
        fail "$label" "No audio stream ($details)"
        return 1
    fi

    # Check video codec matches expectation
    if [ "$video_codec" != "$expect_video_codec" ]; then
        fail "$label" "Expected video codec $expect_video_codec, got $video_codec ($details)"
        return 1
    fi

    # Decode test - verify file is actually playable
    local decode_errors
    decode_errors=$(ffmpeg -v error -i "$file" -f null - 2>&1 | grep -c "error" || true)
    if [ "$decode_errors" -gt 5 ]; then
        fail "$label" "Decode produced $decode_errors errors ($details)"
        return 1
    fi

    pass "$label ($details)"
    return 0
}

# Submit a recording task and wait for it to record
run_recording_test() {
    local label="$1"
    local users_json="$2"        # e.g. '["1001"]' or '["1001","1002"]'
    local decode_mode="$3"       # -1, 0, 1, or 2
    local expect_video_codec="${4:-h264}"

    log "--- $label ---"

    # Generate fresh worker token
    local worker_token
    worker_token=$(generate_token "42")
    if [ -z "$worker_token" ]; then
        fail "$label" "Failed to generate worker token"
        return 1
    fi

    # Clean previous recordings for this test
    rm -f "$RECORD_DIR"/recording_*.mp4 2>/dev/null || true

    # Create timestamp marker for finding new files
    local marker="/tmp/e2e_test_marker_$$"
    touch "$marker"

    # Build JSON payload
    local json_file="/tmp/e2e_test_payload_$$.json"
    cat > "$json_file" <<ENDJSON
{
  "request_id": "e2etest$(date +%s%N | cut -c1-16)",
  "cmd": "record",
  "action": "start",
  "payload": {
    "channel": "$CHANNEL",
    "access_token": "$worker_token",
    "workerUid": 42,
    "users": $users_json,
    "layout": "flat",
    "videoDecodeMode": $decode_mode
  }
}
ENDJSON

    # Submit task
    local response
    response=$(curl -sf -X POST "$API_URL/tasks" \
        -H "Content-Type: application/json" \
        -d @"$json_file" 2>/dev/null) || {
        fail "$label" "API request failed"
        rm -f "$json_file"
        return 1
    }
    rm -f "$json_file"

    local task_id status
    task_id=$(echo "$response" | python3 -c "import json,sys; print(json.load(sys.stdin).get('task_id',''))" 2>/dev/null)
    status=$(echo "$response" | python3 -c "import json,sys; print(json.load(sys.stdin).get('status',''))" 2>/dev/null)

    if [ "$status" != "enqueued" ]; then
        fail "$label" "Task not enqueued: $response"
        return 1
    fi

    log "  Task $task_id enqueued, recording for ${RECORD_SECONDS}s..."
    sleep "$RECORD_SECONDS"

    # Stop task
    curl -sf -X POST "$API_URL/tasks/$task_id/stop" \
        -H "Content-Type: application/json" \
        -d "{\"request_id\": \"stop$(date +%s)\"}" >/dev/null 2>&1 || true

    # Wait for encoder flush
    sleep 5

    # Find output file(s) created after our marker
    local output_files
    output_files=$(find "$RECORD_DIR" -maxdepth 1 -name "recording_*.mp4" -newer "$marker" 2>/dev/null || true)
    rm -f "$marker"

    if [ -z "$output_files" ]; then
        # Fallback: grab most recent mp4
        output_files=$(ls -t "$RECORD_DIR"/recording_*.mp4 2>/dev/null | head -1)
    fi

    if [ -z "$output_files" ]; then
        fail "$label" "No output file produced"
        # Dump relevant logs
        log "  Recent egress logs:"
        grep -E "error|fail|Error|FAIL" "$LOG_DIR/egress.log" 2>/dev/null | tail -10 || true
        return 1
    fi

    # Verify each output file
    local any_pass=false
    for f in $output_files; do
        if verify_recording "$f" "$label" "$expect_video_codec"; then
            any_pass=true
        fi
    done

    if [ "$any_pass" = false ]; then
        return 1
    fi
    return 0
}

# ── Preflight checks ─────────────────────────────────────────────────────────

log "============================================"
log "RTC-Egress E2E Recording Test Suite"
log "============================================"
log ""

# Check required tools
for cmd in atem ffprobe ffmpeg curl python3 redis-cli; do
    if ! command -v "$cmd" &>/dev/null; then
        log "ERROR: Required tool '$cmd' not found"
        exit 1
    fi
done

# Find stream-to-agora
if [ -z "$STREAM_TOOL" ]; then
    if command -v stream-to-agora &>/dev/null; then
        STREAM_TOOL="stream-to-agora"
    elif [ -x "$PROJECT_DIR/../stream-to-agora/target/release/stream-to-agora" ]; then
        STREAM_TOOL="$PROJECT_DIR/../stream-to-agora/target/release/stream-to-agora"
    else
        log "ERROR: stream-to-agora not found. Set STREAM_TOOL env var."
        exit 1
    fi
fi
log "Using stream-to-agora: $STREAM_TOOL"

# Find test video
if [ -z "$TEST_VIDEO" ]; then
    for candidate in \
        /tmp/egress-test-compare/bigbuckbunny_360.m4v \
        /tmp/egress-test-compare/video_a.mp4 \
        /tmp/test_video.mp4; do
        if [ -f "$candidate" ]; then
            TEST_VIDEO="$candidate"
            break
        fi
    done
fi

if [ -z "$TEST_VIDEO" ] || [ ! -f "$TEST_VIDEO" ]; then
    log "Downloading test video..."
    TEST_VIDEO="/tmp/egress-test-compare/bigbuckbunny_360.m4v"
    mkdir -p /tmp/egress-test-compare
    curl -sL "https://download.blender.org/peach/bigbuckbunny_movies/BigBuckBunny_640x360.m4v" \
        -o "$TEST_VIDEO"
fi
log "Using test video: $TEST_VIDEO"

# Check binaries
for bin in eg_worker egress api-server; do
    if [ ! -x "$BIN_DIR/$bin" ]; then
        log "ERROR: $BIN_DIR/$bin not found. Run './build.sh local' first."
        exit 1
    fi
done

# Check Redis
if ! redis-cli ping &>/dev/null; then
    log "ERROR: Redis not available on localhost:6379"
    exit 1
fi

# Check ports are free
for port in $API_PORT 8191 $EGRESS_HEALTH_PORT; do
    if pid=$(lsof -ti ":$port" 2>/dev/null); then
        log "Port $port in use by PID $pid, stopping it..."
        kill "$pid" 2>/dev/null || true
        sleep 1
    fi
done

log "Channel: $CHANNEL"
log "Record duration: ${RECORD_SECONDS}s per test"
log ""

# ── Setup ─────────────────────────────────────────────────────────────────────

mkdir -p "$LOG_DIR" "$RECORD_DIR"

# Start streams (2 users)
log "Starting video streams..."
TOKEN_1001=$(generate_token "1001")
TOKEN_1002=$(generate_token "1002")

if [ -z "$TOKEN_1001" ] || [ -z "$TOKEN_1002" ]; then
    log "ERROR: Failed to generate stream tokens"
    exit 1
fi

"$STREAM_TOOL" --app-id "$APP_ID" --channel "$CHANNEL" \
    --rtc-user-id 1001 --token "$TOKEN_1001" \
    --loop "$TEST_VIDEO" > "$LOG_DIR/stream_1001.log" 2>&1 &
STREAM_PIDS+=($!)

"$STREAM_TOOL" --app-id "$APP_ID" --channel "$CHANNEL" \
    --rtc-user-id 1002 --token "$TOKEN_1002" \
    --loop "$TEST_VIDEO" > "$LOG_DIR/stream_1002.log" 2>&1 &
STREAM_PIDS+=($!)

# Wait for streams to connect
log "Waiting for streams to connect..."
sleep 5

for uid in 1001 1002; do
    if grep -q "ready" "$LOG_DIR/stream_${uid}.log" 2>/dev/null; then
        log "  Stream $uid: connected"
    else
        log "  Stream $uid: FAILED to connect"
        tail -5 "$LOG_DIR/stream_${uid}.log" 2>/dev/null || true
        log "ERROR: Stream $uid did not connect. Aborting."
        exit 1
    fi
done

# Start services
log "Starting API server and egress service..."

export LD_LIBRARY_PATH="$BIN_DIR:${LD_LIBRARY_PATH:-}"

"$BIN_DIR/api-server" --config "$CONFIG_DIR/api_server_config.yaml" \
    > "$LOG_DIR/api_server.log" 2>&1 &
SERVICE_PIDS+=($!)

"$BIN_DIR/egress" --config "$CONFIG_DIR/egress_config.yaml" \
    > "$LOG_DIR/egress.log" 2>&1 &
SERVICE_PIDS+=($!)

if ! wait_for_health "http://localhost:8191/health" "API server"; then
    log "ERROR: API server failed to start"
    tail -10 "$LOG_DIR/api_server.log"
    exit 1
fi

if ! wait_for_health "http://localhost:${EGRESS_HEALTH_PORT}/health" "Egress"; then
    log "ERROR: Egress service failed to start"
    tail -10 "$LOG_DIR/egress.log"
    exit 1
fi

log "Services ready."
log ""

# Restart egress service (workers die after each task, need fresh workers)
restart_egress() {
    local egress_pid
    for pid in "${SERVICE_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            local cmd
            cmd=$(ps -p "$pid" -o comm= 2>/dev/null || true)
            if [[ "$cmd" == *egress* ]]; then
                kill "$pid" 2>/dev/null || true
            fi
        fi
    done
    # Kill any remaining egress on the health port
    lsof -ti ":$EGRESS_HEALTH_PORT" 2>/dev/null | xargs -r kill 2>/dev/null || true
    sleep 3

    "$BIN_DIR/egress" --config "$CONFIG_DIR/egress_config.yaml" \
        > "$LOG_DIR/egress.log" 2>&1 &
    local new_pid=$!
    SERVICE_PIDS+=("$new_pid")

    if ! wait_for_health "http://localhost:${EGRESS_HEALTH_PORT}/health" "Egress"; then
        log "ERROR: Failed to restart egress"
        return 1
    fi
    log "Egress restarted (PID $new_pid)"
}

# ── Test Cases ────────────────────────────────────────────────────────────────
# NOTE: Each test restarts the egress service because workers are single-use.
# A worker process handles one task then exits; the manager currently does not
# respawn dead workers mid-session.

log "==== SINGLE USER TESTS ===="
log ""

# Test 1: Single user, auto decode mode (should auto-select passthrough)
run_recording_test \
    "Single user / auto decode (expect passthrough)" \
    '["1001"]' \
    -1 \
    "h264"
restart_egress

# Test 2: Single user, explicit passthrough
run_recording_test \
    "Single user / passthrough" \
    '["1001"]' \
    0 \
    "h264"
restart_egress

# Test 3: Single user, explicit ffmpeg
run_recording_test \
    "Single user / ffmpeg decode" \
    '["1001"]' \
    1 \
    "h264"
restart_egress

# Test 4: Single user, sdk decode (known crash in SDK v4.4.32)
if [ "$SKIP_SDK" = true ]; then
    skip "Single user / sdk decode" "Skipped (--quick flag, known SDK v4.4.32 crash)"
else
    log "  NOTE: SDK decode mode crashes in Agora SDK v4.4.32 - expecting failure"
    run_recording_test \
        "Single user / sdk decode" \
        '["1001"]' \
        2 \
        "h264" || true
    restart_egress
fi

log ""
log "==== MULTI USER TESTS ===="
log ""

# Test 5: Multi user, auto decode mode (should auto-select ffmpeg)
run_recording_test \
    "Multi user / auto decode (expect ffmpeg)" \
    '["1001","1002"]' \
    -1 \
    "h264"
restart_egress

# Test 6: Multi user, explicit ffmpeg
run_recording_test \
    "Multi user / ffmpeg decode" \
    '["1001","1002"]' \
    1 \
    "h264"
restart_egress

# Test 7: Multi user, passthrough (should fall back to ffmpeg)
run_recording_test \
    "Multi user / passthrough (expect fallback to ffmpeg)" \
    '["1001","1002"]' \
    0 \
    "h264"
restart_egress

# Test 8: Multi user, sdk decode
if [ "$SKIP_SDK" = true ]; then
    skip "Multi user / sdk decode" "Skipped (--quick flag, known SDK v4.4.32 crash)"
else
    log "  NOTE: SDK decode mode crashes in Agora SDK v4.4.32 - expecting failure"
    run_recording_test \
        "Multi user / sdk decode" \
        '["1001","1002"]' \
        2 \
        "h264" || true
    restart_egress
fi

# Test 9: All users (empty list = composite all)
run_recording_test \
    "All users / auto decode (expect ffmpeg)" \
    '[]' \
    -1 \
    "h264"

# ── Summary ───────────────────────────────────────────────────────────────────

log ""
log "============================================"
log "TEST RESULTS"
log "============================================"
for r in "${RESULTS[@]}"; do
    log "  $r"
done
log ""
log "Total: $PASS passed, $FAIL failed, $SKIP skipped"
log "Logs: $LOG_DIR/"
log "============================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
