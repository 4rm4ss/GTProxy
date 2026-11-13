#!/usr/bin/env python3
"""End-to-end test for GTProxy on a single machine.

Starts a fake Growtopia HTTPS endpoint (server_data.php) on 127.0.0.2:443, runs
the real proxy against it, checks the server_data.php rewrite like the game
client would see it, then runs the ENet harness (fake client + fake game
servers) and finally checks the proxy log.

Modes:
  ip        (default) config.json points the proxy at 127.0.0.2 directly.
  hostname  config.json uses www.growtopia1.com, as in real use. The proxy
            resolves it over DNS-over-HTTPS: cloudflare-dns.com and dns.google
            are pointed at the fake server through /etc/hosts (restored
            afterwards). The fake Cloudflare endpoint fails so the Google
            fallback is exercised, TLS is verified against a test CA passed via
            SSL_CERT_FILE, and the SNI the proxy sends is recorded.

Needs root (port 443, /etc/hosts in hostname mode) and Linux (127.0.0.2
loopback plus SO_REUSEPORT so the fake endpoint can share port 443 with the
proxy).

Usage: run_e2e.py <build dir> [ip|hostname]
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
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UPSTREAM_IP = "127.0.0.2"
GAME_HOST = "www.growtopia1.com"
FALLBACK_HOST = "www.growtopia2.com"
DOH_HOSTS = ("cloudflare-dns.com", "dns.google")
GAME_SERVER_PORT = 17091
HOSTS_MARKER = "# gtproxy-e2e"

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
doh_requests = []
sni_names = []


def check(ok, what):
    print(f"[{'PASS' if ok else 'FAIL'}] {what}", flush=True)
    if not ok:
        failures.append(what)


class FakeGrowtopia(BaseHTTPRequestHandler):
    def _reply(self, status, payload, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        # DNS-over-HTTPS (JSON API)
        url = urlparse(self.path)
        doh_requests.append({"host": self.headers.get("Host"), "path": url.path, "query": url.query})
        if url.path == "/dns-query":
            self._reply(503, b"unavailable", "text/plain")  # "Cloudflare" is down
            return
        if url.path == "/resolve":
            name = parse_qs(url.query).get("name", [""])[0]
            answer = {
                "Status": 0,
                "Answer": [
                    {"name": name + ".", "type": 5, "TTL": 60, "data": "gt-edge.example."},
                    {"name": "gt-edge.example.", "type": 1, "TTL": 60, "data": UPSTREAM_IP},
                ],
            }
            self._reply(200, json.dumps(answer).encode(), "application/dns-json")
            return
        self._reply(404, b"not found", "text/plain")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode(errors="replace")
        upstream_requests.append({"path": self.path, "headers": dict(self.headers), "body": body})
        if self.headers.get("Host") == GAME_HOST:
            # growtopia1 is "down": the proxy must fall back to growtopia2.
            self._reply(503, b"Service Unavailable", "text/plain")
            return
        self._reply(200, FAKE_SERVER_DATA.encode(), "text/html")

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
    context.sni_callback = lambda sock, name, ctx: sni_names.append(name)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def make_test_pki(workdir):
    """Test CA plus a server certificate for the game and DoH host names."""
    ca_key, ca_pem = os.path.join(workdir, "ca.key"), os.path.join(workdir, "ca.pem")
    key, csr, pem = (os.path.join(workdir, n) for n in ("upstream.key", "upstream.csr", "upstream.pem"))
    ext = os.path.join(workdir, "upstream.ext")
    run = lambda *args: subprocess.run(args, check=True, capture_output=True)
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
        "-keyout", ca_key, "-out", ca_pem, "-subj", "/CN=GTProxy e2e test CA",
        "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=keyCertSign,cRLSign")
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", csr, "-subj", "/CN=e2e")
    with open(ext, "w") as f:
        names = ",".join(f"DNS:{h}" for h in (GAME_HOST, FALLBACK_HOST, *DOH_HOSTS))
        f.write(f"subjectAltName={names}\nextendedKeyUsage=serverAuth\n")
    run("openssl", "x509", "-req", "-in", csr, "-CA", ca_pem, "-CAkey", ca_key, "-CAcreateserial",
        "-days", "2", "-out", pem, "-extfile", ext)
    return ca_pem, pem, key


def patch_hosts():
    with open("/etc/hosts") as f:
        original = f.read()
    lines = "".join(f"{UPSTREAM_IP} {h} {HOSTS_MARKER}\n" for h in DOH_HOSTS)
    with open("/etc/hosts", "w") as f:
        f.write(original.rstrip("\n") + "\n" + lines)
    return original


def restore_hosts(original):
    with open("/etc/hosts", "w") as f:
        f.write(original)


def client_server_data(cert):
    # Talk to the proxy exactly like the game client: POST form data over HTTPS.
    # Trust the proxy's own certificate; the client reaches it as 127.0.0.1.
    context = ssl.create_default_context(cafile=cert)
    context.check_hostname = False
    conn = http.client.HTTPSConnection("127.0.0.1", 443, context=context, timeout=30)
    conn.request(
        "POST",
        "/growtopia/server_data.php",
        body="version=5.61&platform=0&protocol=225",
        headers={
            "User-Agent": "UbiServices_SDK_2022.Release.9_PC64_ansi_static",
            "Content-Type": "application/x-www-form-urlencoded",
            "Host": GAME_HOST,
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
    mode = sys.argv[2] if len(sys.argv) > 2 else "ip"
    if mode not in ("ip", "hostname"):
        print(__doc__)
        return 2
    print(f"=== e2e mode: {mode} ===")

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

    # The proxy creates its own CA on first start; the "game" trusts it.
    proxy_ca = os.path.join(workdir, "resources", "ca.pem")
    items_dat = os.path.join(workdir, "resources", "items.dat")
    env = dict(os.environ)

    ca_pem, upstream_cert, upstream_key = make_test_pki(workdir)
    if mode == "hostname":
        env["SSL_CERT_FILE"] = ca_pem
        address = GAME_HOST
    else:
        address = UPSTREAM_IP

    with open(os.path.join(workdir, "config.json"), "w") as f:
        json.dump({"server": {"port": 16999, "address": address}}, f)

    original_hosts = patch_hosts() if mode == "hostname" else None
    upstream = start_fake_upstream(upstream_cert, upstream_key)
    log_path = os.path.join(workdir, "e2e-stdout.log")
    log_file = open(log_path, "w")
    wrapper = os.environ.get("E2E_PROXY_WRAPPER", "").split()
    proxy = subprocess.Popen(wrapper + [proxy_bin], cwd=workdir, stdout=log_file, stderr=subprocess.STDOUT, env=env)

    try:
        deadline = time.time() + 60
        while time.time() < deadline:
            with open(log_path) as f:
                if "Core initialized successfully" in f.read():
                    break
            time.sleep(0.1)
        check(proxy.poll() is None, "proxy starts and stays up")

        # --- server_data.php through the proxy -------------------------------
        check(os.path.exists(proxy_ca), "proxy generated its local CA (resources/ca.pem)")
        status, body = client_server_data(proxy_ca)
        fields = parse_text(body)
        check(status == 200, f"server_data.php via proxy returns 200 (got {status})")
        check(fields.get("server") == "127.0.0.1", "server rewritten to 127.0.0.1")
        check(fields.get("port") == "16999", "port rewritten to proxy port 16999")
        check(fields.get("type2") == "1", "type2 set to 1")
        check(fields.get("meta") == "E2E-META-VALUE", "meta passed through unchanged")
        check(fields.get("loginurl") == "login.growtopiagame.com", "loginurl passed through unchanged")
        check(fields.get("beta_port") == "56999", "other fields passed through unchanged")
        check(body.rstrip().endswith("RTENDMARKERBS1001"), "end marker kept")

        expected_requests = 2 if mode == "hostname" else 1
        check(
            len(upstream_requests) == expected_requests,
            f"proxy queried server_data.php {len(upstream_requests)}x (expected {expected_requests})",
        )
        if upstream_requests:
            req = upstream_requests[-1]
            params = parse_qs(req["body"])
            check(req["path"] == "/growtopia/server_data.php", "upstream path is /growtopia/server_data.php")
            check(
                req["body"] == "version=5.61&platform=0&protocol=225",
                f"client form body forwarded byte for byte ({req['body']})",
            )
            check(
                req["headers"].get("Content-Type") == "application/x-www-form-urlencoded",
                "Content-Type forwarded",
            )
            check(
                req["headers"].get("User-Agent") == "UbiServices_SDK_2022.Release.9_PC64_ansi_static",
                "client User-Agent forwarded upstream",
            )
            if mode == "hostname":
                check(upstream_requests[0]["headers"].get("Host") == GAME_HOST, f"tried {GAME_HOST} first")
                check(req["headers"].get("Host") == FALLBACK_HOST, f"fell back to {FALLBACK_HOST}")

        if mode == "hostname":
            check(GAME_HOST in sni_names and FALLBACK_HOST in sni_names, f"TLS SNI carries the game host names ({sni_names})")
            check(any(r["path"] == "/dns-query" for r in doh_requests), "Cloudflare DoH tried first")
            check(any(r["path"] == "/resolve" for r in doh_requests), "fell back to Google DoH")
            check("dns.google" in sni_names, "DoH over verified TLS (SNI dns.google)")

        # --- ENet session -----------------------------------------------------
        harness_args = [harness_bin, items_dat] + (["mismatch"] if mode == "hostname" else [])
        result = subprocess.run(harness_args, capture_output=True, text=True, timeout=300)
        print(result.stdout, end="")
        if result.stderr.strip():
            print(result.stderr)
        check(result.returncode == 0, "ENet harness scenario")

        time.sleep(0.5)
        check(proxy.poll() is None, "proxy still running after the whole session")
    finally:
        proxy.terminate()
        try:
            proxy.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proxy.kill()
        upstream.shutdown()
        log_file.close()
        if original_hosts is not None:
            restore_hosts(original_hosts)

    with open(log_path) as f:
        log = f.read()

    check("Successfully parsed items.dat (version 23, count 21700)" in log, "proxy parsed items.dat sent by the server")
    if mode == "hostname":
        check("asking the client to download it from the server" in log, "proxy forced an items.dat download on hash mismatch")
        check("Got server_data.php from www.growtopia2.com" in log, "proxy log shows growtopia2 fallback")
    else:
        check("Hash matches, loading cached items.dat" in log, "proxy matched the logon items.dat hash (new name suffix)")
    check("reconnecting to last server" in log, "proxy log shows last-server reconnect")
    check("Exception" not in log and "Segmentation" not in log, "no exceptions in proxy log")
    # Reports from the third-party ENet fork (lib/enet submodule) are listed but
    # not counted: signed shift in the range coder and an unaligned header read,
    # both harmless on x86-64 and ARM64.
    sanitizer = [l for l in log.splitlines() if "ERROR: AddressSanitizer" in l or "runtime error:" in l]
    third_party = [l for l in sanitizer if "/lib/enet/" in l]
    for line in third_party:
        print("    (third-party)", line.split("GTProxy/")[-1])
    check(len(sanitizer) == len(third_party), f"no sanitizer reports in GTProxy code ({len(sanitizer) - len(third_party)})")
    world_warnings = log.count("Failed to fully parse world data")
    check(world_warnings >= 1, f"malformed worlds reported as warnings ({world_warnings})")
    check(
        log.find("Failed to fully parse world data") > log.find("E2ETESTWORLD") > 0,
        "valid world parsed without warnings",
    )
    check("Connecting to Growtopia server at 127.0.0.3:17092" in log, "proxy connected to the real sub-server address")
    if mode == "hostname":
        check("via fallback dns.google" in log, "proxy log shows DoH fallback")

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
