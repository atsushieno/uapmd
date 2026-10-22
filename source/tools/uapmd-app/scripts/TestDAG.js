function findPlugin(format, displayName) {
    const count = __remidy_catalog_get_count();
    for (let i = 0; i < count; ++i) {
        const p = __remidy_catalog_get_plugin_at(i);
        if (p && p.format === format && p.displayName === displayName)
            return p;
    }
    throw new Error(`Plugin not found: ${format} ${displayName}`);
}

const trackId = __remidy_sequencer_getTrackInfos()[0]?.trackId;
if (!trackId)
    throw new Error("TestDAG needs at least one track");

const dexed = findPlugin("VST3", "Dexed");
const ripplerX = findPlugin("VST3", "RipplerX");

const dexedInstanceId = __remidy_instance_create(dexed.format, dexed.pluginId, trackId);
if (dexedInstanceId < 0)
    throw new Error("Failed to instantiate Dexed");

const ripplerInstanceId = __remidy_instance_create(ripplerX.format, ripplerX.pluginId, trackId);
if (ripplerInstanceId < 0)
    throw new Error("Failed to instantiate RipplerX");

__remidy_instance_show_details(dexedInstanceId);
__remidy_timeline_show_track_graph(trackId);

({
    trackId,
    dexedInstanceId,
    ripplerInstanceId
});
