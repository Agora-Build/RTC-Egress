"""Local WHIP fault injector for end-to-end tests; no firewall privileges needed."""

import http.client
import http.server
import re
import select
import socket
import struct
import threading


class WhipUdpProxy:
    def __init__(self):
        self.drop_outgoing = threading.Event()
        self.drop_incoming = threading.Event()
        self.stopping = threading.Event()
        self.forge_responses = False
        self.replay_responses = False
        self.error = None
        self.stats = {'requests': 0, 'responses': 0, 'forged': 0, 'replayed': 0,
                      'dropped_outgoing': 0, 'dropped_incoming': 0}
        self.front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.front.bind(('127.0.0.1', 18291))
        self.front.setblocking(False)
        self.peers = {}
        self.clients = {}
        self.responses = {}

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def forward(self):
                body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
                headers = {key: value for key, value in self.headers.items()
                           if key.lower() not in ('host', 'connection', 'content-length')}
                upstream = http.client.HTTPConnection('127.0.0.1', 18289, timeout=5)
                try:
                    upstream.request(self.command, self.path, body=body, headers=headers)
                    response = upstream.getresponse()
                    data = response.read()
                    if response.getheader('Content-Type', '').startswith('application/sdp'):
                        data = re.sub(rb'(a=candidate:[^\r\n]+ UDP [^\r\n]+ 127\.0\.0\.1 )18290( )',
                                      rb'\g<1>18291\2', data, flags=re.IGNORECASE)
                    self.send_response(response.status)
                    for key, value in response.getheaders():
                        if key.lower() in ('content-length', 'connection', 'transfer-encoding'):
                            continue
                        if key.lower() == 'location':
                            value = value.replace('127.0.0.1:18289', '127.0.0.1:18288')
                        self.send_header(key, value)
                    self.send_header('Content-Length', str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                except OSError:
                    self.send_error(502, 'Local test receiver unavailable')
                finally:
                    upstream.close()

            do_POST = forward
            do_DELETE = forward

        self.http = http.server.ThreadingHTTPServer(('127.0.0.1', 18288), Handler)
        self.http.daemon_threads = True
        self.http_thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.udp_thread = threading.Thread(target=self.relay, daemon=True)
        self.http_thread.start()
        self.udp_thread.start()

    def relay(self):
        try:
            while not self.stopping.is_set():
                readable, _, _ = select.select([self.front, *self.clients], [], [], 0.05)
                for source in readable:
                    data, address = source.recvfrom(65535)
                    if source is self.front:
                        if address not in self.peers:
                            upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                            upstream.connect(('127.0.0.1', 18290))
                            upstream.setblocking(False)
                            self.peers[address] = upstream
                            self.clients[upstream] = address
                        binding = len(data) >= 20 and data[:2] == b'\x00\x01'
                        if binding:
                            self.stats['requests'] += 1
                        if binding and self.drop_incoming.is_set() and self.forge_responses:
                            # Matching transaction ID with an invalid MESSAGE-INTEGRITY.
                            forged = struct.pack('!HHI', 0x0101, 24, 0x2112a442) + data[8:20]
                            forged += struct.pack('!HH', 8, 20) + bytes(20)
                            self.front.sendto(forged, address)
                            self.stats['forged'] += 1
                        if binding and self.drop_incoming.is_set() and self.replay_responses:
                            if address in self.responses:
                                self.front.sendto(self.responses[address], address)
                                self.stats['replayed'] += 1
                        if self.drop_outgoing.is_set():
                            self.stats['dropped_outgoing'] += 1
                        else:
                            self.peers[address].send(data)
                    else:
                        client = self.clients[source]
                        if len(data) >= 20 and data[:2] == b'\x01\x01':
                            self.stats['responses'] += 1
                            if not self.drop_incoming.is_set():
                                self.responses[client] = data
                        if self.drop_incoming.is_set():
                            self.stats['dropped_incoming'] += 1
                        else:
                            self.front.sendto(data, client)
        except OSError as error:
            if not self.stopping.is_set():
                self.error = str(error)

    def close(self):
        self.stopping.set()
        self.udp_thread.join(timeout=2)
        self.http.shutdown()
        self.http.server_close()
        self.http_thread.join(timeout=2)
        self.front.close()
        for peer in self.peers.values():
            peer.close()
