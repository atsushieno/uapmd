import { ContainerWindow, GLContextGuard, PluginInstance, PluginScanTool } from '../src';

async function selectPlugin(tool: PluginScanTool) {
    const preferredName = process.env.REMODY_DEBUG_PLUGIN;
    const preferredFormat = process.env.REMODY_DEBUG_FORMAT;
    const plugins = tool.catalog.getPlugins().filter(p => !p.displayName.startsWith('#'));
    if (preferredName) {
        const match = plugins.find(p => p.displayName.includes(preferredName));
        if (match) {
            const format = tool.getFormats().find(f => f.name === match.format);
            return { plugin: match, format };
        }
    }
    for (const plugin of plugins) {
        const format = tool.getFormats().find(f => f.name === plugin.format);
        if (!format) continue;
        if (preferredFormat && format.name !== preferredFormat) continue;
        return { plugin, format };
    }
    return null;
}

async function main() {
    const tool = new PluginScanTool();
    tool.performScanning();
    const selection = await selectPlugin(tool);
    if (!selection) {
        console.error('No suitable plugin found');
        return;
    }
    const { plugin, format } = selection;

    const instance = await PluginInstance.createAsync(format!, plugin);
    instance.configure({ sampleRate: 48000, bufferSizeInSamples: 512 });
    if (!instance.hasUISupport()) {
        console.error('Plugin has no UI support');
        return;
    }

    const guard = new GLContextGuard();
    instance.createUI({ floating: true });
    instance.showUI();
    console.log('Floating UI requested');
    setTimeout(() => {
        instance.hideUI();
        process.exit(0);
    }, 5000);
}

main().catch(err => {
    console.error(err);
    process.exit(1);
});
