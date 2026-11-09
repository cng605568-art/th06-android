#include "platform/AndroidStorage.hpp"

#include <jni.h>

#include <SDL.h>
#include <SDL_system.h>

#include "core/Log.hpp"

namespace touhou {
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

namespace {

// The band's pixels from Java (ARGB ints); an extra trailing int, if present, is returned.
bool copyBand(ActivityClass& a, jintArray pixels, Image& band, jint* extra) {
    const jsize n = a.env->GetArrayLength(pixels);
    const jsize want = band.width * band.height;
    if (n != want && n != want + 1) return false;
    std::vector<jint> argb(static_cast<std::size_t>(n));
    a.env->GetIntArrayRegion(pixels, 0, n, argb.data());
    for (jsize i = 0; i < want; ++i) {
        const auto v = static_cast<std::uint32_t>(argb[static_cast<std::size_t>(i)]);
        std::uint8_t* p = band.rgba.data() + static_cast<std::size_t>(i) * 4;
        p[0] = static_cast<std::uint8_t>(v >> 16);
        p[1] = static_cast<std::uint8_t>(v >> 8);
        p[2] = static_cast<std::uint8_t>(v);
        p[3] = static_cast<std::uint8_t>(v >> 24);
    }
    if (extra && n == want + 1) *extra = argb.back();
    return true;
}

class AndroidTextRenderer final : public TextRenderer {
  public:
    float renderUtf8(std::string_view utf8, const TextStyle& style, float left, float width, TextAlign align,
                     Image& band) override {
        if (utf8.empty() || band.empty()) return 0.0f;
        ActivityClass a;
        const jmethodID mid = a.method("renderTextUtf8", "([BIIIFFIII)[I");
        if (!mid) return 0.0f;
        jbyteArray bytes = a.env->NewByteArray(static_cast<jsize>(utf8.size()));
        a.env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(utf8.size()), reinterpret_cast<const jbyte*>(utf8.data()));
        // The alignment's bit 4 asks for italics (th07's English text; th06 never sets it).
        const jint alignFlags = static_cast<jint>(align) | (style.italic ? kItalicFlag : 0);
        auto pixels = static_cast<jintArray>(a.env->CallStaticObjectMethod(
            a.cls, mid, bytes, style.fontSize, static_cast<jint>(style.rgb), static_cast<jint>(style.shadow), left, width,
            alignFlags, band.width, band.height));
        a.env->DeleteLocalRef(bytes);
        if (a.clearException() || !pixels) return 0.0f;
        jint drawn = 0;  // texels x 256
        const bool ok = copyBand(a, pixels, band, &drawn);
        a.env->DeleteLocalRef(pixels);
        return ok ? static_cast<float>(drawn) / 256.0f : 0.0f;
    }

    // GameActivity.measureTextUtf8(bytes, fontSize, flags) where the activity has it (th07);
    // otherwise a render into a scratch band.
    float measureUtf8(std::string_view utf8, const TextStyle& style) override {
        if (utf8.empty()) return 0.0f;
        if (!measureChecked_) {
            measureChecked_ = true;
            ActivityClass probe;
            hasMeasure_ = probe.method("measureTextUtf8", "([BII)F") != nullptr;
        }
        if (!hasMeasure_) return TextRenderer::measureUtf8(utf8, style);
        ActivityClass a;
        const jmethodID mid = a.method("measureTextUtf8", "([BII)F");
        if (!mid) return 0.0f;
        jbyteArray bytes = a.env->NewByteArray(static_cast<jsize>(utf8.size()));
        a.env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(utf8.size()), reinterpret_cast<const jbyte*>(utf8.data()));
        const jfloat w = a.env->CallStaticFloatMethod(a.cls, mid, bytes, style.fontSize, style.italic ? kItalicFlag : 0);
        a.env->DeleteLocalRef(bytes);
        if (a.clearException()) return 0.0f;
        return w;
    }

  private:
    static constexpr jint kItalicFlag = 16;
    bool measureChecked_ = false, hasMeasure_ = false;

  public:

    bool render(std::span<const std::uint8_t> sjis, const TextStyle& style, float startX, Image& band) override {
        if (sjis.empty() || band.empty()) return false;
        ActivityClass a;
        const jmethodID mid = a.method("renderText", "([BIIIFII)[I");
        if (!mid) return false;
        jbyteArray bytes = a.env->NewByteArray(static_cast<jsize>(sjis.size()));
        a.env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(sjis.size()), reinterpret_cast<const jbyte*>(sjis.data()));
        auto pixels = static_cast<jintArray>(a.env->CallStaticObjectMethod(
            a.cls, mid, bytes, style.fontSize, static_cast<jint>(style.rgb), static_cast<jint>(style.shadow), startX,
            band.width, band.height));
        a.env->DeleteLocalRef(bytes);
        if (a.clearException() || !pixels) return false;
        const jsize n = a.env->GetArrayLength(pixels);
        bool ok = false;
        if (n == band.width * band.height) {
            std::vector<jint> argb(static_cast<std::size_t>(n));
            a.env->GetIntArrayRegion(pixels, 0, n, argb.data());
            for (jsize i = 0; i < n; ++i) {
                const auto v = static_cast<std::uint32_t>(argb[static_cast<std::size_t>(i)]);
                std::uint8_t* p = band.rgba.data() + static_cast<std::size_t>(i) * 4;
                p[0] = static_cast<std::uint8_t>(v >> 16);
                p[1] = static_cast<std::uint8_t>(v >> 8);
                p[2] = static_cast<std::uint8_t>(v);
                p[3] = static_cast<std::uint8_t>(v >> 24);
            }
            ok = true;
        }
        a.env->DeleteLocalRef(pixels);
        return ok;
    }
};

}  // namespace

std::unique_ptr<TextRenderer> makeAndroidTextRenderer() { return std::make_unique<AndroidTextRenderer>(); }

std::vector<std::string> readAndroidGameTextStack() {
    std::vector<std::string> out;
    ActivityClass a;
    const jmethodID mid = a.method("getGameTextStack", "()[Ljava/lang/String;");
    if (!mid) return out;
    auto array = static_cast<jobjectArray>(a.env->CallStaticObjectMethod(a.cls, mid));
    if (a.clearException() || !array) return out;
    const jsize n = a.env->GetArrayLength(array);
    for (jsize i = 0; i < n; ++i) {
        auto s = static_cast<jstring>(a.env->GetObjectArrayElement(array, i));
        out.push_back(toUtf8(a.env, s));
        if (s) a.env->DeleteLocalRef(s);
    }
    a.env->DeleteLocalRef(array);
    return out;
}

ControlOptions readAndroidControlOptions() {
    ControlOptions out;
    ActivityClass a;
    const jmethodID mid = a.method("getControlOptions", "()[I");
    if (!mid) return out;
    auto array = static_cast<jintArray>(a.env->CallStaticObjectMethod(a.cls, mid));
    if (a.clearException() || !array) return out;
    const jsize n = a.env->GetArrayLength(array);
    if (n >= 8) {
        jint v[8] = {};
        a.env->GetIntArrayRegion(array, 0, 8, v);
        out.drag = v[0] != 0;
        out.sensitivity = static_cast<float>(v[1]) / 100.0f;
        out.buttonScale = static_cast<float>(v[2]) / 100.0f;
        out.opacity = static_cast<float>(v[3]) / 100.0f;
        out.leftHanded = v[4] != 0;
        out.focusToggle = v[5] != 0;
        out.autoFire = v[6] != 0;
        out.vibrate = v[7] != 0;
    }
    if (n >= 11) {
        jint p[3] = {};
        a.env->GetIntArrayRegion(array, 8, 3, p);
        out.speedPercent = p[0] < 10 || p[0] > 100 ? 100 : p[0];
        out.showHitbox = p[1] != 0;
        out.saveStates = p[2] != 0;
    }
    a.env->DeleteLocalRef(array);
    return out;
}

DisplayOptions readAndroidDisplayOptions() {
    DisplayOptions out;
    ActivityClass a;
    const jmethodID mid = a.method("getDisplayOptions", "()[I");
    if (!mid) return out;
    auto array = static_cast<jintArray>(a.env->CallStaticObjectMethod(a.cls, mid));
    if (a.clearException() || !array) return out;
    if (a.env->GetArrayLength(array) >= 3) {
        jint v[3] = {};
        a.env->GetIntArrayRegion(array, 0, 3, v);
        out.portrait = v[0] != 0;
        out.scaling = v[1] < 0 || v[1] > 2 ? 0 : v[1];
        out.scanlines = v[2] != 0;
    }
    a.env->DeleteLocalRef(array);
    return out;
}

void androidVibrate(int milliseconds) {
    ActivityClass a;
    const jmethodID mid = a.method("vibrate", "(I)V");
    if (!mid) return;
    a.env->CallStaticVoidMethod(a.cls, mid, static_cast<jint>(milliseconds));
    a.clearException();
}

std::vector<int> androidIntArray(const char* method) {
    std::vector<int> out;
    ActivityClass a;
    const jmethodID mid = a.method(method, "()[I");
    if (!mid) return out;
    auto array = static_cast<jintArray>(a.env->CallStaticObjectMethod(a.cls, mid));
    if (a.clearException() || !array) return out;
    const jsize n = a.env->GetArrayLength(array);
    std::vector<jint> v(static_cast<std::size_t>(n));
    if (n > 0) a.env->GetIntArrayRegion(array, 0, n, v.data());
    a.env->DeleteLocalRef(array);
    out.assign(v.begin(), v.end());
    return out;
}

}  // namespace touhou
