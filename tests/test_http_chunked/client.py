import unittest

import requests

PROXY_ADDR = "127.0.0.1"
PROXY_PORT = 8080
BASE_URL = f"http://{PROXY_ADDR}:{PROXY_PORT}"


class TestForwardingRequest(unittest.TestCase):
    def test_send_request(self):
        # stream=True ensures we read chunked response incrementally
        resp = requests.get(BASE_URL + "/", stream=True)
        data = "".join(chunk.decode() for chunk in resp.iter_content(chunk_size=None))
        self.assertEqual(data, "this is a chunked response")


if __name__ == "__main__":
    unittest.main()
