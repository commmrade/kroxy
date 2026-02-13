#!/bin/bash

# !!!: It is assumed, that kroxy is running with a config supplied with this test

python server.py &
SERVER_PID=$!

sleep 1
python client.py

kill $SERVER_PID
