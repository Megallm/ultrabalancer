# How to Setup UltraBalancer with Docker

## Basic Docker Setup

### Dockerfile
```dockerfile
FROM alpine:3.18

RUN apk update && apk add --no-cache \
    gcc \
    musl-dev \
    make \
    openssl-dev \
    pcre-dev \
    zlib-dev \
    brotli-dev \
    jemalloc-dev \
    libssl3 \
    ca-certificates

WORKDIR /app
COPY . .

RUN make clean && make all

EXPOSE 8080 8443 9090

CMD ["./bin/ultrabalancer", "-f", "/app/config/ultrabalancer.cfg"]
```

### Docker Compose Setup
```yaml
version: '3.8'

services:
  ultrabalancer:
    build: .
    ports:
      - "8080:8080"
      - "8443:8443"
      - "9090:9090"
    volumes:
      - ./config:/app/config:ro
      - ./logs:/app/logs
      - ./certs:/app/certs:ro
    environment:
      - UB_LOG_LEVEL=info
      - UB_WORKER_PROCESSES=auto
    restart: unless-stopped
    depends_on:
      - web1
      - web2
      - web3

  web1:
    image: nginx:alpine
    volumes:
      - ./web1:/usr/share/nginx/html
    environment:
      - SERVER_ID=web1
    expose:
      - "80"

  web2:
    image: nginx:alpine
    volumes:
      - ./web2:/usr/share/nginx/html
    environment:
      - SERVER_ID=web2
    expose:
      - "80"

  web3:
    image: nginx:alpine
    volumes:
      - ./web3:/usr/share/nginx/html
    environment:
      - SERVER_ID=web3
    expose:
      - "80"

networks:
  default:
    driver: bridge
```

## Production Docker Setup

### Multi-stage Dockerfile
```dockerfile
FROM alpine:3.18 AS builder

RUN apk add --no-cache \
    gcc \
    musl-dev \
    make \
    openssl-dev \
    pcre-dev \
    zlib-dev \
    brotli-dev \
    jemalloc-dev

WORKDIR /build
COPY . .
RUN make clean && make all

FROM alpine:3.18 AS runtime

RUN apk add --no-cache \
    openssl \
    pcre \
    zlib \
    brotli \
    jemalloc \
    ca-certificates \
    && adduser -D -s /bin/sh ultrabalancer

WORKDIR /app

COPY --from=builder /build/bin/ultrabalancer /usr/local/bin/
COPY --from=builder /build/config /app/config
COPY docker-entrypoint.sh /usr/local/bin/

RUN chmod +x /usr/local/bin/docker-entrypoint.sh \
    && mkdir -p /app/logs /app/run \
    && chown -R ultrabalancer:ultrabalancer /app

USER ultrabalancer

EXPOSE 8080 8443 9090

HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
  CMD curl -f http://localhost:9090/health || exit 1

ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["ultrabalancer", "-f", "/app/config/ultrabalancer.cfg"]
```

### Docker Entrypoint Script
```bash
#!/bin/sh
set -e

if [ "$1" = 'ultrabalancer' ]; then
    if [ ! -f "/app/config/ultrabalancer.cfg" ]; then
        echo "Generating default configuration..."
        cat > /app/config/ultrabalancer.cfg << EOF
[global]
port = ${UB_PORT:-8080}
ssl_port = ${UB_SSL_PORT:-8443}
stats_port = ${UB_STATS_PORT:-9090}
worker_processes = ${UB_WORKER_PROCESSES:-auto}
log_level = ${UB_LOG_LEVEL:-info}
log_file = /app/logs/ultrabalancer.log

[algorithm]
type = ${UB_ALGORITHM:-round_robin}

[health_check]
interval = ${UB_HEALTH_INTERVAL:-5000}
timeout = ${UB_HEALTH_TIMEOUT:-2000}
path = ${UB_HEALTH_PATH:-/health}
EOF

        if [ -n "$BACKENDS" ]; then
            echo "$BACKENDS" | tr ',' '\n' | while IFS=':' read -r host port weight; do
                cat >> /app/config/ultrabalancer.cfg << EOF

[backend "${host}_${port}"]
server = ${host}:${port}
weight = ${weight:-100}
active = true
EOF
            done
        fi
    fi

    if [ ! -d "/app/logs" ]; then
        mkdir -p /app/logs
    fi

    if [ ! -d "/app/run" ]; then
        mkdir -p /app/run
    fi

    echo "Starting UltraBalancer..."
    exec "$@"
fi

exec "$@"
```

## Docker Swarm Deployment

### Stack Configuration
```yaml
version: '3.8'

services:
  ultrabalancer:
    image: ultrabalancer:latest
    ports:
      - target: 8080
        published: 80
        protocol: tcp
        mode: host
      - target: 8443
        published: 443
        protocol: tcp
        mode: host
    volumes:
      - type: bind
        source: ./config
        target: /app/config
        read_only: true
      - type: volume
        source: logs
        target: /app/logs
    environment:
      BACKENDS: "web1:80:100,web2:80:100,web3:80:150"
      UB_WORKER_PROCESSES: "4"
      UB_LOG_LEVEL: "info"
    deploy:
      replicas: 2
      placement:
        constraints:
          - node.role == manager
      resources:
        limits:
          cpus: '2'
          memory: 1G
        reservations:
          cpus: '1'
          memory: 512M
      restart_policy:
        condition: on-failure
        delay: 5s
        max_attempts: 3
    networks:
      - frontend
      - backend

  web1:
    image: nginx:alpine
    deploy:
      replicas: 2
      placement:
        constraints:
          - node.role == worker
    networks:
      - backend

  web2:
    image: nginx:alpine
    deploy:
      replicas: 2
      placement:
        constraints:
          - node.role == worker
    networks:
      - backend

  web3:
    image: nginx:alpine
    deploy:
      replicas: 3
      placement:
        constraints:
          - node.role == worker
    networks:
      - backend

networks:
  frontend:
    driver: overlay
    attachable: true
  backend:
    driver: overlay
    internal: true

volumes:
  logs:
    driver: local
```

## Kubernetes Integration

### Deployment YAML
```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: ultrabalancer
  labels:
    app: ultrabalancer
spec:
  replicas: 3
  selector:
    matchLabels:
      app: ultrabalancer
  template:
    metadata:
      labels:
        app: ultrabalancer
    spec:
      containers:
      - name: ultrabalancer
        image: ultrabalancer:latest
        ports:
        - containerPort: 8080
          name: http
        - containerPort: 8443
          name: https
        - containerPort: 9090
          name: stats
        env:
        - name: BACKENDS
          value: "backend1:80:100,backend2:80:100,backend3:80:150"
        - name: UB_WORKER_PROCESSES
          value: "auto"
        - name: UB_LOG_LEVEL
          value: "info"
        volumeMounts:
        - name: config
          mountPath: /app/config
          readOnly: true
        - name: logs
          mountPath: /app/logs
        resources:
          limits:
            cpu: 2
            memory: 1Gi
          requests:
            cpu: 1
            memory: 512Mi
        livenessProbe:
          httpGet:
            path: /health
            port: 9090
          initialDelaySeconds: 30
          periodSeconds: 10
        readinessProbe:
          httpGet:
            path: /ready
            port: 9090
          initialDelaySeconds: 5
          periodSeconds: 5
      volumes:
      - name: config
        configMap:
          name: ultrabalancer-config
      - name: logs
        emptyDir: {}

---
apiVersion: v1
kind: Service
metadata:
  name: ultrabalancer-service
spec:
  selector:
    app: ultrabalancer
  ports:
  - name: http
    port: 80
    targetPort: 8080
  - name: https
    port: 443
    targetPort: 8443
  - name: stats
    port: 9090
    targetPort: 9090
  type: LoadBalancer

---
apiVersion: v1
kind: ConfigMap
metadata:
  name: ultrabalancer-config
data:
  ultrabalancer.cfg: |
    [global]
    port = 8080
    ssl_port = 8443
    stats_port = 9090
    worker_processes = auto
    log_level = info

    [algorithm]
    type = round_robin

    [health_check]
    interval = 5000
    timeout = 2000
    path = /health

    [backend "backend1"]
    server = backend1:80
    weight = 100
    active = true

    [backend "backend2"]
    server = backend2:80
    weight = 100
    active = true

    [backend "backend3"]
    server = backend3:80
    weight = 150
    active = true
```

## Docker Environment Configuration

### Environment Variables
```bash
# Basic Configuration
UB_PORT=8080
UB_SSL_PORT=8443
UB_STATS_PORT=9090
UB_WORKER_PROCESSES=auto
UB_LOG_LEVEL=info

# Load Balancing
UB_ALGORITHM=round_robin
BACKENDS=web1:80:100,web2:80:100,web3:80:150

# Health Checks
UB_HEALTH_INTERVAL=5000
UB_HEALTH_TIMEOUT=2000
UB_HEALTH_PATH=/health

# SSL Configuration
UB_SSL_CERT=/app/certs/server.crt
UB_SSL_KEY=/app/certs/server.key
UB_SSL_CA=/app/certs/ca.crt

# Performance Tuning
UB_MAX_CONNECTIONS=10000
UB_KEEPALIVE_TIMEOUT=60
UB_BUFFER_SIZE=16384
```

## Monitoring and Logging

### Docker Compose with Monitoring
```yaml
version: '3.8'

services:
  ultrabalancer:
    build: .
    ports:
      - "8080:8080"
      - "9090:9090"
    volumes:
      - ./logs:/app/logs
    logging:
      driver: "fluentd"
      options:
        fluentd-address: localhost:24224
        tag: ultrabalancer

  prometheus:
    image: prom/prometheus
    ports:
      - "9091:9090"
    volumes:
      - ./monitoring/prometheus.yml:/etc/prometheus/prometheus.yml
    command:
      - '--config.file=/etc/prometheus/prometheus.yml'
      - '--storage.tsdb.path=/prometheus'

  grafana:
    image: grafana/grafana
    ports:
      - "3000:3000"
    volumes:
      - grafana-storage:/var/lib/grafana
    environment:
      - GF_SECURITY_ADMIN_PASSWORD=admin

volumes:
  grafana-storage:
```

## Quick Start Commands

### Build and Run
```bash
# Build the image
docker build -t ultrabalancer:latest .

# Run with default configuration
docker run -d \
  --name ultrabalancer \
  -p 8080:8080 \
  -p 9090:9090 \
  -e BACKENDS="web1:80:100,web2:80:100" \
  ultrabalancer:latest

# Run with custom configuration
docker run -d \
  --name ultrabalancer \
  -p 8080:8080 \
  -v $(pwd)/config:/app/config:ro \
  -v $(pwd)/logs:/app/logs \
  ultrabalancer:latest

# Check logs
docker logs -f ultrabalancer

# Check health
curl http://localhost:9090/health
```

### Docker Compose Commands
```bash
# Start all services
docker-compose up -d

# Scale backend services
docker-compose up -d --scale web1=3 --scale web2=2

# View logs
docker-compose logs -f ultrabalancer

# Restart load balancer
docker-compose restart ultrabalancer

# Stop all services
docker-compose down
```