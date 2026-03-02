#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <uapmd-file/IDocumentProvider.hpp>

#include "AppModel.hpp"

inline void resolveDocumentHandle(
    const uapmd::DocumentHandle& handle,
    std::function<void(const std::filesystem::path&)> onSuccess,
    std::function<void(const std::string&)> onError = {})
{
    auto* provider = uapmd::AppModel::instance().documentProvider();
    if (!provider) {
        if (onError)
            onError("Document provider unavailable");
        return;
    }

    provider->resolveToPath(handle,
        [onSuccess = std::move(onSuccess), onError = std::move(onError)]
        (uapmd::DocumentIOResult ioResult, std::filesystem::path path) mutable {
            if (!ioResult.success) {
                if (onError)
                    onError(ioResult.error);
                return;
            }
            onSuccess(path);
        });
}
