# Native Streaming Outputs

RTMP and WHIP are publishing protocols. File containers remain MP4, AVI, MKV,
and MPEG-TS in `RecordingSink::OutputFormat`.

## Responsibilities

- `RecordingSink` owns recording files, segments, and recording metadata.
- `StreamingSink` owns selection, composition, streaming timing, and task lifecycle.
- `StreamingOutput` owns the destination connection, muxing, bounded network operations,
  cancellation, and session teardown.
- `VideoCompositor`, `AudioMixer`, and `MediaEncoder` are shared media components.

RTMP uses FFmpeg's FLV muxer with H264/AAC. WHIP uses FFmpeg 8's official WHIP
muxer with constrained-baseline H264 and Opus. That muxer handles HTTP SDP exchange,
ICE, DTLS, SRTP, and session DELETE. No WebRTC protocol implementation is duplicated
in egress. Publishing always uses decoded composite media; a single publisher also
passes through the compositor. The `flat`, `spotlight`, and `customized` layouts
apply to streaming. `freestyle` remains an external web recorder layout.

## API Contract

Create a task using the existing endpoint:

```json
{
  "cmd": "rtmp",
  "action": "start",
  "request_id": "publish001",
  "payload": {
    "channel": "demo",
    "access_token": "<rtc-token>",
    "workerUid": 42,
    "users": ["1001", "1002"],
    "layout": "spotlight",
    "output_url": "rtmp://receiver.example/live/stream-key",
    "output_timeout_ms": 5000,
    "output_reconnect_attempts": 5,
    "output_reconnect_delay_ms": 1000
  }
}
```

For WHIP, use `"cmd": "whip"` and an HTTP(S) `output_url`, for example
`https://receiver.example/demo/whip`. An optional `output_token` carries the WHIP
bearer token. RTMP uses credentials/stream keys in its destination URL and rejects
`output_token`. Destination and token values should be treated as credentials.

`output_timeout_ms` defaults to 5000 and accepts 1000 through 30000 milliseconds.
The output canvas defaults to 1280x720; optional even `width` and `height` and custom
`regions` follow the native layout contract. Explicit user order selects the
spotlight speaker. Passthrough requests fall back to FFmpeg decoding for publishing.

Task stop uses the existing `/tasks/{task_id}/stop` endpoint. Initial connection
failures produce failed tasks immediately; explicit successful stop produces STOPPED.
WHIP session teardown gets a bounded one-second deadline. StreamingSink creates no
recording file or recording metadata; tests record at the receiving service.

## Automatic Reconnect

An established stream reconnects when FFmpeg reports a destination write failure.
The task and its Agora connection remain active during recovery. Retry settings
are validated by the HTTP API and native worker:

| Payload field | Default | Accepted values |
| --- | --- | --- |
| `output_reconnect_attempts` | 5 | 0 through 20; zero disables reconnect |
| `output_reconnect_delay_ms` | 1000 | 100 through 10000 milliseconds |

The delay doubles after each failed connection attempt and stops increasing at
10 seconds. Defaults produce delays of 1, 2, 4, 8, and 10 seconds, in addition to
the bounded connection time and session teardown. The retry budget resets after
a successful connection. Exhaustion completes the task as failed with the attempt
count; media processing/encoder failures also remain terminal.

Every reconnect opens fresh H264 and AAC/Opus encoders and a new transport session,
with new codec headers, a first video keyframe, and timestamps starting from the
current RTC media. Input queues stay bounded; only the latest composed video is
retained and buffered outage audio is discarded when the connection opens. Media
lost during the outage is not replayed. WHIP creates a new session and attempts a
bounded DELETE for the previous session.

Stop interrupts retry delays and connection/write operations. A shared stop flag
remains effective across transport resets; cleanup can still use its one-second
teardown window. Downtime counts toward the task's maximum duration, and that
deadline also limits reconnect handshakes. A duration limit completes successfully.

WHIP failure detection follows the FFmpeg muxer's ICE/DTLS and socket errors;
an unresponsive UDP peer may not immediately cause an error. This reconnect policy
does not add a receiver heartbeat or detect a silent UDP blackhole. Deployments
should monitor receiver health.

## Build And Verification

Production images build pinned FFmpeg 8.0.1 using `scripts/build-ffmpeg-whip.sh`,
verified by SHA256, with libx264, libopus, and OpenSSL. The build backports upstream
fixes for positive X509 serial numbers and SHA256 certificate signing, and enables
verification for WHIP HTTPS requests. Images bundle its shared
libraries. Local builds using older FFmpeg can record files and publish RTMP;
WHIP requires the pinned backend:

```bash
bash scripts/build-ffmpeg-whip.sh /tmp/rtc-egress-ffmpeg-8
PKG_CONFIG_PATH=/tmp/rtc-egress-ffmpeg-8/lib/pkgconfig cmake -S . -B build/whip -DCMAKE_BUILD_RPATH=/tmp/rtc-egress-ffmpeg-8/lib
cmake --build build/whip -j 4
PKG_CONFIG_PATH=/tmp/rtc-egress-ffmpeg-8/lib/pkgconfig cmake -S tests -B build/tests-whip -DCMAKE_BUILD_RPATH=/tmp/rtc-egress-ffmpeg-8/lib
cmake --build build/tests-whip -j 4
ctest --test-dir build/tests-whip --output-on-failure
python3 scripts/rtc-egress-streaming-e2e.py --receiver /path/to/mediamtx
python3 scripts/rtc-egress-live-e2e.py --receiver /path/to/mediamtx --stream-sdk-dir /path/to/publisher/sdk
python3 scripts/rtc-egress-live-e2e.py --filter rtmp,whip --restart-receiver --receiver /path/to/mediamtx --stream-sdk-dir /path/to/publisher/sdk
```

The standalone receiver tests cover 20 cases: normal one/two-user output, receiver
restart with one/two users, retry exhaustion, disabled retries, stop during backoff
or a stalled handshake, and duration expiration during backoff or a stalled
handshake, for both protocols. Recovery recordings must fully decode, preserve
both video colors/audio tones, have an early decodable keyframe, and have fresh
synchronized timestamps. MediaMTX may discard the first video keyframe when its
fMP4 segment starts from a slightly later audio timestamp; the receiver check
allows the next keyframe within 1.5 seconds while still requiring full decoding.
Tests also check WHIP session deletion and completion callbacks.
Use `--filter restart` (or other comma-separated substrings) to select cases.

Live tests exercise API requests, Redis routing, Agora publishers, native workers,
layout geometry, stop/status behavior, and receiver recordings. With
`--restart-receiver`, each streaming case verifies that its original task remains
active during the outage, publishes a new receiver recording, and then stops
through the API. Use `atem serv files` to review the generated artifacts.
