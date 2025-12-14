import http from 'http';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import '../../src/eventloop';
import { PluginScanTool, PluginFormat } from '../../src/plugin-scan-tool';
import { PluginCatalogEntry, PluginCatalogEntryInfo } from '../../src/plugin-catalog';
import { ConfigurationRequest, ParameterInfo, PluginInstance } from '../../src/plugin-instance';

interface PluginSummary extends PluginCatalogEntryInfo {
    index: number;
}

interface ParameterSnapshot extends ParameterInfo {
    value: number | null;
}

interface HostState {
    scanning: boolean;
    catalog: PluginSummary[];
    currentPlugin: PluginSummary | null;
    instanceReady: boolean;
    processing: boolean;
    parameters: ParameterSnapshot[];
}

const DEFAULT_CONFIGURATION: ConfigurationRequest = {
    sampleRate: 48000,
    bufferSizeInSamples: 512,
    mainInputChannels: 2,
    mainOutputChannels: 2,
};

class WebPluginHostApp {
    private readonly scanTool = new PluginScanTool();
    private readonly cachePath = path.join(os.homedir(), '.remidy', 'plugin-cache.json');
    private scanning = false;
    private currentInstance: PluginInstance | null = null;
    private currentPlugin: PluginSummary | null = null;
    private processing = false;
    private parameters: ParameterSnapshot[] = [];

    constructor() {
        this.loadCatalogCache();
    }

    dispose(): void {
        this.destroyInstance();
        this.scanTool.dispose();
    }

    getState(): HostState {
        return {
            scanning: this.scanning,
            catalog: this.getCatalogSummaries(),
            currentPlugin: this.currentPlugin,
            instanceReady: Boolean(this.currentInstance),
            processing: this.processing,
            parameters: this.parameters,
        };
    }

    async scan(): Promise<void> {
        if (this.scanning) {
            return;
        }

        this.scanning = true;
        try {
            this.scanTool.performScanning();
            this.ensureCacheDirectory();
            this.scanTool.saveCache(this.cachePath);
        } finally {
            this.scanning = false;
        }
    }

    async loadPlugin(index: number): Promise<void> {
        const entry = this.scanTool.catalog.getPluginAt(index);
        if (!entry) {
            throw new Error(`Plugin at index ${index} not found`);
        }

        const summary = this.describeEntry(entry, index);
        const format = this.findFormat(summary.format);
        if (!format) {
            throw new Error(`Format ${summary.format} is not available`);
        }

        const instance = await PluginInstance.createAsync(format, entry);
        instance.configure(DEFAULT_CONFIGURATION);

        this.replaceInstance(instance, summary);
    }

    async startProcessing(): Promise<void> {
        if (!this.currentInstance || this.processing) {
            return;
        }
        this.currentInstance.startProcessing();
        this.processing = true;
    }

    async stopProcessing(): Promise<void> {
        if (!this.currentInstance || !this.processing) {
            return;
        }

        this.currentInstance.stopProcessing();
        this.processing = false;
    }

    setParameter(paramId: number, value: number): ParameterSnapshot {
        if (!this.currentInstance) {
            throw new Error('No plugin is currently loaded');
        }

        this.currentInstance.setParameterValue(paramId, value);
        this.refreshParameters();

        const updated = this.parameters.find(param => param.id === paramId);
        if (!updated) {
            throw new Error(`Parameter ${paramId} not found`);
        }
        return updated;
    }

    private loadCatalogCache(): void {
        if (!fs.existsSync(this.cachePath)) {
            return;
        }

        try {
            this.scanTool.catalog.load(this.cachePath);
        } catch (error) {
            console.warn(`Failed to load cache at ${this.cachePath}:`, error);
        }
    }

    private ensureCacheDirectory(): void {
        const dir = path.dirname(this.cachePath);
        if (!fs.existsSync(dir)) {
            fs.mkdirSync(dir, { recursive: true });
        }
    }

    private getCatalogSummaries(): PluginSummary[] {
        const entries = this.scanTool.catalog.getPlugins();
        return entries.map((entry, index) => this.describeEntry(entry, index));
    }

    private describeEntry(entry: PluginCatalogEntry, index: number): PluginSummary {
        const data = entry.toJSON();
        return {
            index,
            ...data,
        };
    }

    private findFormat(name: string): PluginFormat | undefined {
        return this.scanTool.getFormats().find(format => format.name === name);
    }

    private refreshParameters(): void {
        if (!this.currentInstance) {
            this.parameters = [];
            return;
        }

        const params = this.currentInstance.getParameters();
        this.parameters = params.map(param => ({
            ...param,
            value: this.currentInstance!.getParameterValue(param.id),
        }));
    }

    private destroyInstance(): void {
        if (this.currentInstance) {
            try {
                if (this.processing) {
                    this.currentInstance.stopProcessing();
                }
            } catch (error) {
                console.warn('Error while stopping plugin instance:', error);
            }

            this.currentInstance.dispose();
            this.currentInstance = null;
        }

        this.processing = false;
        this.currentPlugin = null;
        this.parameters = [];
    }

    private replaceInstance(instance: PluginInstance, summary: PluginSummary): void {
        this.destroyInstance();

        this.currentInstance = instance;
        this.currentPlugin = summary;
        this.processing = false;
        this.refreshParameters();
    }
}

const INDEX_HTML = `<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8" />
    <title>Remidy Plugin Host (Web Example)</title>
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <link rel="stylesheet" href="/styles.css" />
</head>
<body>
    <header>
        <h1>Remidy Plugin Host</h1>
        <p class="subtitle">Experimental web UI example built with @remidy/node</p>
    </header>
    <main>
        <section class="controls">
            <button id="scan-button">Scan Plugins</button>
            <button id="toggle-processing" disabled>Start Processing</button>
            <span id="status-label" class="status-label">Idle</span>
        </section>
        <section class="layout">
            <div class="panel plugins">
                <h2>Plugins</h2>
                <div id="plugin-list" class="plugin-list"></div>
            </div>
            <div class="panel parameters">
                <h2 id="plugin-title">No Plugin Loaded</h2>
                <div id="plugin-meta" class="plugin-meta"></div>
                <div id="parameter-list" class="parameter-list"></div>
            </div>
        </section>
    </main>
    <footer>
        <p>
            This UI talks to a Node.js server that instantiates plugins through remidy. Start the server, then open this page.
        </p>
    </footer>
    <script src="/app.js" type="module"></script>
</body>
</html>`;

const STYLES_CSS = `
:root {
    font-family: system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    color-scheme: dark;
    background-color: #0f111a;
    color: #f5f6fa;
    line-height: 1.5;
}

body {
    margin: 0;
    min-height: 100vh;
    display: flex;
    flex-direction: column;
}

header, footer {
    padding: 1.5rem;
    background: #080a12;
    border-bottom: 1px solid rgba(255, 255, 255, 0.1);
}

footer {
    border-top: 1px solid rgba(255, 255, 255, 0.1);
    margin-top: auto;
}

main {
    padding: 1.5rem;
}

.subtitle {
    margin: 0.25rem 0 0;
    color: rgba(255, 255, 255, 0.7);
}

.controls {
    display: flex;
    gap: 1rem;
    align-items: center;
    margin-bottom: 1.5rem;
}

button {
    background: #6366f1;
    border: none;
    border-radius: 6px;
    color: #fff;
    font-size: 0.95rem;
    padding: 0.6rem 1.2rem;
    cursor: pointer;
    transition: background 0.2s ease;
}

button:disabled {
    opacity: 0.5;
    cursor: not-allowed;
}

button:hover:not(:disabled) {
    background: #7c3aed;
}

.layout {
    display: grid;
    grid-template-columns: 1fr 2fr;
    gap: 1.5rem;
}

.panel {
    border: 1px solid rgba(255, 255, 255, 0.1);
    border-radius: 10px;
    padding: 1rem;
    background: rgba(15, 17, 26, 0.8);
    min-height: 460px;
}

.panel h2 {
    margin-top: 0;
    margin-bottom: 0.5rem;
}

.plugin-list {
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
    max-height: 520px;
    overflow-y: auto;
}

.plugin-item {
    display: flex;
    flex-direction: column;
    gap: 0.2rem;
    padding: 0.75rem;
    border-radius: 8px;
    border: 1px solid transparent;
    background: rgba(255, 255, 255, 0.04);
    cursor: pointer;
    transition: border 0.2s ease, background 0.2s ease;
}

.plugin-item:hover {
    border-color: rgba(99, 102, 241, 0.8);
}

.plugin-item.active {
    border-color: rgba(99, 102, 241, 0.8);
    background: rgba(99, 102, 241, 0.12);
}

.plugin-item strong {
    font-size: 0.95rem;
}

.plugin-item span {
    font-size: 0.8rem;
    color: rgba(255, 255, 255, 0.65);
}

.plugin-meta {
    display: flex;
    flex-direction: column;
    gap: 0.2rem;
    margin-bottom: 1rem;
    color: rgba(255, 255, 255, 0.8);
}

.parameter-list {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    max-height: 520px;
    overflow-y: auto;
}

.parameter {
    padding: 0.75rem;
    border-radius: 8px;
    border: 1px solid rgba(255, 255, 255, 0.08);
    background: rgba(255, 255, 255, 0.02);
}

.parameter label {
    display: block;
    font-weight: 600;
    margin-bottom: 0.4rem;
}

.parameter .value {
    font-size: 0.85rem;
    color: rgba(255, 255, 255, 0.7);
}

.status-label {
    font-size: 0.9rem;
    color: rgba(255, 255, 255, 0.7);
}

@media (max-width: 960px) {
    .layout {
        grid-template-columns: 1fr;
    }
}
`;

const APP_JS = `
async function request(url, options = {}) {
    const response = await fetch(url, {
        headers: { 'Content-Type': 'application/json', ...(options.headers ?? {}) },
        ...options,
    });
    if (!response.ok) {
        const text = await response.text();
        throw new Error(text || response.statusText);
    }
    if (response.status === 204) {
        return null;
    }
    return await response.json();
}

async function fetchState() {
    return await request('/api/state');
}

async function scanPlugins() {
    return await request('/api/scan', { method: 'POST' });
}

async function loadPlugin(index) {
    return await request('/api/instance', {
        method: 'POST',
        body: JSON.stringify({ pluginIndex: index }),
    });
}

async function toggleProcessing(currentlyProcessing) {
    const endpoint = currentlyProcessing ? '/api/instance/stop' : '/api/instance/start';
    return await request(endpoint, { method: 'POST' });
}

async function setParameter(id, value) {
    return await request('/api/instance/parameter', {
        method: 'POST',
        body: JSON.stringify({ id, value }),
    });
}

const state = {
    data: null,
    loading: false,
};

const elements = {
    scanButton: document.getElementById('scan-button'),
    toggleProcessing: document.getElementById('toggle-processing'),
    statusLabel: document.getElementById('status-label'),
    pluginList: document.getElementById('plugin-list'),
    pluginTitle: document.getElementById('plugin-title'),
    pluginMeta: document.getElementById('plugin-meta'),
    parameterList: document.getElementById('parameter-list'),
};

async function refreshState() {
    state.data = await fetchState();
    render();
}

function render() {
    const data = state.data;
    if (!data) {
        return;
    }

    elements.scanButton.disabled = state.loading || data.scanning;
    elements.statusLabel.textContent = data.scanning
        ? 'Scanning plugins...'
        : data.currentPlugin
            ? data.processing
                ? 'Processing audio'
                : 'Plugin loaded'
            : 'Idle';

    if (data.currentPlugin) {
        elements.pluginTitle.textContent = data.currentPlugin.displayName;
        elements.pluginMeta.innerHTML = \`
            <span>Format: \${data.currentPlugin.format}</span>
            <span>Vendor: \${data.currentPlugin.vendorName || 'Unknown'}</span>
            <span>Bundle: \${data.currentPlugin.bundlePath}</span>
        \`;
    } else {
        elements.pluginTitle.textContent = 'No Plugin Loaded';
        elements.pluginMeta.innerHTML = '<span>Select a plugin to instantiate it</span>';
    }

    renderPluginList(data);
    renderParameters(data);

    elements.toggleProcessing.disabled = !data.instanceReady || state.loading;
    elements.toggleProcessing.textContent = data.processing ? 'Stop Processing' : 'Start Processing';
}

function renderPluginList(data) {
    elements.pluginList.innerHTML = '';
    data.catalog.forEach(plugin => {
        const item = document.createElement('button');
        item.className = 'plugin-item';
        if (data.currentPlugin && data.currentPlugin.index === plugin.index) {
            item.classList.add('active');
        }
        item.innerHTML = \`
            <strong>\${plugin.displayName}</strong>
            <span>\${plugin.vendorName || 'Unknown vendor'} · \${plugin.format}</span>
            <span class="path">\${plugin.bundlePath}</span>
        \`;
        item.addEventListener('click', async () => {
            if (state.loading) {
                return;
            }
            state.loading = true;
            render();
            try {
                await loadPlugin(plugin.index);
                await refreshState();
            } catch (error) {
                alert('Failed to load plugin: ' + error.message);
            } finally {
                state.loading = false;
                render();
            }
        });
        elements.pluginList.appendChild(item);
    });
}

function renderParameters(data) {
    elements.parameterList.innerHTML = '';
    if (!data.parameters.length) {
        const empty = document.createElement('p');
        empty.textContent = data.instanceReady
            ? 'Plugin does not expose editable parameters.'
            : 'Select a plugin to see its parameters.';
        elements.parameterList.appendChild(empty);
        return;
    }

    data.parameters.forEach(param => {
        const wrapper = document.createElement('div');
        wrapper.className = 'parameter';

        const label = document.createElement('label');
        label.textContent = \`\${param.name} (#\${param.id})\`;

        const valueLabel = document.createElement('div');
        valueLabel.className = 'value';
        valueLabel.textContent = formatValue(param.value, param);

        const slider = document.createElement('input');
        slider.type = 'range';
        slider.min = param.minValue;
        slider.max = param.maxValue;
        const step = (param.maxValue - param.minValue) / 100;
        slider.step = step > 0 ? step : 0.01;
        slider.value = param.value ?? param.defaultValue ?? 0;
        slider.disabled = !param.isAutomatable || param.isReadonly || state.loading;

        slider.addEventListener('input', () => {
            valueLabel.textContent = formatValue(Number(slider.value), param);
        });

        slider.addEventListener('change', async () => {
            try {
                state.loading = true;
                const updated = await setParameter(param.id, Number(slider.value));
                valueLabel.textContent = formatValue(updated.value, updated);
                await refreshState();
            } catch (error) {
                alert('Failed to set parameter: ' + error.message);
            } finally {
                state.loading = false;
                render();
            }
        });

        wrapper.appendChild(label);
        wrapper.appendChild(valueLabel);
        wrapper.appendChild(slider);
        elements.parameterList.appendChild(wrapper);
    });
}

function formatValue(value, param) {
    if (value === null || value === undefined) {
        return 'n/a';
    }
    const precision = Math.abs(param.maxValue - param.minValue) > 5 ? 2 : 3;
    return Number(value).toFixed(precision);
}

elements.scanButton.addEventListener('click', async () => {
    if (state.loading) {
        return;
    }
    try {
        state.loading = true;
        render();
        await scanPlugins();
        await refreshState();
    } catch (error) {
        alert('Scanning failed: ' + error.message);
    } finally {
        state.loading = false;
        render();
    }
});

elements.toggleProcessing.addEventListener('click', async () => {
    if (!state.data || state.loading || !state.data.instanceReady) {
        return;
    }

    try {
        state.loading = true;
        render();
        await toggleProcessing(state.data.processing);
        await refreshState();
    } catch (error) {
        alert('Failed to toggle processing: ' + error.message);
    } finally {
        state.loading = false;
        render();
    }
});

refreshState().catch(error => {
    console.error(error);
    elements.statusLabel.textContent = 'Failed to contact server';
});
`;

function readJsonBody(req: http.IncomingMessage): Promise<any> {
    return new Promise((resolve, reject) => {
        let data = '';
        req.on('data', chunk => {
            data += chunk;
            if (data.length > 2 * 1024 * 1024) {
                reject(new Error('Request body too large'));
                req.destroy();
            }
        });
        req.on('end', () => {
            if (!data) {
                resolve({});
                return;
            }

            try {
                resolve(JSON.parse(data));
            } catch (error) {
                reject(error);
            }
        });
        req.on('error', reject);
    });
}

function sendJson(res: http.ServerResponse, payload: unknown, statusCode = 200): void {
    res.writeHead(statusCode, {
        'Content-Type': 'application/json; charset=utf-8',
        'Cache-Control': 'no-store',
    });
    res.end(JSON.stringify(payload));
}

function sendText(res: http.ServerResponse, payload: string, contentType: string): void {
    res.writeHead(200, {
        'Content-Type': contentType,
        'Cache-Control': 'no-store',
    });
    res.end(payload);
}

async function main() {
    const app = new WebPluginHostApp();
    const port = Number(process.env.PORT ?? 5173);

    const server = http.createServer(async (req, res) => {
        try {
            const url = new URL(req.url ?? '/', `http://${req.headers.host ?? 'localhost'}`);

            if (req.method === 'GET' && url.pathname === '/') {
                sendText(res, INDEX_HTML, 'text/html; charset=utf-8');
                return;
            }

            if (req.method === 'GET' && url.pathname === '/styles.css') {
                sendText(res, STYLES_CSS, 'text/css; charset=utf-8');
                return;
            }

            if (req.method === 'GET' && url.pathname === '/app.js') {
                sendText(res, APP_JS, 'application/javascript; charset=utf-8');
                return;
            }

            if (req.method === 'GET' && url.pathname === '/api/state') {
                sendJson(res, app.getState());
                return;
            }

            if (req.method === 'POST' && url.pathname === '/api/scan') {
                await app.scan();
                sendJson(res, app.getState());
                return;
            }

            if (req.method === 'POST' && url.pathname === '/api/instance') {
                const body = await readJsonBody(req);
                if (typeof body.pluginIndex !== 'number') {
                    sendJson(res, { error: 'pluginIndex must be a number' }, 400);
                    return;
                }
                await app.loadPlugin(body.pluginIndex);
                sendJson(res, app.getState());
                return;
            }

            if (req.method === 'POST' && url.pathname === '/api/instance/start') {
                await app.startProcessing();
                sendJson(res, app.getState());
                return;
            }

            if (req.method === 'POST' && url.pathname === '/api/instance/stop') {
                await app.stopProcessing();
                sendJson(res, app.getState());
                return;
            }

            if (req.method === 'POST' && url.pathname === '/api/instance/parameter') {
                const body = await readJsonBody(req);
                if (typeof body.id !== 'number' || typeof body.value !== 'number') {
                    sendJson(res, { error: 'id and value must be numbers' }, 400);
                    return;
                }
                const updated = app.setParameter(body.id, body.value);
                sendJson(res, updated);
                return;
            }

            res.statusCode = 404;
            res.end('Not Found');
        } catch (error) {
            console.error('Request failed:', error);
            sendJson(res, { error: (error as Error).message }, 500);
        }
    });

    server.listen(port, () => {
        console.log(`Remidy web plugin host running on http://localhost:${port}`);
    });

    function shutdown() {
        console.log('Shutting down web plugin host...');
        server.close();
        app.dispose();
    }

    process.on('SIGINT', () => {
        shutdown();
        process.exit(0);
    });

    process.on('SIGTERM', () => {
        shutdown();
        process.exit(0);
    });
}

main().catch(error => {
    console.error('Failed to start web plugin host example:', error);
    process.exit(1);
});
