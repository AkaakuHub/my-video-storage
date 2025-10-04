#include <atomic>
#include <cmath> // For tan
#include <jni.h>
#include <string>
#include <thread>
#include <vector>

#include <android/log.h>
#include <android/native_window_jni.h>

#include <EGL/egl.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

// OpenXRのヘッダー
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define LOG_TAG "NativeVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ▼▼▼ 追加 ▼▼▼ 3Dグラフィックスのための簡単な行列計算ヘルパー
// 4x4 Matrix
struct Matrix4f {
  float M[4][4];
};

Matrix4f Matrix4f_CreateIdentity() {
  Matrix4f mat;
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      mat.M[i][j] = (i == j) ? 1.0f : 0.0f;
    }
  }
  return mat;
}

Matrix4f Matrix4f_Multiply(const Matrix4f *a, const Matrix4f *b) {
  Matrix4f result = {};
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      result.M[i][j] = a->M[i][0] * b->M[0][j] + a->M[i][1] * b->M[1][j] +
                       a->M[i][2] * b->M[2][j] + a->M[i][3] * b->M[3][j];
    }
  }
  return result;
}

Matrix4f Matrix4f_CreateProjectionFov(const XrFovf fov, const float nearZ,
                                      const float farZ) {
  const float tanAngleLeft = tanf(fov.angleLeft);
  const float tanAngleRight = tanf(fov.angleRight);
  const float tanAngleDown = tanf(fov.angleDown);
  const float tanAngleUp = tanf(fov.angleUp);

  const float tanAngleWidth = tanAngleRight - tanAngleLeft;
  const float tanAngleHeight = (tanAngleUp - tanAngleDown);

  Matrix4f result = {};
  result.M[0][0] = 2.0f / tanAngleWidth;
  result.M[1][1] = 2.0f / tanAngleHeight;
  result.M[0][2] = (tanAngleRight + tanAngleLeft) / tanAngleWidth;
  result.M[1][2] = (tanAngleUp + tanAngleDown) / tanAngleHeight;
  result.M[2][2] = -(farZ + nearZ) / (farZ - nearZ);
  result.M[3][2] = -1.0f;
  result.M[2][3] = -2.0f * farZ * nearZ / (farZ - nearZ);
  return result;
}

Matrix4f Matrix4f_CreateTranslation(const float x, const float y,
                                    const float z) {
  Matrix4f mat = Matrix4f_CreateIdentity();
  mat.M[0][3] = x;
  mat.M[1][3] = y;
  mat.M[2][3] = z;
  return mat;
}

Matrix4f Matrix4f_Inverse(const Matrix4f *mat) {
  // Note: This is a simplified inverse for translation/rotation only
  Matrix4f result;
  result.M[0][0] = mat->M[0][0];
  result.M[0][1] = mat->M[1][0];
  result.M[0][2] = mat->M[2][0];
  result.M[0][3] = -(mat->M[0][0] * mat->M[0][3] + mat->M[1][0] * mat->M[1][3] +
                     mat->M[2][0] * mat->M[2][3]);
  result.M[1][0] = mat->M[0][1];
  result.M[1][1] = mat->M[1][1];
  result.M[1][2] = mat->M[2][1];
  result.M[1][3] = -(mat->M[0][1] * mat->M[0][3] + mat->M[1][1] * mat->M[1][3] +
                     mat->M[2][1] * mat->M[2][3]);
  result.M[2][0] = mat->M[0][2];
  result.M[2][1] = mat->M[1][2];
  result.M[2][2] = mat->M[2][2];
  result.M[2][3] = -(mat->M[0][2] * mat->M[0][3] + mat->M[1][2] * mat->M[1][3] +
                     mat->M[2][2] * mat->M[2][3]);
  result.M[3][0] = 0.0f;
  result.M[3][1] = 0.0f;
  result.M[3][2] = 0.0f;
  result.M[3][3] = 1.0f;
  return result;
}

Matrix4f Matrix4f_CreateFromXrPose(const XrPosef *pose) {
  Matrix4f mat = Matrix4f_CreateIdentity();
  // Simplified: No rotation yet, just translation
  mat.M[0][3] = pose->position.x;
  mat.M[1][3] = pose->position.y;
  mat.M[2][3] = pose->position.z;
  return mat;
}
// ▲▲▲ 追加 ▲▲▲

// ▼▼▼ 追加 ▼▼▼ シェーダープログラム
// 頂点シェーダー：オブジェクトの頂点を3D空間のどこに表示するかを計算
static const char *VertexShaderSrc = R"glsl(
#version 320 es
uniform mat4 Mvp;
in vec3 VertexPos;
void main() {
   gl_Position = Mvp * vec4(VertexPos, 1.0);
}
)glsl";

// フラグメントシェーダー：各ピクセルの色を決定
static const char *FragmentShaderSrc = R"glsl(
#version 320 es
out lowp vec4 FragColor;
void main() {
   FragColor = vec4(1.0, 1.0, 1.0, 1.0); // 真っ白
}
)glsl";
// ▲▲▲ 追加 ▲▲▲

// --- アプリケーションの状態を管理する構造体 ---
struct AppState {
  JavaVM *javaVm = nullptr;
  jobject activityObject = nullptr;
  ANativeWindow *nativeWindow = nullptr;

  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId systemId = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;

  // ▼▼▼ 追加 ▼▼▼ VR描画に必要な要素
  XrSpace space = XR_NULL_HANDLE;
  std::vector<XrViewConfigurationView> viewConfigViews;
  std::vector<XrView> views;
  std::vector<XrSwapchain> swapchains;
  std::vector<std::vector<XrSwapchainImageOpenGLESKHR>> swapchainImages;
  GLuint screenShaderProgram = 0;
  GLuint screenVbo = 0;
  GLuint screenVao = 0;
  GLint mvpLocation = -1;
  // ▲▲▲ 追加 ▲▲▲

  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLConfig config = nullptr;

  GLuint videoTextureId = 0;
  std::atomic<bool> glInitialized = {false};
  std::atomic<bool> appRunning = {false};
  std::atomic<bool> appResumed = {false};
  std::thread renderThread;

  // ▼▼▼ 追加 ▼▼▼ セッションの状態
  XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
  bool sessionRunning = false;
  // ▲▲▲ 追加 ▲▲▲
};

static AppState appState = {};

// ▼▼▼ 追加 ▼▼▼ シェーダーのコンパイルとリンクを行うヘルパー関数
GLuint CreateShader(GLenum type, const char *src) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, nullptr);
  glCompileShader(shader);
  GLint status;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status == GL_FALSE) {
    char log[1024];
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    LOGE("Shader Compile Error: %s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

GLuint CreateProgram(const char *vs, const char *fs) {
  GLuint vertexShader = CreateShader(GL_VERTEX_SHADER, vs);
  GLuint fragmentShader = CreateShader(GL_FRAGMENT_SHADER, fs);
  if (vertexShader == 0 || fragmentShader == 0)
    return 0;

  GLuint program = glCreateProgram();
  glAttachShader(program, vertexShader);
  glAttachShader(program, fragmentShader);
  glLinkProgram(program);
  GLint status;
  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (status == GL_FALSE) {
    char log[1024];
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    LOGE("Program Link Error: %s", log);
    glDeleteProgram(program);
    return 0;
  }
  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);
  return program;
}

// スクリーン（ただの四角形）の頂点データを作成
void CreateSceneResources() {
  appState.screenShaderProgram =
      CreateProgram(VertexShaderSrc, FragmentShaderSrc);
  appState.mvpLocation =
      glGetUniformLocation(appState.screenShaderProgram, "Mvp");

  // 画面アスペクト比 16:9
  const float width = 1.6f;
  const float height = 0.9f;
  const float vertices[] = {-width, -height, 0.0f, width,  -height, 0.0f,
                            -width, height,  0.0f, width,  -height, 0.0f,
                            width,  height,  0.0f, -width, height,  0.0f};

  glGenVertexArrays(1, &appState.screenVao);
  glBindVertexArray(appState.screenVao);

  glGenBuffers(1, &appState.screenVbo);
  glBindBuffer(GL_ARRAY_BUFFER, appState.screenVbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

  GLint posLocation =
      glGetAttribLocation(appState.screenShaderProgram, "VertexPos");
  glEnableVertexAttribArray(posLocation);
  glVertexAttribPointer(posLocation, 3, GL_FLOAT, GL_FALSE, 0, 0);

  glBindVertexArray(0);
}
// ▲▲▲ 追加 ▲▲▲

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

  const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE,
                                  EGL_OPENGL_ES3_BIT,
                                  EGL_RED_SIZE,
                                  8,
                                  EGL_GREEN_SIZE,
                                  8,
                                  EGL_BLUE_SIZE,
                                  8,
                                  EGL_ALPHA_SIZE,
                                  8,
                                  EGL_DEPTH_SIZE,
                                  24,
                                  EGL_NONE};
  EGLint numConfigs;
  eglChooseConfig(appState.display, configAttribs, &appState.config, 1,
                  &numConfigs);

  const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  appState.context = eglCreateContext(appState.display, appState.config,
                                      EGL_NO_CONTEXT, contextAttribs);
  if (appState.context == EGL_NO_CONTEXT) {
    LOGE("Failed to create EGL context");
    return;
  }
  EGLSurface tinySurface =
      eglCreatePbufferSurface(appState.display, appState.config, nullptr);
  eglMakeCurrent(appState.display, tinySurface, tinySurface, appState.context);

  // 2. OpenXRインスタンスの作成
  // ▼▼▼ このブロックを追加 ▼▼▼
  // OpenXRローダーを初期化します。xrCreateInstanceの前に必ず必要です。
  PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR;
  xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                        (PFN_xrVoidFunction *)&xrInitializeLoaderKHR);
  if (!xrInitializeLoaderKHR) {
    LOGE("Failed to get xrInitializeLoaderKHR function");
    return;
  }
  XrLoaderInitInfoAndroidKHR loaderInitInfo = {
      XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
  loaderInitInfo.applicationVM = appState.javaVm;
  loaderInitInfo.applicationContext = appState.activityObject;
  if (XR_FAILED(xrInitializeLoaderKHR(
          (const XrLoaderInitInfoBaseHeaderKHR *)&loaderInitInfo))) {
    LOGE("Failed to initialize OpenXR loader");
    return;
  }
  // ▲▲▲ 追加ここまで ▲▲▲

  std::vector<const char *> extensions = {
      XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
      XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
  XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
  createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  createInfo.enabledExtensionNames = extensions.data();
  strcpy(createInfo.applicationInfo.applicationName, "MaineVR");
  createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

  XrInstanceCreateInfoAndroidKHR createInfoAndroid = {
      XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
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
  if (XR_FAILED(
          xrGetSystem(appState.instance, &systemGetInfo, &appState.systemId))) {
    LOGE("Failed to get OpenXR system");
    return;
  }

  // ▼▼▼ このブロックを追加 ▼▼▼
  // 4. グラフィックス要件の取得 (xrCreateSessionの前に必須)
  PFN_xrGetOpenGLESGraphicsRequirementsKHR
      pfnGetOpenGLESGraphicsRequirementsKHR = nullptr;
  xrGetInstanceProcAddr(
      appState.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
      (PFN_xrVoidFunction *)&pfnGetOpenGLESGraphicsRequirementsKHR);
  if (!pfnGetOpenGLESGraphicsRequirementsKHR) {
    LOGE("Failed to get xrGetOpenGLESGraphicsRequirementsKHR function");
    return;
  }

  XrGraphicsRequirementsOpenGLESKHR graphicsRequirements = {
      XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
  if (XR_FAILED(pfnGetOpenGLESGraphicsRequirementsKHR(
          appState.instance, appState.systemId, &graphicsRequirements))) {
    LOGE("Failed to get OpenGLES graphics requirements");
    return;
  }
  LOGI("OpenXR requires OpenGLES version %d.%d",
       XR_VERSION_MAJOR(graphicsRequirements.minApiVersionSupported),
       XR_VERSION_MINOR(graphicsRequirements.minApiVersionSupported));
  // ▲▲▲ 追加はここまで ▲▲▲

  // 5. OpenXRセッションの作成
  XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding = {
      XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
  graphicsBinding.display = appState.display;
  graphicsBinding.config = appState.config;
  graphicsBinding.context = appState.context;

  XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
  sessionCreateInfo.next = &graphicsBinding;
  sessionCreateInfo.systemId = appState.systemId;
  if (XR_FAILED(xrCreateSession(appState.instance, &sessionCreateInfo,
                                &appState.session))) {
    LOGE("Failed to create OpenXR session");
    return;
  }
  LOGI("OpenXR session created successfully.");

  // ▼▼▼ 変更/追加 ▼▼▼ ここからグラフィックス周りの設定
  // 参照空間の作成
  XrReferenceSpaceCreateInfo spaceCreateInfo = {
      XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  spaceCreateInfo.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
  xrCreateReferenceSpace(appState.session, &spaceCreateInfo, &appState.space);

  // ビュー（両目）の設定を取得
  uint32_t viewCount = 0;
  xrEnumerateViewConfigurationViews(appState.instance, appState.systemId,
                                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                    0, &viewCount, nullptr);
  appState.viewConfigViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  appState.views.resize(viewCount, {XR_TYPE_VIEW});
  xrEnumerateViewConfigurationViews(appState.instance, appState.systemId,
                                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                    viewCount, &viewCount,
                                    appState.viewConfigViews.data());

  // スワップチェーン（描画用イメージのセット）を作成
  appState.swapchains.resize(viewCount);
  appState.swapchainImages.resize(viewCount);
  for (uint32_t i = 0; i < viewCount; i++) {
    XrSwapchainCreateInfo swapchainCreateInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainCreateInfo.format = GL_SRGB8_ALPHA8; // 推奨フォーマット
    swapchainCreateInfo.width =
        appState.viewConfigViews[i].recommendedImageRectWidth;
    swapchainCreateInfo.height =
        appState.viewConfigViews[i].recommendedImageRectHeight;
    swapchainCreateInfo.sampleCount = 1;
    swapchainCreateInfo.faceCount = 1;
    swapchainCreateInfo.arraySize = 1;
    swapchainCreateInfo.mipCount = 1;
    xrCreateSwapchain(appState.session, &swapchainCreateInfo,
                      &appState.swapchains[i]);

    uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(appState.swapchains[i], 0, &imageCount, nullptr);
    appState.swapchainImages[i].resize(imageCount,
                                       {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(
        appState.swapchains[i], imageCount, &imageCount,
        (XrSwapchainImageBaseHeader *)appState.swapchainImages[i].data());
  }

  // 描画するオブジェクト（スクリーン）を作成
  CreateSceneResources();

  // 5. ビデオテクスチャの生成（これは後で使う）
  glGenTextures(1, &appState.videoTextureId);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, appState.videoTextureId);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
  LOGI("Successfully generated video texture ID: %d", appState.videoTextureId);
  appState.glInitialized = true;

  // 6. OpenXRレンダリングループ
  while (appState.appRunning) {
    // イベント処理
    XrEventDataBuffer eventData = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(appState.instance, &eventData) == XR_SUCCESS) {
      if (eventData.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        auto *stateChanged = (XrEventDataSessionStateChanged *)&eventData;
        appState.sessionState = stateChanged->state;
        LOGI("OpenXR session state changed to: %d", appState.sessionState);
        if (appState.sessionState == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
          beginInfo.primaryViewConfigurationType =
              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          xrBeginSession(appState.session, &beginInfo);
          appState.sessionRunning = true;
        } else if (appState.sessionState == XR_SESSION_STATE_STOPPING) {
          xrEndSession(appState.session);
          appState.sessionRunning = false;
        } else if (appState.sessionState == XR_SESSION_STATE_EXITING) {
          appState.appRunning = false; // アプリケーション終了
        }
      }
      eventData = {XR_TYPE_EVENT_DATA_BUFFER};
    }

    if (!appState.sessionRunning) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    // --- ここからがフレーム毎の描画処理 ---
    XrFrameState frameState = {XR_TYPE_FRAME_STATE};
    xrWaitFrame(appState.session, nullptr, &frameState);
    xrBeginFrame(appState.session, nullptr);

    // レイヤー（描画結果をHMDに送るためのデータ構造）
    std::vector<XrCompositionLayerBaseHeader *> layers;
    XrCompositionLayerProjection projectionLayer = {
        XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerProjectionView> projectionViews(
        viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

    if (frameState.shouldRender == XR_TRUE) {
      // HMDの位置と向きを取得して、各目のビュー行列とプロジェクション行列を計算
      XrViewState viewState = {XR_TYPE_VIEW_STATE};
      XrViewLocateInfo viewLocateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
      viewLocateInfo.viewConfigurationType =
          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      viewLocateInfo.displayTime = frameState.predictedDisplayTime;
      viewLocateInfo.space = appState.space;
      xrLocateViews(appState.session, &viewLocateInfo, &viewState, viewCount,
                    &viewCount, appState.views.data());

      // 各目（ビュー）に対して描画
      for (uint32_t i = 0; i < viewCount; i++) {
        uint32_t imageIndex;
        xrAcquireSwapchainImage(appState.swapchains[i], nullptr, &imageIndex);

        XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(appState.swapchains[i], &waitInfo);

        projectionViews[i].pose = appState.views[i].pose;
        projectionViews[i].fov = appState.views[i].fov;
        projectionViews[i].subImage.swapchain = appState.swapchains[i];
        projectionViews[i].subImage.imageRect.offset = {0, 0};
        projectionViews[i].subImage.imageRect.extent = {
            (int32_t)appState.viewConfigViews[i].recommendedImageRectWidth,
            (int32_t)appState.viewConfigViews[i].recommendedImageRectHeight};

        // OpenGLのフレームバッファに描画ターゲットを設定
        GLuint colorTexture = appState.swapchainImages[i][imageIndex].image;
        GLuint framebuffer;
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, colorTexture, 0);

        // 描画領域を設定
        const auto &rect = projectionViews[i].subImage.imageRect;
        glViewport(rect.offset.x, rect.offset.y, rect.extent.width,
                   rect.extent.height);

        // 背景をクリア（真っ黒）
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // --- ここでスクリーンを描画 ---
        Matrix4f proj =
            Matrix4f_CreateProjectionFov(appState.views[i].fov, 0.1f, 100.0f);
        Matrix4f viewPose = Matrix4f_CreateFromXrPose(&appState.views[i].pose);
        Matrix4f view = Matrix4f_Inverse(&viewPose);

        // スクリーンを2m奥に配置
        Matrix4f model = Matrix4f_CreateTranslation(0.0f, 0.0f, -2.0f);

        Matrix4f vp = Matrix4f_Multiply(&proj, &view);
        Matrix4f mvp = Matrix4f_Multiply(&vp, &model);

        glUseProgram(appState.screenShaderProgram);
        glUniformMatrix4fv(appState.mvpLocation, 1, GL_FALSE, &mvp.M[0][0]);
        glBindVertexArray(appState.screenVao);
        glDrawArrays(GL_TRIANGLES, 0, 6); // 2つの三角形で四角形を描画

        glBindVertexArray(0);
        glUseProgram(0);
        // --- 描画終了 ---

        glDeleteFramebuffers(1, &framebuffer);
        xrReleaseSwapchainImage(appState.swapchains[i], nullptr);
      }

      projectionLayer.space = appState.space;
      projectionLayer.viewCount = viewCount;
      projectionLayer.views = projectionViews.data();
      layers.push_back((XrCompositionLayerBaseHeader *)&projectionLayer);
    }

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = (uint32_t)layers.size();
    endInfo.layers = layers.data();
    xrEndFrame(appState.session, &endInfo);
  }
  // ▲▲▲ 変更/追加 ▲▲▲

  // 7. クリーンアップ
  if (appState.videoTextureId != 0)
    glDeleteTextures(1, &appState.videoTextureId);
  if (appState.screenShaderProgram != 0)
    glDeleteProgram(appState.screenShaderProgram);
  if (appState.screenVbo != 0)
    glDeleteBuffers(1, &appState.screenVbo);
  if (appState.screenVao != 0)
    glDeleteVertexArrays(1, &appState.screenVao);

  for (auto &swapchain : appState.swapchains)
    xrDestroySwapchain(swapchain);
  if (appState.space != XR_NULL_HANDLE)
    xrDestroySpace(appState.space);
  if (appState.session != XR_NULL_HANDLE)
    xrDestroySession(appState.session);
  if (appState.instance != XR_NULL_HANDLE)
    xrDestroyInstance(appState.instance);

  eglMakeCurrent(appState.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                 EGL_NO_CONTEXT);
  if (tinySurface != EGL_NO_SURFACE)
    eglDestroySurface(appState.display, tinySurface);
  if (appState.context != EGL_NO_CONTEXT)
    eglDestroyContext(appState.display, appState.context);
  if (appState.display != EGL_NO_DISPLAY)
    eglTerminate(appState.display);

  LOGI("Render thread finished.");
}

// --- JNI関数 --- (変更なし)

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
  appState.javaVm = vm;
  return JNI_VERSION_1_6;
}
extern "C" JNIEXPORT jstring JNICALL
Java_net_akaaku_mainevr_MainActivity_stringFromJNI(JNIEnv *env, jobject) {
  return env->NewStringUTF("VR Video Bridge Ready!");
}
extern "C" JNIEXPORT jint JNICALL
Java_net_akaaku_mainevr_MainActivity_nativeGetTextureId(JNIEnv *env, jobject) {
  if (appState.glInitialized && appState.videoTextureId > 0) {
    return (jint)appState.videoTextureId;
  }
  return -1;
}
extern "C" JNIEXPORT void JNICALL
Java_net_akaaku_mainevr_MainActivity_nativeOnCreate(JNIEnv *env,
                                                    jobject activity) {
  LOGI("Native engine created.");
  appState.activityObject = env->NewGlobalRef(activity);
  appState.appRunning = true;
  appState.renderThread = std::thread(OpenXrRenderLoop);
}
extern "C" JNIEXPORT void JNICALL
Java_net_akaaku_mainevr_MainActivity_nativeOnResume(JNIEnv *env, jobject) {
  LOGI("Native engine resumed.");
  appState.appResumed = true;
}
extern "C" JNIEXPORT void JNICALL
Java_net_akaaku_mainevr_MainActivity_nativeOnPause(JNIEnv *env, jobject) {
  LOGI("Native engine paused.");
  appState.appResumed = false;
}
extern "C" JNIEXPORT void JNICALL
Java_net_akaaku_mainevr_MainActivity_nativeOnDestroy(JNIEnv *env, jobject) {
  LOGI("Native engine destroyed. Stopping render thread...");
  if (appState.appRunning) {
    appState.appRunning = false;
    if (appState.renderThread.joinable()) {
      appState.renderThread.join();
    }
  }
  if (appState.activityObject != nullptr) {
    env->DeleteGlobalRef(appState.activityObject);
    appState.activityObject = nullptr;
  }
  appState.glInitialized = false;
  appState.videoTextureId = 0;
}
extern "C" JNIEXPORT void JNICALL
Java_net_akaaku_mainevr_MainActivity_nativeSetSurface(JNIEnv *env, jobject,
                                                      jobject surface) {
  if (appState.nativeWindow != nullptr) {
    ANativeWindow_release(appState.nativeWindow);
  }
  if (surface != nullptr) {
    appState.nativeWindow = ANativeWindow_fromSurface(env, surface);
  }
}