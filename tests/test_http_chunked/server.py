import time

from flask import Flask, Response

app = Flask(__name__)


def generate_chunks():
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
    for chunk in chunks:
        yield chunk
        time.sleep(0.4)


@app.route("/")
def index():
    return Response(generate_chunks(), mimetype="text/plain")


if __name__ == "__main__":
    app.run(host="127.0.0.1", port=9090)
