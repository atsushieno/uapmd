function findPlugin(format, displayName) {
    const count = __remidy_catalog_get_count();
    for (let i = 0; i < count; ++i) {
        const p = __remidy_catalog_get_plugin_at(i);
        if (p && p.format === format && p.displayName === displayName)
            return p;
    }
    throw new Error(`Plugin not found: ${format} ${displayName}`);
}

const trackIndex = 0;

const dexed = findPlugin("VST3", "Dexed");
const ripplerX = findPlugin("VST3", "RipplerX");

const dexedInstanceId = __remidy_instance_create(dexed.format, dexed.pluginId, trackIndex);
if (dexedInstanceId < 0)
    throw new Error("Failed to instantiate Dexed");

const ripplerInstanceId = __remidy_instance_create(ripplerX.format, ripplerX.pluginId, trackIndex);
if (ripplerInstanceId < 0)
    throw new Error("Failed to instantiate RipplerX");

__remidy_instance_show_details(dexedInstanceId);
__remidy_timeline_show_track_graph(trackIndex);

({
    trackIndex,
    dexedInstanceId,
    ripplerInstanceId
});
