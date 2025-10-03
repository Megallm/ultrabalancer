import socket
import threading
import time
import requests
import sys

def test_tcp_connection(host='localhost', port=8080):
    """Test basic TCP connectivity"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect((host, port))
        s.close()
        return True
    except:
        return False

def test_http_request(url='http://localhost:8080'):
    """Test HTTP request/response"""
    try:
        r = requests.get(url, timeout=5)
        return r.status_code == 200
    except:
        return False

def test_load_distribution(url='http://localhost:8080', requests_count=100):
    """Test load distribution across backends"""
    backends = {}

    for _ in range(requests_count):
        try:
            r = requests.get(url)
            backend = r.headers.get('X-Server-Id', 'unknown')
            backends[backend] = backends.get(backend, 0) + 1
        except:
            pass

    return backends

def test_concurrent_connections(url='http://localhost:8080', threads=10, requests_per_thread=10):
    """Test concurrent connections"""
    results = {'success': 0, 'failed': 0}
    lock = threading.Lock()

    def worker():
        for _ in range(requests_per_thread):
            try:
                r = requests.get(url, timeout=5)
                with lock:
                    if r.status_code == 200:
                        results['success'] += 1
                    else:
                        results['failed'] += 1
            except:
                with lock:
                    results['failed'] += 1

    threads_list = []
    start = time.time()

    for _ in range(threads):
        t = threading.Thread(target=worker)
        t.start()
        threads_list.append(t)

    for t in threads_list:
        t.join()

    duration = time.time() - start
    total = results['success'] + results['failed']

    return {
        'total': total,
        'success': results['success'],
        'failed': results['failed'],
        'duration': duration,
        'rps': total / duration if duration > 0 else 0
    }

def test_health_endpoint(url='http://localhost:8080/health'):
    """Test health check endpoint"""
    try:
        r = requests.get(url, timeout=5)
        return r.status_code in [200, 204]
    except:
        return False

def test_stats_endpoint(url='http://localhost:8080/stats'):
    """Test statistics endpoint"""
    try:
        r = requests.get(url, timeout=5)
        return r.status_code == 200 and len(r.content) > 0
    except:
        return False

def test_backend_failure_handling():
    """Test backend failure handling"""
    # This would require ability to stop/start backend servers
    # For now, just check if load balancer continues working with some backends down
    pass

def main():
    print("UltraBalancer Integration Tests")
    print("=" * 40)

    # Basic connectivity
    print("\n1. TCP Connectivity Test")
    if test_tcp_connection():
        print("   ✓ TCP connection successful")
    else:
        print("   ✗ TCP connection failed")
        sys.exit(1)

    # HTTP request
    print("\n2. HTTP Request Test")
    if test_http_request():
        print("   ✓ HTTP request successful")
    else:
        print("   ✗ HTTP request failed")

    # Load distribution
    print("\n3. Load Distribution Test")
    distribution = test_load_distribution(requests_count=30)
    print(f"   Requests distribution: {distribution}")
    if len(distribution) > 1:
        print("   Load distributed across backends")
    else:
        print("   Load not distributed")

    # Concurrent connections
    print("\n4. Concurrent Connections Test")
    results = test_concurrent_connections(threads=5, requests_per_thread=10)
    print(f"   Total: {results['total']}, Success: {results['success']}, Failed: {results['failed']}")
    print(f"   Duration: {results['duration']:.2f}s, RPS: {results['rps']:.2f}")
    if results['success'] > results['failed']:
        print("   ✓ Concurrent connections handled")
    else:
        print("   ✗ Too many failed connections")

    # Health endpoint
    print("\n5. Health Endpoint Test")
    if test_health_endpoint():
        print("   ✓ Health endpoint responding")
    else:
        print("   ⚠ Health endpoint not available")

    # Stats endpoint
    print("\n6. Statistics Endpoint Test")
    if test_stats_endpoint():
        print("   ✓ Stats endpoint responding")
    else:
        print("   ⚠ Stats endpoint not available")

    print("\n" + "=" * 40)
    print("Tests completed")

if __name__ == "__main__":
    main()