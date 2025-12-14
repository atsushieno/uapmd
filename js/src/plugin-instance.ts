import * as ffi from './ffi';
import { PluginCatalogEntry } from './plugin-catalog';
import { PluginFormat } from './plugin-scan-tool';

export type NativeHandle = Buffer | bigint | number;

function normalizeHandle(handle?: NativeHandle | null): bigint {
    if (!handle) {
        return 0n;
    }
    if (typeof handle === 'bigint') {
        return handle;
    }
    if (typeof handle === 'number') {
        return BigInt(handle);
    }
    if (Buffer.isBuffer(handle)) {
        if (handle.length >= 8) {
            return handle.readBigUInt64LE(0);
        }
        if (handle.length >= 4) {
            return BigInt(handle.readUInt32LE(0));
        }
        throw new Error('Unsupported native handle length');
    }
    throw new Error('Unsupported native handle type');
}

export interface ConfigurationRequest {
    sampleRate?: number;
    bufferSizeInSamples?: number;
    offlineMode?: boolean;
    mainInputChannels?: number;
    mainOutputChannels?: number;
}

export interface ParameterInfo {
    id: number;
    name: string;
    minValue: number;
    maxValue: number;
    defaultValue: number;
    isAutomatable: boolean;
    isReadonly: boolean;
}

export interface PluginUICreateOptions {
    floating?: boolean;
    parentHandle?: NativeHandle;
    onResize?: (width: number, height: number) => boolean;
}

export class PluginInstance {
    private handle: ffi.PluginInstanceHandle;
    private uiResizeCallback: ((width: number, height: number) => boolean) | null = null;

    /**
     * Create a plugin instance synchronously (not recommended).
     * WARNING: This may deadlock if the plugin requires main thread initialization.
     * Use PluginInstance.createAsync() instead for proper main thread instantiation.
     *
     * @deprecated Use PluginInstance.createAsync() instead
     */
    constructor(format: PluginFormat, entry: PluginCatalogEntry) {
        const handle = ffi.remidy_instance_create(
            format._getHandle(),
            entry._getHandle()
        );

        if (!handle) {
            throw new Error('Failed to create plugin instance');
        }

        this.handle = handle;
    }

    /**
     * Internal factory method to create instance from handle
     */
    private static fromHandle(handle: ffi.PluginInstanceHandle): PluginInstance {
        const instance = Object.create(PluginInstance.prototype);
        instance.handle = handle;
        return instance;
    }

    /**
     * Create a plugin instance asynchronously (recommended).
     * This ensures the plugin is instantiated on the main thread,
     * which is required by many plugin formats.
     */
    static async createAsync(format: PluginFormat, entry: PluginCatalogEntry): Promise<PluginInstance> {
        return new Promise((resolve, reject) => {
            // Use setImmediate to ensure we're on the main event loop
            setImmediate(() => {
                try {
                    const callback = (instance: ffi.PluginInstanceHandle | null, error: string | null, _userData: any) => {
                        if (error) {
                            reject(new Error(`Failed to create plugin instance: ${error}`));
                        } else if (!instance) {
                            reject(new Error('Failed to create plugin instance: unknown error'));
                        } else {
                            resolve(PluginInstance.fromHandle(instance));
                        }
                    };

                    // Call the async create function
                    ffi.remidy_instance_create_async(
                        format._getHandle(),
                        entry._getHandle(),
                        callback,
                        null
                    );
                } catch (error) {
                    reject(error);
                }
            });
        });
    }

    dispose(): void {
        if (this.handle) {
            ffi.remidy_instance_destroy(this.handle);
            this.handle = null as any;
        }
    }

    configure(config: ConfigurationRequest): void {
        const nativeConfig = {
            sample_rate: config.sampleRate ?? 44100,
            buffer_size_in_samples: config.bufferSizeInSamples ?? 4096,
            offline_mode: config.offlineMode ?? false,
            main_input_channels: config.mainInputChannels ?? 0,
            main_output_channels: config.mainOutputChannels ?? 0,
            has_main_input_channels: config.mainInputChannels !== undefined,
            has_main_output_channels: config.mainOutputChannels !== undefined,
        };

        const result = ffi.remidy_instance_configure(this.handle, nativeConfig);
        if (result !== ffi.StatusCode.OK) {
            throw new Error(`Failed to configure plugin instance: status ${result}`);
        }
    }

    startProcessing(): void {
        const result = ffi.remidy_instance_start_processing(this.handle);
        if (result !== ffi.StatusCode.OK) {
            throw new Error(`Failed to start processing: status ${result}`);
        }
    }

    stopProcessing(): void {
        const result = ffi.remidy_instance_stop_processing(this.handle);
        if (result !== ffi.StatusCode.OK) {
            throw new Error(`Failed to stop processing: status ${result}`);
        }
    }

    get parameterCount(): number {
        return ffi.remidy_instance_get_parameter_count(this.handle);
    }

    getParameterInfo(index: number): ParameterInfo | null {
        const info: any = {};
        const result = ffi.remidy_instance_get_parameter_info(this.handle, index, info);

        if (result !== ffi.StatusCode.OK) {
            return null;
        }

        // Convert char array to string
        const nameBuffer = Buffer.from(info.name as Buffer);
        const nullIndex = nameBuffer.indexOf(0);
        const name = nameBuffer.toString('utf8', 0, nullIndex >= 0 ? nullIndex : undefined);

        return {
            id: info.id,
            name: name,
            minValue: info.min_value,
            maxValue: info.max_value,
            defaultValue: info.default_value,
            isAutomatable: info.is_automatable,
            isReadonly: info.is_readonly,
        };
    }

    getParameters(): ParameterInfo[] {
        const count = this.parameterCount;
        const params: ParameterInfo[] = [];

        for (let i = 0; i < count; i++) {
            const param = this.getParameterInfo(i);
            if (param) {
                params.push(param);
            }
        }

        return params;
    }

    getParameterValue(paramId: number): number | null {
        const value = [0.0];  // koffi expects an array for out parameters
        const result = ffi.remidy_instance_get_parameter_value(this.handle, paramId, value);

        if (result !== ffi.StatusCode.OK) {
            return null;
        }

        return value[0];
    }

    setParameterValue(paramId: number, value: number): void {
        const result = ffi.remidy_instance_set_parameter_value(this.handle, paramId, value);
        if (result !== ffi.StatusCode.OK) {
            throw new Error(`Failed to set parameter ${paramId} to ${value}: status ${result}`);
        }
    }

    hasUISupport(): boolean {
        return ffi.remidy_instance_has_ui(this.handle);
    }

    createUI(options: PluginUICreateOptions = {}): void {
        const parentHandle = normalizeHandle(options.parentHandle);
        const resizeCallback = options.onResize
            ? (width: number, height: number) => options.onResize!(width, height)
            : null;

        const status = ffi.remidy_instance_create_ui(
            this.handle,
            options.floating ?? false,
            parentHandle,
            resizeCallback,
            null
        );
        if (status !== ffi.StatusCode.OK) {
            throw new Error(`Failed to create plugin UI: status ${status}`);
        }
        this.uiResizeCallback = resizeCallback;
    }

    destroyUI(): void {
        ffi.remidy_instance_destroy_ui(this.handle);
        this.uiResizeCallback = null;
    }

    showUI(): void {
        const status = ffi.remidy_instance_show_ui(this.handle);
        if (status !== ffi.StatusCode.OK) {
            throw new Error(`Failed to show plugin UI: status ${status}`);
        }
    }

    hideUI(): void {
        const status = ffi.remidy_instance_hide_ui(this.handle);
        if (status !== ffi.StatusCode.OK) {
            throw new Error(`Failed to hide plugin UI: status ${status}`);
        }
    }

    getUISize(): { width: number; height: number } | null {
        const width = [0];
        const height = [0];
        const status = ffi.remidy_instance_get_ui_size(this.handle, width, height);
        if (status === ffi.StatusCode.NOT_SUPPORTED) {
            return null;
        }
        if (status !== ffi.StatusCode.OK) {
            throw new Error(`Failed to query plugin UI size: status ${status}`);
        }
        return { width: width[0], height: height[0] };
    }

    setUISize(width: number, height: number): void {
        const status = ffi.remidy_instance_set_ui_size(this.handle, width, height);
        if (status !== ffi.StatusCode.OK) {
            throw new Error(`Failed to resize plugin UI: status ${status}`);
        }
    }

    canUIResize(): boolean {
        return ffi.remidy_instance_can_ui_resize(this.handle);
    }
}
