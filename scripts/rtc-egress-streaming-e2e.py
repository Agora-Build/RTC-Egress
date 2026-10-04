#!/usr/bin/env python3
"""Publish synthetic media through StreamingSink to a real MediaMTX receiver.

Build tests/ with FFmpeg 8 using CMake first. Supply a MediaMTX v1.21.1 binary
with --receiver. Tests RTMP H264/AAC and WHIP H264/Opus with one and two users,
decoded colors/tones, WHIP session cleanup, and RTMP receiver disconnection.
Artifacts are kept in /tmp; no existing services or recordings are modified.
"""

import argparse
import array
import datetime
import json
import math
from pathlib import Path
import socket
import subprocess
import time
import urllib.request

PROJECT = Path(__file__).resolve().parent.parent


def probe(file):
    result = subprocess.run(['ffprobe', '-v', 'error', '-show_streams', '-show_format',
                             '-of', 'json', str(file)], check=True, capture_output=True)
    return json.loads(result.stdout)


def verify(file, protocol, users, seconds, artifact):
    info = probe(file)
    (artifact / 'probe.json').write_text(json.dumps(info, indent=2) + '\n')
    video = next(item for item in info['streams'] if item['codec_type'] == 'video')
    audio = next(item for item in info['streams'] if item['codec_type'] == 'audio')
    assert video['codec_name'] == 'h264', video
    assert audio['codec_name'] == ('opus' if protocol == 'whip' else 'aac'), audio
    duration = float(info['format']['duration'])
    assert duration >= seconds - 2, duration
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-i', str(file),
                    '-fps_mode', 'passthrough', '-enc_time_base:v', '-1', '-f', 'null', '-'],
                   check=True, capture_output=True, timeout=30)
    raw = subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-ss', str(duration / 2),
                          '-i', str(file), '-frames:v', '1', '-vf', 'scale=320:180',
                          '-pix_fmt', 'rgb24', '-f', 'rawvideo', 'pipe:1'],
                         check=True, capture_output=True).stdout
    assert len(raw) == 320 * 180 * 3
    counts = {'red': 0, 'blue': 0}
    for offset in range(0, len(raw), 3):
        red, green, blue = raw[offset:offset + 3]
        counts['red'] += red > 75 and red > green * 1.4 and red > blue * 1.4
        counts['blue'] += blue > 75 and blue > green * 1.4 and blue > red * 1.4
    # Two side-by-side 16:9 publishers are letterboxed inside half-width slots.
    assert counts['red'] > 320 * 180 * (0.2 if users == 2 else 0.8), counts
    assert (counts['blue'] > 320 * 180 * 0.2 if users == 2 else counts['blue'] == 0), counts
    subprocess.run(['ffmpeg', '-nostdin', '-y', '-v', 'error', '-ss', str(duration / 2),
                    '-i', str(file), '-frames:v', '1', str(artifact / 'frame.jpg')],
                   check=True, capture_output=True)
    pcm = subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-ss', str(duration / 2),
                          '-i', str(file), '-t', '1', '-vn', '-ar', '8000', '-ac', '1',
                          '-f', 's16le', 'pipe:1'], check=True, capture_output=True).stdout
    samples = array.array('h', pcm)
    assert samples
    tones = {}
    for frequency in [440, 880]:
        angle = 2 * math.pi * frequency / 8000
        sine = sum(value * math.sin(angle * i) for i, value in enumerate(samples))
        cosine = sum(value * math.cos(angle * i) for i, value in enumerate(samples))
        tones[str(frequency)] = 2 * math.hypot(sine, cosine) / len(samples)
    assert tones['440'] > 500, tones
    assert (tones['880'] > 500 if users == 2 else tones['880'] < 100), tones
    return {'duration': duration, 'video': video['codec_name'], 'audio': audio['codec_name'],
            'colors': counts, 'tones': tones}


def stop(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def receiver_config(root):
    return {
        'logLevel': 'info', 'api': True, 'apiAddress': '127.0.0.1:18297',
        'rtsp': False, 'rtmp': True, 'rtmpAddress': '127.0.0.1:18235',
        'hls': False, 'srt': False, 'moq': False, 'webrtc': True, 'webrtcAddress': '127.0.0.1:18289',
        'webrtcLocalUDPAddress': '127.0.0.1:18290', 'webrtcIPsFromInterfaces': False,
        'webrtcAdditionalHosts': ['127.0.0.1'],
        # Keep MediaMTX's normal one-second part duration: earlier header emission can write
        # placeholder SPS/PPS when RTC audio arrives before the first video keyframe.
        'pathDefaults': {'record': True, 'recordPath': str(root / 'received/%path/%Y-%m-%d_%H-%M-%S-%f'),
                         'recordFormat': 'fmp4', 'recordPartDuration': '1s', 'recordSegmentDuration': '1h'},
        'paths': {'all_others': {}},
    }


def sessions():
    with urllib.request.urlopen('http://127.0.0.1:18297/v3/webrtcsessions/list', timeout=2) as response:
        return json.load(response)['items']


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--receiver', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, default=PROJECT / 'build/tests-whip/streaming_media_fixture')
    parser.add_argument('--seconds', type=int, default=6)
    args = parser.parse_args()
    if args.seconds < 4:
        parser.error('--seconds must be at least 4')
    root = Path('/tmp') / ('rtc-egress-streaming-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    root.mkdir(mode=0o700)
    print('Artifacts: ' + str(root), flush=True)
    for port in [18235, 18289, 18297]:
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', port))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 18290))
    config = root / 'receiver.json'
    config.write_text(json.dumps(receiver_config(root), indent=2) + '\n')
    results = []
    receiver = publisher = None
    try:
        with (root / 'receiver.log').open('w') as output:
            receiver = subprocess.Popen([str(args.receiver.resolve()), str(config)], cwd=root, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while True:
            try:
                sessions()
                break
            except Exception:
                if receiver.poll() is not None or time.monotonic() >= deadline:
                    details = (root / 'receiver.log').read_text()[-2000:]
                    raise RuntimeError('Receiver failed to become ready:\n' + details)
                time.sleep(0.1)
        for protocol in ['rtmp', 'whip']:
            for users in [1, 2]:
                name = protocol + '_' + str(users) + '_users'
                case = root / name
                case.mkdir()
                url = ('rtmp://127.0.0.1:18235/' + name if protocol == 'rtmp'
                       else 'http://127.0.0.1:18289/' + name + '/whip')
                with (case / 'publisher.log').open('w') as output:
                    result = subprocess.run([str(args.fixture.resolve()), protocol, url, str(users), str(args.seconds)],
                                            cwd=case, stdout=output, stderr=subprocess.STDOUT, timeout=args.seconds + 12)
                assert result.returncode == 0, 'Publishing failed: ' + str(case / 'publisher.log')
                time.sleep(0.5)
                files = list((root / 'received' / name).glob('*.mp4'))
                assert len(files) == 1, files
                metrics = verify(files[0], protocol, users, args.seconds, case)
                if protocol == 'whip':
                    assert not sessions(), 'WHIP session remained after stop'
                results.append({'case': name, 'status': 'PASS', 'metrics': metrics})
                print('PASS ' + name + ': ' + json.dumps(metrics), flush=True)
        case = root / 'rtmp_disconnect'
        case.mkdir()
        with (case / 'publisher.log').open('w') as output:
            publisher = subprocess.Popen([str(args.fixture.resolve()), 'rtmp',
                                           'rtmp://127.0.0.1:18235/disconnect', '2', '20'],
                                          cwd=case, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while 'ready\n' not in (case / 'publisher.log').read_text():
            if publisher.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('Disconnect publisher did not start')
            time.sleep(0.1)
        time.sleep(2)
        stopped = time.monotonic()
        stop(receiver)
        assert publisher.wait(timeout=6) == 1, 'Receiver disconnect was not reported as failure'
        results.append({'case': 'rtmp_disconnect', 'status': 'PASS', 'failure_latency': time.monotonic() - stopped})
        print('PASS rtmp_disconnect', flush=True)
    except Exception as error:
        results.append({'case': 'run', 'status': 'FAIL', 'error': str(error)})
        print('FAIL: ' + str(error), flush=True)
    finally:
        stop(publisher)
        stop(receiver)
        (root / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    return 0 if len(results) == 5 and all(item['status'] == 'PASS' for item in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
