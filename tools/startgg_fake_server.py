#!/usr/bin/env python3
"""A fake start.gg for the TOURNAMENT page, on this computer.

Answers every POST with a saved start.gg answer (tests/fixtures/startgg/
event.json by default), so the game's start.gg requests (libcurl or
WinHTTP, the headers, the pages) can be tried without an account and
without spending start.gg's rate limit. A request without an
Authorization header is refused as start.gg refuses it; --rate-limit N
answers every Nth request with start.gg's rate limit. Each request is
printed (its page and whether a token came, never the token).

In recharged.ini:
    [startgg]
    event = tournament/test/event/singles
    user = 11
    dev_token = anything
    api = http://127.0.0.1:8765/gql

Usage: startgg_fake_server.py [--port 8765] [--answer FILE] [--rate-limit N]
"""

import argparse
import http.server
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RATE_LIMIT = b'{"success":false,"message":"Rate limit exceeded - api-token"}'
NO_TOKEN = b'{"success":false,"message":"Invalid authentication token"}'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--answer", type=Path, default=ROOT / "tests/fixtures/startgg/event.json")
    parser.add_argument("--rate-limit", type=int, default=0, metavar="N")
    args = parser.parse_args()
    count = 0

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            nonlocal count
            count += 1
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            try:
                variables = json.loads(body).get("variables", {})
            except ValueError:
                variables = {}
            token = self.headers.get("Authorization", "").startswith("Bearer ")
            print(f"#{count} slug={variables.get('slug')!r} page={variables.get('page')} token={'yes' if token else 'no'}", flush=True)
            if not token:
                self.answer(401, NO_TOKEN)
            elif args.rate_limit and count % args.rate_limit == 0:
                self.answer(429, RATE_LIMIT)
            else:
                self.answer(200, args.answer.read_bytes())

        def answer(self, status, data):
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *_):
            pass

    server = http.server.HTTPServer(("127.0.0.1", args.port), Handler)
    print(f"fake start.gg on http://127.0.0.1:{args.port}/gql answering {args.answer}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
