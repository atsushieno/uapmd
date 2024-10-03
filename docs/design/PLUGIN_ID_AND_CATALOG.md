
DRAFT DRAFT DRAFT

# Plugin IDs, Plugin Catalog, and Catalog Entry

In Remidy, and within a plugin format, an audio plugin ID is a URI string that is used to identify a specific plugin that resides in a plugin bundle.
With the plugin format name, it can precisely indicate the plugin type/class to instantiate.
`AudioPluginInformation` contains a handful of plugin metadata including the plugin ID and the plugin format ID. It would need instantiation of the plugin, at least once for caching.

There are some requirements imposed on `AudioPluginInformation`:

- deserializable from a string without `AudioPluginInstance`.
- detached from `AudioPluginFormat` instance: no need to internally hold the reference to it.
- serializable to string locally: you can use the plugin bundle location to identify it; not suitable for saving as part of state or song file.
    - It can be used to save plugin list cache.
- serializable to string globally: the identifier works across different devices, and across multiple formats.
    - It can be very inefficient without plugin list cache, as it will have to scan locally installed plugins to find the exact plugin (especially VST3).

We would typically use JSON to convert to and from string.

Neither of `AudioPluginIdentifier` and `AudioPluginInformation` ensures that the indicated plugin is valid.
The plugin may disappear from the system at any time.

There are some requirements for a plugin to meet "Remidy Plugin Metadata Standard":

- vendor name: used by hosts for grouping locally installed plugin products.
- product name: it must identify a plugin product: the same plugin product across different devices, across multiple formats, must offer the consistent plugin State feature.
- version: A plugin version must be uniquely identifiable.
  It works significantly when we have to strictly distinguish a product version e.g. when the saved state loses backward compatibility.

(Local IDs do not have to follow this.)
