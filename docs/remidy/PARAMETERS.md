# Parameter API Design Requirements

Plugin parameter support is achieved through `remidy::PluginParameterSupport` class which is accessible via `parameters()`.

## Parameter API Differences Between Formats

- VST3 and CLAP: parameter values are stored as `double`. AU parameters are `float`. LV2 parameters are practically `float` (lilv Atom does not support `double` values).
- AU: nominally they have stable parameter ID integer, but an infamous fact around AudioUnit is that Apple Logic Pro and GarageBand fail to handle parameter stability by assuming "index" in the parameter list as stable. JUCE team and their plugin developers try hard to workaround this issue by introducing `versionHint`. We should not fail like Logic Pro and GarageBand and always identify parameters by ID, not index.
- LV2: there are multiple different ways to define "parameters" and we will have to support both...
  - Traditional approach: define `lv2:ControlPort`s within the `lv2:Plugin`. They have `lv2:index`, `lv2:name`, and `lv2:symbol`.
  - Modern approach: define global `lv2:Parameter`s, and specify them as `patch:readable` or `patch:writable` within the `lv2:Plugin`. The actual parameters will be passed as `lv2:Atom`s. They have `rdfs:label`, `rdfs:range`, and `pg:group`. Note that there is no numeric ID.
  - They both can have `lv2:default`, `lv2:minimum`, `lv2:maximum`, `lv2:portProperty`, and `unit:unit`.

LV2 brings in the following design principles:

- There is no numeric stable parameter ID. Parameters ID by index is instant and cannot be used in persistable states.
- There may be read-only parameters and write-only parameters (if that makes sense).
- Retrieving parameter values may not work (they may be write-only, or we don't have parameter value store - even if we store any parameter changes there may be parameters without initial default values.)

VST3 and CLAP brings in the following design principles:

- Since VST3 parameter access is achieved via `IEditController` API which involves GUI thread, `PluginParameterSupport` has a `bool accessRequiresMainThread()` property getter function. VST3 returns `true`.
- The same goes for CLAP where `count()` and `get_info()` in `clap_plugin_params` are marked as `[main-thread]`.

## Automatable

We support only "automatable" parameters in some use cases. Namely, non-automatable parameters are not displayed on remidy-plugin-host and not mapped to MIDI-CI AllCtrlList property.

- AU parameters can be `kAudioUnitParameterFlag_NonRealTime`
- VST3 parameters can be `kCanAutomate`
- CLAP parameters can be `CLAP_PARAM_IS_AUTOMATABLE`
- LV2: still wondering, maybe "expensive" parameters had better be excluded...

For example, Tracktion "Collective" contains a lot of NonRealTime parameters that I have no idea what they are for (they are unnamed).

## Sample-accurate Parameter Changes

All VST3, AU, LV2, and CLAP supports sample-accurate parameter changes. Those `setParameter()` calls that come with non-zero `timestamp` are "enqueued" for the next audio processing.

- VST3: use `IParameterChanges`
- AU: use `AudioUnitScheduleParameters`
- LV2: use `Atom_Sequence` (unsupported for ControlPort-based parameters)

## Per-Note Controllers (parameters)

VST3, AU, and CLAP supports per-note parameter controllers.

- VST3: use `INoteExpressionController`
- AU: use parameters with group, channel, or note scope? It is very unclear from the API documentation.
- LV2: not supported
- CLAP: use `clap_event_param_value_t` with `key`.

It should be noted that per-note controllers are defined totally differently from normal parameters. VST3, AU, and MIDI 2.0 UMP define them as such. The only exception is CLAP, which defines parameters in unified way. We return the same list of per-note parameters as normal parameters.

The way how VST3 and AU support per-note controllers (parameters) is different from CLAP also in that they may return different set of parameter definitions *for each channel* (also may differ from "global").  AU even goes further and they may return different set of parameter IDs *for each note*. This makes constructing static list of parameters at configuring connections almost impossible.

## Extraneous CC roundtrip to parameters

One of the problem mapping from parameters to Assignable Controllers is that VST3 parameters are often mapped *from* MIDI CCs for each channel, due to fundamental VST3 design problem that does not support MIDI messages.

If we map every single parameter to Assignable Controller, it feels extraneous to further map them again. But CC-to-parameter mappings happen at plugin side, while Assignable Controllers to parameter mappings happen at the host side. We cannot guess which parameter should map to which CC to send (while we can "guess" by parameter names).
