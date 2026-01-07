// Example: Plugin State Save/Load
// This example demonstrates how to save and load plugin state files from JavaScript

import { PluginInstance, sequencer } from '../src/remidy-bridge.js';

// Example 1: Using the PluginInstance class
function exampleWithPluginInstance() {
    log("=== Example 1: Using PluginInstance class ===");

    // Create a plugin instance (replace with actual format and plugin ID)
    const instance = new PluginInstance("VST3", "com.example.plugin");

    // Modify some parameters
    const params = instance.getParameters();
    if (params.length > 0) {
        instance.setParameterValue(params[0].id, 0.75);
        log(`Set parameter ${params[0].name} to 0.75`);
    }

    // Save the plugin state
    const saveResult = instance.saveState("/tmp/my-plugin-state.state");
    if (saveResult.success) {
        log(`✓ Plugin state saved to: ${saveResult.filepath}`);
    } else {
        log(`✗ Failed to save state: ${saveResult.error}`);
    }

    // Modify the parameter again
    if (params.length > 0) {
        instance.setParameterValue(params[0].id, 0.25);
        log(`Changed parameter ${params[0].name} to 0.25`);
    }

    // Load the previously saved state
    const loadResult = instance.loadState("/tmp/my-plugin-state.state");
    if (loadResult.success) {
        log(`✓ Plugin state loaded from: ${loadResult.filepath}`);

        // Verify the parameter was restored
        if (params.length > 0) {
            const restoredValue = instance.getParameterValue(params[0].id);
            log(`Parameter ${params[0].name} restored to: ${restoredValue}`);
        }
    } else {
        log(`✗ Failed to load state: ${loadResult.error}`);
    }

    instance.dispose();
}

// Example 2: Using the sequencer API
function exampleWithSequencer() {
    log("\n=== Example 2: Using sequencer API ===");

    // Get existing instances
    const instanceIds = sequencer.getInstanceIds();
    if (instanceIds.length === 0) {
        log("No plugin instances found. Create one first!");
        return;
    }

    const instanceId = instanceIds[0];
    const pluginName = sequencer.getPluginName(instanceId);
    const pluginFormat = sequencer.getPluginFormat(instanceId);

    log(`Working with: ${pluginName} (${pluginFormat})`);

    // Save state using sequencer
    const filepath = `/tmp/${pluginName.replace(/\s+/g, '_')}.state`;
    const saveResult = sequencer.savePluginState(instanceId, filepath);

    if (saveResult.success) {
        log(`✓ Saved state to: ${saveResult.filepath}`);
    } else {
        log(`✗ Failed to save: ${saveResult.error}`);
    }

    // Load state using sequencer
    const loadResult = sequencer.loadPluginState(instanceId, filepath);

    if (loadResult.success) {
        log(`✓ Loaded state from: ${loadResult.filepath}`);
    } else {
        log(`✗ Failed to load: ${loadResult.error}`);
    }
}

// Example 3: Batch save/load all plugin instances
function exampleBatchSaveLoad() {
    log("\n=== Example 3: Batch Save/Load All Instances ===");

    const instanceIds = sequencer.getInstanceIds();
    const savedStates = [];

    // Save all plugin states
    log(`Saving ${instanceIds.length} plugin states...`);
    for (const instanceId of instanceIds) {
        const pluginName = sequencer.getPluginName(instanceId);
        const filepath = `/tmp/batch_${instanceId}_${pluginName.replace(/\s+/g, '_')}.state`;

        const result = sequencer.savePluginState(instanceId, filepath);
        if (result.success) {
            log(`  ✓ Saved instance ${instanceId}: ${pluginName}`);
            savedStates.push({ instanceId, filepath });
        } else {
            log(`  ✗ Failed to save instance ${instanceId}: ${result.error}`);
        }
    }

    // Load all saved states
    log(`\nLoading ${savedStates.length} plugin states...`);
    for (const { instanceId, filepath } of savedStates) {
        const result = sequencer.loadPluginState(instanceId, filepath);
        if (result.success) {
            log(`  ✓ Loaded instance ${instanceId}`);
        } else {
            log(`  ✗ Failed to load instance ${instanceId}: ${result.error}`);
        }
    }
}

// Run the examples
try {
    // Uncomment the example you want to run:

    // exampleWithPluginInstance();
    exampleWithSequencer();
    // exampleBatchSaveLoad();

} catch (error) {
    log(`Error: ${error.message}`);
}
