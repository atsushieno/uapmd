
DRAFT DRAFT DRAFT

# Portable Plugin IDs, Plugin Catalog, and Plugin Metadata

Identifying an audio plugin is a crucial task for audio plugin hosts, but it is not really rock solid among various plugin formats. In this document we describe how plugin identifiers work (and should work) as well as how plugin catalogs are (and should be) organized in Remidy.

## Objectives

Remidy has a clear objectives on its plugin identification system that most of the existing plugins do not meet. The key concepts are:

1. **instantiation**: we must be able to instantiate a plugin from an identifier
2. **fast instantiation**: we should be able to instantiate a plugin without scanning all the installed plugins on the system every time
3. **globally identifiable**: an identifier should be resilient against file system changes
4. **consistent**: like a cool URL never changes, a plugin of the same identifier should maintain backward compatibility of its audio/MIDI outputs.
5. **fast scanning**: each plugin format provides optimal way to scan plugin so that users can quickly identify which to instantiate, and multi-format wrappers like Remidy should not block their efficient design.
6. **platform-agnostic**: the identifier should work for a plugin that works across multiple platforms.
7. **format-agnostic**: the identifier should work for a plugin whose states work across multiple formats

### instantiation

Regarding #1, a plugin ID within the format usually does not describe which format it is in. Therefore, to achieve #1, we need **both** a plugin ID and the format specifier.

It should be noted that the actual plugin ID **might** depend on the actual `AudioPluginFormat` implementation. Unless there is only one dominant `AudioPluginFormat` class, the format identifier should also **identify the implementation** (Remidy itself provides the reference implementations for VST3 and AU and we use `VST3` and `AU` for each).

### fast instantiation

Regarding #2, it is impossible for such a "non-fast" plugin format to instantiate a plugin only with a global identifier, where a "non-fast" plugin format here means such a format that requires loading the library of a plugin.

With VST3 (at least earlier than v3.7.6, those without moduleinfo.json), fast instantiation is impossible only with the plugin class ID (TUID) because we cannot know which plugin bundle contains the plugin without loading the bundle library. Almost same with CLAP, as (whereas the community claims that we do not have to "instantiate" the plugins) we still have to load the dynamic library to find relevant symbols to retrieve the plugins metadata. VST3 with `moduleinfo.json`, AudioUnit and LV2 are good citizens here, because they have metadata.

In Remidy `AudioPluginFormat` API, we have the following functions to describe how each plugin formats work regarding plugin scanning:

- `scanRequiresLoadLibrary()` indicates whether the format requires loading the library to scan the plugins (VST3: Maybe, AU: NO, LV2: NO, CLAP: YES)
- `scanRequiresInstantiation()` indicates whether the format requires instantiation IF the library needs to be loaded (VST3: YES, AU: NO, LV2: NO, CLAP: NO).

For a plugin format that supports scanning without loading libraries, building a local plugins catalog is a trivial task so that you would not have to resort to plugin catalogs.

### global identification

Regarding #3, a global identification name cannot depend on anything specific to one user's environment such as installation file path. Doing so means 
that the plugin becomes useless if the plugin location has changed (even 
moving between `/Library/Audio/Plug-Ins/Component` and `~/Library/Audio/Plug-Ins/Component`).

It is also inappropriate if the ID is not persistent - if the plugin ID of the same plugin entity changes its ID every time it is launched (or a DAW is launched), that means for a DAW user that the plugin they used has simply vanished and their dependent tracks become invalid.

### backward compatibility

Regarding #4, it is also inappropriate if the name of the product does not change but the audio/MIDI outputs changes significantly.

Including the product name or the vendor name into the plugin ID is sometimes unsafe either, because they are not always persistent and your plugin users should not be messed their track-making effort by the plugin vendor's company name changes e.g. by acquisition.

A related issue is that there is no reliable identifier in Apple AudioUnit. There are only 3 fields of 4-bytes (`manufacturerName`, `componentType`, 
and `componentSubtype`). If we include `name` of the plugin from `AudioComponentDescription`, it will become practically identifying (less fear of conflicts), but plugin vendors may change it at any time. Thus it is not a good idea to use it, and we can only resort to those 12 bytes at best.

### fast scanning

Regarding #5 (it's looking similar to #2 but what we actually need is quite different), our plugin metadata should not try to offer "too much" to avoid needing to instantiate plugins beyond what plugin formats provide.

For example, scanning AudioUnit plugins in JUCE takes too long because it instantiates all the plugins just like it does for VST3. But Apple's AudioToolbox provides fast scanning because it can scan only plugins' `Info.plist` files to avoid loading huge plugin binaries. Remidy can construct AudioUnit plugin catalog of hundreds within a second or two.

### platform and format agnostic

Regarding #6 and #7, Most cross-platform plugin formats achieve that as they do not have to change the behavior per platform. But most of the plugins do not work in format-agnostic way, as they do not save and load state in the compatible binary manner. We need to use something like MIDI-CI Property Exchange Get and Set Device State. Or YAMAHA DX-7 Cart which is just a sequence of MIDI 1.0 SysEx messages.

It is mostly not about plugin identification, but if there is no way to extract the "identity" of the "same" plugin, then cross-format portability will not happen. So it is still an essential requirement here.

## Dealing With Inconsistency

Users may remove audio plugins at any time and we cannot detect any changes so far (in theory we could though, at least for those plugin formats that are based on filesystem using filesystem watcher). When we report users about missing plugins, the error message would not make sense without plugin name (human-readable). PluginIDs are not meant for human beings (especially on AudioUnit, as their IDs are based only on 3 four-byte fields).

You should probably save the entire `PluginCatalogEntry` instead of the plugin ID. The simple plugin ID string isn't enough as you will have to save its plugin format name, anyway.

----

## Remidy Plugin URI

In Remidy, and within a plugin format, an audio plugin ID is a URI string that is used to identify a specific plugin that resides in a plugin bundle.
Along with the plugin format name, it can precisely indicate the plugin type/class to instantiate, regardless of the environment (e.g. resilient against path changes).


`PluginCatalogEntry` contains a handful of plugin metadata including the plugin ID and the plugin format ID. It would need instantiation of the plugin, at least once for caching.

There are some requirements imposed on `PluginCatalogEntry`:

- deserializable from a string without `AudioPluginInstance`.
- detached from `AudioPluginFormat` instance: no need to internally hold the reference to it.
- serializable to string locally: you can use the plugin bundle location to identify it; not suitable for saving as part of state or song file.
    - It can be used to save plugin list cache.
- serializable to string globally: the identifier works across different devices, and across multiple formats.
    - It can be very inefficient without plugin list cache, as it will have to scan locally installed plugins to find the exact plugin (especially VST3).

We would typically use JSON to convert to and from string.

Neither of the plugin ID and `PluginCatalogEntry` field ensures that the indicated plugin is valid.
The plugin may disappear from the system at any time.

There are some requirements for a plugin to meet "Remidy Plugin Metadata Standard":

- vendor name: used by hosts for grouping locally installed plugin products.
- product name: it must identify a plugin product: the same plugin product across different devices, across multiple formats, must offer the consistent plugin State feature.
- version: A plugin version must be uniquely identifiable.
  It works significantly when we have to strictly distinguish a product version e.g. when the saved state loses backward compatibility.

(Local IDs do not have to follow this.)
