# server.py
import time
from flask import Flask, request, abort

app = Flask(__name__)

@app.route("/", methods=["GET", "POST"])
def index():
    if request.method == "GET":
        time.sleep(1)  # Simulate delayed processing to test if proxy allows server to respond after client half-closes
        return "response"
    else:
        # Force reading the full request body (will fail on incomplete/chunked input)
        try:
            _ = request.data
        except Exception:
            abort(400)
        return "full post response"

if __name__ == "__main__":
    app.run(host="127.0.0.1", port=9090)
