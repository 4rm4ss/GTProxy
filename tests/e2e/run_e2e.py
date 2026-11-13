#!/usr/bin/env python3
"""End-to-end test for GTProxy on a single machine.

Starts a fake Growtopia HTTPS endpoint (server_data.php) on 127.0.0.2:443, runs
the real proxy against it, checks the server_data.php rewrite like the game
client would see it, then runs the ENet harness (fake client + fake game
servers) and finally checks the proxy log.

Needs permission to bind port 443 (root/admin) and Linux (127.0.0.2 loopback
plus SO_REUSEPORT so the fake endpoint can share port 443 with the proxy).

Usage: run_e2e.py <build dir>
"""
import http.client
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UPSTREAM_IP = "127.0.0.2"
GAME_SERVER_PORT = 17091

FAKE_SERVER_DATA = (
    "server|127.0.0.1\n"
    f"port|{GAME_SERVER_PORT}\n"
    "loginurl|login.growtopiagame.com\n"
    "type|1\n"
    "#maint|Server is under maintenance.\n"
    "meta|E2E-META-VALUE\n"
    "beta_server|grow-main-test.growtopiagame.com\n"
    "beta_port|56999\n"
    "type2|1\n"
    "RTENDMARKERBS1001"
)

failures = []
upstream_requests = []


def check(ok, what):
    print(f"[{'PASS' if ok else 'FAIL'}] {what}", flush=True)
    if not ok:
        failures.append(what)


class FakeGrowtopia(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode(errors="replace")
        upstream_requests.append({"path": self.path, "headers": dict(self.headers), "body": body})
        payload = FAKE_SERVER_DATA.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


class ReusePortHTTPServer(HTTPServer):
    def server_bind(self):
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        super().server_bind()


def start_fake_upstream(cert, key):
    server = ReusePortHTTPServer((UPSTREAM_IP, 443), FakeGrowtopia)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def client_server_data(cert):
    # Talk to the proxy exactly like the game client: POST form data over HTTPS.
    # Trust the proxy's own certificate; it is not issued for 127.0.0.1.
    context = ssl.create_default_context(cafile=cert)
    context.check_hostname = False
    conn = http.client.HTTPSConnection("127.0.0.1", 443, context=context, timeout=20)
    body = "version=5.61&platform=0&protocol=225"
    conn.request(
        "POST",
        "/growtopia/server_data.php",
        body=body,
        headers={
            "User-Agent": "UbiServices_SDK_2022.Release.9_PC64_ansi_static",
            "Content-Type": "application/x-www-form-urlencoded",
            "Host": "www.growtopia1.com",
        },
    )
    response = conn.getresponse()
    return response.status, response.read().decode(errors="replace")


def parse_text(text):
    out = {}
    for line in text.splitlines():
        if "|" in line:
            key, value = line.split("|", 1)
            out.setdefault(key, value)
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    build = os.path.abspath(sys.argv[1])
    proxy_bin = os.path.join(build, "src", "Proxy")
    harness_bin = os.path.join(build, "tests", "GTProxy_e2e")
    for path in (proxy_bin, harness_bin):
        if not os.path.exists(path):
            print(f"missing {path} (configure with -DGTPROXY_BUILD_E2E=ON)")
            return 2

    workdir = tempfile.mkdtemp(prefix="gtproxy-e2e-")
    shutil.copytree(os.path.join(ROOT, "resources"), os.path.join(workdir, "resources"))
    os.makedirs(os.path.join(workdir, "scripts"))
    shutil.copy(os.path.join(ROOT, "scripts", "rainbow.lua"), os.path.join(workdir, "scripts"))
    with open(os.path.join(workdir, "config.json"), "w") as f:
        json.dump({"server": {"port": 16999, "address": UPSTREAM_IP}}, f)

    cert = os.path.join(workdir, "resources", "cert.pem")
    key = os.path.join(workdir, "resources", "key.pem")
    items_dat = os.path.join(workdir, "resources", "items.dat")

    upstream = start_fake_upstream(cert, key)
    log_path = os.path.join(workdir, "e2e-stdout.log")
    log_file = open(log_path, "w")
    proxy = subprocess.Popen(os.environ.get("E2E_PROXY_WRAPPER","").split() + [proxy_bin], cwd=workdir, stdout=log_file, stderr=subprocess.STDOUT)

    try:
        deadline = time.time() + 15
        while time.time() < deadline:
            with open(log_path) as f:
                if "Core initialized successfully" in f.read():
                    break
            time.sleep(0.1)
        check(proxy.poll() is None, "proxy starts and stays up")

        # --- server_data.php through the proxy -------------------------------
        status, body = client_server_data(cert)
        fields = parse_text(body)
        check(status == 200, f"server_data.php via proxy returns 200 (got {status})")
        check(fields.get("server") == "127.0.0.1", "server rewritten to 127.0.0.1")
        check(fields.get("port") == "16999", "port rewritten to proxy port 16999")
        check(fields.get("type2") == "1", "type2 set to 1")
        check(fields.get("meta") == "E2E-META-VALUE", "meta passed through unchanged")
        check(fields.get("loginurl") == "login.growtopiagame.com", "loginurl passed through unchanged")
        check(fields.get("beta_port") == "56999", "other fields passed through unchanged")
        check(body.rstrip().endswith("RTENDMARKERBS1001"), "end marker kept")

        check(len(upstream_requests) == 1, "proxy queried the game's server_data.php once")
        if upstream_requests:
            req = upstream_requests[0]
            params = parse_qs(req["body"])
            check(req["path"] == "/growtopia/server_data.php", "upstream path is /growtopia/server_data.php")
            check(
                params.get("version") == ["5.61"] and params.get("protocol") == ["225"] and params.get("platform") == ["0"],
                f"client form fields forwarded upstream ({req['body']})",
            )
            check(
                req["headers"].get("User-Agent") == "UbiServices_SDK_2022.Release.9_PC64_ansi_static",
                "client User-Agent forwarded upstream",
            )

        # --- ENet session -----------------------------------------------------
        result = subprocess.run([harness_bin, items_dat], capture_output=True, text=True, timeout=180)
        print(result.stdout, end="")
        if result.stderr.strip():
            print(result.stderr)
        check(result.returncode == 0, "ENet harness scenario")

        time.sleep(0.5)
        check(proxy.poll() is None, "proxy still running after the whole session")
    finally:
        proxy.terminate()
        try:
            proxy.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proxy.kill()
        upstream.shutdown()
        log_file.close()

    with open(log_path) as f:
        log = f.read()

    check("Successfully parsed items.dat (version 23, count 21700)" in log, "proxy parsed items.dat sent by the server")
    check("Hash matches, loading cached items.dat" in log, "proxy matched the logon items.dat hash (new name suffix)")
    check("Exception" not in log and "Segmentation" not in log, "no exceptions in proxy log")
    world_warnings = log.count("Failed to fully parse world data")
    check(world_warnings >= 1, f"malformed worlds reported as warnings ({world_warnings})")
    first_world = log.split("E2ETESTWORLD", 1)[-1] if "E2ETESTWORLD" in log else ""
    check(
        "Failed to fully parse world data" not in first_world.split("[ClientBound] Decoding packet", 2)[0]
        if first_world else False,
        "valid world parsed without warnings",
    )
    check("Connecting to Growtopia server at 127.0.0.3:17092" in log, "proxy connected to the real sub-server address")

    errors = [line for line in log.splitlines() if "[error]" in line]
    print(f"\nproxy log: {log_path} ({len(errors)} error lines)")
    for line in errors[:20]:
        print("   ", line)

    print(f"\n{len(failures)} failure(s)")
    for failure in failures:
        print("  -", failure)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
