#!/bin/bash

cleanup() {
    killall -9 python3 ultrabalancer 2>/dev/null || true
    sleep 1
}

trap cleanup EXIT

echo "Starting backends..."
python3 -m http.server 9001 >/dev/null 2>&1 &
python3 -m http.server 9002 >/dev/null 2>&1 &

sleep 2

echo "Starting load balancer..."
./bin/ultrabalancer -p 8080 -b 127.0.0.1:9001 -b 127.0.0.1:9002 >/tmp/simple_lb.log 2>&1 &
LB_PID=$!

sleep 3

echo "Test 1: Single request"
RESULT1=$(curl -s -w "\n%{http_code}" http://localhost:8080/ 2>&1)
CODE1=$(echo "$RESULT1" | tail -1)
if [ "$CODE1" = "200" ]; then
    echo "✓ Request succeeded (HTTP $CODE1)"
else
    echo "✗ Request failed (HTTP $CODE1)"
fi

sleep 1

echo ""
echo "Test 2: Sequential requests"
SUCCESS=0
for i in {1..10}; do
    CODE=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:8080/ 2>&1)
    if [ "$CODE" = "200" ]; then
        ((SUCCESS++))
    fi
done
echo "✓ $SUCCESS/10 requests succeeded"

sleep 1

echo ""
echo "Test 3: Some concurrent requests (5)"
SUCCESS=0
for i in {1..5}; do
    (curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/ > /tmp/curl_$i.out 2>&1 && echo "200" > /tmp/curl_$i.status || echo "FAIL" > /tmp/curl_$i.status) &
done
wait

for i in {1..5}; do
    if [ -f /tmp/curl_$i.status ]; then
        STATUS=$(cat /tmp/curl_$i.status)
        if [ "$STATUS" = "200" ]; then
            ((SUCCESS++))
        fi
    fi
done
echo "✓ $SUCCESS/5 concurrent requests succeeded"

echo ""
echo "Load balancer log summary:"
echo "Active connections at end:"
tail -50 /tmp/simple_lb.log | grep "Active Connections" | tail -1

kill $LB_PID 2>/dev/null || true
wait $LB_PID 2>/dev/null || true

cleanup
echo ""
echo "Basic tests complete"
