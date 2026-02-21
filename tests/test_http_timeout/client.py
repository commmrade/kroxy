import http.client
import socket
import unittest

PROXY_ADDR = "127.0.0.1"
PROXY_PORT = 8080


class TestForwardingRequest(unittest.TestCase):
    def test_timeout_service(self):
        """Tests when the upstream service takes too long to respond (504)."""
        client = http.client.HTTPConnection(PROXY_ADDR, PROXY_PORT)
        client.request("GET", "/")
        resp = client.getresponse()
        self.assertEqual(resp.getcode(), 504)

    def test_timeout_client(self):
        """
        Tests when the client connects but sends nothing.
        The server should return a 408 Request Timeout.
        """
        # Using a raw socket to ensure we don't send any HTTP headers automatically
        sock = socket.create_connection((PROXY_ADDR, PROXY_PORT))

        # We wrap it in an HTTP response reader after a delay
        # or simply wait for the server to push the 408 response.
        sock.settimeout(10.0)  # Local safety timeout

        try:
            # Read the response sent by the server after it triggers its internal timeout
            response_data = sock.recv(4096).decode("utf-8")

            # Verify the status code 408 is in the status line
            self.assertIn("408", response_data)
        except socket.timeout:
            self.fail(
                "The server did not send a 408 response within the expected time."
            )
        finally:
            sock.close()


if __name__ == "__main__":
    unittest.main()
