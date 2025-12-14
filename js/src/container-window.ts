import * as ffi from './ffi';

export interface Bounds {
    x: number;
    y: number;
    width: number;
    height: number;
}

export class ContainerWindow {
    private handle: ffi.ContainerWindowHandle | null;
    private activeShowCalls = 0;

    constructor(title: string, width: number, height: number) {
        this.handle = ffi.remidy_container_window_create(
            title,
            width,
            height,
            null,
            null
        );

        if (!this.handle) {
            throw new Error('Failed to create container window');
        }
    }

    dispose(): void {
        if (this.handle) {
            ffi.remidy_container_window_destroy(this.handle);
            this.handle = null;
        }
    }

    show(visible: boolean): void {
        if (!this.handle) {
            throw new Error('Container window is not available');
        }
        this.activeShowCalls++;
        if (this.activeShowCalls > 8) {
            console.error('[remidy-node] ContainerWindow.show re-entry detected', {
                visible,
                depth: this.activeShowCalls,
                stack: new Error().stack,
            });
        }
        ffi.remidy_container_window_show(this.handle, visible);
        this.activeShowCalls = Math.max(0, this.activeShowCalls - 1);
    }

    resize(width: number, height: number): void {
        if (!this.handle) {
            throw new Error('Container window is not available');
        }
        ffi.remidy_container_window_resize(this.handle, width, height);
    }

    getBounds(): Bounds {
        if (!this.handle) {
            throw new Error('Container window is not available');
        }
        return ffi.remidy_container_window_get_bounds(this.handle);
    }

    get nativeHandle(): bigint {
        if (!this.handle) {
            throw new Error('Container window is not available');
        }
        return ffi.remidy_container_window_get_handle(this.handle);
    }
}
