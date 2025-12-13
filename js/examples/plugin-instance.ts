#!/usr/bin/env node
/**
 * Example: Creating and configuring a plugin instance
 *
 * This example demonstrates how to:
 * 1. Load a plugin catalog
 * 2. Create a plugin instance
 * 3. Configure the plugin
 * 4. Query and manipulate parameters
 */

import { PluginScanTool, PluginInstance } from '../src';
import * as path from 'path';
import * as os from 'os';

async function main() {
    console.log('Remidy Plugin Instance Example\n');

    const scanTool = new PluginScanTool();

    try {
        // Load from cache if available
        const cacheFile = path.join(os.homedir(), '.remidy', 'plugin-cache.json');
        const catalog = scanTool.catalog;

        try {
            console.log(`Loading plugin cache from ${cacheFile}...`);
            catalog.load(cacheFile);
            console.log(`Loaded ${catalog.count} plugins from cache\n`);
        } catch (error) {
            console.log('Cache not found, performing scan...');
            scanTool.performScanning();
            console.log(`Found ${catalog.count} plugins\n`);
        }

        // Find a plugin to instantiate (first available)
        if (catalog.count === 0) {
            console.log('No plugins found!');
            return;
        }

        const plugin = catalog.getPluginAt(0);
        if (!plugin) {
            console.log('Failed to get plugin');
            return;
        }

        console.log(`Loading plugin: ${plugin.displayName}`);
        console.log(`  Format: ${plugin.format}`);
        console.log(`  Vendor: ${plugin.vendorName}\n`);

        // Find the format handler
        const formats = scanTool.getFormats();
        const format = formats.find(f => f.name === plugin.format);

        if (!format) {
            console.log(`Format ${plugin.format} not found`);
            return;
        }

        // Create instance
        console.log('Creating plugin instance...');
        const instance = new PluginInstance(format, plugin);

        // Configure the plugin
        console.log('Configuring plugin...');
        instance.configure({
            sampleRate: 48000,
            bufferSizeInSamples: 512,
            mainInputChannels: 2,
            mainOutputChannels: 2,
        });

        // List parameters
        console.log(`\nPlugin has ${instance.parameterCount} parameters:\n`);
        const params = instance.getParameters();

        params.slice(0, 10).forEach(param => {  // Show first 10 parameters
            console.log(`[${param.id}] ${param.name}`);
            console.log(`  Range: ${param.minValue} - ${param.maxValue}`);
            console.log(`  Default: ${param.defaultValue}`);
            console.log(`  Automatable: ${param.isAutomatable}, Read-only: ${param.isReadonly}`);

            // Get current value
            const currentValue = instance.getParameterValue(param.id);
            if (currentValue !== null) {
                console.log(`  Current value: ${currentValue}`);
            }
            console.log();
        });

        if (params.length > 10) {
            console.log(`... and ${params.length - 10} more parameters\n`);
        }

        // Try to set a parameter (first writable, automatable one)
        const writableParam = params.find(p => !p.isReadonly && p.isAutomatable);
        if (writableParam) {
            console.log(`Setting parameter "${writableParam.name}" to its default value...`);
            instance.setParameterValue(writableParam.id, writableParam.defaultValue);

            const newValue = instance.getParameterValue(writableParam.id);
            console.log(`New value: ${newValue}\n`);
        }

        // Start/stop processing
        console.log('Starting processing...');
        instance.startProcessing();
        console.log('Processing started');

        console.log('Stopping processing...');
        instance.stopProcessing();
        console.log('Processing stopped');

        // Cleanup
        instance.dispose();
        console.log('\nPlugin instance disposed successfully');

    } catch (error) {
        console.error('Error:', error);
        process.exit(1);
    } finally {
        scanTool.dispose();
    }
}

main();
