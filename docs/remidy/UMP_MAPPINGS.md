
# Dealing with UMP Sequence

Every plugin format specifies their own event I/O messages for their audio processing. We can commonize some of them as UMP, which was the original idea in the core of UAPMD. But there are more commonized features in remidy API that are more expressive than UMP, and converting all events to UMP is not a versatile option. We also have to deal with plugin-format-specific MIDI mapping features. Therefore, the idea is to deal with them in some uniform and consistent way across platforms.

## Event Types

The following event types need to be supported:

| Type | has remidy-intrinsic |
|-|-|
| note on and off | - |
| parameter changes | + |
| per-note controllers | + |
| program changes | + |
| pitch bend | - |

## The Core Concept behind the Rewrites

The initial optimistic `UmpInputDispatcher` design was based on the assumption that plugins in the format which accepts native MIDI (1.0 / 2.0) messages can directly handle our inputs. This however will never happen, because plugins in those formats that can handle MIDI 1.0 / 2.0 inputs still cannot understand how UAPMD maps parameters to NRPNs, etc. Therefore, we have to give translation layer in `remidy` layer.

### UAPMD translation layer

UAPMD translation layer handles plugin host specific features to generate UMP or MIDI-CI messages.

```
[user] - (plugin commands) -> [AudioPluginHostPAL (- UMP encoder ->) uapmd-app] - (UMP) -> [UMP client]

[user] <- [AudioPluginHostPAL (<- uapmd decoder -) uapmd-app] <- (UMP) - [UMP client]
```

- Plugin parameter changes are normalized and transferred as Assignable Controllers.
- Per-note controllers are normalized and transferred as Per-Note Assignable Controllers.
- Plugin parameter metadata list and per-note controller metadata are translated to MIDI-CI AllCtrlList and CtrlMapList PE messages.
- Plugin get/set state are translated to MIDI-CI StateList and State PE messages.
- Other messages are passed through.

For more details, see `UapmdUmpMapper` and `UapmdMidiCISessions`.

### Remidy MIDI mapping layer

After uapmd-app separated those uapmd intrinsics, remidy handles the UMP inputs along with its integrated MIDI mapping features that may need interaction between host (remidy itself) and the context plugin.

It is done per plugin format. For example, VST3 translator has to handle these packets:

- channel pressure
- program change
- pitch bend

They take channel to determine the destination.

- key pressure

It takes channel and key.

### (no) support for format-specific mappings API

Here is another complication: VST3 provides MIDI mapping interaction on `IMidiMapping` and it requires MIDI 1.0 inputs, so we have to down-translate them first. There is also `IMidiMapping2` which does not require down-translation (since VST3.8.0).

At this moment, we don't provide ways to access to these interfaces. It might have happened when CLAP had its draft MIDI mappings API, but it's gone.
