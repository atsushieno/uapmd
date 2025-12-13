#!/usr/bin/env node
/**
 * Example: Scanning for audio plugins
 *
 * This example demonstrates how to:
 * 1. Create a plugin scan tool
 * 2. Perform plugin scanning
 * 3. List discovered plugins
 * 4. Save/load plugin cache
 */

import { PluginScanTool } from '../src';
import * as path from 'path';
import * as os from 'os';

async function main() {
    console.log('Remidy Plugin Scanner Example\n');

    const scanTool = new PluginScanTool();

    try {
        // Set cache file location
        const cacheFile = path.join(os.homedir(), '.remidy', 'plugin-cache.json');
        console.log(`Cache file: ${cacheFile}\n`);

        // Get available plugin formats
        const formats = scanTool.getFormats();
        console.log('Available plugin formats:');
        formats.forEach(format => {
            console.log(`  - ${format.name}`);
        });
        console.log();

        // Perform scanning
        console.log('Scanning for plugins...');
        const startTime = Date.now();
        scanTool.performScanning();
        const scanDuration = Date.now() - startTime;
        console.log(`Scan completed in ${scanDuration}ms\n`);

        // Get catalog
        const catalog = scanTool.catalog;
        console.log(`Found ${catalog.count} plugins:\n`);

        // List all plugins
        for (const plugin of catalog) {
            console.log(`${plugin.displayName}`);
            console.log(`  Format: ${plugin.format}`);
            console.log(`  Vendor: ${plugin.vendorName}`);
            console.log(`  ID: ${plugin.pluginId}`);
            console.log(`  Bundle: ${plugin.bundlePath}`);
            if (plugin.productUrl) {
                console.log(`  URL: ${plugin.productUrl}`);
            }
            console.log();
        }

        // Filter by format
        console.log('\nPlugins by format:');
        formats.forEach(format => {
            const plugins = catalog.getPlugins();
            const filtered = scanTool.filterByFormat(plugins, format.name);
            console.log(`  ${format.name}: ${filtered.length} plugins`);
        });

        // Save cache
        console.log(`\nSaving cache to ${cacheFile}...`);
        scanTool.saveCache(cacheFile);
        console.log('Cache saved successfully');

    } catch (error) {
        console.error('Error:', error);
        process.exit(1);
    } finally {
        scanTool.dispose();
    }
}

main();
