# How to Setup UltraBalancer with TypeScript

## Node.js Integration

### Installation
```bash
npm install ultrabalancer-node
npm install @types/ultrabalancer-node --save-dev
```

### Basic TypeScript Setup
```typescript
import { UltraBalancer, BackendConfig, LoadBalancerConfig } from 'ultrabalancer-node';

interface ServerConfig {
    host: string;
    port: number;
    weight?: number;
    active?: boolean;
}

class LoadBalancerService {
    private lb: UltraBalancer;

    constructor() {
        const config: LoadBalancerConfig = {
            port: 8080,
            algorithm: 'round_robin',
            maxConnections: 10000,
            workerProcesses: 'auto'
        };

        this.lb = new UltraBalancer(config);
    }

    async addBackends(servers: ServerConfig[]): Promise<void> {
        for (const server of servers) {
            await this.lb.addBackend({
                host: server.host,
                port: server.port,
                weight: server.weight || 100,
                active: server.active !== false
            });
        }
    }

    async start(): Promise<void> {
        await this.lb.start();
        console.log('Load balancer started on port 8080');
    }
}

const servers: ServerConfig[] = [
    { host: '192.168.1.10', port: 3000, weight: 100 },
    { host: '192.168.1.11', port: 3000, weight: 150 },
    { host: '192.168.1.12', port: 3000, weight: 100 }
];

const lbService = new LoadBalancerService();
await lbService.addBackends(servers);
await lbService.start();
```

## Express.js Middleware Integration

### Middleware Setup
```typescript
import express from 'express';
import { UltraBalancerMiddleware, MiddlewareConfig } from 'ultrabalancer-node';

const app = express();

const lbConfig: MiddlewareConfig = {
    backends: [
        'http://192.168.1.10:3000',
        'http://192.168.1.11:3000',
        'http://192.168.1.12:3000'
    ],
    algorithm: 'round_robin',
    healthCheck: {
        interval: 5000,
        timeout: 2000,
        path: '/health'
    }
};

app.use('/api', UltraBalancerMiddleware(lbConfig));

app.listen(8080, () => {
    console.log('Server with load balancer running on port 8080');
});
```

## Advanced Configuration

### Health Monitoring
```typescript
interface HealthConfig {
    interval: number;
    timeout: number;
    retries: number;
    path: string;
    expectedStatus: number;
}

class AdvancedLoadBalancer {
    private lb: UltraBalancer;

    constructor() {
        this.lb = new UltraBalancer({
            port: 8080,
            algorithm: 'least_connections',
            ssl: {
                enabled: true,
                certFile: './certs/server.crt',
                keyFile: './certs/server.key'
            }
        });

        this.setupHealthChecks();
        this.setupMonitoring();
    }

    private setupHealthChecks(): void {
        const healthConfig: HealthConfig = {
            interval: 5000,
            timeout: 2000,
            retries: 3,
            path: '/health',
            expectedStatus: 200
        };

        this.lb.configureHealthChecks(healthConfig);
    }

    private setupMonitoring(): void {
        this.lb.on('backend_down', (backend: BackendConfig) => {
            console.error(`Backend ${backend.host}:${backend.port} is down`);
        });

        this.lb.on('backend_up', (backend: BackendConfig) => {
            console.log(`Backend ${backend.host}:${backend.port} is back up`);
        });

        this.lb.on('request_distributed', (backend: BackendConfig, requestId: string) => {
            console.log(`Request ${requestId} sent to ${backend.host}:${backend.port}`);
        });
    }

    async getStats(): Promise<LoadBalancerStats> {
        return await this.lb.getStatistics();
    }
}
```

## WebSocket Support

### WebSocket Proxy
```typescript
import WebSocket from 'ws';
import { WebSocketBalancer } from 'ultrabalancer-node';

const wsBalancer = new WebSocketBalancer({
    port: 8081,
    backends: [
        'ws://192.168.1.10:3001',
        'ws://192.168.1.11:3001',
        'ws://192.168.1.12:3001'
    ],
    stickySession: true
});

wsBalancer.on('connection', (ws: WebSocket, backend: string) => {
    console.log(`WebSocket connection established with ${backend}`);
});

await wsBalancer.start();
```

## Dynamic Configuration

### Runtime Backend Management
```typescript
interface BackendManager {
    addBackend(config: BackendConfig): Promise<void>;
    removeBackend(id: string): Promise<void>;
    updateBackend(id: string, config: Partial<BackendConfig>): Promise<void>;
    listBackends(): Promise<BackendConfig[]>;
}

class DynamicLoadBalancer implements BackendManager {
    private lb: UltraBalancer;
    private backends: Map<string, BackendConfig> = new Map();

    async addBackend(config: BackendConfig): Promise<void> {
        const id = `${config.host}:${config.port}`;
        await this.lb.addBackend(config);
        this.backends.set(id, config);
        console.log(`Backend ${id} added successfully`);
    }

    async removeBackend(id: string): Promise<void> {
        await this.lb.removeBackend(id);
        this.backends.delete(id);
        console.log(`Backend ${id} removed successfully`);
    }

    async updateBackend(id: string, updates: Partial<BackendConfig>): Promise<void> {
        const existing = this.backends.get(id);
        if (!existing) throw new Error(`Backend ${id} not found`);

        const updated = { ...existing, ...updates };
        await this.lb.updateBackend(id, updated);
        this.backends.set(id, updated);
        console.log(`Backend ${id} updated successfully`);
    }

    async listBackends(): Promise<BackendConfig[]> {
        return Array.from(this.backends.values());
    }
}
```

## Performance Monitoring

### Metrics Collection
```typescript
interface Metrics {
    totalRequests: number;
    activeConnections: number;
    averageResponseTime: number;
    backendStats: BackendStats[];
}

class MetricsCollector {
    private lb: UltraBalancer;

    constructor(lb: UltraBalancer) {
        this.lb = lb;
        this.startMetricsCollection();
    }

    private startMetricsCollection(): void {
        setInterval(async () => {
            const metrics = await this.collectMetrics();
            this.publishMetrics(metrics);
        }, 10000);
    }

    private async collectMetrics(): Promise<Metrics> {
        const stats = await this.lb.getStatistics();

        return {
            totalRequests: stats.totalRequests,
            activeConnections: stats.activeConnections,
            averageResponseTime: stats.averageResponseTime,
            backendStats: stats.backends
        };
    }

    private publishMetrics(metrics: Metrics): void {
        console.log('Load Balancer Metrics:', JSON.stringify(metrics, null, 2));
    }
}
```

## Error Handling

### Comprehensive Error Management
```typescript
enum LoadBalancerError {
    BACKEND_UNAVAILABLE = 'BACKEND_UNAVAILABLE',
    CONNECTION_TIMEOUT = 'CONNECTION_TIMEOUT',
    HEALTH_CHECK_FAILED = 'HEALTH_CHECK_FAILED',
    CONFIGURATION_ERROR = 'CONFIGURATION_ERROR'
}

class ErrorHandler {
    static handle(error: Error, context: string): void {
        console.error(`[${context}] Error:`, error.message);

        if (error.message.includes('ECONNREFUSED')) {
            console.error('Backend connection refused - check if backend servers are running');
        } else if (error.message.includes('ETIMEDOUT')) {
            console.error('Connection timeout - consider increasing timeout values');
        }
    }
}

try {
    await lbService.start();
} catch (error) {
    ErrorHandler.handle(error as Error, 'LoadBalancer Start');
}
```

## Testing

### Unit Tests
```typescript
import { describe, it, expect, beforeEach } from 'vitest';
import { UltraBalancer } from 'ultrabalancer-node';

describe('UltraBalancer', () => {
    let lb: UltraBalancer;

    beforeEach(() => {
        lb = new UltraBalancer({
            port: 8080,
            algorithm: 'round_robin'
        });
    });

    it('should add backends correctly', async () => {
        await lb.addBackend({ host: '127.0.0.1', port: 3000 });
        const backends = await lb.listBackends();
        expect(backends).toHaveLength(1);
    });

    it('should distribute requests using round robin', async () => {
        await lb.addBackend({ host: '127.0.0.1', port: 3000 });
        await lb.addBackend({ host: '127.0.0.1', port: 3001 });

        const distribution = await lb.testDistribution(10);
        expect(distribution['127.0.0.1:3000']).toBe(5);
        expect(distribution['127.0.0.1:3001']).toBe(5);
    });
});
```