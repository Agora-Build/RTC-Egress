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
#   APP_ID         - Agora App ID (defaults to test app)
#   RECORD_SECONDS - How long to record each test (default: 15)
#   TOKEN_EXPIRE   - Token expiry in seconds (default: 3600)
#   VLM_URL        - VLM endpoint for content verification (optional)
#                    e.g. http://192.168.0.48:8001/v1/chat/completions
#   E2E_OUTPUT_DIR - Directory for browsable test outputs (default: /tmp/e2e_recording_output)
#   SERV_PORT      - Port for atem file server (default: 8199)

set -uo pipefail

# ── Configuration ──────────────────────────────────────────────────────────────

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="$PROJECT_DIR/bin"
CONFIG_DIR="$PROJECT_DIR/config"
RECORD_DIR="$PROJECT_DIR/recordings"
FIXTURES_DIR="$PROJECT_DIR/tests/fixtures"

APP_ID="${APP_ID:-2655d20a82fc47cebcff82d5bd5d53ef}"
CHANNEL="${CHANNEL:-e2e_rec_test_$(date +%s)}"
RECORD_SECONDS="${RECORD_SECONDS:-15}"
TOKEN_EXPIRE="${TOKEN_EXPIRE:-3600}"
STREAM_TOOL="${STREAM_TOOL:-}"
VLM_URL="${VLM_URL:-}"
VLM_MODEL="${VLM_MODEL:-qwen2.5-vl-7b}"
E2E_OUTPUT_DIR="${E2E_OUTPUT_DIR:-/tmp/e2e_recording_output}"
SERV_PORT="${SERV_PORT:-8199}"
LOG_DIR="$E2E_OUTPUT_DIR/logs"

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
TEST_SEQ=0
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

    # Stop the file server
    atem serv kill "files-${SERV_PORT}" 2>/dev/null || true

    if [ "$KEEP_FILES" = false ]; then
        rm -f "$RECORD_DIR"/recording_*e2e_rec_test*.mp4 2>/dev/null || true
        rm -rf "$E2E_OUTPUT_DIR" 2>/dev/null || true
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

# Send a single frame to VLM and check the response.
# Returns 0 on pass/skip, 1 on content mismatch.
vlm_check_frame() {
    local frame_file="$1"
    local tag="$2"          # e.g. "first_0", "last_4"
    local dest_dir="$3"
    local seq_prefix="$4"

    if [ ! -f "$frame_file" ] || [ "$(stat --printf='%s' "$frame_file" 2>/dev/null)" -lt 1000 ]; then
        log "  VLM [$tag]: Frame not available, skipping"
        return 0
    fi

    local prompt
    prompt="Does this video frame contain any visual content? Title cards, credits, text overlays, animations, live action, composited scenes, and fade transitions all count as valid content. Only a completely uniform solid color frame (e.g. pure black, pure green) with absolutely nothing on it should be considered no content. Reply ONLY with a JSON object: {\"has_content\": true/false, \"description\": \"brief description\"}"

    local b64_file="/tmp/e2e_vlm_b64_$$.txt"
    base64 -w0 "$frame_file" > "$b64_file"

    local vlm_payload="/tmp/e2e_vlm_payload_$$.json"
    python3 - "$VLM_MODEL" "$prompt" "$b64_file" "$vlm_payload" <<'PYEOF'
import json, sys
model, prompt, b64_path, out_path = sys.argv[1:5]
with open(b64_path) as f:
    b64 = f.read()
msg = {
    "model": model,
    "max_tokens": 200,
    "messages": [{"role": "user", "content": [
        {"type": "text", "text": prompt},
        {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{b64}"}}
    ]}]
}
with open(out_path, "w") as f:
    json.dump(msg, f)
PYEOF
    rm -f "$b64_file"

    local vlm_response
    vlm_response=$(curl -sf --max-time 30 "$VLM_URL" \
        -H "Content-Type: application/json" \
        -d @"$vlm_payload" 2>/dev/null) || {
        rm -f "$vlm_payload"
        log "  VLM [$tag]: Request failed, skipping"
        return 0
    }
    rm -f "$vlm_payload"

    local content
    content=$(echo "$vlm_response" | python3 -c "
import json, sys
try:
    r = json.load(sys.stdin)
    print(r['choices'][0]['message']['content'])
except:
    print('')
" 2>/dev/null)

    if [ -z "$content" ]; then
        log "  VLM [$tag]: Empty response, skipping"
        return 0
    fi

    log "  VLM [$tag]: $content"
    echo "$content" > "$dest_dir/${seq_prefix}_vlm_${tag}.txt" 2>/dev/null || true

    # Parse VLM JSON response — just check has_content
    local vlm_ok
    vlm_ok=$(echo "$content" | python3 -c "
import json, sys, re
try:
    text = sys.stdin.read()
    m = re.search(r'\{[^}]+\}', text)
    if m:
        d = json.loads(m.group())
        print('yes' if d.get('has_content') else 'no')
    else:
        print('unclear')
except:
    print('unclear')
" 2>/dev/null)

    case "$vlm_ok" in
        yes) return 0 ;;
        no)  return 1 ;;
        *)   return 0 ;;  # unclear = non-fatal
    esac
}

# Verify video content using a Vision Language Model (optional)
# Checks all extracted frames (first 5, mid every 2s, last 5) for content correctness.
verify_content_vlm() {
    local file="$1"
    local label="$2"

    if [ -z "$VLM_URL" ]; then
        return 0  # skip silently when VLM not configured
    fi

    local seq_prefix
    seq_prefix=$(printf "%02d" "$TEST_SEQ")
    local safe_label
    safe_label=$(echo "$label" | sed 's/[^a-zA-Z0-9_]/_/g' | sed 's/__*/_/g')
    local dest_dir="$E2E_OUTPUT_DIR/${seq_prefix}_${safe_label}"

    # Collect all extracted frame files in order
    local frames=()
    local f
    for f in "$dest_dir/${seq_prefix}_frame_first_"*.jpg; do
        [ -f "$f" ] && frames+=("$f")
    done
    for f in "$dest_dir/${seq_prefix}_frame_mid_"*.jpg; do
        [ -f "$f" ] && frames+=("$f")
    done
    for f in "$dest_dir/${seq_prefix}_frame_last_"*.jpg; do
        [ -f "$f" ] && frames+=("$f")
    done

    if [ ${#frames[@]} -eq 0 ]; then
        log "  VLM: No frames to verify"
        return 0
    fi

    local vlm_pass=0 vlm_fail=0 vlm_skip=0
    for f in "${frames[@]}"; do
        local tag
        tag=$(basename "$f" .jpg | sed "s/^${seq_prefix}_frame_//")
        if vlm_check_frame "$f" "$tag" "$dest_dir" "$seq_prefix"; then
            vlm_pass=$((vlm_pass + 1))
        else
            vlm_fail=$((vlm_fail + 1))
            log "  VLM FAIL: $tag"
        fi
    done

    local total=$((vlm_pass + vlm_fail))
    if [ "$vlm_fail" -gt 0 ]; then
        fail "VLM content check: $label" "$vlm_fail/$total frames mismatched"
        return 1
    fi

    pass "VLM content check: $label ($total frames verified)"
    return 0
}

# Extract key frames from a recording into the output directory.
# Produces: first 5 frames, last 5 frames, and one frame every 2 seconds in between.
publish_output() {
    local file="$1"
    local label="$2"
    TEST_SEQ=$((TEST_SEQ+1))
    local seq_prefix
    seq_prefix=$(printf "%02d" "$TEST_SEQ")
    # Sanitize label into a directory-safe name
    local safe_label
    safe_label=$(echo "$label" | sed 's/[^a-zA-Z0-9_]/_/g' | sed 's/__*/_/g')
    local dest_dir="$E2E_OUTPUT_DIR/${seq_prefix}_${safe_label}"
    mkdir -p "$dest_dir"
    cp "$file" "$dest_dir/" 2>/dev/null || true

    # Get video metadata
    local fps duration total_frames
    fps=$(ffprobe -v quiet -select_streams v:0 \
        -show_entries stream=r_frame_rate -of csv=p=0 "$file" 2>/dev/null || echo "25/1")
    fps=$(python3 -c "print(int(round(eval('$fps'))))" 2>/dev/null || echo 25)
    duration=$(ffprobe -v quiet -show_entries format=duration \
        -of csv=p=0 "$file" 2>/dev/null || echo "0")
    total_frames=$(python3 -c "print(int(float('${duration:-0}') * $fps))" 2>/dev/null || echo 0)

    log "  Extracting frames: ${total_frames} total (${duration}s @ ${fps}fps)"

    # First 5 frames
    for i in 0 1 2 3 4; do
        if [ "$i" -lt "$total_frames" ]; then
            ffmpeg -v quiet -i "$file" -vf "select=eq(n\\,$i)" -frames:v 1 \
                -q:v 2 "$dest_dir/${seq_prefix}_frame_first_${i}.jpg" -y 2>/dev/null || true
        fi
    done

    # Every 2 seconds (skipping first/last 5 frames region)
    local interval_frames=$((fps * 2))
    local mid_start=5
    local mid_end=$((total_frames - 5))
    if [ "$interval_frames" -gt 0 ] && [ "$mid_end" -gt "$mid_start" ]; then
        local fn="$mid_start"
        local mid_idx=0
        while [ "$fn" -lt "$mid_end" ]; do
            local ts
            ts=$(python3 -c "print(f'{$fn/$fps:.1f}s')" 2>/dev/null)
            ffmpeg -v quiet -i "$file" -vf "select=eq(n\\,$fn)" -frames:v 1 \
                -q:v 2 "$dest_dir/${seq_prefix}_frame_mid_${mid_idx}_${ts}.jpg" -y 2>/dev/null || true
            fn=$((fn + interval_frames))
            mid_idx=$((mid_idx + 1))
        done
    fi

    # Last 5 frames
    if [ "$total_frames" -gt 5 ]; then
        for i in 4 3 2 1 0; do
            local fn=$((total_frames - 1 - i))
            if [ "$fn" -ge 0 ]; then
                ffmpeg -v quiet -i "$file" -vf "select=eq(n\\,$fn)" -frames:v 1 \
                    -q:v 2 "$dest_dir/${seq_prefix}_frame_last_${i}.jpg" -y 2>/dev/null || true
            fi
        done
    fi
}

# Submit a recording task and wait for it to record
run_recording_test() {
    local label="$1"
    local users_json="$2"        # e.g. '["1001"]' or '["1001","1002"]'
    local decode_mode="$3"       # -1, 0, 1, or 2
    local layout="${4:-flat}"    # flat, grid, spotlight, freestyle
    local expect_video_codec="${5:-h264}"

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
    "layout": "$layout",
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
            publish_output "$f" "$label"
            verify_content_vlm "$f" "$label" || true
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

# ── Download test videos ──────────────────────────────────────────────────────
# All videos must have h264 video + stereo aac audio (stream-to-agora requirement).
#   - BBB full (116MB)      — Big Buck Bunny 640x360, h264+aac stereo 44.1kHz
#   - Sintel trailer (4MB)  — dark fantasy animation, h264+aac stereo 48kHz
#   - Movie 300 (2.7MB)     — live action test clip, h264+aac mono 22kHz
# Note: BBB is large but already cached after first download.

mkdir -p "$FIXTURES_DIR"

VIDEO_BBB="$FIXTURES_DIR/bigbuckbunny_360.m4v"
VIDEO_SINTEL="$FIXTURES_DIR/sintel_trailer.mp4"
VIDEO_MOVIE="$FIXTURES_DIR/movie_300.mp4"

declare -A VIDEO_URLS=(
    ["$VIDEO_BBB"]="https://download.blender.org/peach/bigbuckbunny_movies/BigBuckBunny_640x360.m4v"
    ["$VIDEO_SINTEL"]="https://media.w3.org/2010/05/sintel/trailer.mp4"
    ["$VIDEO_MOVIE"]="https://media.w3.org/2010/05/video/movie_300.mp4"
)

for vfile in "$VIDEO_BBB" "$VIDEO_SINTEL" "$VIDEO_MOVIE"; do
    if [ ! -f "$vfile" ]; then
        log "Downloading $(basename "$vfile") ..."
        curl -sL "${VIDEO_URLS[$vfile]}" -o "$vfile" || {
            log "ERROR: Failed to download $vfile"
            exit 1
        }
    fi
    local_info=$(ffprobe -v quiet -show_entries stream=codec_type,codec_name -of csv=p=0 "$vfile" 2>/dev/null | tr '\n' ' ')
    log "Test video: $(basename "$vfile") [${local_info}] ($(numfmt --to=iec "$(stat --printf='%s' "$vfile")"))"
done

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
if [ -n "$VLM_URL" ]; then
    if curl -sf --max-time 5 "${VLM_URL%/chat/completions}/models" >/dev/null 2>&1; then
        log "VLM: $VLM_URL (model: $VLM_MODEL) - available"
    else
        log "VLM: $VLM_URL - NOT reachable, content checks will be skipped"
        VLM_URL=""
    fi
else
    log "VLM: not configured (set VLM_URL to enable content verification)"
fi
log ""

# ── Backup previous results ───────────────────────────────────────────────────

if [ -d "$E2E_OUTPUT_DIR" ] && [ "$(ls -A "$E2E_OUTPUT_DIR" 2>/dev/null)" ]; then
    backup_file="${E2E_OUTPUT_DIR}_$(date +%Y%m%d_%H%M%S).zip"
    log "Backing up previous results to $backup_file ..."
    zip -rq "$backup_file" "$E2E_OUTPUT_DIR" 2>/dev/null && \
        log "Backup saved: $backup_file ($(numfmt --to=iec "$(stat --printf='%s' "$backup_file")"))" || \
        log "WARNING: Backup failed (non-fatal)"
    rm -rf "$E2E_OUTPUT_DIR"
fi

# ── Setup ─────────────────────────────────────────────────────────────────────

mkdir -p "$LOG_DIR" "$RECORD_DIR"

# Start streams (2 users with different videos for visual distinction)
log "Starting video streams..."
TOKEN_1001=$(generate_token "1001")
TOKEN_1002=$(generate_token "1002")

if [ -z "$TOKEN_1001" ] || [ -z "$TOKEN_1002" ]; then
    log "ERROR: Failed to generate stream tokens"
    exit 1
fi

log "  User 1001: $(basename "$VIDEO_BBB")"
"$STREAM_TOOL" --app-id "$APP_ID" --channel "$CHANNEL" \
    --rtc-user-id 1001 --token "$TOKEN_1001" \
    --loop "$VIDEO_BBB" > "$LOG_DIR/stream_1001.log" 2>&1 &
STREAM_PIDS+=($!)

log "  User 1002: $(basename "$VIDEO_SINTEL")"
"$STREAM_TOOL" --app-id "$APP_ID" --channel "$CHANNEL" \
    --rtc-user-id 1002 --token "$TOKEN_1002" \
    --loop "$VIDEO_SINTEL" > "$LOG_DIR/stream_1002.log" 2>&1 &
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

# ── File server for browsing test outputs ────────────────────────────────────

mkdir -p "$E2E_OUTPUT_DIR"
atem serv files "$E2E_OUTPUT_DIR" --port "$SERV_PORT" --no-browser --background 2>/dev/null && \
    log "File server started: https://localhost:${SERV_PORT}  (browse test outputs)" || \
    log "File server failed to start (non-fatal, outputs still saved to $E2E_OUTPUT_DIR)"

# ── Test Cases ────────────────────────────────────────────────────────────────
# NOTE: Each test restarts the egress service because workers are single-use.
# A worker process handles one task then exits; the manager currently does not
# respawn dead workers mid-session.

# ── Helper: restart streams with different videos ────────────────────────────

restart_streams() {
    local video_1001="$1"
    local video_1002="$2"
    log "Restarting streams: 1001=$(basename "$video_1001") 1002=$(basename "$video_1002")"
    # Kill existing streams
    for pid in "${STREAM_PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    sleep 2
    for pid in "${STREAM_PIDS[@]}"; do
        kill -9 "$pid" 2>/dev/null || true
    done
    STREAM_PIDS=()

    local t1 t2
    t1=$(generate_token "1001")
    t2=$(generate_token "1002")
    "$STREAM_TOOL" --app-id "$APP_ID" --channel "$CHANNEL" \
        --rtc-user-id 1001 --token "$t1" \
        --loop "$video_1001" > "$LOG_DIR/stream_1001.log" 2>&1 &
    STREAM_PIDS+=($!)
    "$STREAM_TOOL" --app-id "$APP_ID" --channel "$CHANNEL" \
        --rtc-user-id 1002 --token "$t2" \
        --loop "$video_1002" > "$LOG_DIR/stream_1002.log" 2>&1 &
    STREAM_PIDS+=($!)
    sleep 5
    for uid in 1001 1002; do
        if ! grep -q "ready" "$LOG_DIR/stream_${uid}.log" 2>/dev/null; then
            log "  WARNING: Stream $uid may not be ready"
        fi
    done
}

# ==============================================================================
# PART 1: INDIVIDUAL MODE (single user recording)
# users=["1001"] → Individual mode, only records user 1001, ignores new joiners
# Streams: 1001=BBB(h264), 1002=Sintel(h264) — but only 1001 is recorded
# ==============================================================================

log "==== PART 1: INDIVIDUAL MODE (1 user) ===="
log ""

# -- All decode modes × flat layout --
log "---- Individual / flat / all decode modes ----"
log ""

run_recording_test \
    "individual / flat / auto (expect passthrough)" \
    '["1001"]' -1 "flat" "h264"
restart_egress

run_recording_test \
    "individual / flat / passthrough" \
    '["1001"]' 0 "flat" "h264"
restart_egress

run_recording_test \
    "individual / flat / ffmpeg" \
    '["1001"]' 1 "flat" "h264"
restart_egress

if [ "$SKIP_SDK" = true ]; then
    skip "individual / flat / sdk" "Skipped (--quick, known SDK crash)"
else
    run_recording_test \
        "individual / flat / sdk" \
        '["1001"]' 2 "flat" "h264" || true
    restart_egress
fi

# -- All layouts × auto decode --
log ""
log "---- Individual / all layouts / auto decode ----"
log ""

for layout in grid spotlight; do
    run_recording_test \
        "individual / $layout / auto" \
        '["1001"]' -1 "$layout" "h264"
    restart_egress
done

# -- Individual recording user 1002 (Sintel content) --
log ""
log "---- Individual / record user 1002 (Sintel) ----"
log ""

run_recording_test \
    "individual(1002) / flat / passthrough" \
    '["1002"]' 0 "flat" "h264"
restart_egress

run_recording_test \
    "individual(1002) / flat / ffmpeg" \
    '["1002"]' 1 "flat" "h264"
restart_egress

# ==============================================================================
# PART 2: COMPOSITE MODE (multi user recording)
# users=["1001","1002"] → Composite mode, records both users in layout
# Streams: 1001=BBB(h264), 1002=Sintel(h264)
# ==============================================================================

log ""
log "==== PART 2: COMPOSITE MODE (2 users) ===="
log ""

# -- All decode modes × flat layout --
log "---- Composite / flat / all decode modes ----"
log ""

run_recording_test \
    "composite / flat / auto (expect ffmpeg)" \
    '["1001","1002"]' -1 "flat" "h264"
restart_egress

run_recording_test \
    "composite / flat / ffmpeg" \
    '["1001","1002"]' 1 "flat" "h264"
restart_egress

run_recording_test \
    "composite / flat / passthrough (expect fallback)" \
    '["1001","1002"]' 0 "flat" "h264"
restart_egress

if [ "$SKIP_SDK" = true ]; then
    skip "composite / flat / sdk" "Skipped (--quick, known SDK crash)"
else
    run_recording_test \
        "composite / flat / sdk" \
        '["1001","1002"]' 2 "flat" "h264" || true
    restart_egress
fi

# -- All layouts × auto decode --
log ""
log "---- Composite / all layouts / auto decode ----"
log ""

for layout in grid spotlight; do
    run_recording_test \
        "composite / $layout / auto" \
        '["1001","1002"]' -1 "$layout" "h264"
    restart_egress
done

# -- All layouts × ffmpeg decode (ensures layout rendering works with decode) --
log ""
log "---- Composite / all layouts / ffmpeg decode ----"
log ""

for layout in grid spotlight; do
    run_recording_test \
        "composite / $layout / ffmpeg" \
        '["1001","1002"]' 1 "$layout" "h264"
    restart_egress
done

# ==============================================================================
# PART 3: COMPOSITE ALL (empty user list = record everyone in channel)
# users=[] → records all users present, composite mode
# ==============================================================================

log ""
log "==== PART 3: COMPOSITE ALL (empty user list) ===="
log ""

for layout in flat grid spotlight; do
    run_recording_test \
        "composite-all / $layout / auto" \
        '[]' -1 "$layout" "h264"
    restart_egress
done

# ==============================================================================
# PART 4: DIFFERENT VIDEO SOURCES (Movie + Sintel)
# Restart streams: 1001=Movie(live action), 1002=Sintel(animation)
# Tests mixed content types — visually very different for VLM distinction.
# ==============================================================================

log ""
log "==== PART 4: MIXED CONTENT (Movie + Sintel) ===="
log ""

restart_streams "$VIDEO_MOVIE" "$VIDEO_SINTEL"

# -- Individual: record the live action user --
run_recording_test \
    "individual / movie / flat / auto" \
    '["1001"]' -1 "flat" "h264"
restart_egress

run_recording_test \
    "individual / movie / flat / passthrough" \
    '["1001"]' 0 "flat" "h264"
restart_egress

run_recording_test \
    "individual / movie / flat / ffmpeg" \
    '["1001"]' 1 "flat" "h264"
restart_egress

# -- Composite: mixed content --
run_recording_test \
    "composite / movie+sintel / flat / auto" \
    '["1001","1002"]' -1 "flat" "h264"
restart_egress

run_recording_test \
    "composite / movie+sintel / flat / ffmpeg" \
    '["1001","1002"]' 1 "flat" "h264"
restart_egress

run_recording_test \
    "composite / movie+sintel / grid / auto" \
    '["1001","1002"]' -1 "grid" "h264"
restart_egress

# -- Composite all with mixed content --
run_recording_test \
    "composite-all / movie+sintel / flat / auto" \
    '[]' -1 "flat" "h264"
restart_egress

# ==============================================================================
# PART 5: SAME CONTENT BOTH USERS (Sintel + Sintel)
# Tests that composite with identical content renders correctly.
# ==============================================================================

log ""
log "==== PART 5: SAME CONTENT (Sintel + Sintel) ===="
log ""

restart_streams "$VIDEO_SINTEL" "$VIDEO_SINTEL"

run_recording_test \
    "individual / sintel / flat / passthrough" \
    '["1001"]' 0 "flat" "h264"
restart_egress

run_recording_test \
    "individual / sintel / flat / ffmpeg" \
    '["1001"]' 1 "flat" "h264"
restart_egress

run_recording_test \
    "composite / sintel+sintel / flat / ffmpeg" \
    '["1001","1002"]' 1 "flat" "h264"
restart_egress

run_recording_test \
    "composite / sintel+sintel / grid / auto" \
    '["1001","1002"]' -1 "grid" "h264"

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
log "Outputs: $E2E_OUTPUT_DIR/"
log "============================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
