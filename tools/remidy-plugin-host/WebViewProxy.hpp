#pragma once

#include <vector>
#include <map>
#include <span>
#include <string>
#include <functional>
#include <future>

namespace uapmd {
    // At this state, taking particular WebView implementation as our implementation foundation
    // seems risky, so I would rather wrap some of those in our WebView usage.
    // This class provides no better functionality than any of the underlying implementation,
    // but we do not want to work on higher-level features on top of unstable foundation.
    class WebViewProxy {
    public:
        struct Content {
            std::string mimeType;
            std::string_view data;
        };

        struct Configuration {
            bool enableDebugger{false};
            bool transparentBackground{false};
            std::function<std::optional<Content>(const std::string& path)> resolvePath;
        };

        enum class ValueType {
            Array,
            Map,
            Double,
            Int,
            Bool,
            String
        };

        class Value {
        protected:
            explicit Value() {}
        public:
            virtual ~Value() {}

            virtual ValueType type() = 0;
            // You can skip implementation anything other than the expected type.
            virtual bool toBool() { throw std::runtime_error("Unsupported type"); }
            virtual int32_t toInt() { throw std::runtime_error("Unsupported type"); }
            virtual double toDouble() { throw std::runtime_error("Unsupported type"); }
            virtual std::string_view toString() { throw std::runtime_error("Unsupported type"); }
            virtual std::vector<Value> toArray() { throw std::runtime_error("Unsupported type"); }
            virtual std::map<Value,Value> toMap() { throw std::runtime_error("Unsupported type"); }
        };

    protected:
        Configuration config;
        explicit WebViewProxy(Configuration& config) : config(config) {}

    public:
        virtual ~WebViewProxy() = default;

        virtual void navigateTo(const std::string& url) = 0;
        virtual void navigateToLocalFile(const std::string& localFile) = 0;
        virtual void loadContent(const std::string& data) = 0;

        // functions work asynchronously (in this common layer)
        virtual void registerFunction(const std::string &jsName, std::function<std::string(const std::string_view&)>&& func) = 0;
        virtual void registerFunction(const std::string& jsName, ValueType returnType, std::function<Value*(const std::vector<Value*>)>&& func) = 0;
        virtual void evalJS(const std::string& js) = 0;

        // UI controller
        virtual void windowTitle(const std::string& title) = 0;
        virtual void show() = 0;
        virtual void hide() = 0;

        /*
        // The returned value has to be deleted at the caller site.
        virtual Value* toValue(const std::string_view& s) = 0;
        // The returned value has to be deleted at the caller site.
        virtual Value* toValue(int32_t i) = 0;
        // The returned value has to be deleted at the caller site.
        virtual Value* toValue(double d) = 0;
         */
    };
}
