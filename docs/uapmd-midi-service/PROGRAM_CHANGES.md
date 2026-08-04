
# Program Changes in UAPMD

(It is a design documentation before actually determining our API.)

## Preset Resources

Presets are stored in certain locations for each format:

- VST3: https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Locations+Format/Preset+Locations.html
- AU: https://developer.apple.com/library/archive/technotes/tn2157/_index.html#//apple_ref/doc/uid/DTS40011953
- LV2: https://lv2plug.in/pages/filesystem-hierarchy-standard.html (also see https://github.com/lv2/lilv/issues/55 though)
- CLAP: https://github.com/free-audio/clap/blob/main/include/clap/factory/preset-discovery.h

## Plugin Format APIs

Program changes will be mapped to/from plugin presets API. But every plugin format has different ways to support presets:

- VST3: [Presets and program lists](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Presets+Program+Lists/Index.html#program-lists) should be used. `IUnitInfo` provides access to "program list" and "program index". The program list maps to bank, and program index maps to program change.
- AudioUnit: According to this [AUv2 documentation](https://developer.apple.com/library/archive/technotes/tn2157/_index.html#//apple_ref/doc/uid/DTS40011953) it should be mapped to `kAudioUnitProperty_FactoryPresets`, although [AudioToolbox documentation](https://developer.apple.com/documentation/audiotoolbox/auaudiounitpreset?language=objc) says `fullState` corresponds to `kAudioUnitProperty_ClassInfo`.
- LV2: [LV2 Presets](https://lv2plug.in/ns/ext/presets) should be used. However they have no index. It is problematic because we usually expect program number as a stable information (SMF2 would just store program changes by index), but an LV2 preset may be mapped to different index every time it is loaded or even instantiated. Even if we have stable way to map presets from the manifest e.g. order of items in the metadata, the items may be inserted.
- CLAP: CLAP organizes presets in somewhat unique way. `factory/preset-discovery.h`](https://github.com/free-audio/clap/blob/main/include/clap/factory/preset-discovery.h) provides ways to get presets catalog and [`ext/preset-load.h`](https://github.com/free-audio/clap/blob/main/include/clap/ext/preset-load.h) lets plugins load them. Just like LV2, there is no stable way to map from program number to a preset, so it is problematic in CLAP too.

## Commonized API

Our typical use of presets API is (1) query the presets and present them to the users, (2) let user choose one, and (3) load it (which most likely resets the state).

We need some customization point to get program index from stable ID string (which seems available on all those plugin formats).

- `PluginPresetSupport` class
  - `isIndexStable() : bool` (true for VST3)
  - `isIndexId() : bool` (true for VST3 and AU)
  - `getPresetIndexForId(std::string id) : int32_t`
  - `getPresetCount() : int32_t`
  - `getPresetInfo(int32_t index) : PresetInfo`
  - `load(int32_t index)`
- `PluginPresetInfo` class
  - `id : std::string`
  - `name : std::string`
  - `bank : int32_t` - if the format API supports
  - `index : int32_t`
