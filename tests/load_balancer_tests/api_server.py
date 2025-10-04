#!/usr/bin/env python3
import http.server
import socketserver
import json
import sys
import signal
import time

class Handler(http.server.BaseHTTPRequestHandler):
    count = 0

    def log_message(self, format, *args):
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError):
            pass

    def do_GET(self):
        Handler.count += 1
        port = self.server.server_address[1]

        response = None
        if self.path == '/api/users':
            response = {
                'users': [
                    {'id': 1, 'name': 'Alice', 'age': 25},
                    {'id': 2, 'name': 'Bob', 'age': 30},
                    {'id': 3, 'name': 'Charlie', 'age': 35}
                ],
                'backend': f'server-{port}',
                'request_number': Handler.count
            }
        elif self.path == '/api/products':
            response = {
                'products': [
                    {'id': 1, 'name': 'Laptop', 'price': 999},
                    {'id': 2, 'name': 'Phone', 'price': 699},
                    {'id': 3, 'name': 'Tablet', 'price': 499}
                ],
                'backend': f'server-{port}',
                'request_number': Handler.count
            }
        elif self.path == '/api/status':
            response = {
                'status': 'ok',
                'backend': f'server-{port}',
                'requests_served': Handler.count
            }
        else:
            response = {
                'message': 'API Server',
                'backend': f'server-{port}',
                'request_number': Handler.count,
                'endpoints': ['/api/users', '/api/products', '/api/status']
            }

        try:
            response_data = json.dumps(response).encode()
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.send_header('Content-Length', len(response_data))
            self.send_header('Connection', 'keep-alive')
            self.end_headers()
            self.wfile.write(response_data)
        except BrokenPipeError:
            pass
        except Exception:
            pass

if __name__ == "__main__":
    port = int(sys.argv[1])

    def signal_handler(sig, frame):
        print(f"\nShutting down server on port {port}")
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    socketserver.TCPServer.allow_reuse_address = True

    with socketserver.ThreadingTCPServer(("127.0.0.1", port), Handler) as httpd:
        httpd.socket.setsockopt(socketserver.socket.SOL_SOCKET, socketserver.socket.SO_KEEPALIVE, 1)
        httpd.request_queue_size = 128
        print(f"Server running on port {port}")
        httpd.serve_forever()
