#!/usr/bin/env python3
import requests
import time
import random
from concurrent.futures import ThreadPoolExecutor
import threading

URL = "http://127.0.0.1:8080"
RATE_LIMIT = 100

endpoints = ['/api/users', '/api/products', '/api/status', '/']

count = 0
success = 0
failed = 0
lock = threading.Lock()
running = True

def send_request():
    global count, success, failed
    endpoint = random.choice(endpoints)
    try:
        response = requests.get(f"{URL}{endpoint}", timeout=2)
        with lock: 
            count += 1
            if response.status_code == 200:
                success += 1
    except:
        with lock:
            count += 1
            failed += 1

print(f"\nSending load to {URL}")
print(f"Rate: {RATE_LIMIT} req/s")
print(f"Endpoints: {endpoints}")
print(f"\nPress Ctrl+C to stop\n")

start = time.time()

with ThreadPoolExecutor(max_workers=20) as executor:
    try:
        while True:
            for _ in range(RATE_LIMIT // 10):
                executor.submit(send_request)
            
            time.sleep(0.1)
            
            elapsed = int(time.time() - start)
            rate = count // (elapsed + 1) if elapsed > 0 else count
            print(f"\rTime: {elapsed}s | Total: {count} | Success: {success} | Failed: {failed} | Rate: ~{rate} req/s   ", end='', flush=True)
    except KeyboardInterrupt:
        print("\n\nStopping...")
        running = False

time.sleep(1)

elapsed = time.time() - start
print(f"\nTotal Requests: {count}")
print(f"Successful: {success}")
print(f"Failed: {failed}")
print(f"Average: {int(count / elapsed)} req/s")
print(f"Duration: {int(elapsed)}s\n")
