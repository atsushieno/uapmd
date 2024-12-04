
# WebView UI

## Using WebView

For remidy-plugin-host, we chose WebView as its primary UI technology. There is no compelling reason, but at least it resolves many problems that native C++ desktop UI frameworks could not e.g. input methods just work fine on Linux (compared to juce_gui_basics, pugl, etc.).

## Interface first

There are some known cross-platform WebView implementations (in liberal licenses):

- webview.h (including NuiCpp)
- saucer
- choc WebView

(I don't name native WebView APIs here; they mean, we just have to create another x-plat wrapper that just does not make sense; we could just use existing one of those.)

At this state, taking particular WebView implementation as our implementation foundation seems risky, so I decided to rather define an interface `WebViewProxy` and wrap some of those ^ in it. This class provides no better functionality than any of the underlying implementation, but we do not want to work on higher-level features on top of unstable foundation.

### Required backend features

I have created a list of required features and how these WebView libraries support them:

| API | webview.h or Nui | saucer | Tracktion/choc |
|-|-|-|-|
| class | `webview_t` | `saucer::smartview<T>` | `choc::ui::WebView` |
| bind | `webview_bind()` | `expose()` | `bind()` |
| eval | `webview_eval()` | `execute()` | `evaluateJavascript()` |
| return | `webview_return()` | `evaluate()` | N/A |
| serialize | use JSON String | `serializer` + `glaze` backend | `choc::value` API |
| resolve | N/A | `saucer::embedded_files` | "fetchResource" |
| loop | `webview_run()` | `saucer::application` | `choc::ui::EventLoop` |

##  C++/JS Interoperability Regarding Parameters and Returns

If there is only one cool way to work with stringly-typed function invocations between C++ and JavaScript, that would have been cool. Unfortunately the world is divided and every WebView implementation offers their own way, so unifying them would need some significant effort.

In short term, we could survive with JSON-ized parameters and JSON-ized returns.


## UI WebComponents

There are couple of UI components that are already implemented:

- `RemidyAudioDeviceSetupElement` : audio device setup (WIP)
- `RemidyAudioPluginEntryListElement` : plugin list, selectable
- `RemidyAudioPluginInstanceControlElement` : plugin instance controller

They are implemented as WebComponents using some open source WebComponents such as [shoelace](https://shoelace.style/).

They all come up with:

- `remidy_XXX()` : JS-callable function defined via `WebViewProxy::registerFunction()`.
  - It is called by UI component JS code (not directly by apps).
- `remidy_XXX_stub()` : stub function for above, used until WebView registers the corresponding function.
- `XXXEvent` : event class that can be sent by C++ `WebViewProxy::evalJS()` using `window.dispatchEvent()` JS snippet.
  - The corresponding event listener is usually added at `connectedCallback()` in each WebComponent.

