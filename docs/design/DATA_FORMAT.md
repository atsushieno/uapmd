# UAPMD Data Format

## UAPMD configuration file

```
{
  "audio-buses": [
    {
      "name": "Stereo",
      "channels": [
        { "name": "L" },
        { "name": "R" }
      ]
    }
  ],
}

```

## MIDI device configuration file

It is designed to be interchange-able; you can copy the config file to anywhere else, and as long as the same plugin of the same unique ID is available, then it can be loaded.

There is no audio port connection configuration in v1. All those audio ports are connected in the bus definition order. We only have Stereo so far.

```
{
  "midi-devices": [
    {
      "name": "My instruments",
      "audio_buses": "Stereo",
      "function-blocks": [
        {
          "channel": 0,
          "name": "Instrument1",
          "graph": {
            "format": "uapmd-v1", # | *
            "plugins": [
              {
                "format": "MIDI2", # | "VST3" | "AUv2" | "AUv3" | LV2" | "CLAP" | *
                "id": "plugin_unique_id_per_format",
                "instance_name": "name_if_needed",
                "state": {
                  collection of state property name-value pairs
                }
              },
              { "instance_name": "Reverb", ..." }
            ]
          }
        }
      ]
    }
  ]
}
```
