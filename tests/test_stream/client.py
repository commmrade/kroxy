import http.client
import unittest

PROXY_ADDR = "127.0.0.1"
PROXY_PORT = 8080


class TestForwardingRequest(unittest.TestCase):
    def test_send_request(self):
        client = http.client.HTTPConnection(PROXY_ADDR, PROXY_PORT)
        client.request("GET", "/")
        resp = client.getresponse()
        self.assertEqual(resp.read().decode(), "this is a test")


if __name__ == "__main__":
    unittest.main()
