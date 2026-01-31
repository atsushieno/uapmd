
# Use of Assignable Controllers

Assignable Controllers in MIDI 2.0 UMP existed as NRPN in MIDI 1.0 (in 7-bit resolution). UAPMD makes use of them as parameter controllers.

The same goes for Per-Note Controllers, but we only have a 7-bit index for that. This imposes on a significant limitation on AU and CLAP plugins (VST3 Per-Note Controllers only support 7-bit index so it is non-issue for them). LV2 does not even support per-note controllers.

Since UMP expects 32-bit unsigned integers, it works more like VST3's normalized value. We have to convert normalized `uint32_t` values to and from plain `double` values. Any value that cannot be represented in `uint32_t` (such as NaN) are not supported.

### UapmdUmpInputMapping and UapmdUmpOutputMapping

Those classes indicate how we map plugin features in UAPMD.

## Persistent identity

In our traditional MIDI processing, there is kind of premise that Control index, bank and index for RPNs and NRPNs are immutable. Audio plugins, as discussed in [the remidy parameter design doc](../remidy/PARAMETERS.md), every plugin format has stable IDs for parameters. But we cannot simply assign those stable IDs to controllers because (1) the stable IDs may not be an integer, and (2) their valid range is quite limited (7-bit index, 14-bit bank) so the IDs mat not fit within it.

A compromised conclusion is that those controller index and bank IDs are not regarded as stable. We cannot simply store assignable controllers without mapping information, so we will have to store them as part of a "project".

## Event output retrieval

UAPMD has to retrieve parameter change event outputs stream on each audio `process()` (as in remidy wording). `PluginParameterSupport` has `parameterChangeEvent` which we can add a listener, but we have to revisit the entire even workflow. Currently only LV2 plugins work well with it.

A complicated problem here is that those even retrieval must be done in realtime manner on the audio thread, and the event listener is not necessarily running there.

## Process Inquiry

MIDI-CI specifies an interesting feature called Process Inquiry and in particular MIDI Message Report. It works like MIDI dump. A client requests some dump of controller values, and the MIDI device responds to it with a bunch of CC/RPN/NRPN, note state for each key, etc.

Sometimes we have to deal with batch notification of parameters, so it will be a useful feature. We have basic Process Inquiry foundation.

