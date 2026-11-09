"""Runtime regression prepared for later execution on Linux; no third-party modules."""
import http.client
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

pipeline, fixture_generator = sys.argv[1:3]


def require(value, message):
    if not value:
        raise RuntimeError(message)


with tempfile.TemporaryDirectory() as directory:
    fixture = str(pathlib.Path(directory) / "frame.jpg")
    subprocess.run([fixture_generator, fixture], check=True)
    for view in ("raw", "cpu"):
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        args = [pipeline, "--jpeg-file", fixture, "--width", "4", "--height", "4",
                "--frames", "0", "--process", "cpu", "--preview", view,
                "--port", str(port)]
        with tempfile.TemporaryFile() as log:
            process = subprocess.Popen(args, stdout=log, stderr=log)
            peers = []
            try:
                deadline = time.monotonic() + 10
                while True:
                    require(process.poll() is None, "pipeline exited during preview startup")
                    try:
                        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                        connection.request("GET", "/")
                        page = connection.getresponse()
                        require(page.status == 200 and b"stream.mjpg" in page.read(), "watch page")
                        connection.close()
                        break
                    except (ConnectionRefusedError, ConnectionResetError):
                        if time.monotonic() > deadline:
                            raise
                        time.sleep(0.05)
                for _ in range(2):
                    stream = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
                    stream.request("GET", "/stream.mjpg")
                    response = stream.getresponse()
                    require(response.status == 200, "stream status")
                    require("boundary=frame" in response.getheader("Content-Type"), "multipart header")
                    require(response.readline().strip() == b"--frame", "frame boundary")
                    headers = {}
                    while True:
                        line = response.readline().strip()
                        if not line:
                            break
                        key, value = line.split(b":", 1)
                        headers[key.lower()] = value.strip()
                    length = int(headers[b"content-length"])
                    jpeg = response.read(length)
                    require(jpeg[:2] == b"\xff\xd8" and jpeg[-2:] == b"\xff\xd9", "valid JPEG envelope")
                    stream.close()
                # Leave a streaming peer unread and an incomplete HTTP request open at stop.
                slow = socket.create_connection(("127.0.0.1", port), timeout=2)
                slow.sendall(b"GET /stream.mjpg HTTP/1.1\r\nHost: localhost\r\n\r\n")
                peers.append(slow)
                partial = socket.create_connection(("127.0.0.1", port), timeout=2)
                partial.sendall(b"GET /")
                peers.append(partial)
                occupied = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
                require(occupied.returncode != 0 and b"preview bind" in occupied.stderr, "occupied port diagnostic")
                process.terminate()
                require(process.wait(timeout=5) == 0, "bounded shutdown with connected clients")
            except Exception:
                log.seek(0)
                print(log.read().decode(errors="replace"), file=sys.stderr)
                raise
            finally:
                for peer in peers:
                    peer.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()
