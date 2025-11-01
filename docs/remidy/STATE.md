
# Remidy "Portable Audio Plugin State" Standards

This documentation describes how Remidy aims to achieve "portability" of audio plugin states.

## What is an audio plugin state?

A plugin "state" is used for a few purposes with certain requirements:

- It needs to save and load the song at least on the same machine so that the DAW user can continue composition. This is about DSP and to/from storage.
- It needs to save and load the plugin UI internals while manipulating DAWs: it saves its internals whenever user closes the UI, and restores it when the UI is reopened (or launched for the first time after the instantiation and loading the *UI* state, if exists). It is primarily in memory, but saving UI states to storage is a plus.
- It *should* ensure portability, across platforms and plugin formats; DAW users want to have their compositions "portable". They should not be lost when the composers switch to another computer only because its data format is "incompatible" with the same software on the different computers.
- The state binaries *should* be backward compatible - the new versions of the plugin should be able to restore the state binaries from its old versions.
- Simple API: host and plugin developers should be able to use simple load/save functions without boilerplate work.

Achieving "interoperability" is however not easy; we need some agreement from hosts and plugins. No approvals are required from plugin *format* developers though.

## How Each Plugin Format Deals with the States

### VST3

VST3 offers `getState(IBStream*)` and `setState(IBStream*)` in `IComponent` interface. It should be noted that `IComponent` is a common basis for `IAudioProcessor` and `IEditController` while it is not the base class for both. That is, the state API in this class covers both the DSP state and the GUI state.

Also, this simple API only to write to `IBStream` means that its primary focus is to work as a simplified facade for saving *all* the state in one call. It does not provide its internals, like parameters and non-parameters.

Regarding GUI state, `IEditController` has `setComponentState()` without getter. This likely means that we need to pass whatever we acquire via `getState()` i.e. the entire state including the DSP state.

### AU

AUv2 does not offer a simplified facade like that. It offers parameter API, and property API (getter and setter). When a host has to save the state, it has to save the properties and parameter trees. GUI state depends on the UI framework.

In AudioUnitSDK, there is a state saver function `SaveState()` and and a loader function `RestoreState()` in `AUBase`. The save function preserves AudioUnit intrinsics such as manufacturer, type, and subType, beyond user-provided parameters and properties, and the restore function verifies some of those intrinsics. Since they are not accessible API from the host, I'm not sure when they should be used and how we should take them into consideration though. Hosts are only required to retrieve parameters and properties to "restore" the state.

### LV2

LV2 kind of offers a simplified facade (`save()` function in `LV2_State_Interface` extension), and it is up to plugin developer what they serialize as states (in their calls to the host's `LV2_State_Store_Function`). Host cannot indicate that the plugin should preserve all the parameters (the concept of "parameter" is ambiguous in LV2 either; there might be ControlPorts, or a pair of write-only Atom input port and notification-only Atom output port).

In another aspect, LV2 is seriously committed to state portability; it has an option in the parameter list of its `LV2_State_Flags` whose value can be `LV2_STATE_IS_POD`, `LV2_STATE_IS_PORTABLE` and `LV2_STATE_IS_NATIVE` (they are flags because it is also used by host when they invoke `save()` function). "POD" is not portable enough as it may depend on the computer architecture (endianness etc.).

The LV2 UI specification states that the UI should be designed to be runnable in another process, which implies that the UI state is not covered in the DSP process at all.

### CLAP

CLAP has two-fold extensions for state. One is `state` and another is `state-context`. The `state` extension works just like what VST3 offers. It can just serialize the state into a flat binary stream. The `state-context` extension gives the perspective on in which plugin usage scenarios it wants to save and load the state:

- `CLAP_STATE_CONTEXT_FOR_PRESET` : it is used when user wants to save the state as a preset
- `CLAP_STATE_CONTEXT_FOR_DUPLICATE` : it is used when a DAW needs to duplicate the instance state
- `CLAP_STATE_CONTEXT_FOR_PROJECT` : it is used when user wants to save the state in a song file

The CLAP state extension specification does not clarify that it covers the UI state, but its functions are annotated the `[main-thread]` which *likely* means that it also implicitly covers the UI state.

## `AudioPluginInstance` API to load and save state.

Since our target plugins, especially VST3 and CLAP, can only save the entire state to binary, our API cannot split parameter saving and non-parameter saving in separate functions either. Therefore, we need to offer simple `save()` and `load()` with just one single stream parameter.

## Making Mere Binary Blob to Portable

Remidy is only a hosting library that cannot control what each plugin internally serialize, so what we can do is limited. Still, we are the entrypoint to our host and every host needs to go through our API.

One thing we can do is to interpret the state binary in our way. If we can detect that the plugin saves and restores the state in certain binary format, then we can achieve portable state together. What we need here is a standardization for some portable state.

We need some detailed principles.

### Scope of Portability

Portable states should be required only to certain use cases. LV2 State extension brought in clear milestones on how to achieve that, and both the LV2 extension and CLAP `state-context` extension brought in an excellent usage scenarios (yet I'm not sure it should be limited to those enumerated values).

### No UI State included

The plugin state should NOT contain its UI state. With a normal UI framework, they can detect if its window is being closed and created, so they can save and load their state at their timing in their manner anyway. They do not have to be passed to the host - plugins can take care of them solely by its own. Plugin format should care only about what needs to cross the program binary border.

Saving UI state in the state API brings in a major drawback: restriction with the UI thread. While some people might find it better, we should in general prefer thread characteristics agnostic. AudioUnit works better in that regard and we should follow that. And to achieve that, we should simply eliminate UI state.

VST3 and CLAP specifications are clearly source of problem. We should impose further restrictions on how they should be used. No UI thread dependents. Period.

### Dependencies on external files

Basically we do not claim perfectly portable regarding external file dependencies; the resources in the saved state should be freely distributable without extra copyright concern, and as long as the song file consumer (the author who wants to use it on another machine, or those listeners who downloaded the song file) can prepare the same set of dependent resources, it should be fine.

They should just be resolvable from different file paths, according to the plugin app configurations (e.g. app-level user sampler file paths).

LV2 kind of brought in great insight: the state file path should not be stored as is but should be treated relatively. That does not have to cross the DAW-plugin binary border though i.e. the plugin can handle it solely by its own.
