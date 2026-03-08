#include "PluginFormatWebCLAPInternal.hpp"

#if defined(__EMSCRIPTEN__)

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace remidy::webclap {

    namespace {
        constexpr const char* kWebclapManifestJson = R"json(
{
  "plugins": [
    {
      "format": "WebCLAP",
      "id": "wclap:uk.co.signalsmith.basics.chorus",
      "bundle": "https://webclap.github.io/browser-test-host/examples/signalsmith-basics/basics.wclap.tar.gz",
      "name": "[Basics] Chorus",
      "vendor": "Signalsmith Audio",
      "url": "https://github.com/WebCLAP/browser-test-host"
    },
    {
      "format": "WebCLAP",
      "id": "wclap:uk.co.signalsmith.basics.limiter",
      "bundle": "https://webclap.github.io/browser-test-host/examples/signalsmith-basics/basics.wclap.tar.gz",
      "name": "[Basics] Limiter",
      "vendor": "Signalsmith Audio",
      "url": "https://github.com/WebCLAP/browser-test-host"
    },
    {
      "format": "WebCLAP",
      "id": "wclap:uk.co.signalsmith.basics.freq-shifter",
      "bundle": "https://webclap.github.io/browser-test-host/examples/signalsmith-basics/basics.wclap.tar.gz",
      "name": "[Basics] Frequency Shifter",
      "vendor": "Signalsmith Audio",
      "url": "https://github.com/WebCLAP/browser-test-host"
    },
    {
      "format": "WebCLAP",
      "id": "wclap:uk.co.signalsmith.basics.analyser",
      "bundle": "https://webclap.github.io/browser-test-host/examples/signalsmith-basics/basics.wclap.tar.gz",
      "name": "[Basics] Analyser",
      "vendor": "Signalsmith Audio",
      "url": "https://github.com/WebCLAP/browser-test-host"
    },
    {
      "format": "WebCLAP",
      "id": "wclap:uk.co.signalsmith.basics.crunch",
      "bundle": "https://webclap.github.io/browser-test-host/examples/signalsmith-basics/basics.wclap.tar.gz",
      "name": "[Basics] Crunch",
      "vendor": "Signalsmith Audio",
      "url": "https://github.com/WebCLAP/browser-test-host"
    },
    {
      "format": "WebCLAP",
      "id": "wclap:uk.co.signalsmith.basics.reverb",
      "bundle": "https://webclap.github.io/browser-test-host/examples/signalsmith-basics/basics.wclap.tar.gz",
      "name": "[Basics] Reverb",
      "vendor": "Signalsmith Audio",
      "url": "https://github.com/WebCLAP/browser-test-host"
    },
    {
      "format": "WebCLAP",
      "id": "wclap:uk.co.signalsmith-audio.plugins.example-audio-plugin",
      "bundle": "https://webclap.github.io/browser-test-host/examples/signalsmith-clap-cpp/example-plugins.wclap.tar.gz",
      "name": "C++ Example Audio Plugin (Chorus)",
      "vendor": "Signalsmith Audio",
      "url": "https://github.com/WebCLAP/browser-test-host"
    },
    {
      "format": "WebCLAP",
      "id": "wclap:uk.co.signalsmith-audio.plugins.example-note-plugin",
      "bundle": "https://webclap.github.io/browser-test-host/examples/signalsmith-clap-cpp/example-plugins.wclap.tar.gz",
      "name": "C++ Example Note Plugin",
      "vendor": "Signalsmith Audio",
      "url": "https://github.com/WebCLAP/browser-test-host"
    },
    {
      "format": "WebCLAP",
      "id": "wclap:uk.co.signalsmith-audio.plugins.example-synth",
      "bundle": "https://webclap.github.io/browser-test-host/examples/signalsmith-clap-cpp/example-plugins.wclap.tar.gz",
      "name": "C++ Example Synth",
      "vendor": "Signalsmith Audio",
      "url": "https://github.com/WebCLAP/browser-test-host"
    },
    {
      "format": "WebCLAP",
      "id": "wclap:uk.co.signalsmith-audio.plugins.example-keyboard",
      "bundle": "https://webclap.github.io/browser-test-host/examples/signalsmith-clap-cpp/example-plugins.wclap.tar.gz",
      "name": "C++ Example Virtual Keyboard",
      "vendor": "Signalsmith Audio",
      "url": "https://github.com/WebCLAP/browser-test-host"
    }
  ],
  "denyList": []
}
)json";

        std::optional<std::string> readManifestFile(const std::filesystem::path& path) {
            std::error_code ec;
            if (path.empty() || !std::filesystem::exists(path, ec))
                return std::nullopt;
            std::ifstream stream(path);
            if (!stream.is_open())
                return std::nullopt;
            std::ostringstream buffer;
            buffer << stream.rdbuf();
            return buffer.str();
        }

    } // namespace

    std::string resolveManifestPayload() {
        if (const char* overrideValue = std::getenv("UAPMD_WEBCLAP_MANIFEST")) {
            std::string overrideString{overrideValue};
            if (!overrideString.empty()) {
                if (overrideString.front() == '{')
                    return overrideString;
                if (auto fromFile = readManifestFile(std::filesystem::path{overrideString}))
                    return *fromFile;
            }
        }
        static const std::filesystem::path defaultPath{"/browser/runtime/plugin-manifest.json"};
        if (auto fromFile = readManifestFile(defaultPath))
            return *fromFile;
        return kWebclapManifestJson;
    }

} // namespace remidy::webclap

#endif
