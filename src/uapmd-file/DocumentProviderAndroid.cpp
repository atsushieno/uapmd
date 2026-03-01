#include <uapmd-file/IDocumentProvider.hpp>

#include <jmi.h>
#include <jni.h>
#include <android/log.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

#define UAPMD_FILE_LOG_TAG "uapmd-file"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, UAPMD_FILE_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  UAPMD_FILE_LOG_TAG, __VA_ARGS__)

namespace uapmd {

// ─── Java class tags for JMI ─────────────────────────────────────────────────

struct JUri : jmi::ClassTag {
    static constexpr auto name() { return JMISTR("android/net/Uri"); }
};
struct JIntent : jmi::ClassTag {
    static constexpr auto name() { return JMISTR("android/content/Intent"); }
};
struct JContentResolver : jmi::ClassTag {
    static constexpr auto name() { return JMISTR("android/content/ContentResolver"); }
};
struct JContext : jmi::ClassTag {
    static constexpr auto name() { return JMISTR("android/content/Context"); }
};
struct JActivity : jmi::ClassTag {
    static constexpr auto name() { return JMISTR("android/app/Activity"); }
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace {

// Parse a URI string into a jobject (local ref; caller must DeleteLocalRef).
// Uses raw JNI because Uri.parse() is a static method.
jobject parseUri(JNIEnv* env, const std::string& uriStr)
{
    jclass cls = env->FindClass("android/net/Uri");
    jmethodID parse = env->GetStaticMethodID(cls, "parse",
        "(Ljava/lang/String;)Landroid/net/Uri;");
    jstring s = env->NewStringUTF(uriStr.c_str());
    jobject uri = env->CallStaticObjectMethod(cls, parse, s);
    env->DeleteLocalRef(s);
    env->DeleteLocalRef(cls);
    return uri;
}

// Get the URI string from a Uri jobject.
std::string uriToString(JNIEnv* env, jobject uriObj)
{
    jclass cls = env->GetObjectClass(uriObj);
    jmethodID toStr = env->GetMethodID(cls, "toString", "()Ljava/lang/String;");
    auto jstr = static_cast<jstring>(env->CallObjectMethod(uriObj, toStr));
    const char* chars = env->GetStringUTFChars(jstr, nullptr);
    std::string result = chars;
    env->ReleaseStringUTFChars(jstr, chars);
    env->DeleteLocalRef(jstr);
    env->DeleteLocalRef(cls);
    return result;
}

// Best-effort display name via Uri.getLastPathSegment().
// A proper implementation would query OpenableColumns.DISPLAY_NAME via
// ContentResolver.query(), but this is sufficient for a first pass.
std::string displayNameFromUri(JNIEnv* env, jobject uriObj)
{
    jclass cls = env->GetObjectClass(uriObj);
    jmethodID seg = env->GetMethodID(cls, "getLastPathSegment", "()Ljava/lang/String;");
    auto jstr = static_cast<jstring>(env->CallObjectMethod(uriObj, seg));
    env->DeleteLocalRef(cls);
    if (!jstr) return {};
    const char* chars = env->GetStringUTFChars(jstr, nullptr);
    std::string result = chars;
    env->ReleaseStringUTFChars(jstr, chars);
    env->DeleteLocalRef(jstr);
    return result;
}

// Read all bytes from a Java InputStream into a vector.
std::vector<uint8_t> drainInputStream(JNIEnv* env, jobject stream)
{
    jclass cls = env->GetObjectClass(stream);
    jmethodID readMethod  = env->GetMethodID(cls, "read", "([BII)I");
    jmethodID closeMethod = env->GetMethodID(cls, "close", "()V");
    env->DeleteLocalRef(cls);

    constexpr int kBuf = 4096;
    jbyteArray buf = env->NewByteArray(kBuf);
    std::vector<uint8_t> result;

    while (true) {
        jint n = env->CallIntMethod(stream, readMethod, buf, 0, kBuf);
        if (n <= 0) break;
        jbyte* bytes = env->GetByteArrayElements(buf, nullptr);
        result.insert(result.end(), bytes, bytes + n);
        env->ReleaseByteArrayElements(buf, bytes, JNI_ABORT);
    }
    env->DeleteLocalRef(buf);
    env->CallVoidMethod(stream, closeMethod);
    return result;
}

// Write all bytes to a Java OutputStream and close it.
void fillOutputStream(JNIEnv* env, jobject stream, const std::vector<uint8_t>& data)
{
    jclass cls = env->GetObjectClass(stream);
    jmethodID writeMethod = env->GetMethodID(cls, "write", "([BII)V");
    jmethodID flushMethod = env->GetMethodID(cls, "flush", "()V");
    jmethodID closeMethod = env->GetMethodID(cls, "close", "()V");
    env->DeleteLocalRef(cls);

    constexpr int kBuf = 4096;
    jbyteArray buf = env->NewByteArray(kBuf);
    size_t offset = 0;
    while (offset < data.size()) {
        auto n = static_cast<jint>(std::min<size_t>(kBuf, data.size() - offset));
        env->SetByteArrayRegion(buf, 0, n,
            reinterpret_cast<const jbyte*>(data.data() + offset));
        env->CallVoidMethod(stream, writeMethod, buf, 0, n);
        offset += n;
    }
    env->DeleteLocalRef(buf);
    env->CallVoidMethod(stream, flushMethod);
    env->CallVoidMethod(stream, closeMethod);
}

// Replace non-alphanumeric characters so a URI can be used as a filename.
std::string sanitizeForFilename(const std::string& id)
{
    std::string out;
    out.reserve(id.size());
    for (unsigned char c : id)
        out += (std::isalnum(c) || c == '.') ? static_cast<char>(c) : '_';
    return out;
}

} // anonymous namespace

// ─── Provider ────────────────────────────────────────────────────────────────

class DocumentProviderAndroid : public IDocumentProvider {
    jobject activity_;  // Global ref to the host Activity

    int next_request_code_ = 0x4F50; // 'OP' — arbitrary start value
    struct PendingPick {
        PickCallback callback;
        bool allow_multiple;
    };
    std::mutex pending_mutex_;
    std::map<int, PendingPick> pending_picks_;

    // Temp files created by resolveToPath, keyed by handle id
    std::mutex temp_mutex_;
    std::map<std::string, std::filesystem::path> temp_files_;

    // Cached cache-dir path (populated on first resolveToPath call)
    std::string cache_dir_;

public:
    explicit DocumentProviderAndroid(JNIEnv* env, jobject activity)
        : activity_(env->NewGlobalRef(activity))
    {}

    ~DocumentProviderAndroid() override
    {
        jmi::getEnv()->DeleteGlobalRef(activity_);

        std::lock_guard lock(temp_mutex_);
        for (auto& [id, path] : temp_files_) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    // ── Picking ──────────────────────────────────────────────────────────────

    void pickOpenDocuments(
        std::vector<DocumentFilter> filters,
        bool allowMultiple,
        PickCallback callback) override
    {
        JNIEnv* env = jmi::getEnv();

        // Build the Intent
        jstring action = env->NewStringUTF("android.intent.action.OPEN_DOCUMENT");
        jmi::JObject<JIntent> intent;
        intent.create(action);
        env->DeleteLocalRef(action);

        // Collect MIME types
        std::vector<std::string> mimes;
        for (auto& f : filters)
            for (auto& m : f.mime_types)
                mimes.push_back(m);

        if (mimes.size() == 1) {
            jstring mt = env->NewStringUTF(mimes[0].c_str());
            intent.call<void>("setType", mt);
            env->DeleteLocalRef(mt);
        } else {
            // Broad type + EXTRA_MIME_TYPES for multiple
            jstring any = env->NewStringUTF("*/*");
            intent.call<void>("setType", any);
            env->DeleteLocalRef(any);

            if (!mimes.empty()) {
                jclass strClass = env->FindClass("java/lang/String");
                jobjectArray arr = env->NewObjectArray(
                    static_cast<jsize>(mimes.size()), strClass, nullptr);
                for (size_t i = 0; i < mimes.size(); ++i) {
                    jstring s = env->NewStringUTF(mimes[i].c_str());
                    env->SetObjectArrayElement(arr, static_cast<jsize>(i), s);
                    env->DeleteLocalRef(s);
                }
                jstring extraKey = env->NewStringUTF("android.intent.extra.MIME_TYPES");
                // jobjectArray has no jmi::signature<> specialisation — use raw JNI
                jclass intentCls = env->GetObjectClass(intent);
                jmethodID putExtraArr = env->GetMethodID(intentCls, "putExtra",
                    "(Ljava/lang/String;[Ljava/lang/String;)Landroid/content/Intent;");
                env->CallObjectMethod(intent, putExtraArr, extraKey, arr);
                env->DeleteLocalRef(intentCls);
                env->DeleteLocalRef(extraKey);
                env->DeleteLocalRef(arr);
                env->DeleteLocalRef(strClass);
            }
        }

        jstring cat = env->NewStringUTF("android.intent.category.OPENABLE");
        intent.call<void>("addCategory", cat);
        env->DeleteLocalRef(cat);

        if (allowMultiple) {
            jstring key = env->NewStringUTF("android.intent.extra.ALLOW_MULTIPLE");
            intent.call<void>("putExtra", key, static_cast<jboolean>(JNI_TRUE));
            env->DeleteLocalRef(key);
        }

        int requestCode = registerPick(std::move(callback), allowMultiple);
        jmi::JObject<JActivity> act{activity_, false};
        act.call<void>("startActivityForResult", intent, static_cast<jint>(requestCode));
    }

    void pickSaveDocument(
        std::string defaultName,
        std::vector<DocumentFilter> filters,
        PickCallback callback) override
    {
        JNIEnv* env = jmi::getEnv();

        jstring action = env->NewStringUTF("android.intent.action.CREATE_DOCUMENT");
        jmi::JObject<JIntent> intent;
        intent.create(action);
        env->DeleteLocalRef(action);

        // Pick MIME type from first filter that has one
        std::string mime = "*/*";
        for (auto& f : filters)
            if (!f.mime_types.empty()) { mime = f.mime_types[0]; break; }
        jstring mt = env->NewStringUTF(mime.c_str());
        intent.call<void>("setType", mt);
        env->DeleteLocalRef(mt);

        jstring cat = env->NewStringUTF("android.intent.category.OPENABLE");
        intent.call<void>("addCategory", cat);
        env->DeleteLocalRef(cat);

        if (!defaultName.empty()) {
            jstring titleKey = env->NewStringUTF("android.intent.extra.TITLE");
            jstring titleVal = env->NewStringUTF(defaultName.c_str());
            intent.call<void>("putExtra", titleKey, titleVal);
            env->DeleteLocalRef(titleKey);
            env->DeleteLocalRef(titleVal);
        }

        int requestCode = registerPick(std::move(callback), false);
        jmi::JObject<JActivity> act{activity_, false};
        act.call<void>("startActivityForResult", intent, static_cast<jint>(requestCode));
    }

    // ── Called by the bridge when Activity.onActivityResult fires ─────────────

    void onActivityResult(JNIEnv* env, int requestCode, int resultCode, jobject intentObj)
    {
        PendingPick pick;
        {
            std::lock_guard lock(pending_mutex_);
            auto it = pending_picks_.find(requestCode);
            if (it == pending_picks_.end()) return;
            pick = std::move(it->second);
            pending_picks_.erase(it);
        }

        constexpr int RESULT_OK = -1; // android.app.Activity.RESULT_OK
        if (resultCode != RESULT_OK || !intentObj) {
            pick.callback({true, {}, {}}); // Cancelled
            return;
        }

        jmi::JObject<JIntent> intent{intentObj};
        std::vector<DocumentHandle> handles;

        // Multiple selection comes via ClipData
        jobject clipData = intent.call<jobject>("getClipData");
        if (clipData) {
            jclass cdClass   = env->GetObjectClass(clipData);
            jmethodID getCount = env->GetMethodID(cdClass, "getItemCount", "()I");
            jmethodID getItem  = env->GetMethodID(cdClass, "getItemAt",
                "(I)Landroid/content/ClipData$Item;");
            jint count = env->CallIntMethod(clipData, getCount);

            for (jint i = 0; i < count; ++i) {
                jobject item = env->CallObjectMethod(clipData, getItem, i);
                jclass itemClass = env->GetObjectClass(item);
                jmethodID getUri = env->GetMethodID(itemClass, "getUri", "()Landroid/net/Uri;");
                jobject uriObj = env->CallObjectMethod(item, getUri);

                handles.push_back(makeHandle(env, uriObj));
                takePersistablePermission(env, uriObj);

                env->DeleteLocalRef(uriObj);
                env->DeleteLocalRef(itemClass);
                env->DeleteLocalRef(item);
            }
            env->DeleteLocalRef(cdClass);
            env->DeleteLocalRef(clipData);
        } else {
            // Single selection via getData()
            jobject uriObj = intent.call<jobject>("getData");
            if (uriObj) {
                handles.push_back(makeHandle(env, uriObj));
                takePersistablePermission(env, uriObj);
                env->DeleteLocalRef(uriObj);
            }
        }

        pick.callback({true, std::move(handles), {}});
    }

    // ── I/O ──────────────────────────────────────────────────────────────────

    void readDocument(DocumentHandle handle, ReadCallback callback) override
    {
        JNIEnv* env = jmi::getEnv();
        auto resolver = contentResolver(env);

        jobject uriObj = parseUri(env, handle.id);
        jobject stream = resolver.call<jobject>("openInputStream", uriObj);
        env->DeleteLocalRef(uriObj);

        if (!stream) {
            callback({false, "openInputStream failed for: " + handle.id}, {});
            return;
        }
        auto data = drainInputStream(env, stream);
        env->DeleteLocalRef(stream);
        callback({true, {}}, std::move(data));
    }

    void writeDocument(DocumentHandle handle, std::vector<uint8_t> data, WriteCallback callback) override
    {
        JNIEnv* env = jmi::getEnv();
        auto resolver = contentResolver(env);

        jobject uriObj = parseUri(env, handle.id);
        // "wt" truncates the file before writing
        jstring mode = env->NewStringUTF("wt");
        jobject stream = resolver.call<jobject>("openOutputStream", uriObj, mode);
        env->DeleteLocalRef(mode);
        env->DeleteLocalRef(uriObj);

        if (!stream) {
            callback({false, "openOutputStream failed for: " + handle.id});
            return;
        }
        fillOutputStream(env, stream, data);
        env->DeleteLocalRef(stream);
        callback({true, {}});
    }

    void resolveToPath(DocumentHandle handle, PathCallback callback) override
    {
        // Return a cached temp file if still valid
        {
            std::lock_guard lock(temp_mutex_);
            auto it = temp_files_.find(handle.id);
            if (it != temp_files_.end() && std::filesystem::exists(it->second)) {
                callback({true, {}}, it->second);
                return;
            }
        }

        // Stream the content URI into a temp file
        readDocument(handle, [this, handle, callback]
            (DocumentIOResult r, std::vector<uint8_t> data)
        {
            if (!r.success) {
                callback({false, r.error}, {});
                return;
            }

            auto tempPath = cacheDir() / ("uapmd_" + sanitizeForFilename(handle.id));
            std::ofstream f(tempPath, std::ios::binary | std::ios::trunc);
            if (!f) {
                callback({false, "Failed to create temp file at " + tempPath.string()}, {});
                return;
            }
            f.write(reinterpret_cast<const char*>(data.data()),
                    static_cast<std::streamsize>(data.size()));
            f.close();

            {
                std::lock_guard lock(temp_mutex_);
                temp_files_[handle.id] = tempPath;
            }
            callback({true, {}}, tempPath);
        });
    }

    // URI permission was already taken in onActivityResult; just serialise the URI.
    std::string persistHandle(const DocumentHandle& handle) override
    {
        return handle.id;
    }

    std::optional<DocumentHandle> restoreHandle(const std::string& token) override
    {
        if (token.empty()) return std::nullopt;
        DocumentHandle h;
        h.id = token;
        // Reconstruct a best-effort display name from the URI's last path segment
        auto slash = token.rfind('/');
        h.display_name = (slash != std::string::npos) ? token.substr(slash + 1) : token;
        return h;
    }

private:
    int registerPick(PickCallback callback, bool allowMultiple)
    {
        std::lock_guard lock(pending_mutex_);
        int code = next_request_code_++;
        pending_picks_[code] = {std::move(callback), allowMultiple};
        return code;
    }

    DocumentHandle makeHandle(JNIEnv* env, jobject uriObj)
    {
        DocumentHandle h;
        h.id = uriToString(env, uriObj);
        h.display_name = displayNameFromUri(env, uriObj);
        return h;
    }

    void takePersistablePermission(JNIEnv* env, jobject uriObj)
    {
        auto resolver = contentResolver(env);
        // FLAG_GRANT_READ_URI_PERMISSION (1) | FLAG_GRANT_WRITE_URI_PERMISSION (2)
        resolver.call<void>("takePersistableUriPermission", uriObj, static_cast<jint>(3));
    }

    jmi::JObject<JContentResolver> contentResolver(JNIEnv*)
    {
        jmi::JObject<JContext> ctx{activity_, false};
        return jmi::JObject<JContentResolver>{ctx.call<jobject>("getContentResolver")};
    }

    std::filesystem::path cacheDir()
    {
        if (!cache_dir_.empty())
            return std::filesystem::path(cache_dir_);

        JNIEnv* env = jmi::getEnv();
        jmi::JObject<JContext> ctx{activity_, false};
        jobject fileObj = ctx.call<jobject>("getCacheDir");
        jclass fileCls = env->GetObjectClass(fileObj);
        jmethodID getPath = env->GetMethodID(fileCls, "getAbsolutePath", "()Ljava/lang/String;");
        auto jstr = static_cast<jstring>(env->CallObjectMethod(fileObj, getPath));
        const char* chars = env->GetStringUTFChars(jstr, nullptr);
        cache_dir_ = chars;
        env->ReleaseStringUTFChars(jstr, chars);
        env->DeleteLocalRef(jstr);
        env->DeleteLocalRef(fileCls);
        env->DeleteLocalRef(fileObj);
        return std::filesystem::path(cache_dir_);
    }
};

// ─── Global instance and bridge ──────────────────────────────────────────────

static DocumentProviderAndroid* g_active_provider = nullptr;

void initDocumentProvider(JNIEnv* env, jobject activity)
{
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    jmi::javaVM(vm); // Must be called before any jmi::env() usage

    delete g_active_provider;
    g_active_provider = new DocumentProviderAndroid(env, activity);
}

void documentProvider_onActivityResult(
    JNIEnv* env, int requestCode, int resultCode, jobject intent)
{
    if (g_active_provider)
        g_active_provider->onActivityResult(env, requestCode, resultCode, intent);
}

std::unique_ptr<IDocumentProvider> createDocumentProvider()
{
    if (!g_active_provider) {
        LOGE("createDocumentProvider() called before initDocumentProvider(). "
             "Call initDocumentProvider(env, activity) first.");
        return nullptr;
    }
    // Return a non-owning wrapper around the global singleton.
    // The singleton lifetime is managed by initDocumentProvider / process exit.
    struct NonOwning : IDocumentProvider {
        DocumentProviderAndroid* p;
        explicit NonOwning(DocumentProviderAndroid* p_) : p(p_) {}
        void pickOpenDocuments(std::vector<DocumentFilter> f, bool m, PickCallback cb) override
            { p->pickOpenDocuments(std::move(f), m, std::move(cb)); }
        void pickSaveDocument(std::string n, std::vector<DocumentFilter> f, PickCallback cb) override
            { p->pickSaveDocument(std::move(n), std::move(f), std::move(cb)); }
        void readDocument(DocumentHandle h, ReadCallback cb) override
            { p->readDocument(std::move(h), std::move(cb)); }
        void writeDocument(DocumentHandle h, std::vector<uint8_t> d, WriteCallback cb) override
            { p->writeDocument(std::move(h), std::move(d), std::move(cb)); }
        void resolveToPath(DocumentHandle h, PathCallback cb) override
            { p->resolveToPath(std::move(h), std::move(cb)); }
        std::string persistHandle(const DocumentHandle& h) override
            { return p->persistHandle(h); }
        std::optional<DocumentHandle> restoreHandle(const std::string& t) override
            { return p->restoreHandle(t); }
    };
    return std::make_unique<NonOwning>(g_active_provider);
}

} // namespace uapmd
