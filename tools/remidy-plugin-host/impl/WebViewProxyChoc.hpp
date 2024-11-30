#pragma once

#include "../WebViewProxy.hpp"
#include <choc/gui/choc_WebView.h>
#include <choc/gui/choc_DesktopWindow.h>

namespace uapmd {
class WebViewProxyChoc : public WebViewProxy {
    choc::ui::WebView webview;
    choc::ui::DesktopWindow window;

    choc::value::Value toChocValue(ValueType type, uapmd::WebViewProxy::Value* src) {
            switch (type) {
                case ValueType::Double:
                    return choc::value::createFloat64(src->toDouble());
                case ValueType::Int:
                    return choc::value::createInt32(src->toInt());
                case ValueType::Bool:
                    return choc::value::createBool(src->toBool());
                case ValueType::String:
                    return choc::value::createString(src->toString());
                case ValueType::Array:
                case ValueType::Map:
                    throw std::runtime_error("Not implemented yet");
            }
        }
    public:
        explicit WebViewProxyChoc(WebViewProxy::Configuration& config) :
                WebViewProxy(config),
                window({ 100, 100, 800, 600 }) {
            auto cfg = this->config;
            webview = choc::ui::WebView(choc::ui::WebView::Options{
                //.acceptsFirstMouseClick =
                //.customSchemeURI =
                //.customUserAgent =
                .enableDebugMode = cfg.enableDebugger,
                .fetchResource = [cfg](const std::string& path) -> std::optional<choc::ui::WebView::Options::Resource> {
                    auto res = cfg.resolvePath(path);
                    if (res)
                        return choc::ui::WebView::Options::Resource(res->data, res->mimeType);
                    else
                        return {};
                },
                .transparentBackground = config.transparentBackground,
                .enableDefaultClipboardKeyShortcutsInSafari = true
            });
            window.setContent(&webview);
        }
        ~WebViewProxyChoc() override = default;

        void navigateTo(const std::string &url) override { webview.navigate(url); }
        void navigateToLocalFile(const std::string &localFile) override { webview.navigate(localFile); }
        void loadContent(const std::string &data) override { webview.setHTML(data); }
        // The input and the output are JSON string.
        void registerFunction(const std::string &jsName, std::function<std::string(const std::string_view&)>&& func) override {
            webview.bind(jsName, [&](const choc::value::ValueView& args) -> choc::value::Value {
                return choc::value::createString(func(choc::json::toString(args, true)));
            });
        }
        void registerFunction(const std::string &jsName, ValueType returnType, std::function<Value*(const std::vector<Value*>)> &&func) override {
            std::runtime_error("Not implemented");
            /*
                auto f = [this,returnType,func](const choc::value::ValueView& args) {
                    std::vector<Value*> implArgs{};
                    for (auto arg : args) {
                        if (arg.isBool())
                            implArgs.emplace_back(toValue(arg.getBool()));
                        else if (arg.isFloat64())
                            implArgs.emplace_back(toValue(arg.getFloat64()));
                        else if (arg.isString())
                            implArgs.emplace_back(toValue(arg.getString()));
                    }
                    auto result = func(implArgs);
                    auto ret = toChocValue(returnType, result);
                    for (auto i : implArgs)
                        delete i;
                    delete result;
                    return ret;
                };
                webview.bind(jsName, f);
             */
        }
        void evalJS(const std::string &js) override { webview.evaluateJavascript(js); }

        // WebView UI functionality

        void windowTitle(const std::string &title) override {
            window.setWindowTitle(title);
        }

        void show() override {
            window.setVisible(true);
            window.toFront();
        }
        void hide() override { window.setVisible(false); }
    };
}
