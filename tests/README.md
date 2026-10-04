# Practical Egress Tests

The native media regressions use the real recording sink, compositor, FFmpeg
encoders, and decoders. They generate PCM and YUV input, record MP4 files, and
inspect the resulting media and metadata without Agora credentials.

```sh
cmake -S tests -B build/tests
cmake --build build/tests --target recording_thread_tests -j 4
ctest --test-dir build/tests -R '^RecordingThreadTests$' --output-on-failure
```

`recording_media_tests.cpp` covers:

- One publisher's tone and recording duration.
- Two concurrent publishers' tones with alternating callback order.
- A late publisher with a different sample rate and channel count, followed by departure.
- Immediate shutdown with queued PCM and a partial AAC frame.
- Real H264 passthrough, AAC finalization, the last video frame, and file metadata.
- Passthrough audio pause/resume timing and draining a large resampled callback.
- Paired video callbacks and a brief gap in one publisher's video.
- Explicit frame expiry and immediate restoration when a publisher returns.

The existing four-user recording test also checks actual H264/AAC streams,
dimensions, duration, packet counts, and increasing timestamps.

Run all native suites after building all test targets:

```sh
cmake --build build/tests -j 4
ctest --test-dir build/tests --output-on-failure --timeout 120
```

## Live Agora End To End

Build `bin/eg_worker`, `bin/api-server`, and `bin/egress`. Select the desired
Agora project in atem. The live runner requires `atem`, `stream-to-agora`,
FFmpeg/ffprobe, `redis-cli`, and an empty Redis database 15 on localhost.
The publisher's SDK libraries must match its binary; they can differ from
the SDK used by egress.

```sh
python3 scripts/rtc-egress-live-e2e.py --stream-sdk-dir /path/to/publisher/rtc/sdk
```

The runner exercises 16 single-user and multi-user API recording scenarios
with labeled red/blue video and 440/880 Hz tones. It checks full decoding,
duration, both users, both tones, STOPPED task state, and completed metadata.
It reserves and cleans its Redis keys and stops its own services.

Use `--filter single` or `--filter multi` for a subset, and `--source-dir`
to reuse fixtures. Recheck saved artifacts without connecting to Agora:

```sh
python3 scripts/rtc-egress-live-e2e.py --verify-run /tmp/rtc-egress-e2e-TIMESTAMP
```

Serve the resulting report, recordings, and frames for review:

```sh
atem serv files /tmp/rtc-egress-e2e-TIMESTAMP --no-browser --background
```
