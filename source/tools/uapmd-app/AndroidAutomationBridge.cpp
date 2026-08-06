#if defined(__ANDROID__)

#include <uapmd-app-model/uapmd-app-model.hpp>

#include <android/log.h>
#include <jni.h>
#include <remidy/remidy.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr const char* kLogTag = "uapmd-adb";

// Shared across JNI calls from arbitrary threads; evaluateScriptNow() takes
// this lock for the whole bootstrap+evaluate sequence since UapmdJSRuntime
// itself is not thread-safe for concurrent use.
std::mutex g_js_runtime_mutex;
std::unique_ptr<uapmd_app::UapmdJSRuntime> g_js_runtime;

struct AutomationJob {
    std::string job_id;
    std::string state{"running"};
    std::string result{"null"};
};

std::mutex g_jobs_mutex;
std::map<std::string, std::shared_ptr<AutomationJob>> g_jobs;
std::atomic<uint64_t> g_next_job_id{1};

std::string evaluateScriptNow(const std::string& code) {
    std::lock_guard lock(g_js_runtime_mutex);
    if (!g_js_runtime)
        g_js_runtime = std::make_unique<uapmd_app::UapmdJSRuntime>();
    g_js_runtime->ensureApiBootstrapped();
    return g_js_runtime->evaluateScript(code);
}

std::string escapeJson(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", ch & 0xFF);
                    escaped += buf;
                } else {
                    escaped += ch;
                }
                break;
        }
    }
    return escaped;
}

std::string jobToJson(const AutomationJob& job) {
    return "{\"jobId\":\"" + escapeJson(job.job_id) +
           "\",\"state\":\"" + escapeJson(job.state) +
           "\",\"result\":" + job.result + "}";
}

std::string evaluateScriptOnAppThread(const std::string& code) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::string result;
    std::string error;

    remidy::EventLoop::enqueueTaskOnMainThread([&]() {
        try {
            result = evaluateScriptNow(code);
        } catch (const std::exception& e) {
            error = e.what();
        }
        {
            std::lock_guard lock(mutex);
            done = true;
        }
        cv.notify_one();
    });

    std::unique_lock lock(mutex);
    if (!cv.wait_for(lock, std::chrono::seconds(90), [&] { return done; }))
        return "ERROR: timed out waiting for app thread";
    if (!error.empty())
        return "ERROR: " + error;
    return result;
}

std::shared_ptr<AutomationJob> createJob() {
    auto job = std::make_shared<AutomationJob>();
    job->job_id = "job-" + std::to_string(g_next_job_id.fetch_add(1));
    std::lock_guard lock(g_jobs_mutex);
    g_jobs[job->job_id] = job;
    return job;
}

std::shared_ptr<AutomationJob> findJob(const std::string& job_id) {
    std::lock_guard lock(g_jobs_mutex);
    auto it = g_jobs.find(job_id);
    return it == g_jobs.end() ? nullptr : it->second;
}

void removeJob(const std::string& job_id) {
    std::lock_guard lock(g_jobs_mutex);
    g_jobs.erase(job_id);
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_dev_atsushieno_uapmd_MainActivity_nativeRunAutomationScript(
    JNIEnv* env, jclass, jstring code) {
    if (!code)
        return env->NewStringUTF("ERROR: missing script");

    const char* chars = env->GetStringUTFChars(code, nullptr);
    std::string script = chars ? chars : "";
    if (chars)
        env->ReleaseStringUTFChars(code, chars);

    if (script.empty())
        return env->NewStringUTF("ERROR: empty script");

    const std::string result = evaluateScriptOnAppThread(script);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", result.c_str());
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_atsushieno_uapmd_MainActivity_nativeStartAutomationScriptJob(
    JNIEnv* env, jclass, jstring code) {
    if (!code)
        return env->NewStringUTF("ERROR: missing script");

    const char* chars = env->GetStringUTFChars(code, nullptr);
    std::string script = chars ? chars : "";
    if (chars)
        env->ReleaseStringUTFChars(code, chars);
    if (script.empty())
        return env->NewStringUTF("ERROR: empty script");

    auto job = createJob();
    std::thread([job, script = std::move(script)]() mutable {
        const std::string result = evaluateScriptOnAppThread(script);
        std::lock_guard lock(g_jobs_mutex);
        job->state = "completed";
        job->result = "\"" + escapeJson(result) + "\"";
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "async job %s completed: %s",
                            job->job_id.c_str(), result.c_str());
    }).detach();

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "started async job %s",
                        job->job_id.c_str());
    return env->NewStringUTF(job->job_id.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_atsushieno_uapmd_MainActivity_nativeGetAutomationScriptJob(
    JNIEnv* env, jclass, jstring jobId) {
    if (!jobId)
        return env->NewStringUTF("{\"error\":\"missing job id\"}");

    const char* chars = env->GetStringUTFChars(jobId, nullptr);
    std::string key = chars ? chars : "";
    if (chars)
        env->ReleaseStringUTFChars(jobId, chars);

    auto job = findJob(key);
    if (!job)
        return env->NewStringUTF("{\"error\":\"job not found\"}");

    const auto json = jobToJson(*job);
    return env->NewStringUTF(json.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_dev_atsushieno_uapmd_MainActivity_nativeClearAutomationScriptJob(
    JNIEnv* env, jclass, jstring jobId) {
    if (!jobId)
        return;
    const char* chars = env->GetStringUTFChars(jobId, nullptr);
    std::string key = chars ? chars : "";
    if (chars)
        env->ReleaseStringUTFChars(jobId, chars);
    removeJob(key);
}

#endif
