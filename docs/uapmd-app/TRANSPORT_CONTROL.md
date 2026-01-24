WIP WIP WIP

# Transport control

## Abstract

The sequencer is supposed to deal with multiple tracks, and each track contains multiple audio or MIDI(2) clips. They can be relocated, which brings in various complicated situation for MIDI clips:

- The clip can suddenly go out. It must stop any ongoing MIDI notes. Any continuous operations must terminate.
- It suddenly starts playing in the middle of the clip. It must enqueue all the control change values but ideally after collecting only the "latest" values.
- We cannot simply enqueue all the events in the clip because they can be moved.

Audio clips can also bring in some complication:

- Not all the audio clips can be immediately playable. There should be prefetched from file stream. Moving the clip position can result in sudden transition to "must prefetch" state.

## Requirements

There can be various operations that affect transport control:

- user moves a clip
- user inserts a clip
- user removes a clip
- user changes time-stretching / audio warping on a clip
- user removes a track
- user clones a track
- user pastes clip selections into a new location
  - onto different track
- user changes playback tempo
- user changes tempo on the master track

## Implementation

We need a handful of distinct layers:

- audio/MIDI dispatcher: pretty much like `choc::audio::AudioMIDIBlockDispatcher` but we need support for UMP
- clip/track manager: organizes tracks and clips in each track, as current (as of v0.1.3) `player` API is designed. And it has to determine which clips are in progress
- transport controller: manages play/pause/stop state
