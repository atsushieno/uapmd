
DRAFT DRAFT DRAFT

# Presets API

Presets are not supported well in plugin hosting API in general. Most JUCE plugins return empty presets, and they work rather like program changes.
LV2 supports presets fairly well, but they are unordered and come without stable numeric IDs.

## Preset types

We will have to deal with factory presets and user presets.

- Factory presets are read-only. They do not have to exist at all.
- User presets may or may not exist. They can be writable, but hosts do not have to offer the saving functionality.

## Preset identification

In theory, a factory preset identity can be stable i.e. users can expect that the identity is backward-compatible and user music projects can leave the references to those factory presets.

User preset identity cannot be assured as backward-compatible, so when a music project uses some presets they should be stored as states instead of references to the presets.

## Relation to presets and program lists

- VST3 has distinct concepts on selecting a program. Their presets API works more like a state, while their program change with UnitInfo API works more like a parameter.

## Listing presets

- VST3 presets are stored at certain directories: https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Locations+Format/Preset+Locations.html
  - vst3_public_sdk has `vstpresetfile.h` API and implementation.
- CLAP is similar, using presets factory API: https://github.com/free-audio/clap/blob/main/include/clap/factory/preset-discovery.h
- AU presets can be acquired in different ways for factory and user: https://developer.apple.com/library/archive/technotes/tn2157/_index.html#//apple_ref/doc/uid/DTS40011953
  - factory presets are properties: `kAudioUnitProperty_FactoryPresets`
  - custom presets are loaded more like other plugin formats using files in certain directories

## Loading presets

VST3, AU, LV2 presets can be loaded as states.

The same principle goes for CLAP, but CLAP presets needs to be loaded using preset-load extension, because the factory API provides different identification for factory presets and user presets, and thus requires different arguments: https://github.com/free-audio/clap/blob/main/include/clap/ext/preset-load.h

VST3 UnitInfo-based program changes are settable via parameters API.
