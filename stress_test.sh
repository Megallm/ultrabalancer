#!/bin/bash

cleanup() {
    killall -9 python3 ultrabalancer 2>/dev/null || true
}

trap cleanup EXIT

echo "Starting backends..."
python3 -m http.server 9001 >/dev/null 2>&1 &
B1=$!
python3 -m http.server 9002 >/dev/null 2>&1 &
B2=$!
python3 -m http.server 9003 >/dev/null 2>&1 &
B3=$!

sleep 2

echo "Starting load balancer..."
./bin/ultrabalancer -p 8080 -b 127.0.0.1:9001 -b 127.0.0.1:9002 -b 127.0.0.1:9003 >/tmp/lb_stress.log 2>&1 &
LB=$!

sleep 3

echo "Running concurrent requests..."
for i in {1..50}; do
    curl -s http://localhost:8080/ >/dev/null &
done

wait

echo "Completed 50 concurrent requests"
echo ""
echo "Checking for errors in logs:"
grep -i "error\|failed" /tmp/lb_stress.log | tail -10 || echo "No errors found"

echo ""
echo "Connection stats:"
grep -i "sent\|read" /tmp/lb_stress.log | tail -20

kill $LB $B1 $B2 $B3 2>/dev/null || true
wait 2>/dev/null || true
echo ""
echo "Test complete"
