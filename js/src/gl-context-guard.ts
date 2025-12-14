import * as ffi from './ffi';

export class GLContextGuard {
    private handle: ffi.GLContextGuardHandle | null;

    constructor() {
        this.handle = ffi.remidy_gl_context_guard_create();
        if (!this.handle) {
            throw new Error('Failed to create GLContextGuard');
        }
    }

    dispose(): void {
        if (this.handle) {
            ffi.remidy_gl_context_guard_destroy(this.handle);
            this.handle = null;
        }
    }
}
