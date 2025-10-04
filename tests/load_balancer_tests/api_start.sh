#!/bin/bash

echo ""
echo "Starting 5 API servers..."
echo ""

python3 api_server.py 3001 &
python3 api_server.py 3002 &
python3 api_server.py 3003 &
python3 api_server.py 3004 &
python3 api_server.py 3005 &

sleep 2

echo "All 5 servers running on ports 3001-3005"
echo ""
echo "Run this in another terminal:"
echo ""
echo "cd /home/shubham/Documents/ultra-balancer/core"
echo "./bin/ultrabalancer -p 8080 -a round-robin -b 127.0.0.1:3001 -b 127.0.0.1:3002 -b 127.0.0.1:3003 -b 127.0.0.1:3004 -b 127.0.0.1:3005"
echo ""

wait