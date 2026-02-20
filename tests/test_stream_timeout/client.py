import socket
import unittest

PROXY_ADDR = "127.0.0.1"
PROXY_PORT = 8080


class TestForwardingRequest(unittest.TestCase):
    def test_timeout_client(self):
        """
        Tests that the proxy closes the connection (Reset/FIN)
        if the client connects but sends no data.
        """
        sock = socket.create_connection((PROXY_ADDR, PROXY_PORT))
        # Set a timeout slightly longer than your proxy's configured timeout
        sock.settimeout(5.0)

        try:
            # We expect recv to either return 0 bytes (FIN) or
            # raise a ConnectionResetError (RST)
            data = sock.recv(4096)

            # If recv returns b'', it means the server closed the connection gracefully (FIN)
            self.assertEqual(
                data,
                b"",
                "Server should have closed the connection, but sent data instead.",
            )

        except (ConnectionResetError, BrokenPipeError):
            # This is a success case for an abrupt socket reset
            pass
        except socket.timeout:
            self.fail(
                "The proxy hung and didn't close the connection within the expected timeout."
            )
        finally:
            sock.close()


if __name__ == "__main__":
    unittest.main()
