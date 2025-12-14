# @remidy/node

Node.js/TypeScript bindings for the [remidy](https://github.com/atsushieno/uapmd) audio plugin hosting library.

## Features

- **Plugin Discovery**: Scan for VST3, CLAP, LV2, and AudioUnit plugins
- **Plugin Instantiation**: Load and configure plugin instances
- **Parameter Control**: Query and manipulate plugin parameters
- **Catalog Management**: Save and load plugin catalogs for fast startup
- **TypeScript Support**: Full type definitions included
- **Cross-Platform**: Works on macOS, Linux, and Windows

## Installation

```bash
npm install @remidy/node
```

### Building from Source

1. Build the native C API library:
```bash
cd js
chmod +x build-native.sh
./build-native.sh
```

2. Build the TypeScript bindings:
```bash
npm install
npm run build
```

## Quick Start

### Scanning for Plugins

```typescript
import { PluginScanTool } from '@remidy/node';

const scanTool = new PluginScanTool();

// Perform plugin scan
scanTool.performScanning();

// Get catalog
const catalog = scanTool.catalog;
console.log(`Found ${catalog.count} plugins`);

// Iterate through plugins
for (const plugin of catalog) {
    console.log(`${plugin.displayName} (${plugin.format})`);
    console.log(`  Vendor: ${plugin.vendorName}`);
    console.log(`  Bundle: ${plugin.bundlePath}`);
}

// Save cache for faster subsequent scans
scanTool.saveCache('plugin-cache.json');

scanTool.dispose();
```

### Loading a Plugin Instance

```typescript
import { PluginScanTool, PluginInstance } from '@remidy/node';

const scanTool = new PluginScanTool();

// Load from cache
scanTool.catalog.load('plugin-cache.json');

// Get a plugin
const plugin = scanTool.catalog.getPluginAt(0);

// Find the format
const formats = scanTool.getFormats();
const format = formats.find(f => f.name === plugin.format);

// Create instance
const instance = new PluginInstance(format, plugin);

// Configure
instance.configure({
    sampleRate: 48000,
    bufferSizeInSamples: 512,
    mainInputChannels: 2,
    mainOutputChannels: 2,
});

// Start processing
instance.startProcessing();

// ... do work ...

// Stop and cleanup
instance.stopProcessing();
instance.dispose();
scanTool.dispose();
```

### Working with Parameters

```typescript
// Get all parameters
const params = instance.getParameters();

params.forEach(param => {
    console.log(`${param.name}: ${param.minValue} - ${param.maxValue}`);

    // Get current value
    const value = instance.getParameterValue(param.id);
    console.log(`  Current: ${value}`);

    // Set to default
    if (!param.isReadonly) {
        instance.setParameterValue(param.id, param.defaultValue);
    }
});
```

## API Reference

### PluginScanTool

Main interface for discovering and managing plugins.

#### Methods

- `constructor()`: Create a new scan tool
- `performScanning()`: Scan for plugins on the system
- `getFormats()`: Get available plugin formats (VST3, CLAP, LV2, AU)
- `saveCache(path: string)`: Save catalog to cache file
- `setCacheFile(path: string)`: Set default cache file path
- `filterByFormat(entries, format)`: Filter plugins by format
- `dispose()`: Clean up resources

#### Properties

- `catalog`: Access to the plugin catalog

### PluginCatalog

Manages the collection of discovered plugins.

#### Methods

- `load(path: string)`: Load catalog from file
- `save(path: string)`: Save catalog to file
- `getPluginAt(index: number)`: Get plugin by index
- `getPlugins()`: Get all plugins as array
- `clear()`: Clear all entries
- `dispose()`: Clean up resources

#### Properties

- `count`: Number of plugins in catalog

### PluginCatalogEntry

Information about a discovered plugin.

#### Properties

- `format`: Plugin format (e.g., "VST3", "CLAP", "LV2", "AU")
- `pluginId`: Unique plugin identifier
- `displayName`: Human-readable plugin name
- `vendorName`: Plugin vendor/manufacturer
- `productUrl`: Product URL (if available)
- `bundlePath`: File system path to plugin bundle

### PluginInstance

Represents a loaded plugin instance.

#### Methods

- `constructor(format, entry)`: Create new instance
- `configure(config)`: Configure audio settings
- `startProcessing()`: Start audio processing
- `stopProcessing()`: Stop audio processing
- `getParameters()`: Get all parameters
- `getParameterInfo(index)`: Get parameter info by index
- `getParameterValue(id)`: Get parameter value
- `setParameterValue(id, value)`: Set parameter value
- `dispose()`: Clean up resources

#### Properties

- `parameterCount`: Number of parameters

### ConfigurationRequest

Audio configuration for plugin instance.

#### Properties

- `sampleRate?: number`: Sample rate (default: 44100)
- `bufferSizeInSamples?: number`: Buffer size (default: 4096)
- `offlineMode?: boolean`: Offline processing mode
- `mainInputChannels?: number`: Number of input channels
- `mainOutputChannels?: number`: Number of output channels

### ParameterInfo

Information about a plugin parameter.

#### Properties

- `id`: Parameter ID
- `name`: Display name
- `minValue`: Minimum value
- `maxValue`: Maximum value
- `defaultValue`: Default value
- `isAutomatable`: Can be automated
- `isReadonly`: Is read-only

### GUI Utilities

The package also exposes light bindings for remidy-gui helpers:

- `ContainerWindow`: Create native top-level windows (HWND/NSView/X11) that plugins can embed their editors into.
- `GLContextGuard`: RAII helper that captures the current OpenGL context and restores it when disposed, mirroring the native host’s guard against misbehaving plugins.

### ContainerWindow

Native window container for plugin UI editors.

#### Methods

- `constructor(title, width, height, onClose?)`: Create a new container window
- `show(visible)`: Show or hide the window
- `resize(width, height)`: Resize the window
- `getBounds()`: Get window bounds (x, y, width, height)
- `dispose()`: Clean up resources

#### Properties

- `nativeHandle`: Platform-specific window handle (HWND on Windows, NSView* on macOS, XID on Linux)

### Bounds

Window bounds information.

#### Properties

- `x`: X position
- `y`: Y position
- `width`: Width in pixels
- `height`: Height in pixels

## Examples

See the `examples/` directory for complete working examples:

- `scan-plugins.ts`: Plugin scanning and catalog management
- `plugin-instance.ts`: Loading and controlling plugin instances
- `web-plugin-host/`: Browser-based UI that mirrors the native `remidy-plugin-host` tool
- `electron-plugin-host/`: Desktop Electron UI that can open plugin editors via native container windows

Run examples:

```bash
npm run build
node dist/examples/scan-plugins.js
node dist/examples/plugin-instance.js
# start the web UI server (then open http://localhost:5173)
npm run example:web-host
# launch the Electron desktop host
npm run example:electron-host
```

## Architecture

This library consists of two layers:

1. **Native C API** (`native/`): C wrapper around the C++ remidy library
2. **TypeScript Bindings** (`src/`): High-level TypeScript API using [koffi](https://github.com/Koromix/koffi) for FFI

## Requirements

- Node.js >= 18
- C++20 compiler
- CMake >= 3.21
- remidy library built and available

## License

MIT

## Contributing

Contributions welcome! This is a work in progress.

## Related Projects

- [remidy](https://github.com/atsushieno/uapmd): The underlying C++ audio plugin hosting library
- [koffi](https://github.com/Koromix/koffi): Fast and easy-to-use Node.js FFI library
