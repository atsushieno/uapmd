#pragma once

#include "saucer/smartview.hpp"
#include "choc/network/choc_MIMETypes.h"

namespace remidy::webui::saucer_wrapper {

    class SaucerWebEmbedded {
        std::shared_ptr<saucer::application> app_;
        saucer::smartview<saucer::serializers::glaze::serializer> smartview;
        std::filesystem::path webroot;
        std::unordered_map<std::string, saucer::embedded_file> files{};

    public:
        static saucer::stash<> getStash(const std::filesystem::directory_entry &e) {
            std::ifstream ifs{};
            ifs.open(e.path().c_str());
            std::ostringstream oss{};
            oss << ifs.rdbuf();
            return saucer::make_stash(oss.str());
        }

        void processDirectory(const std::filesystem::path dir) {
            for (auto &e: std::filesystem::directory_iterator{dir}) {
                if (e.is_directory())
                    processDirectory(e.path());
                else {
                    auto file = saucer::embedded_file{
                            .content = saucer::stash<>::lazy([e]() { return getStash(e); }),
                            .mime = choc::network::getMIMETypeFromFilename(e.path())};
                    files.emplace(e.path(), file);
                }
            }
        }

        SaucerWebEmbedded(std::filesystem::path &webroot) :
                app_(saucer::application::init({.id = "SaucerWebEmbedded"})),
                smartview(saucer::smartview{{.application = app_}}),
                webroot(webroot) {

            processDirectory(webroot);

            smartview.embed(files);
        }

        std::shared_ptr<saucer::application> app() { return app_; }

        saucer::smartview<saucer::serializers::glaze::serializer> &webview() { return smartview; }
    };

}