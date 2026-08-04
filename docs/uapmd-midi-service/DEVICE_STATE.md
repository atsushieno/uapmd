### MIDI-CI Get and Set Device State Property as the common binary format

Defining our own binary data format ("defined by Remidy") is not a preferred option if there is already another standardized specification for that. And there is, MIDI-CI Get and Set Device State Property specification as part of MIDI 2.0 specification.

It should be noted that both LV2 and CLAP provides the use cases where portability is not required. If portability comes without cost then we do not have to worry about use cases i.e. they could have been always portable. Implementing portable state is probably annoying e.g. always having to follow endianness-agnostic states is cumbersome.

