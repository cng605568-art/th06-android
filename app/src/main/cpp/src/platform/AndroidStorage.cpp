#include "platform/AndroidStorage.hpp"

#include <jni.h>

#include <SDL.h>
#include <SDL_system.h>

#include "core/Log.hpp"

namespace th06 {
namespace {

// Resolves a static method on the running activity's own class. Going through the
// activity object (instead of FindClass) matters: SDL's main thread has no app class
// loader, so FindClass("io/github/...") would fail there.
struct ActivityClass {
    JNIEnv* env = nullptr;
    jobject activity = nullptr;
    jclass cls = nullptr;

    ActivityClass() {
        env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (!env) return;
        activity = static_cast<jobject>(SDL_AndroidGetActivity());
        if (activity) cls = env->GetObjectClass(activity);
    }
    ~ActivityClass() {
        if (!env) return;
        if (cls) env->DeleteLocalRef(cls);
        if (activity) env->DeleteLocalRef(activity);
    }
    ActivityClass(const ActivityClass&) = delete;
    ActivityClass& operator=(const ActivityClass&) = delete;

    jmethodID method(const char* name, const char* signature) const {
        if (!cls) return nullptr;
        jmethodID id = env->GetStaticMethodID(cls, name, signature);
        if (clearException()) return nullptr;
        return id;
    }
    bool clearException() const {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
};

std::string toUtf8(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(s, chars);
    return out;
}

}  // namespace

std::vector<AndroidDataSource> listAndroidDataSources() {
    std::vector<AndroidDataSource> out;
    ActivityClass a;
    const jmethodID mid = a.method("getDataSources", "()[Ljava/lang/String;");
    if (!mid) {
        log::error("GameActivity.getDataSources() not found");
        return out;
    }
    auto array = static_cast<jobjectArray>(a.env->CallStaticObjectMethod(a.cls, mid));
    if (a.clearException() || !array) return out;

    const jsize n = a.env->GetArrayLength(array);
    for (jsize i = 0; i + 2 < n; i += 3) {
        AndroidDataSource src;
        for (int k = 0; k < 3; ++k) {
            auto s = static_cast<jstring>(a.env->GetObjectArrayElement(array, i + k));
            std::string v = toUtf8(a.env, s);
            if (s) a.env->DeleteLocalRef(s);
            (k == 0 ? src.name : k == 1 ? src.uri : src.format) = std::move(v);
        }
        out.push_back(std::move(src));
    }
    a.env->DeleteLocalRef(array);
    log::info("Launcher selected {} archive(s)", out.size());
    return out;
}

int openAndroidDataSource(const std::string& uri) {
    ActivityClass a;
    const jmethodID mid = a.method("openDataSource", "(Ljava/lang/String;)I");
    if (!mid) return -1;
    jstring juri = a.env->NewStringUTF(uri.c_str());
    const jint fd = a.env->CallStaticIntMethod(a.cls, mid, juri);
    a.env->DeleteLocalRef(juri);
    if (a.clearException()) return -1;
    return fd;
}

}  // namespace th06
