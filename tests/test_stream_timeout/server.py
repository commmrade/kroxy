#!/usr/bin/env python3

import socket
import time

HOST = "127.0.0.1"
PORT = 9090


def main():
    # Create a TCP/IP socket
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    # Bind the socket to the address and port
    server.bind((HOST, PORT))
    server.listen(5)
    print(f"[GhostServer] Listening on {HOST}:{PORT}")
    print("[GhostServer] I will accept connections and then do absolutely nothing.")

    while True:
        # Accept the incoming connection
        conn, addr = server.accept()
        print(
            f"[GhostServer] Connected by {addr}. Ignoring request to trigger timeout..."
        )

        try:
            # We wrap this in a loop to keep the socket open without sending anything.
            # If we don't read the data, the client's buffers might fill,
            # but for a simple timeout test, just sitting idle is enough.
            while True:
                data = conn.recv(1024)
                if not data:
                    break
                # We received data (the HTTP request), but we are NOT sending a response.
                print(f"[GhostServer] Received request data, staying silent...")

                # Sleep indefinitely to hold the connection open
                time.sleep(3600)
        except Exception as e:
            print(f"[GhostServer] Connection closed or errored: {e}")
        finally:
            conn.close()


if __name__ == "__main__":
    main()
