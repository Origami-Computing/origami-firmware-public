"""Synthetic SD media through the real queue to a staging backend, with network faults.

Requires DEVICE_UPLOAD_TEST_KEY (never logged), --host LAN IP, --upstream staging
or a loopback staging fixture, an empty device queue, and development firmware.
Retains test files/entries for inspection; preserves API key and Wi-Fi, restoring
prior backend settings. Normal backend-switch token invalidation still applies. --isolated-account substitutes only the proxy upstream credential,
with exact generated-file and device-IP admission; it does not test direct device
authentication against staging. Failure never clears unrelated work. Logs/recovery stay private.
"""
import ssl, certifi
import argparse, hashlib, http.client, json, os, re, struct, threading, time, urllib.request, urllib.parse, urllib.error, uuid, wave, io, zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from email import policy
from email.parser import BytesParser
from morning_alarm_device_check import Device
from upload_fixture_serial import put_fixture, FixtureTransferError


def is_fixture_upload(body, content_type, fixtures):
    """Only exact host-generated media may cross the staging fault proxy."""
    try:
        message = BytesParser(policy=policy.default).parsebytes(
            b'Content-Type: ' + content_type.encode('ascii') + b'\r\n\r\n' + body)
        if message.get_content_type() != 'multipart/form-data' or message.defects:
            return False
        if (message.preamble or '').strip() or (message.epilogue or '').strip():
            return False
        parts = list(message.iter_parts())
        if any(p.defects or p.is_multipart() for p in parts): return False
        media = [p for p in parts if p.get_filename() is not None]
        fields = {}
        for part in parts:
            name = part.get_param('name', header='content-disposition')
            if part.get_filename() is not None:
                if name != 'media': return False
                continue
            if name in fields: return False
            fields[name] = part.get_payload(decode=True)
        return (len(media) == 1 and fields in (
                    {'platform': b'device', 'sessionId': b'origami-device'},
                    {'platform': b'device', 'sessionId': b'origami-device', 'agentMode': b'timeline_only'}) and
                media[0].get_filename() in fixtures and
                media[0].get_payload(decode=True) == fixtures[media[0].get_filename()])
    except (ValueError, UnicodeError):
        return False


def run(args):
    upstream = urllib.parse.urlsplit(args.upstream)
    assert upstream.hostname in ('kami2-staging.up.railway.app', '127.0.0.1', 'localhost'), 'Staging only'
    key = os.environ['DEVICE_UPLOAD_TEST_KEY']
    tls = ssl.create_default_context(cafile=certifi.where())
    args.output.mkdir(parents=True, exist_ok=True); args.output.chmod(0o700)
    state = {'mode': 'normal', 'calls': [], 'held': threading.Event(), 'release': threading.Event(), 'fixtures': {}, 'device_ip': None}
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_POST(self):
            if self.path != '/api/chat': self.send_error(404); return
            if self.client_address[0] != state['device_ip']: self.send_error(403); return
            if not args.isolated_account and self.headers.get('X-Api-Key') != key: self.send_error(401); return
            n = int(self.headers.get('Content-Length', 0))
            if not 0 < n < 2 * 1024 * 1024: self.send_error(413); return
            self.connection.settimeout(20)
            body = self.rfile.read(n)
            if len(body) != n: return
            if not is_fixture_upload(body, self.headers.get('Content-Type', ''), state['fixtures']):
                self.send_error(403); return
            call = {'bytes': n, 'sha256': hashlib.sha256(body).hexdigest()}
            state['calls'].append(call); attempt = len(state['calls'])
            if state['mode'] == '503' and attempt == 1:
                call.update(status=503, upstream_done=True)
                self.send_response(503); self.send_header('Content-Length', '0'); self.end_headers(); return
            conn_cls = http.client.HTTPSConnection if upstream.scheme == 'https' else http.client.HTTPConnection
            conn = conn_cls(upstream.hostname, upstream.port, timeout=40, **({'context': tls} if upstream.scheme == 'https' else {}))
            try:
                conn.request('POST', '/api/chat', body=body, headers={
                    'X-Api-Key': key, 'Content-Type': self.headers['Content-Type'], 'Content-Length': str(n)})
                response = conn.getresponse(); payload = response.read(); call['status'] = response.status
                call['upstream_request_id'] = response.getheader('x-railway-request-id')
                if response.status == 200: call['receipt'] = json.loads(payload)
                elif response.status == 400:
                    try:
                        message = json.loads(payload).get('error')
                        if message in ('Unsupported chat platform', 'One nonempty device media file required'):
                            call['response_error'] = message
                    except (ValueError, AttributeError): pass
            except (OSError, http.client.HTTPException) as error:
                call.update(status=502, upstream_error=type(error).__name__)
                try:
                    self.send_response(502); self.send_header('Content-Length', '0'); self.end_headers()
                except (BrokenPipeError, ConnectionResetError): pass
                return
            finally:
                conn.close(); call['upstream_done'] = True
            if attempt == 1 and state['mode'] in ('lost_response', 'camera', 'photos', 'videos', 'reboot'):
                if state['mode'] != 'lost_response':
                    state['held'].set(); state['release'].wait(50)
                self.close_connection = True; return
            if state['mode'] == 'batch':
                state['release'].wait(30) # Hold first acknowledgment until backlog is established.
            if state['mode'] == 'fragmented':
                for byte in f'HTTP/1.1 {response.status} OK\r\n'.encode():
                    self.connection.sendall(bytes([byte])); time.sleep(.015)
                self.connection.sendall(f'Content-Length: {len(payload)}\r\nConnection: close\r\n\r\n'.encode() + payload)
                self.close_connection = True
            else:
                self.send_response(response.status); self.send_header('Content-Length', str(len(payload))); self.end_headers()
                try: self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError): pass
    server = ThreadingHTTPServer(('0.0.0.0', args.fixture_port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    d = Device(args.port, args.output); original = None; saved_dev_url = None; report = {'passed': False, 'restored': False, 'cases': []}
    def timeline():
        # Observation traffic can lose a response independently of a committed
        # upload. Retry only transient I/O/5xx, retain every failure in evidence,
        # and keep semantic assertions (missing/duplicate entries) strict.
        for attempt in range(3):
            req = urllib.request.Request('https://kami2-staging.up.railway.app/api/entries?types=image,audio&limit=100', headers={'X-Api-Key': key})
            try:
                with urllib.request.urlopen(req, timeout=20, context=tls) as response:
                    return json.load(response)
            except urllib.error.HTTPError as error:
                if error.code < 500: raise
                reason = 'HTTP ' + str(error.code)
            except (OSError, http.client.HTTPException) as error:
                reason = type(error).__name__
            report.setdefault('timeline_read_errors', []).append(reason)
            print('RETRY timeline observation '+reason, flush=True)
            if attempt == 2: raise AssertionError('Timeline unavailable after three bounded reads')
            d.read(1)
    def command(cmd, wait=.6): return d.command(cmd, wait)
    def backend():
        for _ in range(10):
            rows = re.findall(r'\[backend-dev\] url=(\S+) dev=([01])', command('backend status', 1))
            if rows: return {'url': rows[-1][0], 'dev': int(rows[-1][1])}
        raise AssertionError('No backend observation')
    def queue():
        for _ in range(10):
            t = command('queue status', .5); m = re.search(r'QUEUE STATUS count=(\d+) pending=(\d+) worker=(\d+)', t)
            if m: return tuple(map(int, m.groups()))
        raise AssertionError('No queue observation')
    def set_url(url):
        reply = command('backend set ' + url, 2)
        assert '[backend-dev] set ok=1' in reply, 'Backend setter rejected persistence'
        assert backend()['url'] == url
    serial_ready = True
    def put(path, data):
        nonlocal serial_ready
        state['fixtures'][Path(path).name] = data
        serial_ready = False
        try:
            put_fixture(d, path, data)
            serial_ready = True
        except FixtureTransferError as error:
            serial_ready = error.serial_ready
            raise
    try:
        d.read(30); command('home', 2)
        assert queue()[0] == 0, 'Requires empty queue; nothing changed'
        original = backend()
        if not args.isolated_account:
            assert urllib.parse.urlsplit(original['url']).hostname == 'kami2-staging.up.railway.app', 'Device must already target staging'
        deadline = time.monotonic()+60
        match = None
        while not match and time.monotonic()<deadline:
            wifi = command('wifi status', 2)
            match = re.search(r'connected=1[^\n]*ip=(\d+\.\d+\.\d+\.\d+)', wifi)
        assert match, 'Device must be connected to the same LAN'
        assert queue()[0] == 0, 'Queue changed while waiting for Wi-Fi; nothing changed'
        state['device_ip'] = match[1]
        report['isolated_account_proxy'] = args.isolated_account
        (args.output/'original-backend.json').write_text(json.dumps(original)); (args.output/'original-backend.json').chmod(0o600)
        fixture_url = args.fixture_url or f'http://{args.host}:{args.fixture_port}'
        if args.fixture_url:
            target = urllib.parse.urlsplit(fixture_url)
            assert target.scheme == 'https' and target.hostname.endswith('.trycloudflare.com') and target.path in ('', '/'), 'Expected explicit HTTPS test tunnel'
        # setBackendUrl persists only in DEV mode. Capture its prior URL before
        # changing it; otherwise a reboot silently returns queued fixtures to prod.
        if not original['dev']:
            command('backend mode dev', 2)
        dev_state = backend()
        assert dev_state['dev'] == 1, 'Cannot persist isolated test backend'
        saved_dev_url = dev_state['url']
        (args.output/'original-backend.json').write_text(json.dumps({**original, 'saved_dev_url': saved_dev_url}))
        set_url(fixture_url)
        assert backend() == {'url': fixture_url, 'dev': 1}
        # A valid fixed JPEG is supplied by the host; synthetic PCM silence is valid WAV.
        image = args.jpeg.read_bytes(); assert image[:2] == b'\xff\xd8'
        wav = io.BytesIO()
        with wave.open(wav, 'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(b'\x00\x00'*16000)
        audio = wav.getvalue()
        cases = [(kind, mode) for mode in ('normal', '503', 'fragmented', 'lost_response', 'camera', 'reboot') for kind in ('image', 'audio')]
        if args.case: cases = [] if args.case == 'batch' else [(kind, args.case) for kind in ('image', 'audio')]
        for kind, mode in cases:
            assert queue()[0] == 0
            state['calls'] = []; state['mode'] = mode; state['held'].clear(); state['release'].clear()
            data = image if kind == 'image' else audio
            path = '/upload_queue/test-' + uuid.uuid4().hex[:8] + ('.jpg' if kind == 'image' else '.wav')
            put(path, data)
            started = time.monotonic()
            t = command(f'queue enqueue {kind} {path}', .5)
            assert 'QUEUE ENQUEUE OK' in t, t[-500:]
            if mode in ('camera', 'photos', 'videos', 'reboot'):
                deadline = time.monotonic() + 40
                while not state['held'].is_set() and time.monotonic() < deadline: t += d.read(.2)
                assert state['held'].is_set(), 'Backend did not commit before handoff'
                if mode == 'camera':
                    t += command('camera open', 6)
                    assert '[cam] Viewfinder started' in t, 'Camera did not acquire its normal preview session'
                    assert queue()[:2] == (1, 1), 'Interrupted item not retained'
                    assert queue()[2] == 0, 'Camera started without upload quiescence'
                    command('home', 2)
                elif mode in ('photos', 'videos'):
                    if mode == 'videos':
                        transition = command('videos open', .3)
                        marker = '[videos] active=1 ready=1'
                    else:
                        transition = command('menu', .4) + command('rot -24', .3)
                        for _ in range(12):
                            scene = command('scene', .4)
                            selected = re.search(r'id=list.row.(\d+).label[^\n]*text="([^"]*)" fg=0xFFFF bg=0x0000', scene)
                            rows = re.findall(r'id=list.row.(\d+).label[^\n]*text="([^"]*)"', scene)
                            target = next((r for r in rows if r[1].startswith('Photos')), None)
                            if selected and target:
                                delta = int(target[0])-int(selected[1])
                                if delta: transition += command('rot '+str(delta), .3)
                                transition += command('c', .3); break
                            transition += command('rot 3', .3)
                        else: raise AssertionError('Cannot select Photos')
                        marker = '[photos] render'
                    deadline = time.monotonic()+15
                    def view_ready():
                        return marker in transition or (mode == 'photos' and '[photos] catalog count=0 state=0 newest=-' in transition)
                    while not view_ready() and time.monotonic()<deadline:
                        transition += command('videos status', .2) if mode == 'videos' else d.read(.2)
                    assert view_ready(), 'Preview did not become ready'
                    if mode == 'photos' and marker not in transition:
                        report['photos_empty_catalog'] = True
                    assert queue() == (1, 1, 0), 'Preview did not quiesce and retain upload'
                    assert 'Voice pause timed out' not in transition
                    command('home', 2)
                else:
                    t += command('tutorial reboot' , 1); d.read(30); command('home', 2)
                    assert backend() == {'url': fixture_url, 'dev': 1}, 'Backend isolation lost after reboot'
                state['release'].set()
            deadline = time.monotonic() + 100
            while time.monotonic() < deadline:
                t += d.read(.3)
                if queue()[0] == 0: break
            else: raise AssertionError(f'Queue did not drain for {kind}/{mode}')
            # A timed-out device request can still be finishing upstream. Keep
            # its receipt/error observation before evaluating duplicate safety.
            observation_deadline = time.monotonic() + 45
            while any(not c.get('upstream_done') for c in state['calls']) and time.monotonic() < observation_deadline:
                d.read(.2)
            calls = list(state['calls'])
            assert all(c.get('upstream_done') for c in calls), 'Unfinished upstream observation'
            expected = 1 if mode in ('normal', 'fragmented') else 2
            assert expected <= len(calls) <= 4, (mode, calls) # Permit bounded additional real network retries.
            assert len({c['sha256'] for c in calls}) == 1, 'Retry request bytes changed'
            receipts = [c['receipt'] for c in calls if 'receipt' in c]
            assert receipts and all(r['uploaded'] and r['kind'] == kind for r in receipts)
            assert len({r['id'] for r in receipts}) == 1, 'Duplicate entries'
            if mode in ('lost_response', 'camera', 'photos', 'videos', 'reboot'): assert receipts[-1]['duplicate']
            # Check through the deployed timeline API, independent of the fixture's status line.
            feed = timeline()
            assert sum(r['id'] == receipts[-1]['id'] for r in feed) == 1
            report['cases'].append({'kind': kind, 'fault': mode, 'attempts': len(calls), 'receipt': receipts[-1]['id'],
                'timeline': True, 'extra_attempts': len(calls)-expected,
                'proxy_errors': [c['upstream_error'] for c in calls if 'upstream_error' in c], 'seconds': round(time.monotonic()-started, 2), 'path': path})
            print('PASS', kind, mode, 'attempts='+str(len(calls)), flush=True)
        if not args.case or args.case == 'batch':
            assert queue()[0] == 0
            state['calls'] = []; state['mode'] = 'batch'; state['release'].clear()
            paths = []
            # Stage all files first, then enqueue rapidly enough to establish a
            # real mixed backlog rather than four independent empty-queue runs.
            for kind in ('image', 'audio', 'image', 'audio'):
                path = '/upload_queue/test-' + uuid.uuid4().hex[:8] + ('.jpg' if kind == 'image' else '.wav')
                put(path, image if kind == 'image' else audio); paths.append((kind, path))
            started = time.monotonic()
            for kind, path in paths:
                response = command(f'queue enqueue {kind} {path}', .2)
                deadline = time.monotonic() + 10
                while 'QUEUE ENQUEUE OK' not in response and time.monotonic() < deadline:
                    response += d.read(.1)
                assert 'QUEUE ENQUEUE OK' in response, 'No enqueue acknowledgment'
            peak = queue()[0]
            assert peak >= 2, 'Did not establish a queued backlog'
            state['release'].set()
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                d.read(.3); count = queue()[0]; peak = max(peak, count)
                if count == 0: break
            else: raise AssertionError('Mixed backlog did not drain')
            deadline = time.monotonic() + 45
            while any(not c.get('upstream_done') for c in state['calls']) and time.monotonic() < deadline: d.read(.2)
            calls = state['calls']; assert 4 <= len(calls) <= 12
            assert all(c.get('upstream_done') for c in calls)
            receipts = {c['receipt']['id']: c['receipt'] for c in calls if 'receipt' in c}
            assert len(receipts) == 4 and len({c['sha256'] for c in calls}) == 4
            assert sorted(r['kind'] for r in receipts.values()) == ['audio', 'audio', 'image', 'image']
            assert all(r['uploaded'] for r in receipts.values())
            feed = timeline()
            assert all(sum(r['id'] == receipt for r in feed) == 1 for receipt in receipts)
            report['cases'].append({'kind': 'mixed', 'fault': 'batch', 'items': 4, 'peak_queue': peak,
                'attempts': len(calls), 'timeline': True, 'receipts': list(receipts),
                'seconds': round(time.monotonic()-started, 2), 'paths': [path for kind, path in paths]})
            print('PASS mixed backlog items=4 peak='+str(peak)+' attempts='+str(len(calls)), flush=True)
        report['passed'] = True
    finally:
        state['release'].set()
        try:
            if original and not serial_ready:
                report['restoration_error'] = 'Binary SD receiver did not exit; no control commands sent'
            if original and serial_ready:
                command('home', 1)
                # Never silently send a failed fixture to a different backend.
                for _ in range(20):
                    status = command('queue status', .5)
                    snapshot = re.search(r'QUEUE STATUS count=(\d+) pending=(\d+) worker=(\d+)', status)
                    if not snapshot: continue
                    count, pending, worker = map(int, snapshot.groups())
                    if count == 0 and worker == 0: break
                    if count == 0: continue  # Final receipt may precede worker retirement.
                    paths = re.findall(r'QUEUE ITEM [^\n]*path=(\S+) error=', status)
                    assert all(Path(p).name in state['fixtures'] and p.startswith('/upload_queue/test-') for p in paths), 'Unrelated queue work present; refusing cleanup/backend switch'
                    if len(paths) != count: continue  # Queue changed during diagnostic snapshot.
                    assert 'QUEUE CLEAR OK' in command('queue clear', 6), 'Fixture queue did not quiesce'
                    report['cancelled_fixture_items'] = count
                else: raise AssertionError('Fixture queue did not quiesce; backend not restored')
                if saved_dev_url is not None:
                    set_url(saved_dev_url)
                command('backend mode '+('dev' if original['dev'] else 'prod'), 2)
                set_url(original['url']); report['restored'] = backend() == original
        finally:
            d.close(); server.shutdown(); server.server_close()
            report['last_case_calls'] = state['calls'] # Preserve statuses/receipt IDs even when an assertion fails.
            (args.output/'summary.json').write_text(json.dumps(report, indent=2)+'\n')

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--host',required=True);p.add_argument('--upstream',default='https://kami2-staging.up.railway.app')
    p.add_argument('--fixture-port',type=int,default=8798);p.add_argument('--jpeg',type=Path,required=True)
    p.add_argument('--fixture-url', help='Explicit HTTPS quick-tunnel URL forwarding to the local fault listener')
    p.add_argument('--isolated-account', action='store_true', help='Proxy only exact generated fixtures into a separate staging account; never changes or forwards the device account key')
    p.add_argument('--case',choices=['normal','503','fragmented','lost_response','camera','photos','videos','reboot','batch'])
    run(p.parse_args())
