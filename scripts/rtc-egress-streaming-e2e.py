#!/usr/bin/env python3
"""Publish synthetic media through StreamingSink to a real MediaMTX receiver.

Build tests/ with FFmpeg 8 using CMake first. Supply a MediaMTX v1.21.1 binary
with --receiver. Tests RTMP H264/AAC and WHIP H264/Opus with one and two users,
decoded colors/tones, initial connection retries, fresh sessions after receiver
restarts, silent UDP outages, authenticated consent, bounded retries, stop
cancellation, task duration deadlines, and WHIP session cleanup.
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

from whip_udp_proxy import WhipUdpProxy

PROJECT = Path(__file__).resolve().parent.parent


def probe(file):
    result = subprocess.run(['ffprobe', '-v', 'error', '-show_streams', '-show_format',
                             '-of', 'json', str(file)], check=True, capture_output=True)
    return json.loads(result.stdout)


def verify(file, protocol, users, seconds, artifact, fresh=False):
    info = probe(file)
    (artifact / 'probe.json').write_text(json.dumps(info, indent=2) + '\n')
    video = next(item for item in info['streams'] if item['codec_type'] == 'video')
    audio = next(item for item in info['streams'] if item['codec_type'] == 'audio')
    assert video['codec_name'] == 'h264', video
    assert audio['codec_name'] == ('opus' if protocol == 'whip' else 'aac'), audio
    duration = float(info['format']['duration'])
    assert duration >= seconds - 2, duration
    packets = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                              '-read_intervals', '%+2', '-show_packets', '-of', 'json', str(file)],
                             check=True, capture_output=True)
    video_packets = json.loads(packets.stdout)['packets']
    first = video_packets[0]
    # MediaMTX can drop the initial IDR if audio starts its fMP4 segment a few us later.
    # Require a promptly usable keyframe, while checking the fresh origin separately.
    first_key = next(packet for packet in video_packets if 'K' in packet['flags'])
    assert float(first_key['pts_time']) < 1.5, first_key
    # Initial fixtures deliberately start audio 400 ms before video; recovery should start fresh.
    start_limit = 0.25 if fresh else 0.6
    assert abs(float(first['pts_time'])) < start_limit, first
    assert abs(float(video.get('start_time', 0)) - float(audio.get('start_time', 0))) < start_limit, info
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
            'colors': counts, 'tones': tones, 'first_video_packet': first,
            'first_keyframe': first_key}


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
        # Allow a recovered session to stay muted until the fixture ends.
        'webrtcTrackGatherTimeout': '30s',
        # Keep MediaMTX's normal one-second part duration: earlier header emission can write
        # placeholder SPS/PPS when RTC audio arrives before the first video keyframe.
        'pathDefaults': {'record': True, 'recordPath': str(root / 'received/%path/%Y-%m-%d_%H-%M-%S-%f'),
                         'recordFormat': 'fmp4', 'recordPartDuration': '1s', 'recordSegmentDuration': '1h'},
        'paths': {'all_others': {}},
    }


def sessions():
    with urllib.request.urlopen('http://127.0.0.1:18297/v3/webrtcsessions/list', timeout=2) as response:
        return json.load(response)['items']


def wait_log(process, log, text, timeout=10):
    deadline = time.monotonic() + timeout
    while text not in log.read_text():
        if process.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError('Publisher did not report ' + text + ': ' + str(log))
        time.sleep(0.05)


def destination(protocol, name):
    return ('rtmp://127.0.0.1:18235/' + name if protocol == 'rtmp'
            else 'http://127.0.0.1:18289/' + name + '/whip')


def wait_media(process, name, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and process.poll() is None:
        with urllib.request.urlopen('http://127.0.0.1:18297/v3/paths/list', timeout=2) as response:
            if any(item['name'] == name and item.get('ready') for item in json.load(response)['items']):
                return
        time.sleep(0.1)
    raise RuntimeError('Receiver did not receive media: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--receiver', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, default=PROJECT / 'build/tests-whip/streaming_media_fixture')
    parser.add_argument('--seconds', type=int, default=6)
    parser.add_argument('--filter', default='', help='Run cases matching any comma-separated substring')
    args = parser.parse_args()
    if args.seconds < 4 or args.seconds > 52:
        parser.error('--seconds must be between 4 and 52')
    root = Path('/tmp') / ('rtc-egress-streaming-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    root.mkdir(mode=0o700)
    print('Artifacts: ' + str(root), flush=True)
    for port in [18235, 18288, 18289, 18297]:
        with socket.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(('127.0.0.1', port))
    for port in [18290, 18291]:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(('127.0.0.1', port))
    config = root / 'receiver.json'
    config.write_text(json.dumps(receiver_config(root), indent=2) + '\n')
    results = []
    expected = 0
    receiver = publisher = proxy = None

    def selected(name):
        return not args.filter or any(part in name for part in args.filter.split(','))

    def start_receiver():
        nonlocal receiver
        with (root / 'receiver.log').open('a') as output:
            receiver = subprocess.Popen([str(args.receiver.resolve()), str(config)], cwd=root,
                                        stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while True:
            try:
                sessions()
                return
            except (OSError, ValueError):
                if receiver.poll() is not None or time.monotonic() >= deadline:
                    details = (root / 'receiver.log').read_text()[-2000:]
                    raise RuntimeError('Receiver failed to become ready:\n' + details)
                time.sleep(0.1)

    def start_publisher(case, protocol, users, seconds, attempts=5, delay=1000,
                        max_seconds=28800, timeout=2000, url=None, idle_after_ms=None):
        nonlocal publisher
        command = [str(args.fixture.resolve()), protocol,
                   url or destination(protocol, case.name), str(users), str(seconds),
                   str(attempts), str(delay), str(max_seconds), str(timeout)]
        if idle_after_ms is not None:
            command.append(str(idle_after_ms))
        with (case / 'publisher.log').open('w') as output:
            publisher = subprocess.Popen(command,
                                         cwd=case, stdout=output, stderr=subprocess.STDOUT)
        wait_log(publisher, case / 'publisher.log', 'ready\n')

    try:
        start_receiver()
        for protocol in ['rtmp', 'whip']:
            for users in [1, 2]:
                name = protocol + '_' + str(users) + '_users'
                if not selected(name):
                    continue
                expected += 1
                case = root / name
                case.mkdir()
                start_publisher(case, protocol, users, args.seconds)
                assert publisher.wait(timeout=args.seconds + 12) == 0, 'Publishing failed: ' + str(case / 'publisher.log')
                time.sleep(0.5)
                files = list((root / 'received' / name).glob('*.mp4'))
                assert len(files) == 1, files
                metrics = verify(files[0], protocol, users, args.seconds, case)
                if protocol == 'whip':
                    assert not sessions(), 'WHIP session remained after stop'
                results.append({'case': name, 'status': 'PASS', 'metrics': metrics})
                print('PASS ' + name + ': ' + json.dumps(metrics), flush=True)
        for protocol in ['rtmp', 'whip']:
            for users in [1, 2]:
                name = protocol + '_restart_' + str(users) + '_users'
                if not selected(name):
                    continue
                expected += 1
                case = root / name
                case.mkdir()
                start_publisher(case, protocol, users, args.seconds + 8, delay=500)
                wait_media(publisher, name)
                time.sleep(1.2)
                stop(receiver)
                wait_log(publisher, case / 'publisher.log', 'reconnecting\n')
                time.sleep(0.7)
                assert publisher.poll() is None, 'Publisher exited during outage'
                start_receiver()
                wait_log(publisher, case / 'publisher.log', 'recovered\n')
                assert publisher.wait(timeout=args.seconds + 12) == 0, 'Publisher failed to recover'
                time.sleep(0.5)
                files = sorted((root / 'received' / name).glob('*.mp4'))
                assert len(files) == 2, files
                metrics = verify(files[-1], protocol, users, args.seconds, case, fresh=True)
                if protocol == 'whip':
                    assert not sessions(), 'Recovered WHIP session remained after stop'
                results.append({'case': name, 'status': 'PASS', 'metrics': metrics})
                print('PASS ' + name + ': ' + json.dumps(metrics), flush=True)

            for scenario in ['exhaustion', 'disabled', 'stop_delay', 'stop_handshake',
                             'duration_delay', 'duration_handshake']:
                name = protocol + '_' + scenario
                if not selected(name):
                    continue
                expected += 1
                case = root / name
                case.mkdir()
                attempts = 0 if scenario == 'disabled' else 2
                delay = 5000 if scenario.endswith('_delay') else 100
                max_seconds = 5 if scenario.startswith('duration_') else 28800
                timeout = 30000 if scenario.endswith('_handshake') else 2000
                start_publisher(case, protocol, 2, 30, attempts, delay, max_seconds, timeout)
                time.sleep(2)
                stopped = time.monotonic()
                stop(receiver)
                log = case / 'publisher.log'
                if scenario.endswith('_handshake'):
                    # Accept TCP without answering RTMP/HTTP, so a reconnect handshake stalls.
                    with socket.socket() as server:
                        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                        server.bind(('127.0.0.1', 18235 if protocol == 'rtmp' else 18289))
                        server.listen(1)
                        server.settimeout(5)
                        client, _ = server.accept()
                        with client:
                            if scenario == 'stop_handshake':
                                stopped = time.monotonic()
                                publisher.terminate()
                            assert publisher.wait(timeout=6) == 0, 'Stalled reconnect did not stop'
                elif scenario == 'stop_delay':
                    wait_log(publisher, log, 'reconnecting\n')
                    stopped = time.monotonic()
                    publisher.terminate()
                    assert publisher.wait(timeout=3) == 0, 'Retry delay did not stop'
                else:
                    expected_code = 0 if scenario.startswith('duration_') else 1
                    assert publisher.wait(timeout=8) == expected_code, 'Unexpected completion: ' + str(log)
                latency = time.monotonic() - stopped
                content = log.read_text()
                if scenario.startswith('stop_'):
                    assert latency < 2, latency
                    assert 'complete ' not in content, content
                elif scenario.startswith('duration_'):
                    assert 'complete success Streaming duration reached' in content, content
                else:
                    assert 'complete failed Streaming destination reconnect exhausted after ' + str(attempts) + ' attempts' in content, content
                    assert content.count('reconnect attempt ') == attempts, content
                start_receiver()
                assert not sessions(), 'WHIP session remained after terminal completion'
                results.append({'case': name, 'status': 'PASS', 'completion_latency': latency})
                print('PASS ' + name + ': ' + str(round(latency, 3)) + 's', flush=True)

        for protocol in ['rtmp', 'whip']:
            for users in [1, 2]:
                for scenario in ['recovery', 'exhaustion', 'disabled', 'stop', 'duration']:
                    name = protocol + '_startup_' + scenario + '_' + str(users) + '_users'
                    if not selected(name):
                        continue
                    expected += 1
                    case = root / name
                    case.mkdir()
                    stop(receiver)
                    attempts = 0 if scenario == 'disabled' else 3
                    start_publisher(case, protocol, users, args.seconds + 5, attempts=attempts,
                                    delay=500, max_seconds=1 if scenario == 'duration' else 28800)
                    log = case / 'publisher.log'
                    if scenario == 'recovery':
                        wait_log(publisher, log, 'reconnecting\n')
                        start_receiver()
                        wait_log(publisher, log, 'recovered\n')
                        assert publisher.wait(timeout=args.seconds + 10) == 0
                        time.sleep(0.5)
                        files = list((root / 'received' / name).glob('*.mp4'))
                        assert len(files) == 1, files
                        metrics = verify(files[0], protocol, users, args.seconds, case, fresh=True)
                    else:
                        if scenario == 'stop':
                            wait_log(publisher, log, 'reconnecting\n')
                            stopped = time.monotonic()
                            publisher.terminate()
                        assert publisher.wait(timeout=8) == (1 if scenario in ('exhaustion', 'disabled') else 0)
                        content = log.read_text()
                        if scenario == 'stop':
                            assert time.monotonic() - stopped < 2
                            assert 'complete ' not in content, content
                        elif scenario == 'duration':
                            assert 'complete success Streaming duration reached' in content, content
                        else:
                            assert 'complete failed Streaming destination reconnect exhausted after ' + str(attempts) + ' attempts' in content
                            assert content.count('reconnect attempt ') == attempts, content
                        metrics = {'completion': content.split('complete ')[-1].splitlines()[0]}
                        start_receiver()
                    assert not sessions(), 'WHIP session remained after startup test'
                    results.append({'case': name, 'status': 'PASS', 'metrics': metrics})
                    print('PASS ' + name, flush=True)

        for users in [1, 2]:
            for scenario in ['stop', 'duration', 'exhaustion']:
                name = 'whip_initial_ice_' + scenario + '_' + str(users) + '_users'
                if not selected(name):
                    continue
                expected += 1
                case = root / name
                case.mkdir()
                proxy = WhipUdpProxy()
                proxy.drop_outgoing.set()
                proxy.drop_incoming.set()
                start_publisher(case, 'whip', users, 20, attempts=1, delay=100,
                                max_seconds=2 if scenario == 'duration' else 28800,
                                timeout=1000 if scenario == 'exhaustion' else 30000,
                                url='http://127.0.0.1:18288/' + name + '/whip')
                deadline = time.monotonic() + 3
                while not proxy.stats['requests'] and publisher.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.01)
                assert proxy.stats['requests'] > 0, 'Initial ICE handshake never started'
                stopped = time.monotonic()
                if scenario == 'stop':
                    publisher.terminate()
                assert publisher.wait(timeout=6) == (1 if scenario == 'exhaustion' else 0)
                content = (case / 'publisher.log').read_text()
                if scenario == 'stop':
                    assert time.monotonic() - stopped < 2
                    assert 'complete ' not in content
                elif scenario == 'duration':
                    assert 'complete success Streaming duration reached' in content
                else:
                    assert 'complete failed Streaming destination reconnect exhausted after 1 attempts' in content
                    assert content.count('reconnect attempt ') == 1
                assert not sessions(), 'WHIP session remained after failed initial ICE handshake'
                assert proxy.error is None, proxy.error
                results.append({'case': name, 'status': 'PASS', 'proxy': dict(proxy.stats)})
                print('PASS ' + name + ': ' + json.dumps(proxy.stats), flush=True)
                proxy.close()
                proxy = None

        for users in [1, 2]:
            for scenario in ['both', 'outgoing', 'incoming_forged', 'incoming_replayed',
                             'idle', 'idle_outage', 'exhaustion', 'stop', 'duration']:
                name = 'whip_blackhole_' + scenario + '_' + str(users) + '_users'
                if not selected(name):
                    continue
                expected += 1
                case = root / name
                case.mkdir()
                proxy = WhipUdpProxy()
                idle = scenario in ('idle', 'idle_outage')
                start_publisher(case, 'whip', users, 16, attempts=2, delay=500,
                                max_seconds=6 if scenario == 'duration' else 28800,
                                url='http://127.0.0.1:18288/' + name + '/whip',
                                idle_after_ms=3000 if idle else None)
                wait_media(publisher, name)
                time.sleep(2.5)
                baseline = proxy.stats['requests']
                detection = None
                if scenario == 'idle':
                    assert publisher.wait(timeout=18) == 0, 'Healthy idle stream failed'
                    assert proxy.stats['requests'] >= baseline + 5, proxy.stats
                    assert 'reconnect attempt ' not in (case / 'publisher.log').read_text()
                else:
                    if scenario in ('outgoing', 'both', 'idle_outage', 'exhaustion', 'stop', 'duration'):
                        proxy.drop_outgoing.set()
                    if scenario != 'outgoing':
                        proxy.drop_incoming.set()
                    proxy.forge_responses = scenario == 'incoming_forged'
                    proxy.replay_responses = scenario == 'incoming_replayed'
                    outage = time.monotonic()
                    wait_log(publisher, case / 'publisher.log', 'reconnecting\n', timeout=4)
                    detection = time.monotonic() - outage
                    assert 'WHIP ICE consent expired' in (case / 'publisher.log').read_text()
                    assert detection < 3, detection
                    if scenario == 'stop':
                        stopped = time.monotonic()
                        publisher.terminate()
                        assert publisher.wait(timeout=3) == 0
                        assert time.monotonic() - stopped < 2
                    elif scenario in ('exhaustion', 'duration'):
                        assert publisher.wait(timeout=9) == (1 if scenario == 'exhaustion' else 0)
                        expected_message = ('complete failed Streaming destination reconnect exhausted after 2 attempts'
                                            if scenario == 'exhaustion'
                                            else 'complete success Streaming duration reached')
                        assert expected_message in (case / 'publisher.log').read_text()
                    else:
                        proxy.drop_outgoing.clear()
                        proxy.drop_incoming.clear()
                        wait_log(publisher, case / 'publisher.log', 'recovered\n', timeout=6)
                        assert publisher.wait(timeout=18) == 0
                        if not idle:
                            time.sleep(0.5)
                            files = sorted((root / 'received' / name).glob('*.mp4'))
                            assert len(files) == 2, files
                            verify(files[-1], 'whip', users, 6, case, fresh=True)
                    if scenario == 'incoming_forged':
                        assert proxy.stats['forged'] > 0, proxy.stats
                    if scenario == 'incoming_replayed':
                        assert proxy.stats['replayed'] > 0, proxy.stats
                assert not sessions(), 'WHIP session remained after blackhole test'
                assert proxy.error is None, proxy.error
                results.append({'case': name, 'status': 'PASS', 'proxy': dict(proxy.stats),
                                'detection_seconds': detection})
                print('PASS ' + name + ': ' + json.dumps(proxy.stats), flush=True)
                proxy.close()
                proxy = None
    except Exception as error:
        results.append({'case': 'run', 'status': 'FAIL', 'error': str(error)})
        print('FAIL: ' + str(error), flush=True)
    finally:
        stop(publisher)
        if proxy:
            proxy.close()
        stop(receiver)
        (root / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    return 0 if expected and len(results) == expected and all(item['status'] == 'PASS' for item in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
