
# GUI and threading

## UI threading in remidy

All of those supported plugin formats deal with the UI thread (or "main" thread, which is *usually* equivalent), and some formats (namely VST3 and CLAP) require some plugin API calls happen on the UI thread. This is achieved in remidy by "dispatching" function calls to those plugin APIs to the UI thread.

At the same time, remidy itself is (should be) UI-framework-agnostic, it has to be externally configurable. We provide the entry points to the event loop controller by `EventLoop` class. If you want to use certain UI framework, implement the functions and set it by `EventLoop::instance()`.

## PluginUISupport API

Our GUI support is commonized in `PluginUISupport` API. It is basically designed after what CLAP GUI extension does.

GUI extensibility on each plugin format differs each other:

| format | exposed native handle | exposing API |
|-|-|-|
| VST3 | parent | `void*`, `IPlugView::attached()` |
| AU | plugin UI | `NSView`, `AudioUnitCocoaViewInfo` |
| LV2 | plugin UI | `LV2UI_Widget`, `LV2UI_Descriptor.instantiate` |
| CLAP | parent | `clap_window_t*`, `clap_gui_plugin_t.set_parent()` |

In current sources, the GUI support is implemented mostly by AI agents (Claude Code and Codex).
