
# Use of Assignable Controllers

Assignable Controllers in MIDI 2.0 UMP existed as NRPN in MIDI 1.0 (in 7-bit resolution). UAPMD makes use of them as parameter controllers.

The same goes for Per-Note Controllers, but we only have a 7-bit index for that. This imposes on a significant limitation on AU and CLAP plugins (VST3 Per-Note Controllers only support 7-bit index so it is non-issue for them). LV2 does not even support per-note controllers.

Since UMP expects 32-bit unsigned integers, it works more like VST3's normalized value. We have to convert normalized `uint32_t` values to and from plain `double` values. Any value that cannot be represented in `uint32_t` (such as NaN) are not supported.


## Persistent identity

