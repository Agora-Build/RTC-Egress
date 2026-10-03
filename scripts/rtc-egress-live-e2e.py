#!/usr/bin/env python3
"""Exercise the HTTP API, Redis queue, native egress, and live Agora publishers.

Requires freshly built bin/api-server, bin/egress, and bin/eg_worker; atem with
the desired Agora project selected; stream-to-agora; ffmpeg/ffprobe; redis-cli;
and an empty Redis database 15. Generates labeled red/blue videos with 440/880
Hz audio and checks codecs, duration, full decoding, both users, both tones,
and finalized metadata. Artifacts and redacted logs remain in /tmp.

Usage:
    python3 scripts/rtc-egress-live-e2e.py
    python3 scripts/rtc-egress-live-e2e.py --filter single --seconds 15
    python3 scripts/rtc-egress-live-e2e.py --stream-sdk-dir /path/to/rtc/sdk
    python3 scripts/rtc-egress-live-e2e.py --verify-run /tmp/rtc-egress-e2e-...

Publisher and egress SDK libraries are configured separately because their
SDK versions can differ. A source directory from a previous run can be reused
with --source-dir. No existing recordings or running services are removed.
"""

import argparse
import array
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.request

PROJECT = Path(__file__).resolve().parent.parent
STREAM = PROJECT.parent / 'stream-to-agora/target/release/stream-to-agora'
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--seconds', type=int, default=15, help='Recording time per case (minimum 10)')
parser.add_argument('--filter', default='', help='Run cases whose names contain this text')
parser.add_argument('--source-dir', type=Path, help='Reuse user_1001.mp4 and user_1002.mp4 fixtures')
parser.add_argument('--stream-tool', type=Path, default=Path(shutil.which('stream-to-agora') or STREAM),
                    help='Publisher executable (default: PATH or sibling stream-to-agora repository)')
parser.add_argument('--stream-sdk-dir', type=Path, help='SDK library directory matching the publisher binary')
parser.add_argument('--verify-run', type=Path, help='Recheck saved recordings without starting services or publishers')
args = parser.parse_args()
if args.seconds < 10:
    parser.error('--seconds must be at least 10 for meaningful audio/video verification')
STREAM = args.stream_tool.resolve()
if args.source_dir:
    args.source_dir = args.source_dir.resolve()
ROOT = args.verify_run.resolve() if args.verify_run else Path('/tmp') / (
    'rtc-egress-e2e-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
if args.verify_run:
    if not (ROOT / 'results.json').is_file():
        parser.error('--verify-run must contain results.json from a completed live run')
else:
    ROOT.mkdir(mode=0o700)
    (ROOT / 'logs').mkdir()
    (ROOT / 'sources').mkdir()
    (ROOT / 'bin').mkdir()
    (ROOT / 'bin/eg_worker').symlink_to(PROJECT / 'bin/eg_worker')
SECRETS = []
PROCESSES = []
RESULTS = []
REDIS_OWNED = False
CHANNEL = 'egress_e2e_' + str(int(time.time()))
ENV = os.environ.copy()
for key in ['CONFIG_FILE', 'CONFIG_DIR', 'REDIS_ADDR', 'REDIS_DB', 'REDIS_PASSWORD',
            'AGORA_APP_ID', 'API_PORT', 'HEALTH_PORT', 'SERVER_REGION', 'SERVER_WORKERS']:
    ENV.pop(key, None)
ENV['LD_LIBRARY_PATH'] = str(PROJECT / 'bin') + ':' + ENV.get('LD_LIBRARY_PATH', '')
ENV['GIN_MODE'] = 'release'
STREAM_ENV = ENV.copy()
if args.stream_sdk_dir:
    STREAM_ENV['LD_LIBRARY_PATH'] = str(args.stream_sdk_dir.resolve())
else:
    STREAM_ENV.pop('LD_LIBRARY_PATH', None)


def clean(text):
    for value in SECRETS:
        text = text.replace(value, '[REDACTED]')
    return text


def log(message):
    text = '[' + time.strftime('%H:%M:%S') + '] ' + clean(message)
    print(text, flush=True)
    with (ROOT / 'run.log').open('a') as output:
        output.write(text + '\n')


def run(command, timeout=60, env=None):
    result = subprocess.run(command, cwd=ROOT, env=env or ENV, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError('Command failed (' + Path(command[0]).name + '): ' + clean(result.stderr.decode(errors='replace')[-2500:]))
    return result.stdout


def start(command, path, env=None):
    process = subprocess.Popen(command, cwd=ROOT, env=env or ENV, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=True)
    process.log_path = path
    process.log_thread = threading.Thread(target=capture_log, args=(process, path), daemon=True)
    process.log_thread.start()
    PROCESSES.append(process)
    return process


def capture_log(process, path):
    with path.open('w') as output:
        for line in iter(process.stdout.readline, b''):
            output.write(clean(line.decode(errors='replace')))
            output.flush()


def descendants(pid):
    found = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
            if int(stat[1]) == pid:
                found.append(int(entry.name))
        except (OSError, ValueError, IndexError):
            pass
    return found + [child for child_pid in found for child in descendants(child_pid)]


def stop(process):
    if process is None or process.poll() is not None:
        return
    children = descendants(process.pid)
    os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)
    for child in children:
        try:
            os.kill(child, signal.SIGTERM)
        except ProcessLookupError:
            pass
    process.log_thread.join(timeout=2)


def request(url, payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=10) as response:
        return json.load(response)


def wait_health(url, process):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError('Service exited: ' + str(process.log_path))
        try:
            request(url)
            return
        except Exception:
            time.sleep(0.25)
    raise RuntimeError('Service did not become healthy: ' + url)


def wait_ready(process):
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError('Publisher exited with code ' + str(process.returncode) + ': ' + str(process.log_path))
        if process.log_path.exists() and re.search(r'^ready$', process.log_path.read_text(), re.M):
            return
        time.sleep(0.25)
    raise RuntimeError('Publisher did not connect: ' + str(process.log_path))


def token(uid):
    output = run(['atem', 'token', 'rtc', 'create', '--channel', CHANNEL,
                  '--rtc-user-id', str(uid), '--expire', '3600']).decode()
    lines = [line.strip() for line in output.splitlines()]
    for i, line in enumerate(lines):
        if line.startswith('RTC Token') and i + 1 < len(lines):
            value = lines[i + 1]
            if len(value) > 50:
                SECRETS.append(value)
                return value
    raise RuntimeError('atem did not return an RTC token')


def write_json(path, value):
    path.write_text(clean(json.dumps(value, indent=2)) + '\n')


def report_results(commit):
    passed = sum(item['status'] == 'PASS' for item in RESULTS)
    lines = ['# Live RTC Egress E2E Results', '', 'Recorded commit: `' + commit + '`', '',
             str(passed) + ' passed, ' + str(len(RESULTS) - passed) + ' failed.', '',
             '| Case | Result | Detail |', '| --- | --- | --- |']
    for result in RESULTS:
        detail = result.get('error', 'Video, audio, content, task stop, and metadata checks passed')
        lines.append('| ' + result['case'] + ' | ' + result['status'] + ' | ' +
                     detail.replace('|', '\\|').replace('\n', ' ') + ' |')
    lines.extend(['', 'Each case directory retains its recording, probe data, sample frames,',
                  'audio measurements, task responses, and logs where available.', ''])
    (ROOT / 'report.md').write_text(clean('\n'.join(lines)))
    log('RESULTS: ' + str(passed) + ' passed, ' + str(len(RESULTS) - passed) + ' failed')
    log('Artifacts: ' + str(ROOT))
    return 0 if passed == len(RESULTS) and RESULTS else 1


def fixture(uid, color, frequency):
    path = ROOT / 'sources' / ('user_' + str(uid) + '.mp4')
    if args.source_dir:
        cached = args.source_dir / path.name
        if cached.is_file():
            path.symlink_to(cached)
            return path
    log('Generating distinguishable publisher ' + str(uid) + ' (' + color + ', ' + str(frequency) + ' Hz)')
    run(['ffmpeg', '-nostdin', '-v', 'error', '-f', 'lavfi', '-i',
         'color=c=' + color + ':size=640x360:rate=24', '-f', 'lavfi', '-i',
         'sine=frequency=' + str(frequency) + ':sample_rate=48000', '-t', '600',
         '-vf', "drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf:text='USER " + str(uid) +
         "':fontcolor=white:fontsize=48:x=(w-tw)/2:y=(h-th)/2,drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf:text='%{pts\\:hms}':fontcolor=white:fontsize=24:x=20:y=h-40",
         '-c:v', 'libx264', '-preset', 'ultrafast', '-tune', 'zerolatency', '-g', '48',
         '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-ar', '48000', '-ac', '2',
         '-b:a', '128k', '-movflags', '+faststart', str(path)], timeout=180)
    return path


def check_recording(file, case, expect_multi):
    (case / 'metrics.json').unlink(missing_ok=True)
    issues = []
    probe = json.loads(run(['ffprobe', '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(file)]))
    write_json(case / 'ffprobe.json', probe)
    videos = [item for item in probe['streams'] if item['codec_type'] == 'video']
    audios = [item for item in probe['streams'] if item['codec_type'] == 'audio']
    if len(videos) != 1 or len(audios) != 1:
        raise RuntimeError('Expected one video and one audio stream')
    video, audio = videos[0], audios[0]
    if video['codec_name'] != 'h264' or audio['codec_name'] != 'aac':
        raise RuntimeError('Unexpected video/audio codecs')
    video_duration, audio_duration = float(video['duration']), float(audio['duration'])
    if min(video_duration, audio_duration) < args.seconds - 7:
        raise RuntimeError('Recording too short: video=' + str(video_duration) + ' audio=' + str(audio_duration))
    if abs(video_duration - audio_duration) > 2.0:
        raise RuntimeError('Audio/video durations differ by more than 2 seconds')
    # Preserve variable frame timing so the null muxer does not round distinct timestamps together.
    decode = subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-i', str(file),
                             '-fps_mode', 'passthrough', '-enc_time_base:v', '-1', '-f', 'null', '-'],
                            cwd=ROOT, env=ENV, capture_output=True, timeout=45)
    (case / 'decode.log').write_text(clean(decode.stderr.decode(errors='replace')))
    if decode.returncode or decode.stderr.strip():
        raise RuntimeError('Full-file decoding failed; inspect decode.log')
    frames = []
    # The compositor needs a few seconds to stabilize after users join.
    for i, position in enumerate([0.5, 0.7, 0.9]):
        timestamp = min(video_duration, audio_duration) * position
        raw = run(['ffmpeg', '-nostdin', '-v', 'error', '-ss', str(timestamp), '-i', str(file),
                   '-frames:v', '1', '-vf', 'scale=320:180', '-pix_fmt', 'rgb24', '-f', 'rawvideo', 'pipe:1'])
        if len(raw) != 320 * 180 * 3:
            raise RuntimeError('Could not extract sample frame')
        red = blue = 0
        for p in range(0, len(raw), 3):
            r, g, b = raw[p:p + 3]
            red += r > 75 and r > g * 1.4 and r > b * 1.4
            blue += b > 75 and b > g * 1.4 and b > r * 1.4
        red_fraction, blue_fraction = red / (320 * 180), blue / (320 * 180)
        frames.append({'time': round(timestamp, 3), 'red_fraction': round(red_fraction, 4),
                       'blue_fraction': round(blue_fraction, 4), 'sha256': hashlib.sha256(raw).hexdigest()})
        run(['ffmpeg', '-nostdin', '-y', '-v', 'error', '-ss', str(timestamp), '-i', str(file),
             '-frames:v', '1', '-q:v', '2', str(case / ('frame_' + str(i + 1) + '.jpg'))])
        if red_fraction < 0.03 or (expect_multi and blue_fraction < 0.03):
            issues.append('Missing publisher content at ' + str(round(timestamp, 3)) +
                          's: red=' + str(round(red_fraction, 4)) + ' blue=' + str(round(blue_fraction, 4)))
        if not expect_multi and blue_fraction > 0.01:
            issues.append('Unexpected second user in single-user output')
    if len({frame['sha256'] for frame in frames}) < 2:
        issues.append('Video frames do not change')
    pcm = run(['ffmpeg', '-nostdin', '-v', 'error', '-ss', str(audio_duration * 0.5), '-i', str(file),
               '-t', '1', '-vn', '-ar', '8000', '-ac', '1', '-f', 's16le', 'pipe:1'])
    samples = array.array('h', pcm)
    if sys.byteorder != 'little':
        samples.byteswap()
    if not samples:
        raise RuntimeError('Could not decode audio samples')
    rms = math.sqrt(sum(value * value for value in samples) / len(samples))
    tones = {}
    for frequency in [440, 880]:
        angle = 2 * math.pi * frequency / 8000
        sine = sum(value * math.sin(angle * i) for i, value in enumerate(samples))
        cosine = sum(value * math.cos(angle * i) for i, value in enumerate(samples))
        tones[str(frequency)] = round(2 * math.hypot(sine, cosine) / len(samples), 2)
    metrics = {'video_duration': video_duration, 'audio_duration': audio_duration,
               'width': video['width'], 'height': video['height'], 'size': file.stat().st_size,
               'frames': frames, 'audio_rms': round(rms, 2), 'audio_tone_amplitudes': tones}
    if rms < 100 or tones['440'] < 50 or (expect_multi and tones['880'] < 50):
        issues.append('Publisher audio tones are missing or distorted: RMS=' +
                      str(round(rms, 2)) + ' tones=' + str(tones))
    metadata_paths = list((case / 'recordings').glob('*.json'))
    if not metadata_paths:
        issues.append('Recording metadata was not generated')
    else:
        metadata = [json.loads(path.read_text()) for path in metadata_paths]
        if not any(item.get('sessionCompleted') is True for item in metadata):
            issues.append('Recording metadata did not finalize')
        if not any(entry.get('isComplete') for item in metadata for entry in item.get('files', [])):
            issues.append('Recording file metadata did not mark output complete')
    metrics['validation_errors'] = issues
    write_json(case / 'metrics.json', metrics)
    if issues:
        raise RuntimeError('; '.join(issues))
    return metrics


def test_case(name, users, decode_mode, layout, publishers):
    case = ROOT / name
    case.mkdir()
    (case / 'recordings').mkdir()
    egress_config = {
        'server': {'health_port': 18192, 'workers': 1, 'region': '', 'task_ttl': 600,
                   'worker_patterns': ['egress:record:*', 'egress:*:record:*']},
        'redis': {'addr': '127.0.0.1:6379', 'password': '', 'db': 15},
        'agora': {'app_id': APP_ID, 'channel_name': '', 'access_token': '', 'egress_uid': '42', 'rtc_timeout': 15},
        'snapshots': {'output_dir': str(case / 'snapshots'), 'width': 1280, 'height': 720,
                      'layout': layout, 'interval_in_ms': 20000, 'quality': 90},
        'recording': {'output_dir': str(case / 'recordings'), 'layout': layout, 'format': 'mp4',
                      'max_duration_seconds': 120,
                      'video': {'enabled': True, 'width': 1280, 'height': 720, 'fps': 30,
                                'bitrate': 2000000, 'codec': 'libx264', 'buffer_size': 30},
                      'audio': {'enabled': True, 'sample_rate': 48000, 'channels': 2,
                                'bitrate': 128000, 'codec': 'aac', 'buffer_size': 100}},
        'log': {'level': 'info', 'path': str(case / 'native'), 'max_size': 25, 'max_backups': 1, 'max_age': 1}}
    write_json(case / 'egress_config.json', egress_config)
    process = start([str(PROJECT / 'bin/egress'), '--config', str(case / 'egress_config.json')], case / 'egress.log')
    result = {'case': name, 'users': users, 'decode_mode': decode_mode, 'layout': layout,
              'publisher_count': len(publishers), 'status': 'FAIL'}
    log('RUN ' + name + ' (' + str(len(publishers)) + ' active publisher(s))')
    try:
        wait_health('http://127.0.0.1:18192/health', process)
        time.sleep(1)
        for publisher in publishers:
            if publisher.poll() is not None:
                raise RuntimeError('Publisher stopped before recording')
        response = request(API_URL + '/tasks', {
            'request_id': 'e2e' + str(time.time_ns()), 'cmd': 'record', 'action': 'start',
            'payload': {'channel': CHANNEL, 'access_token': token(42), 'workerUid': 42,
                        'users': users, 'layout': layout, 'videoDecodeMode': decode_mode}})
        task_id = response.get('task_id')
        if not task_id or response.get('status') != 'enqueued':
            raise RuntimeError('Recording task was not enqueued: ' + str(response))
        result['task_id'] = task_id
        write_json(case / 'start_response.json', response)
        time.sleep(args.seconds)
        stopped = request(API_URL + '/tasks/' + task_id + '/stop', {'request_id': 'stop' + str(time.time_ns())})
        write_json(case / 'stop_response.json', stopped)
        deadline = time.monotonic() + 15
        terminal = None
        while time.monotonic() < deadline:
            terminal = request(API_URL + '/tasks/' + task_id + '/status',
                               {'request_id': 'status' + str(time.time_ns())})
            if terminal.get('state') in ['STOPPED', 'FAILED', 'TIMEOUT']:
                break
            time.sleep(0.25)
        write_json(case / 'task_status.json', terminal)
        if terminal.get('state') != 'STOPPED':
            raise RuntimeError('Recording did not stop successfully: ' + str(terminal))
        time.sleep(1)
        if process.poll() is not None:
            raise RuntimeError('Egress exited unexpectedly')
        for publisher in publishers:
            if publisher.poll() is not None:
                raise RuntimeError('Publisher stopped during recording')
        files = list((case / 'recordings').glob('*.mp4'))
        if len(files) != 1:
            raise RuntimeError('Expected one fresh MP4 output, found ' + str(len(files)))
        result['recording'] = str(files[0].relative_to(ROOT))
        metrics = check_recording(files[0], case, len(publishers) > 1)
        result.update({'status': 'PASS', 'recording': str(files[0].relative_to(ROOT)), 'metrics': metrics})
        log('PASS ' + name + ': video=' + str(metrics['video_duration']) + 's audio=' + str(metrics['audio_duration']) +
            's, tones=' + str(metrics['audio_tone_amplitudes']))
    except Exception as error:
        result['error'] = clean(str(error))
        if (case / 'metrics.json').exists():
            result['metrics'] = json.loads((case / 'metrics.json').read_text())
        log('FAIL ' + name + ': ' + result['error'])
    finally:
        stop(process)
        RESULTS.append(result)
        write_json(case / 'result.json', result)
        write_json(ROOT / 'results.json', {'commit': COMMIT, 'channel': CHANNEL,
                                         'record_seconds': args.seconds, 'results': RESULTS})


def main():
    global APP_ID, API_URL, COMMIT, REDIS_OWNED
    log('Artifacts: ' + str(ROOT))
    COMMIT = run(['git', '-C', str(PROJECT), 'rev-parse', 'HEAD']).decode().strip()
    current = run(['atem', 'config', 'show']).decode()
    selected = re.search(r'^Current project:.*\(([a-fA-F0-9]{32})\)', current, re.M)
    if not selected:
        raise RuntimeError('atem has no selected Agora project')
    APP_ID = selected.group(1)
    API_URL = 'http://127.0.0.1:18091/egress/v1/' + APP_ID
    for port in [18091, 18191, 18192]:
        with socket.socket() as test_socket:
            test_socket.bind(('127.0.0.1', port))
    for entry in Path('/proc').iterdir():
        try:
            if (entry.name.isdigit() and entry.stat().st_uid == os.getuid()
                    and (entry / 'comm').read_text().strip() in ['eg_worker', 'egress']
                    and (entry / 'stat').read_text().rsplit(')', 1)[1].split()[0] != 'Z'):
                raise RuntimeError('An existing native egress service is running; its worker sockets must be preserved')
        except (OSError, ValueError):
            pass
    token(1001)
    run([str(STREAM), '--help'], env=STREAM_ENV)
    if run(['redis-cli', '-n', '15', 'DBSIZE']).strip() != b'0':
        raise RuntimeError('Redis database 15 is not empty; leave its contents untouched')
    owner = run(['redis-cli', '-n', '15', 'SET', 'rtc-egress-e2e:owner', CHANNEL, 'NX', 'EX', '1800']).strip()
    if owner != b'OK':
        raise RuntimeError('Redis test database could not be reserved')
    REDIS_OWNED = True
    source_1 = fixture(1001, 'red', 440)
    source_2 = fixture(1002, 'blue', 880)
    api_config = {'redis': {'addr': '127.0.0.1:6379', 'password': '', 'db': 15},
                  'server': {'port': 18091, 'health_port': 18191, 'region': '', 'task_ttl': 600}}
    write_json(ROOT / 'api_config.json', api_config)
    api = start([str(PROJECT / 'bin/api-server'), '--config', str(ROOT / 'api_config.json')], ROOT / 'logs/api.log')
    wait_health('http://127.0.0.1:18191/health', api)
    publisher_1 = start([str(STREAM), '--app-id', APP_ID, '--channel', CHANNEL, '--rtc-user-id', '1001',
                         '--token', token(1001), '--mode', 'encoded', str(source_1)], ROOT / 'logs/publisher_1001.log', env=STREAM_ENV)
    wait_ready(publisher_1)
    log('Single publisher connected; starting single-user cases')
    single_cases = [('single_flat_auto', ['1001'], -1, 'flat'),
                    ('single_flat_passthrough', ['1001'], 0, 'flat'),
                    ('single_flat_ffmpeg', ['1001'], 1, 'flat'),
                    ('single_flat_sdk', ['1001'], 2, 'flat'),
                    ('single_spotlight_auto', ['1001'], -1, 'spotlight'),
                    ('single_spotlight_ffmpeg', ['1001'], 1, 'spotlight')]
    for name, users, mode, layout in single_cases:
        if not args.filter or args.filter in name:
            test_case(name, users, mode, layout, [publisher_1])
    publisher_2 = start([str(STREAM), '--app-id', APP_ID, '--channel', CHANNEL, '--rtc-user-id', '1002',
                         '--token', token(1002), '--mode', 'encoded', str(source_2)], ROOT / 'logs/publisher_1002.log', env=STREAM_ENV)
    wait_ready(publisher_2)
    log('Two publishers connected; starting multi-user cases')
    multi_cases = [('multi_flat_auto', ['1001', '1002'], -1, 'flat'),
                   ('multi_flat_ffmpeg', ['1001', '1002'], 1, 'flat'),
                   ('multi_flat_passthrough_fallback', ['1001', '1002'], 0, 'flat'),
                   ('multi_flat_sdk', ['1001', '1002'], 2, 'flat'),
                   ('multi_spotlight_auto', ['1001', '1002'], -1, 'spotlight'),
                   ('multi_spotlight_ffmpeg', ['1001', '1002'], 1, 'spotlight'),
                   ('all_flat_auto', [], -1, 'flat'), ('all_flat_ffmpeg', [], 1, 'flat'),
                   ('all_spotlight_auto', [], -1, 'spotlight'), ('all_spotlight_ffmpeg', [], 1, 'spotlight')]
    for name, users, mode, layout in multi_cases:
        if not args.filter or args.filter in name:
            test_case(name, users, mode, layout, [publisher_1, publisher_2])
    return report_results(COMMIT)


def verify_saved_run():
    original = json.loads((ROOT / 'results.json').read_text())
    args.seconds = original.get('record_seconds', args.seconds)
    backup = ROOT / 'results_before_verification.json'
    if not backup.exists():
        write_json(backup, original)
    log('Rechecking saved live recordings: ' + str(ROOT))
    for result in original['results']:
        case = ROOT / result['case']
        result['status'] = 'FAIL'
        result.pop('error', None)
        try:
            terminal = json.loads((case / 'task_status.json').read_text())
            if terminal.get('state') != 'STOPPED':
                raise RuntimeError('Recording did not stop successfully: ' + str(terminal))
            files = list((case / 'recordings').glob('*.mp4'))
            if len(files) != 1:
                raise RuntimeError('Expected one fresh MP4 output, found ' + str(len(files)))
            result['recording'] = str(files[0].relative_to(ROOT))
            result['metrics'] = check_recording(files[0], case, result['publisher_count'] > 1)
            result['status'] = 'PASS'
            result['recording'] = str(files[0].relative_to(ROOT))
            log('PASS ' + result['case'])
        except Exception as error:
            result['error'] = clean(str(error))
            if (case / 'metrics.json').exists():
                result['metrics'] = json.loads((case / 'metrics.json').read_text())
            log('FAIL ' + result['case'] + ': ' + result['error'])
        RESULTS.append(result)
        write_json(case / 'result.json', result)
    original['results'] = RESULTS
    write_json(ROOT / 'results.json', original)
    return report_results(original['commit'])


def interrupted(signum, frame):
    raise KeyboardInterrupt()


signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGINT, interrupted)
exit_code = 1
try:
    exit_code = verify_saved_run() if args.verify_run else main()
except BaseException as error:
    log('RUN ERROR: ' + type(error).__name__ + ': ' + str(error))
finally:
    for process in reversed(PROCESSES):
        stop(process)
    if REDIS_OWNED and run(['redis-cli', '-n', '15', 'GET', 'rtc-egress-e2e:owner']).decode().strip() == CHANNEL:
        keys = run(['redis-cli', '-n', '15', '--scan', '--pattern', 'egress:*']).decode().splitlines()
        if keys:
            run(['redis-cli', '-n', '15', 'DEL'] + keys)
        run(['redis-cli', '-n', '15', 'DEL', 'rtc-egress-e2e:owner'])
        log('Removed test-owned Redis keys')
    for path in ROOT.rglob('*'):
        if path.is_file() and path.suffix in ['.json', '.log', '.txt']:
            content = path.read_bytes()
            cleaned = content
            for value in SECRETS:
                cleaned = cleaned.replace(value.encode(), b'[REDACTED]')
            if cleaned != content:
                path.write_bytes(cleaned)
    log('Test-owned services stopped')
sys.exit(exit_code)
