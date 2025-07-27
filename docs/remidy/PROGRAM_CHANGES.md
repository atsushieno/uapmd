
# Program Changes in UAPMD

(It is a design documentation before actually determining our API.)

Program changes will be mapped to/from plugin presets API. But every plugin format has different ways to support presets:

- VST3: [Presets and program lists](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Presets+Program+Lists/Index.html#program-lists) should be used. `IUnitInfo` provides access to "program list" and "program index". The program list maps to bank, and program index maps to program change.
- AudioUnit: According to this [AUv2 documentation](https://developer.apple.com/library/archive/technotes/tn2157/_index.html#//apple_ref/doc/uid/DTS40011953) it should be mapped to `kAudioUnitProperty_FactoryPresets`, although [AudioToolbox documentation](https://developer.apple.com/documentation/audiotoolbox/auaudiounitpreset?language=objc) says `fullState` corresponds to `kAudioUnitProperty_ClassInfo`.
- LV2: [LV2 Presets](https://lv2plug.in/ns/ext/presets) should be used. However they have no index. It is problematic because we usually expect program number as a stable information (SMF2 would just store program changes by index), but an LV2 preset may be mapped to different index every time it is loaded or even instantiated. Even if we have stable way to map presets from the manifest e.g. order of items in the metadata, the items may be inserted.
- CLAP: CLAP organizes presets in somewhat unique way. `factory/preset-discovery.h`](https://github.com/free-audio/clap/blob/main/include/clap/factory/preset-discovery.h) provides ways to get presets catalog and [`ext/preset-load.h`](https://github.com/free-audio/clap/blob/main/include/clap/ext/preset-load.h) lets plugins load them. Just like LV2, there is no stable way to map from program number to a preset, so it is problematic in CLAP too.

