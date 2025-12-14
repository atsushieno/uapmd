process.on('uncaughtException', (error: unknown) => {
    console.error('[remidy-node] uncaught exception', error);
    if (error instanceof Error && error.stack) {
        console.error(error.stack);
    }
});
process.on('unhandledRejection', reason => {
    console.error('[remidy-node] unhandled rejection', reason);
});

// eslint-disable-next-line @typescript-eslint/no-var-requires
const electron: any = require('electron');
const { app, BrowserWindow, ipcMain } = electron;
type IpcMainInvokeEvent = typeof electron extends { IpcMainInvokeEvent: infer T } ? T : any;
import {
    ContainerWindow,
    GLContextGuard,
    PluginCatalogEntry,
    PluginInstance,
    PluginScanTool,
    PluginFormat,
    type ParameterInfo,
    type PluginUICreateOptions,
} from '../../src';
import { dialog } from 'electron';

interface PluginSummary {
    index: number;
    displayName: string;
    vendorName: string;
    format: string;
    bundlePath: string;
}

interface ParameterSnapshot extends ParameterInfo {
    value: number | null;
}

interface HostState {
    scanning: boolean;
    catalog: PluginSummary[];
    currentPlugin: PluginSummary | null;
    instanceReady: boolean;
    uiSupported: boolean;
    uiVisible: boolean;
    parameters: ParameterSnapshot[];
}

class ElectronPluginHost {
    private readonly scanTool = new PluginScanTool();
    private catalogEntries: PluginCatalogEntry[] = [];
    private formats: PluginFormat[] = [];
    private formatsLoaded = false;
    private currentInstance: PluginInstance | null = null;
    private currentPlugin: PluginSummary | null = null;
    private scanning = false;
    private parameterSnapshots: ParameterSnapshot[] = [];
    private pluginWindow: ContainerWindow | null = null;
    private pluginUIEmbedded = false;
    private pluginUIVisible = false;
    private pluginUISupported = false;
    private useFloatingWindow(): boolean {
        return true;
    }

    dispose(): void {
        this.destroyCurrentInstance();
        this.scanTool.dispose();
    }

    getState(): HostState {
        return {
            scanning: this.scanning,
            catalog: this.catalogEntries.map((entry, index) => this.describeEntry(entry, index)),
            currentPlugin: this.currentPlugin,
            instanceReady: Boolean(this.currentInstance),
            uiSupported: this.pluginUISupported,
            uiVisible: this.pluginUIVisible,
            parameters: this.parameterSnapshots,
        };
    }

    async scanPlugins(): Promise<void> {
        if (this.scanning) {
            return;
        }
        this.scanning = true;
        try {
            this.scanTool.performScanning();
            this.catalogEntries = this.scanTool.catalog.getPlugins();
            this.formatsLoaded = false;
            this.formats = [];
        } finally {
            this.scanning = false;
        }
    }

    async loadPlugin(index: number): Promise<void> {
        const entry = this.catalogEntries[index];
        if (!entry) {
            throw new Error(`Plugin at index ${index} not found`);
        }

        const summary = this.describeEntry(entry, index);
        const format = this.ensureFormats().find(f => f.name === summary.format);
        if (!format) {
            throw new Error(`Format ${summary.format} is not available`);
        }

        this.destroyCurrentInstance();

        const instance = await PluginInstance.createAsync(format, entry);
        instance.configure({
            sampleRate: 48000,
            bufferSizeInSamples: 512,
            mainInputChannels: 2,
            mainOutputChannels: 2,
        });

        this.currentInstance = instance;
        this.currentPlugin = summary;
        this.pluginUISupported = instance.hasUISupport();
        this.refreshParameters();
    }

    setParameter(paramId: number, value: number): void {
        if (!this.currentInstance) {
            throw new Error('No plugin instance available');
        }
        this.currentInstance.setParameterValue(paramId, value);
        this.refreshParameters();
    }

    async togglePluginUI(): Promise<void> {
        if (!this.currentInstance) {
            throw new Error('No plugin instance available');
        }
        console.log('[remidy-node] main: togglePluginUI', { visible: this.pluginUIVisible, supported: this.pluginUISupported });
        if (this.pluginUIVisible) {
            this.hidePluginUI();
        } else {
            await this.showPluginUI();
        }
    }

    private ensureFormats() {
        if (!this.formatsLoaded) {
            this.formats = this.scanTool.getFormats();
            this.formatsLoaded = true;
        }
        return this.formats;
    }

    private describeEntry(entry: PluginCatalogEntry, index: number): PluginSummary {
        return {
            index,
            displayName: entry.displayName,
            vendorName: entry.vendorName,
            format: entry.format,
            bundlePath: entry.bundlePath,
        };
    }

    private refreshParameters(): void {
        if (!this.currentInstance) {
            this.parameterSnapshots = [];
            return;
        }
        const params = this.currentInstance.getParameters();
        this.parameterSnapshots = params.map(param => ({
            ...param,
            value: this.currentInstance!.getParameterValue(param.id),
        }));
    }

    private destroyCurrentInstance(): void {
        if (!this.currentInstance) {
            return;
        }
        try {
            this.hidePluginUI();
            const guard = new GLContextGuard();
            try {
                this.currentInstance.destroyUI();
            } finally {
                guard.dispose();
            }
        } catch {
            // ignore
        }
        this.currentInstance.dispose();
        this.currentInstance = null;
        this.currentPlugin = null;
        this.parameterSnapshots = [];
        this.pluginUIEmbedded = false;
        this.pluginUIVisible = false;
        this.pluginUISupported = false;
        this.destroyPluginWindow();
    }

    private destroyPluginWindow(): void {
        if (this.pluginWindow) {
            this.pluginWindow.dispose();
            this.pluginWindow = null;
        }
        this.pluginUIEmbedded = false;
        this.pluginUIVisible = false;
    }

    private ensurePluginWindow(): ContainerWindow {
        if (!this.pluginWindow) {
            const title = this.currentPlugin ? `${this.currentPlugin.displayName} (${this.currentPlugin.format})` : 'Plugin UI';
            this.pluginWindow = new ContainerWindow(title, 800, 600);
        }
        return this.pluginWindow;
    }

    private async showPluginUI(): Promise<void> {
        if (!this.currentInstance) {
            return;
        }
        if (!this.pluginUISupported) {
            throw new Error('This plugin does not expose a UI');
        }
        const floating = this.useFloatingWindow();
        console.log('[remidy-node] main: showPluginUI start', { floating, plugin: this.currentPlugin });
        const guard = new GLContextGuard();
        try {
            let window: ContainerWindow | null = null;
            let resizeHandler: PluginUICreateOptions['onResize'] | undefined;
            if (!floating) {
                window = this.ensurePluginWindow();
                resizeHandler = (width, height) => {
                    window!.resize(width, height);
                    return true;
                };
            }

            if (!this.pluginUIEmbedded) {
                if (!floating) {
                    window!.resize(800, 600);
                }
                this.currentInstance.createUI({
                    floating,
                    parentHandle: floating ? undefined : window!.nativeHandle,
                    onResize: resizeHandler,
                });
                this.pluginUIEmbedded = true;
                if (!floating) {
                    try {
                        const size = this.currentInstance.getUISize();
                        if (size) {
                            window!.resize(size.width, size.height);
                        }
                    } catch (error) {
                        console.warn('Failed to query plugin UI size – using default window size:', error);
                    }
                }
            }

            this.currentInstance.showUI();
            if (!floating) {
                await new Promise<void>((resolve, reject) => {
                    setImmediate(() => {
                        try {
                            window!.show(true);
                            resolve();
                        } catch (error) {
                            reject(error);
                        }
                    });
                });
            }
            this.pluginUIVisible = true;
        } catch (error) {
            dialog.showErrorBox('Remidy Plugin Host', `Failed to open plugin UI: ${error}`);
            throw error;
        } finally {
            guard.dispose();
        }
    }

    private hidePluginUI(skipContainerHide: boolean = false): void {
        if (!this.currentInstance || !this.pluginUIEmbedded) {
            return;
        }
        console.log('[remidy-node] main: hidePluginUI', { skipContainerHide });
        const guard = new GLContextGuard();
        try {
            try {
                this.currentInstance.hideUI();
            } catch {
                // ignore
            }
        } finally {
            guard.dispose();
        }
        if (!this.useFloatingWindow() && !skipContainerHide && this.pluginWindow) {
            this.pluginWindow.show(false);
        }
        this.pluginUIVisible = false;
    }
}

const host = new ElectronPluginHost();

function createHtmlDataUrl(script: string): string {
    const html = `
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8" />
    <title>Remidy Plugin Host (Electron)</title>
    <style>
        :root {
            font-family: system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
            color-scheme: dark;
            background: #0f111a;
            color: #f5f6fa;
        }
        body { margin: 0; padding: 1.5rem; background: #0f111a; }
        header { margin-bottom: 1rem; }
        button {
            background: #6366f1;
            border: none;
            border-radius: 6px;
            padding: 0.5rem 1rem;
            color: #fff;
            cursor: pointer;
            margin-right: 0.5rem;
        }
        button:disabled { opacity: 0.5; cursor: not-allowed; }
        main { display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; }
        .panel {
            border: 1px solid rgba(255,255,255,0.1);
            border-radius: 10px;
            padding: 1rem;
            min-height: 420px;
            background: rgba(255,255,255,0.02);
        }
        .plugin-item {
            border: 1px solid transparent;
            border-radius: 8px;
            padding: 0.5rem;
            margin-bottom: 0.5rem;
            background: rgba(255,255,255,0.03);
        }
        .plugin-item.active {
            border-color: rgba(99,102,241,0.8);
            background: rgba(99,102,241,0.12);
        }
        .params {
            max-height: 360px;
            overflow-y: auto;
        }
        .param {
            border-bottom: 1px solid rgba(255,255,255,0.08);
            padding: 0.5rem 0;
        }
        .param:last-child { border-bottom: none; }
        input[type="range"] { width: 100%; }
    </style>
</head>
<body>
    <header>
        <h1>Remidy Plugin Host</h1>
        <div>
            <button id="scan-button">Scan Plugins</button>
            <button id="toggle-ui" disabled>Open Plugin UI</button>
            <span id="status">Idle</span>
        </div>
    </header>
    <main>
        <section class="panel">
            <h2>Catalog</h2>
            <div id="plugin-list"></div>
        </section>
        <section class="panel">
            <h2 id="plugin-title">No Plugin Loaded</h2>
            <div id="plugin-meta"></div>
            <div class="params" id="parameter-list"></div>
        </section>
    </main>
    <script>
${script}
    </script>
</body>
</html>`;
    return `data:text/html;base64,${Buffer.from(html, 'utf8').toString('base64')}`;
}

const RENDERER_SCRIPT = `
const { ipcRenderer } = require('electron');

['log','warn','error'].forEach(level => {
    const original = console[level].bind(console);
    console[level] = (...args) => {
        ipcRenderer.send('host:renderer-log', { level, args: args.map(String) });
        original(...args);
    };
});

console.log('[renderer] script loaded');

window.addEventListener('DOMContentLoaded', () => {
    console.log('[renderer] DOMContentLoaded');
});

const state = { data: null, loading: false };
const elements = {
    scanButton: document.getElementById('scan-button'),
    toggleUi: document.getElementById('toggle-ui'),
    status: document.getElementById('status'),
    pluginList: document.getElementById('plugin-list'),
    pluginTitle: document.getElementById('plugin-title'),
    pluginMeta: document.getElementById('plugin-meta'),
    parameterList: document.getElementById('parameter-list'),
};

async function refreshState() {
    state.data = await ipcRenderer.invoke('host:get-state');
    render();
}

elements.scanButton.addEventListener('click', async () => {
    if (state.loading) return;
    state.loading = true;
    render();
    try {
        await ipcRenderer.invoke('host:scan');
        await refreshState();
    } finally {
        state.loading = false;
        render();
    }
});

elements.toggleUi.addEventListener('click', async () => {
    console.log('[remidy-node] renderer: toggle-ui clicked');
    if (!state.data || !state.data.instanceReady || state.loading) {
        console.warn('[remidy-node] renderer: toggle-ui click ignored');
        return;
    }
    state.loading = true;
    render();
    try {
        await ipcRenderer.invoke('host:toggle-ui');
        await refreshState();
    } catch (error) {
        console.error('[remidy-node] renderer: toggle-ui failed', error);
        alert('Failed to toggle plugin UI: ' + (error?.message ?? error));
    } finally {
        state.loading = false;
        render();
    }
});

function render() {
    const data = state.data;
    if (!data) {
        elements.status.textContent = 'Idle';
        elements.toggleUi.disabled = true;
        return;
    }

    elements.status.textContent = data.scanning
        ? 'Scanning...'
        : data.currentPlugin
            ? (data.uiVisible ? 'Plugin UI Visible' : 'Plugin ready')
            : 'Idle';

    renderCatalog(data);
    renderPluginDetails(data);

    elements.toggleUi.disabled = !data.instanceReady || !data.uiSupported || data.scanning || state.loading;
    elements.toggleUi.textContent = data.uiVisible ? 'Hide Plugin UI' : 'Open Plugin UI';
    elements.toggleUi.title = data.uiSupported ? '' : 'Selected plugin does not expose a custom UI';
}

function renderCatalog(data) {
    elements.pluginList.innerHTML = '';
    data.catalog.forEach(plugin => {
        const wrapper = document.createElement('div');
        wrapper.className = 'plugin-item';
        if (data.currentPlugin && data.currentPlugin.index === plugin.index) {
            wrapper.classList.add('active');
        }
        const title = document.createElement('strong');
        title.textContent = plugin.displayName;
        const vendor = document.createElement('div');
        vendor.textContent = (plugin.vendorName || 'Unknown vendor') + ' · ' + plugin.format;
        const loadButton = document.createElement('button');
        loadButton.textContent = 'Load';
        loadButton.addEventListener('click', async () => {
            state.loading = true;
            render();
            try {
                await ipcRenderer.invoke('host:load-plugin', { index: plugin.index });
                await refreshState();
            } catch (error) {
                console.error(error);
                alert('Failed to load plugin: ' + error.message);
            } finally {
                state.loading = false;
                render();
            }
        });
        wrapper.appendChild(title);
        wrapper.appendChild(vendor);
        wrapper.appendChild(loadButton);
        elements.pluginList.appendChild(wrapper);
    });
}

function renderPluginDetails(data) {
    if (data.currentPlugin) {
        elements.pluginTitle.textContent = data.currentPlugin.displayName;
        let meta = data.currentPlugin.bundlePath;
        if (!data.uiSupported) {
            meta += ' - No custom UI reported';
        }
        elements.pluginMeta.textContent = meta;
    } else {
        elements.pluginTitle.textContent = 'No Plugin Loaded';
        elements.pluginMeta.textContent = 'Select a plugin to instantiate it.';
    }

    elements.parameterList.innerHTML = '';
    if (!data.parameters.length) {
        const empty = document.createElement('p');
        empty.textContent = data.instanceReady
            ? 'Plugin does not expose editable parameters.'
            : 'Instantiate a plugin to inspect parameters.';
        elements.parameterList.appendChild(empty);
        return;
    }

    data.parameters.forEach(param => {
        const row = document.createElement('div');
        row.className = 'param';
        const label = document.createElement('div');
        label.textContent = param.name + ' (#' + param.id + ')';
        const value = document.createElement('div');
        value.textContent = param.value !== null ? param.value.toFixed(3) : 'n/a';
        const slider = document.createElement('input');
        slider.type = 'range';
        slider.min = param.minValue;
        slider.max = param.maxValue;
        const step = (param.maxValue - param.minValue) / 200;
        slider.step = step > 0 ? step : 0.01;
        slider.value = param.value ?? param.defaultValue ?? 0;
        slider.disabled = !param.isAutomatable || param.isReadonly || state.loading;
        slider.addEventListener('input', () => {
            value.textContent = Number(slider.value).toFixed(3);
        });
        slider.addEventListener('change', async () => {
            try {
                await ipcRenderer.invoke('host:set-parameter', { id: param.id, value: Number(slider.value) });
                await refreshState();
            } catch (error) {
                console.error(error);
                alert('Failed to update parameter: ' + error.message);
            }
        });
        row.appendChild(label);
        row.appendChild(value);
        row.appendChild(slider);
        elements.parameterList.appendChild(row);
    });
}

refreshState().catch(error => {
    console.error(error);
    elements.status.textContent = 'Failed to contact backend';
});
`;

async function createMainWindow(): Promise<void> {
    const window = new BrowserWindow({
        width: 1200,
        height: 720,
        webPreferences: {
            nodeIntegration: true,
            contextIsolation: false,
        },
    });

    await window.loadURL(createHtmlDataUrl(RENDERER_SCRIPT));
}

ipcMain.handle('host:get-state', () => host.getState());
ipcMain.handle('host:scan', async () => {
    await host.scanPlugins();
    return host.getState();
});
ipcMain.handle('host:load-plugin', async (_event: IpcMainInvokeEvent, args: { index: number }) => {
    await host.loadPlugin(args.index);
    return host.getState();
});
ipcMain.handle('host:toggle-ui', async () => {
    console.log('[remidy-node] main: host:toggle-ui');
    await host.togglePluginUI();
    return host.getState();
});
ipcMain.handle('host:set-parameter', async (_event: IpcMainInvokeEvent, args: { id: number; value: number }) => {
    host.setParameter(args.id, args.value);
    return host.getState();
});

ipcMain.on('host:renderer-log', (_event: IpcMainInvokeEvent, payload: { level?: string; args?: string[] }) => {
    const level = payload?.level ?? 'log';
    const args = payload?.args ?? [];
    const target = (console as unknown as Record<string, (...a: unknown[]) => void>)[level];
    if (typeof target === 'function') {
        target('[renderer]', ...args);
    } else {
        console.log('[renderer]', ...args);
    }
});

app.whenReady().then(async () => {
    host.scanPlugins().catch(() => { /* ignore */ });
    await createMainWindow();
});

app.on('window-all-closed', () => {
    host.dispose();
    if (process.platform !== 'darwin') {
        app.quit();
    }
});
