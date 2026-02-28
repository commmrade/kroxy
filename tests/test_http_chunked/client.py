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

        chunks = [
            "this ",
            "is ",
            "a ",
            "chunked ",
            "response",
            "sdfkljajladjshkljklaSJKLDDKSHJKSADJSAHJSKADSJAHKLSAJHKLdsadhgaghasdhgkjsaghjkadghjkdgahkjsdgkahsjdghsjadgshjag",
            "dsajdajsajksajhksahjksahjk",
            "filler" * 400,
        ]
        full_msg = ""
        for chunk in chunks:
            full_msg += chunk

        self.assertEqual(data, full_msg)


if __name__ == "__main__":
    unittest.main()
