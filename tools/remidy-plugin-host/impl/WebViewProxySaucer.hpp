#pragma once
#include "../WebViewProxy.hpp"
#include "SaucerWebEmbedded.hpp"

namespace uapmd {
class WebViewProxySaucer : public WebViewProxy {
        SaucerWebEmbedded& embedded;
        saucer::smartview<saucer::serializers::glaze::serializer>& webview;

        /*
#define SIMPLE_TYPE_VALUE(CLASSNAME, VALUE_TYPE, CTYPE, TO_FUNC) \
        class CLASSNAME : public Value { \
            CTYPE value; \
        public: \
            CLASSNAME(CTYPE v) : value(v) {} \
            ValueType type() override { return ValueType::VALUE_TYPE; } \
            CTYPE TO_FUNC () override { return value; } \
        }

        SIMPLE_TYPE_VALUE(BoolValue, Bool, bool, toBool);
        SIMPLE_TYPE_VALUE(IntValue, Int, int32_t, toInt);
        SIMPLE_TYPE_VALUE(DoubleValue, Double, double, toDouble);
        SIMPLE_TYPE_VALUE(StringValue, String, std::string_view, toString);
*/
    public:
        WebViewProxySaucer(Configuration& config, SaucerWebEmbedded& embedded) :
            WebViewProxy(config), embedded(embedded), webview(embedded.webview()) {

            webview.set_dev_tools(config.enableDebugger);
        }

        void navigateTo(const std::string &url) override { webview.set_url(url); }

        void navigateToLocalFile(const std::string &localFile) override {
            webview.set_url("saucer://embedded/" + localFile);
        }
        void loadContent(const std::string &data) override {
            // FIXME: maybe we can use make_stash<>() that lazily loads embedded_files.
            throw std::runtime_error("Not supported");
        }

        void registerFunction(const std::string &jsName, std::function<std::string(const std::string_view&)>&& func) override {
            webview.expose(jsName, std::move(func),  saucer::launch::async);
            evalJS(std::format("{} = saucer.exposed.{}", jsName, jsName));
        }
#if 1
        void registerFunction(const std::string &jsName, ValueType returnType, std::function<Value*(const std::vector<Value*>)>&& func) override {
            throw std::runtime_error("Not supported");
        }
#else
        void registerFunction(const std::string &jsName, ValueType returnType, std::function<Value*(const std::vector<Value*>)>&& func) override {
            auto f = std::move(func);

#define UAPMD_WEBVIEW_SAUCER_EXPOSE(TO_TYPE) \
            webview.expose(jsName, [&](std::vector<std::any> args) { \
                std::vector<Value*> remidyArgs{}; \
                auto result = f(remidyArgs); \
                auto ret = result->TO_TYPE(); \
                delete result; \
                return ret; \
            }, saucer::launch::async)

            switch (returnType) {
                case ValueType::Bool:
                    UAPMD_WEBVIEW_SAUCER_EXPOSE(toBool);
                case ValueType::Double:
                    UAPMD_WEBVIEW_SAUCER_EXPOSE(toDouble);
                case ValueType::Int:
                    UAPMD_WEBVIEW_SAUCER_EXPOSE(toInt);
                case ValueType::String:
                    UAPMD_WEBVIEW_SAUCER_EXPOSE(toString);
                case ValueType::Array: {
                    UAPMD_WEBVIEW_SAUCER_EXPOSE(toArray);
                }
                case ValueType::Map:
                    UAPMD_WEBVIEW_SAUCER_EXPOSE(toMap);
            }
        }
#endif
        void evalJS(const std::string &js) override { webview.execute(js); }

        /*
        // The returned value has to be deleted at the caller site.
        Value* toValue(const std::string_view& s) override { return new StringValue(s); }
        // The returned value has to be deleted at the caller site.
        Value* toValue(int32_t i) { return new IntValue(i); }
        // The returned value has to be deleted at the caller site.
        Value* toValue(double d) { return new DoubleValue(d); }
         */

        // WebView UI functionality

        void windowTitle(const std::string &title) override { webview.set_title(title); }
        void show() override { webview.show(); }
        void hide() override { webview.hide(); }
    };
}
