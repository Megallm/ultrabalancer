#!/bin/bash

# Simple backend servers for testing
for port in 8001 8002 8003; do
    python3 -m http.server $port &
done

echo "Started test backend servers on ports 8001, 8002, 8003"
echo "PIDs: $(jobs -p)"

# Start load balancer
./bin/ultrabalancer -p 8080 -b 127.0.0.1:8001 -b 127.0.0.1:8002 -b 127.0.0.1:8003 &

echo "Load balancer started on port 8080"

# Wait for servers to start
sleep 2

# Run tests
echo "Running basic connectivity test..."
curl -s http://localhost:8080/ > /dev/null && echo "✓ Basic connectivity" || echo "✗ Failed"

echo "Running load distribution test..."
for i in {1..10}; do
    curl -s http://localhost:8080/ > /dev/null
done
echo "✓ Load distribution test complete"

echo "Running concurrent connection test..."
ab -n 1000 -c 10 http://localhost:8080/ 2>/dev/null | grep "Requests per second"

echo "Checking stats endpoint..."
curl -s http://localhost:8080/stats > /dev/null && echo "✓ Stats endpoint" || echo "✗ Stats failed"

# Cleanup
echo "Press Ctrl+C to stop all servers"
wait