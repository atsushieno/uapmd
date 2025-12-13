import * as ffi from './ffi';
import { PluginCatalog } from './plugin-catalog';

export interface PluginFormatInfo {
    name: string;
}

export class PluginFormat {
    constructor(
        private handle: ffi.PluginFormatHandle,
        public readonly name: string
    ) {}

    /** @internal */
    _getHandle(): ffi.PluginFormatHandle {
        return this.handle;
    }
}

export class PluginScanTool {
    private handle: ffi.PluginScanToolHandle;
    private catalogInstance: PluginCatalog | null = null;

    constructor() {
        this.handle = ffi.remidy_scan_tool_create();
    }

    dispose(): void {
        if (this.handle) {
            ffi.remidy_scan_tool_destroy(this.handle);
            this.handle = null as any;
        }
    }

    get catalog(): PluginCatalog {
        if (!this.catalogInstance) {
            const catalogHandle = ffi.remidy_scan_tool_get_catalog(this.handle);
            // Note: The catalog is owned by the scan tool, so we don't want to destroy it
            this.catalogInstance = new PluginCatalog(catalogHandle, false);
        }
        return this.catalogInstance;
    }

    setCacheFile(path: string): void {
        ffi.remidy_scan_tool_set_cache_file(this.handle, path);
    }

    performScanning(): void {
        const result = ffi.remidy_scan_tool_perform_scanning(this.handle);
        if (result !== ffi.StatusCode.OK) {
            throw new Error(`Plugin scanning failed with status ${result}`);
        }
    }

    saveCache(path: string): void {
        const result = ffi.remidy_scan_tool_save_cache(this.handle, path);
        if (result !== ffi.StatusCode.OK) {
            throw new Error(`Failed to save cache to ${path}: status ${result}`);
        }
    }

    getFormats(): PluginFormat[] {
        const count = ffi.remidy_scan_tool_get_format_count(this.handle);
        const formats: PluginFormat[] = [];

        for (let i = 0; i < count; i++) {
            const info = ffi.remidy_scan_tool_get_format_at(this.handle, i);
            if (info.handle) {
                formats.push(new PluginFormat(info.handle, info.name));
            }
        }

        return formats;
    }

    filterByFormat(entries: import('./plugin-catalog').PluginCatalogEntry[], format: string): import('./plugin-catalog').PluginCatalogEntry[] {
        return entries.filter(entry => entry.format === format);
    }
}
