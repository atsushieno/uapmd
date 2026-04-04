# Extensibility Design Documentation

remidy and uapmd are designed be independent of product agnostic (unlike Tracktion Engine) and open to any format (unlike DAWproject).

- uapmd defines `AudioPluginHostAPI` and `AudioPluginInstanceAPI` pure virtual interfaces. While they are close to remidy API, they collect general interface. In the early development stage it was intended to adopt juce_audio_plugin_client, but remidy is almost complete with no known failed plugins nowaday.
- uapmd MIDI 2.0 Function Block integration is now designed to be adaptable to any `AudioPluginInstanceAPI`, `MidiIODevice` etc. that implements the interfaces defined in in `uapmd`.
- uapmd defines `AudioPluginGraph` pure virtual interface. We know that DAWs design and implement audio plugin graphs in their own design, but we need minimum interface to the audio graph so that we can at least add and remove a plugin node (which leads to designing "default add operation" in each implementation), each plugin node's inputs and outputs can be intercepted externally (e.g. to pick up MIDI-CI messages), and process audio. As long as `processAudio()` is RT-safe, their internal connection and serialization in a project file can be anything.
