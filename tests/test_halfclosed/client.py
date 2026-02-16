# client.py
import unittest
import socket

PROXY_ADDR = "127.0.0.1"
PROXY_PORT = 8080
TARGET = "http://127.0.0.1:9090"

class TestProxyHalfClosedSupport(unittest.TestCase):
    def _connect_to_proxy(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((PROXY_ADDR, PROXY_PORT))
        return s

    def _recv_all(self, s, timeout=5):
        s.settimeout(timeout)
        response = b""
        try:
            while True:
                data = s.recv(4096)
                if not data:
                    break
                response += data
        except socket.timeout:
            pass
        return response

    def test_get_no_body_half_close_from_client(self):
        """Test half-close after a complete no-body request (GET).
        Server delays response. If proxy improperly full-closes the backend connection
        on client half-close, server cannot send response → test times out/fails."""
        s = self._connect_to_proxy()
        request = f"GET {TARGET}/ HTTP/1.1\r\nHost: 127.0.0.1:9090\r\nConnection: close\r\n\r\n".encode()
        s.sendall(request)
        s.shutdown(socket.SHUT_WR)  # Half-close immediately
        response = self._recv_all(s)
        s.close()

        self.assertGreater(len(response), 0, "No response received - proxy likely full-closed backend connection")
        self.assertIn(b"200 OK", response)
        self.assertIn(b"response", response)

    def test_chunked_post_incomplete_half_close_from_client(self):
        """Test half-close after incomplete chunked body (no final 0 chunk).
        If proxy does not treat half-close as input end and hangs waiting for more chunks,
        no response is returned → test times out/fails."""
        s = self._connect_to_proxy()
        headers = f"POST {TARGET}/ HTTP/1.1\r\nHost: 127.0.0.1:9090\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n".encode()
        chunk = b"5\r\nhello\r\n"  # One chunk, then half-close (incomplete)

        s.sendall(headers)
        s.sendall(chunk)
        s.shutdown(socket.SHUT_WR)  # Half-close without final 0 chunk
        response = self._recv_all(s)
        s.close()

        self.assertGreater(len(response), 0, "No response received - proxy likely hung waiting for more chunks")
        self.assertIn(b"HTTP/1.1", response)
        # Server aborts with 400 on incomplete chunked input
        self.assertIn(b"400", response)

    def test_normal_forwarding(self):
        """Basic sanity check that normal requests work through the proxy."""
        import requests
        proxies = {"http": f"http://{PROXY_ADDR}:{PROXY_PORT}", "https": f"http://{PROXY_ADDR}:{PROXY_PORT}"}
        resp = requests.get(f"{TARGET}/", proxies=proxies, timeout=5)
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.text, "response")

if __name__ == "__main__":
    unittest.main(verbosity=2)
