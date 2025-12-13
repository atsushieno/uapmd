import * as ffi from './ffi';

/**
 * Initialize the remidy EventLoop for Node.js
 * This must be called before creating any plugin instances
 */
let initialized = false;

export function initializeEventLoop(): void {
    if (initialized) {
        return;
    }

    // Create a callback that will enqueue tasks on Node.js event loop
    const enqueueCallback = (taskPtr: any, userDataPtr: any, _context: any) => {
        // Schedule the task to run on the next tick of the Node.js event loop
        setImmediate(() => {
            // Call the C++ task function with its user data
            taskPtr(userDataPtr);
        });
    };

    // Initialize the EventLoop with our Node.js adapter
    ffi.remidy_eventloop_init_nodejs(enqueueCallback, null);
    initialized = true;
}

// Auto-initialize when the module is loaded
initializeEventLoop();
