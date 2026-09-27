"""Serve local WASM contract artifacts with the isolation required by pthreads."""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class IsolatedHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory")
    parser.add_argument("--port", type=int, default=8765)
    arguments = parser.parse_args()
    with ThreadingHTTPServer(
        ("127.0.0.1", arguments.port),
        partial(IsolatedHandler, directory=arguments.directory),
    ) as server:
        server.serve_forever()
