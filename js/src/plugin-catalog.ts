import * as ffi from './ffi';

export interface PluginCatalogEntryInfo {
    format: string;
    pluginId: string;
    displayName: string;
    vendorName: string;
    productUrl: string;
    bundlePath: string;
}

export class PluginCatalogEntry {
    constructor(private handle: ffi.PluginCatalogEntryHandle) {}

    get format(): string {
        return ffi.remidy_entry_get_format(this.handle);
    }

    get pluginId(): string {
        return ffi.remidy_entry_get_plugin_id(this.handle);
    }

    get displayName(): string {
        return ffi.remidy_entry_get_display_name(this.handle);
    }

    get vendorName(): string {
        return ffi.remidy_entry_get_vendor_name(this.handle);
    }

    get productUrl(): string {
        return ffi.remidy_entry_get_product_url(this.handle);
    }

    get bundlePath(): string {
        return ffi.remidy_entry_get_bundle_path(this.handle);
    }

    toJSON(): PluginCatalogEntryInfo {
        return {
            format: this.format,
            pluginId: this.pluginId,
            displayName: this.displayName,
            vendorName: this.vendorName,
            productUrl: this.productUrl,
            bundlePath: this.bundlePath,
        };
    }

    /** @internal */
    _getHandle(): ffi.PluginCatalogEntryHandle {
        return this.handle;
    }
}

export class PluginCatalog {
    private handle: ffi.PluginCatalogHandle;
    private owned: boolean;

    constructor(handle?: ffi.PluginCatalogHandle, owned: boolean = true) {
        if (handle) {
            this.handle = handle;
            this.owned = owned;
        } else {
            this.handle = ffi.remidy_catalog_create();
            this.owned = true;
        }
    }

    dispose(): void {
        if (this.owned && this.handle) {
            ffi.remidy_catalog_destroy(this.handle);
            this.handle = null as any;
        }
    }

    clear(): void {
        ffi.remidy_catalog_clear(this.handle);
    }

    load(path: string): void {
        const result = ffi.remidy_catalog_load(this.handle, path);
        if (result !== ffi.StatusCode.OK) {
            throw new Error(`Failed to load catalog from ${path}: status ${result}`);
        }
    }

    save(path: string): void {
        const result = ffi.remidy_catalog_save(this.handle, path);
        if (result !== ffi.StatusCode.OK) {
            throw new Error(`Failed to save catalog to ${path}: status ${result}`);
        }
    }

    get count(): number {
        return ffi.remidy_catalog_get_plugin_count(this.handle);
    }

    getPluginAt(index: number): PluginCatalogEntry | null {
        const entryHandle = ffi.remidy_catalog_get_plugin_at(this.handle, index);
        if (!entryHandle) {
            return null;
        }
        return new PluginCatalogEntry(entryHandle);
    }

    getPlugins(): PluginCatalogEntry[] {
        const count = this.count;
        const plugins: PluginCatalogEntry[] = [];
        for (let i = 0; i < count; i++) {
            const plugin = this.getPluginAt(i);
            if (plugin) {
                plugins.push(plugin);
            }
        }
        return plugins;
    }

    *[Symbol.iterator](): Iterator<PluginCatalogEntry> {
        const count = this.count;
        for (let i = 0; i < count; i++) {
            const plugin = this.getPluginAt(i);
            if (plugin) {
                yield plugin;
            }
        }
    }

    /** @internal */
    _getHandle(): ffi.PluginCatalogHandle {
        return this.handle;
    }
}
