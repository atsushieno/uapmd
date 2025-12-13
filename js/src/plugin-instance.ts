import * as ffi from './ffi';
import { PluginCatalogEntry } from './plugin-catalog';
import { PluginFormat } from './plugin-scan-tool';

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

export class PluginInstance {
    private handle: ffi.PluginInstanceHandle;

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
        const nameBytes = info.name as number[];
        let name = '';
        for (let i = 0; i < nameBytes.length && nameBytes[i] !== 0; i++) {
            name += String.fromCharCode(nameBytes[i]);
        }

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
        const value: any = { value: 0 };
        const result = ffi.remidy_instance_get_parameter_value(this.handle, paramId, value);

        if (result !== ffi.StatusCode.OK) {
            return null;
        }

        return value.value;
    }

    setParameterValue(paramId: number, value: number): void {
        const result = ffi.remidy_instance_set_parameter_value(this.handle, paramId, value);
        if (result !== ffi.StatusCode.OK) {
            throw new Error(`Failed to set parameter ${paramId} to ${value}: status ${result}`);
        }
    }
}
