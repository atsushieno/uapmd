# Tail Processing and Transport Quiet Detection (AI slop)

`TailProcessManager` manages audio processing between a transport Stop or Pause
and the point at which the engine output is considered completely quiet.

## Responsibilities

The manager owns:

- the active tail-drain state and remaining sample count;
- transport-quiet detection;
- the stopped-output silence hold used after track freezing starts; and
- registration and dispatch of transport-quiet listeners.

`SequencerEngine` coordinates the process. On Stop or Pause it obtains the
required drain length from `LatencyCompensationManager` and passes that value
to `TailProcessManager`. The managers do not depend directly on each other.
See [LATENCY_COMPENSATION.md](LATENCY_COMPENSATION.md) for calculation of the
drain length.

## Stop and Pause

Stop and Pause use the same tail-processing path. Their only transport
difference is the final position: Stop resets it, while Pause retains it.

The declared drain length is rounded up to an audio-processing quantum. During
the drain, the graph continues processing and the private render position
advances so delayed audio and finite plugin tails can reach the output.

After the declared drain finishes, the manager observes the mixed output. The
transport becomes quiet only after the peak remains at or below `0.0001`
(-80 dB) continuously for 250 ms. This additional observation handles plugins
that report a zero or inaccurate tail while release audio is still audible.

Starting or resuming transport cancels pending tail processing and clears the
quiet state. Explicit transport or timing changes may also cancel a pending
drain.

## Quiet Event

The audio thread updates drain and silence counters using atomics. It does not
lock, allocate, or invoke listeners.

When quiet is detected, the audio thread increments an event sequence and
wakes a dispatch thread. The dispatch thread snapshots the registered
listeners under a mutex and invokes them outside the realtime audio thread.
Listeners must unregister before their owning object is destroyed.

Track freezing registers a listener for this event and starts pending renders
only after transport has become quiet.

## Output Silence Hold

Once a stopped transport enters track-freeze rendering, device output is held
at silence until an explicit Play or Resume. This prevents previously buffered
tail audio from becoming audible after offline rendering finishes. Audio used
for quiet detection is measured before this final output silencing step.
