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
    "output_timeout_ms": 5000
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

Task stop uses the existing `/tasks/{task_id}/stop` endpoint. Startup failures and
failed packet writes produce failed tasks; explicit successful stop produces STOPPED.
WHIP session teardown gets a bounded one-second deadline. StreamingSink creates no
recording file or recording metadata; tests record at the receiving service.

The transport backend currently provides no automatic reconnect. WHIP failure
detection follows the FFmpeg muxer's ICE/DTLS and socket errors; an unresponsive UDP
peer may not immediately cause an error. Deployments should monitor receiver health.

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
```

The standalone receiver tests verify one/two-user media decoding, codecs, both
video colors and audio tones, audio arriving before video, WHIP session deletion,
and RTMP disconnection. Live
tests exercise API requests, Redis routing, Agora publishers, native workers,
layout geometry, stop/status behavior, and receiver recordings. Use `atem serv
files` to review the generated artifacts.
