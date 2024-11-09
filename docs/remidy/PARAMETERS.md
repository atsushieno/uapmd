# Parameter API Design Requirements

## Parameter API Differences Between Formats

- VST3 and CLAP: parameter values are stored as `double`.
  - AU parameters are `float`.
  - LV2 parameters are practically `float` (lilv Atom does not support `double` values).
- AU: nominally they have stable parameter ID integer, but an infamous fact around AudioUnit is that Apple Logic Pro and GarageBand fail to handle parameter stability by assuming "index" in the parameter list as stable. JUCE team and their plugin developers try hard to workaround this issue by introducing `versionHint`. We should not fail like Logic Pro and GarageBand and always identify parameters by ID, not index.
- LV2: there are multiple different ways to define "parameters" and we will have to support both...
  - Traditional approach: define `lv2:ControlPort`s within the `lv2:Plugin`. They have `lv2:index`, `lv2:name`, and `lv2:symbol`.
  - Modern approach: define global `lv2:Parameter`s, and specify them as `patch:readable` or `patch:writable` within the `lv2:Plugin`. The actual parameters will be passed as `lv2:Atom`s. They have `rdfs:label`, `rdfs:range`, and `pg:group`. Note that there is no numeric ID.
  - They both can have `lv2:default`, `lv2:minimum`, `lv2:maximum`, `lv2:portProperty`, and `unit:unit`.

LV2 brings in the following design principles:

- There is no numeric stable parameter ID. Parameters ID by index is instant and cannot be used in persistable states.
- There may be read-only parameters and write-only parameters (if that makes sense).
- Retrieving parameter values may not work (they may be write-only, or we don't have parameter value store - even if we store any parameter changes there may be parameters without initial default values.)
