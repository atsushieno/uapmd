// JavaScript Evaluation Engine for UAPMD
// Import the remidy bridge module for plugin access
import { PluginScanTool, sequencer } from 'remidy-bridge';

// The public API is available via the global 'uapmd' object:
//   uapmd.catalog.*      - Plugin discovery and management
//   uapmd.scanTool.*     - Plugin scanning and caching
//   uapmd.instancing.*   - Plugin instance creation
//   uapmd.instance(id)   - Get PluginInstance wrapper (getParameters, setParameterValue, etc.)
//   uapmd.sequencer.*    - Audio engine, MIDI, and transport
//
// You can use either the OO wrapper (PluginScanTool, sequencer) or
// the direct uapmd.* API. Both approaches work!
//
// Example using uapmd API:
//   const id = uapmd.instancing.create("VST3", pluginId);
//   const instance = uapmd.instance(id);
//   instance.showUI();
//   instance.setParameterValue(0, 0.5);

// Example: Create tracks for VST3 Dexed, LV2 RipplerX, CLAP Six Sines, AU Surge XT, and AUv3 Mela FX
const scanTool = new PluginScanTool();
const catalog = scanTool.catalog;

// Helper function to find a plugin by name and format
// First tries exact match (case-insensitive), then falls back to includes()
function findPlugin(displayName, format) {
    const plugins = catalog.getPlugins();
    const lowerName = displayName.toLowerCase();

    // Try exact match first
    let plugin = plugins.find(p =>
        p.displayName.toLowerCase() === lowerName &&
        p.format === format
    );

    // Fall back to includes() if exact match not found
    if (!plugin) {
        plugin = plugins.find(p =>
            p.displayName.toLowerCase().includes(lowerName) &&
            p.format === format
        );
    }

    return plugin;
}

// Start from a clean slate
log("Removing existing tracks...");
sequencer.clearTracks();

// Create five tracks with specific plugins
const tracksToCreate = [
    { name: 'Dexed', format: 'VST3' },
    { name: 'RipplerX', format: 'LV2' },
    { name: 'OctaSine', format: 'CLAP' },
    { name: 'Surge XT', format: 'AU' },
    { name: 'Mela FX', format: 'AU' }
];

const instanceIds = [];

for (const trackConfig of tracksToCreate) {
    const plugin = findPlugin(trackConfig.name, trackConfig.format);

    if (plugin) {
        // Use the low-level sequencer API directly, matching the C++ AppModel::instantiatePlugin pattern
        const trackIndex = sequencer.addTrack();
        if (trackIndex < 0) {
            log(`Failed to create track for ${plugin.displayName}`);
            continue;
        }

        const instanceId = sequencer.createPluginInstance(plugin.format, plugin.pluginId, trackIndex);
        if (instanceId >= 0) {
            instanceIds.push(instanceId);
            sequencer.showPluginUI(instanceId);
        } else {
            log(`Failed to create ${plugin.displayName}`);
        }
    } else {
        log(`Plugin not found: ${trackConfig.name} (${trackConfig.format})`);
    }
}

// List all active tracks
const tracks = sequencer.getTrackInfos();
log(`Created ${instanceIds.length} track(s):`);
for (const track of tracks) {
    for (const node of track.nodes) {
        log(`  Track ${track.trackIndex}: ${node.displayName} (${node.format})`);
    }
}

// Example: Send MIDI notes to all created instances
// Uncomment to test MIDI functionality
// log('\nSending test MIDI notes to all instances...');
// for (const instanceId of instanceIds) {
//     sequencer.sendNoteOn(instanceId, 60);  // C4
//     sequencer.sendNoteOn(instanceId, 64);  // E4
//     sequencer.sendNoteOn(instanceId, 67);  // G4
// }
