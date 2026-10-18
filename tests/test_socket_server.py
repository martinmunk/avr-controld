"""Integration tests for the Unix-socket server (real socket, fake backend)."""

import socket
import threading
import time

from avrcontrold.socket_server import SocketServer
from fakes import FakeBackend


def _client(path, line, timeout=3):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(path)
    s.sendall((line + "\n").encode())
    data = b""
    while b"\n" not in data:
        chunk = s.recv(256)
        if not chunk:
            break
        data += chunk
    s.close()
    return data.decode().strip()


def _wait_socket(path, timeout=3):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            s.close()
            return True
        except OSError:
            time.sleep(0.02)
    return False


def make_server(tmp_path, backend=None, ceiling=100, **kw):
    be = backend or FakeBackend(volume=40)
    path = str(tmp_path / "test.sock")
    srv = SocketServer(path, be, threading.Lock(), ceiling, **kw)
    assert srv.start_listening()
    srv.start()
    assert _wait_socket(path)
    return be, path, srv


def test_ping_and_caps(tmp_path):
    be, path, srv = make_server(tmp_path)
    try:
        assert _client(path, "PING") == "OK"
        assert _client(path, "CAPS").startswith("OK abs=1")
    finally:
        srv.stop()


def test_get_set_over_socket(tmp_path):
    be, path, srv = make_server(tmp_path)
    try:
        assert _client(path, "GET volume") == "OK 40"
        assert _client(path, "SET volume 55") == "OK"
        assert _client(path, "GET volume") == "OK 55"
    finally:
        srv.stop()


def test_ceiling_enforced_over_socket(tmp_path):
    be = FakeBackend(volume=10)
    be, path, srv = make_server(tmp_path, backend=be, ceiling=30)
    try:
        assert _client(path, "SET volume 100") == "OK"
        assert _client(path, "GET volume") == "OK 30"
    finally:
        srv.stop()


def test_shutdown_invokes_callback(tmp_path):
    called = threading.Event()
    be, path, srv = make_server(tmp_path, on_shutdown=called.set)
    try:
        assert _client(path, "SHUTDOWN") == "OK"
        assert called.wait(2)
    finally:
        srv.stop()


def test_multiple_commands_one_connection(tmp_path):
    be, path, srv = make_server(tmp_path)
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect(path)
        s.sendall(b"PING\nGET volume\nVOLUP\n")
        time.sleep(0.2)
        data = b""
        s.setblocking(False)
        try:
            while True:
                chunk = s.recv(256)
                if not chunk:
                    break
                data += chunk
        except BlockingIOError:
            pass
        s.close()
        lines = data.decode().split("\n")
        assert lines[0] == "OK"
        assert lines[1] == "OK 40"
        assert lines[2] == "OK"
    finally:
        srv.stop()
