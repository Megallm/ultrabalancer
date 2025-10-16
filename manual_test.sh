#!/bin/bash

cleanup() {
    killall -9 python3 ultrabalancer 2>/dev/null || true
}

trap cleanup EXIT

python3 -m http.server 9001 >/dev/null 2>&1 &
BACKEND1=$!
python3 -m http.server 9002 >/dev/null 2>&1 &
BACKEND2=$!

sleep 2

./bin/ultrabalancer -p 8080 -b 127.0.0.1:9001 -b 127.0.0.1:9002 >/tmp/lb_test.log 2>&1 &
LB=$!

sleep 3

echo "Test 1:"
curl -s http://localhost:8080/ | head -10

echo ""
echo "Test 2:"
curl -s http://localhost:8080/ | head -10

echo ""
echo "Test 3:"
curl -s http://localhost:8080/ | head -10

echo ""
echo "Load balancer logs:"
tail -20 /tmp/lb_test.log

kill $LB $BACKEND1 $BACKEND2 2>/dev/null || true
