from http.server import BaseHTTPRequestHandler, HTTPServer

hostName = "127.0.0.1"
serverPort = 9090


class MyServer(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-type", "text/plain")
        self.send_header("Content-Length", str(len("this is a test")))
        self.end_headers()
        self.wfile.write(bytes("this is a test", "utf-8"))


if __name__ == "__main__":
    webServer = HTTPServer((hostName, serverPort), MyServer)
    try:
        webServer.serve_forever()
    except KeyboardInterrupt:
        pass

    webServer.server_close()
