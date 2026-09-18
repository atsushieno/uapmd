# Custom Plugin Formats

At `uapmd-plugin-hosting` (also at `remidy`) we can add support for custom audio plugin formats.
Ideally an addin for a plugin format would be appropriate, but it is up to how you use the relevant API.

We have an example addin for Reaper JSFX support (making use of [ysfx](https://github.com/JoepVanlier/ysfx), with a handful of GUI support rewrites to use ImGui instead of JUCE (the hooks are simple, rather than implementing everything).
