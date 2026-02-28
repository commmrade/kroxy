import argparse

from flask import Flask, jsonify, request

app = Flask(__name__)


@app.route("/")
def index():
    return f"Hello from PID {os.getpid()} on port {app.config['PORT']}"


@app.route("/health")
def health():
    return jsonify({"status": "ok"})


@app.route("/echo", methods=["POST"])
def echo():
    print("%s port" % app.config["PORT"])
    return jsonify(
        {
            "method": request.method,
            "headers": dict(request.headers),
            "body": request.get_data(as_text=True),
        }
    )


if __name__ == "__main__":
    import os

    parser = argparse.ArgumentParser(description="Simple Flask server")
    parser.add_argument("--host", default="0.0.0.0", help="Bind host")
    parser.add_argument("--port", type=int, required=True, help="Port to listen on")
    parser.add_argument("--debug", action="store_true", help="Enable debug mode")

    args = parser.parse_args()

    app.config["PORT"] = args.port

    app.run(
        host=args.host,
        port=args.port,
        debug=args.debug,
        use_reloader=False,  # important when running multiple instances
    )
