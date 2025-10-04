#include <jni.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

#include <android/log.h>
#include <android/native_window_jni.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

// OpenXRのヘッダー
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>


#define LOG_TAG "NativeVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// --- アプリケーションの状態を管理する構造体 ---
struct AppState {
    JavaVM* javaVm = nullptr;
    jobject activityObject = nullptr;
    ANativeWindow* nativeWindow = nullptr;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig config = nullptr;

    GLuint videoTextureId = 0;
    std::atomic<bool> glInitialized = {false};
    std::atomic<bool> appRunning = {false};
    std::atomic<bool> appResumed = {false};
    std::thread renderThread;
};

static AppState appState = {};

// --- OpenXRの初期化とレンダリングループ ---

void OpenXrRenderLoop() {
    LOGI("Render thread started.");

    // 1. EGL環境の初期化
    appState.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (appState.display == EGL_NO_DISPLAY) {
        LOGE("Failed to get EGL display");
        return;
    }
    eglInitialize(appState.display, nullptr, nullptr);

    // ▼▼▼ 修正点 1: 変数を配列として宣言 ▼▼▼
    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLint numConfigs;
    // この関数の2番目の引数はポインタを期待しており、配列を渡すと自動的にポインタとして扱われます。
    eglChooseConfig(appState.display, configAttribs, &appState.config, 1, &numConfigs);

    // ▼▼▼ 修正点 2: 変数を配列として宣言 ▼▼▼
    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    appState.context = eglCreateContext(appState.display, appState.config, EGL_NO_CONTEXT, contextAttribs);
    if (appState.context == EGL_NO_CONTEXT) {
        LOGE("Failed to create EGL context");
        return;
    }

    // 2. OpenXRインスタンスの作成
    std::vector<const char*> extensions = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.enabledExtensionNames = extensions.data();
    strcpy(createInfo.applicationInfo.applicationName, "MaineVR");
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrInstanceCreateInfoAndroidKHR createInfoAndroid = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    createInfoAndroid.applicationVM = appState.javaVm;
    createInfoAndroid.applicationActivity = appState.activityObject;
    createInfo.next = &createInfoAndroid;

    if (XR_FAILED(xrCreateInstance(&createInfo, &appState.instance))) {
        LOGE("Failed to create OpenXR instance");
        return;
    }

    // 3. OpenXRシステムの取得
    XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(appState.instance, &systemGetInfo, &appState.systemId))) {
        LOGE("Failed to get OpenXR system");
        return;
    }

    // 4. OpenXRセッションの作成 (ここでOpenGLコンテキストが紐付けられる)
    XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    graphicsBinding.display = appState.display;
    graphicsBinding.config = appState.config;
    graphicsBinding.context = appState.context;

    XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &graphicsBinding;
    sessionCreateInfo.systemId = appState.systemId;
    if (XR_FAILED(xrCreateSession(appState.instance, &sessionCreateInfo, &appState.session))) {
        LOGE("Failed to create OpenXR session");
        return;
    }
    LOGI("OpenXR session created successfully. OpenGL context is now valid on this thread.");

    // 5. ビデオテクスチャの生成
    LOGI("InitializeGraphics: Generating video texture...");
    glGenTextures(1, &appState.videoTextureId);
    if (appState.videoTextureId == 0) {
        LOGE("Failed to generate video texture. glGetError() = %d", glGetError());
        return;
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, appState.videoTextureId);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    LOGI("Successfully generated video texture ID: %d", appState.videoTextureId);
    appState.glInitialized = true;

    // 6. OpenXRレンダリングループ
    while (appState.appRunning) {
        if (!appState.appResumed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        // TODO: ここで実際のOpenXRのフレームごとのレンダリング処理（xrWaitFrame, xrBeginFrame, etc.）を行います。
        std::this_thread::sleep_for(std::chrono::milliseconds(11)); // 約90fps
    }

    // 7. クリーンアップ
    if (appState.videoTextureId!= 0) {
        glDeleteTextures(1, &appState.videoTextureId);
    }
    if (appState.session!= XR_NULL_HANDLE) xrDestroySession(appState.session);
    if (appState.instance!= XR_NULL_HANDLE) xrDestroyInstance(appState.instance);
    if (appState.display!= EGL_NO_DISPLAY) {
        eglDestroyContext(appState.display, appState.context);
        eglTerminate(appState.display);
    }

    LOGI("Render thread finished.");
}


// --- JNI関数 ---

// ライブラリロード時にJavaVMを取得
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    appState.javaVm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jstring JNICALL
Java_net_akaaku_mainevr_MainActivity_stringFromJNI(JNIEnv* env, jobject) {
    return env->NewStringUTF("VR Video Bridge Ready!");
}

extern "C" JNIEXPORT jint JNICALL
Java_net_akaaku_mainevr_MainActivity_nativeGetTextureId(JNIEnv* env, jobject) {
    if (appState.glInitialized && appState.videoTextureId > 0) {
        return (jint)appState.videoTextureId;
    }
    return -1;
}

extern "C" JNIEXPORT void JNICALL
Java_net_akaaku_mainevr_MainActivity_nativeOnCreate(JNIEnv* env, jobject activity) {
    LOGI("Native engine created.");
    appState.activityObject = env->NewGlobalRef(activity);
    appState.appRunning = true;
    appState.renderThread = std::thread(OpenXrRenderLoop);
}

extern "C" JNIEXPORT void JNICALL
Java_net_akaaku_mainevr_MainActivity_nativeOnResume(JNIEnv* env, jobject) {
    LOGI("Native engine resumed.");
    appState.appResumed = true;
}

extern "C" JNIEXPORT void JNICALL
Java_net_akaaku_mainevr_MainActivity_nativeOnPause(JNIEnv* env, jobject) {
    LOGI("Native engine paused.");
    appState.appResumed = false;
}

extern "C" JNIEXPORT void JNICALL
Java_net_akaaku_mainevr_MainActivity_nativeOnDestroy(JNIEnv* env, jobject) {
    LOGI("Native engine destroyed. Stopping render thread...");
    if (appState.appRunning) {
        appState.appRunning = false;
        if (appState.renderThread.joinable()) {
            appState.renderThread.join();
        }
    }
    if (appState.activityObject!= nullptr) {
        env->DeleteGlobalRef(appState.activityObject);
        appState.activityObject = nullptr;
    }
    appState.glInitialized = false;
    appState.videoTextureId = 0;
}

extern "C" JNIEXPORT void JNICALL
Java_net_akaaku_mainevr_MainActivity_nativeSetSurface(JNIEnv* env, jobject, jobject surface) {
    LOGI("Surface received from Java layer");
    if (appState.nativeWindow!= nullptr) {
        ANativeWindow_release(appState.nativeWindow);
        appState.nativeWindow = nullptr;
    }
    if (surface!= nullptr) {
        appState.nativeWindow = ANativeWindow_fromSurface(env, surface);
    }
}