#include "render_thread.h"

#include <android/log.h>
#include <android/trace.h>

#include <chrono>

#define LOG_TAG "VehicleHud"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

const char* kVertexShader = R"(
attribute vec4 aPosition;
uniform float uAngle;
void main() {
    float s = sin(uAngle);
    float c = cos(uAngle);
    vec2 rotated = vec2(
        aPosition.x * c - aPosition.y * s,
        aPosition.x * s + aPosition.y * c
    );
    gl_Position = vec4(rotated, aPosition.z, aPosition.w);
}
)";

const char* kFragmentShader = R"(
precision mediump float;
void main() {
    gl_FragColor = vec4(0.20, 0.85, 0.55, 1.0);
}
)";

// A thin needle pointing straight up (+Y) at angle 0, pivoting around the
// origin — rotated per frame by uAngle to point at the current speed.
constexpr GLfloat kNeedle[] = {
        -0.03f, 0.0f, 0.0f, // base left
         0.03f, 0.0f, 0.0f, // base right
         0.0f,  0.6f, 0.0f, // tip
};

constexpr float kMaxSpeedForSweepKph = 180.0f;
// Gauge sweeps this many radians end to end, centered on straight up.
constexpr float kSweepRadians = 4.0f;

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint linkProgram(const char* vertexSrc, const char* fragmentSrc) {
    GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSrc);
    GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    if (vertex == 0 || fragment == 0) {
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);

    // Shader objects are refcounted by the program once attached; safe to
    // delete our reference to them right after linking either way.
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("Program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

} // namespace

RenderThread::RenderThread(ANativeWindow* window) : window_(window) {}

RenderThread::~RenderThread() {
    stop();
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
}

void RenderThread::start() {
    if (running_.exchange(true)) {
        return; // already running
    }
    telemetry_.start(); // independent thread; synchronized only via latest()
    thread_ = std::thread(&RenderThread::run, this);
}

void RenderThread::stop() {
    if (!running_.exchange(false)) {
        return; // already stopped, or never started
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    telemetry_.stop(); // joins the telemetry thread too
}

bool RenderThread::initEgl() {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }

    if (!eglInitialize(display_, nullptr, nullptr)) {
        LOGE("eglInitialize failed");
        return false;
    }

    const EGLint configAttribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE,
    };
    EGLConfig config;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(display_, configAttribs, &config, 1, &numConfigs) || numConfigs < 1) {
        LOGE("eglChooseConfig failed");
        return false;
    }

    EGLint format = 0;
    eglGetConfigAttrib(display_, config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(window_, 0, 0, format);

    surface_ = eglCreateWindowSurface(display_, config, window_, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed");
        return false;
    }

    const EGLint contextAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE,
    };
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttribs);
    if (context_ == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return false;
    }

    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        LOGE("eglMakeCurrent failed");
        return false;
    }

    program_ = linkProgram(kVertexShader, kFragmentShader);
    if (program_ == 0) {
        return false;
    }
    positionHandle_ = glGetAttribLocation(program_, "aPosition");
    angleHandle_ = glGetUniformLocation(program_, "uAngle");

    // These were previously re-issued every frame in renderFrame(), but
    // none of them change from frame to frame: there's exactly one
    // program, one vertex attribute layout, and one clear color for this
    // context's whole lifetime. Set once here instead of every ~16ms.
    glClearColor(0.05f, 0.07f, 0.09f, 1.0f);
    glUseProgram(program_);
    glEnableVertexAttribArray(positionHandle_);
    glVertexAttribPointer(positionHandle_, 3, GL_FLOAT, GL_FALSE, 0, kNeedle);

    LOGI("EGL initialized on render thread");
    return true;
}

void RenderThread::teardownEgl() {
    if (display_ == EGL_NO_DISPLAY) {
        return; // initEgl never completed; nothing to tear down.
    }

    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }

    // Detach current before destroying — a context must not be current
    // when it's destroyed. Every call in this function runs on the same
    // thread that created these objects.
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(display_, context_);
        context_ = EGL_NO_CONTEXT;
    }
    if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }
    eglTerminate(display_);
    display_ = EGL_NO_DISPLAY;

    LOGI("EGL torn down on render thread");
}

void RenderThread::renderFrame(const TelemetrySample& sample) {
    // ATrace sections show up as named slices in a real Perfetto/systrace
    // capture (see docs/PROFILING.md) — this is what a production Android
    // perf investigation actually uses, on top of the always-on rolling
    // logcat average below for quick, no-capture-needed feedback.
    ATrace_beginSection("VehicleHud::renderFrame");
    const auto drawStart = std::chrono::steady_clock::now();

    glClear(GL_COLOR_BUFFER_BIT);

    // Map speed to a sweep angle, centered on straight up (angle 0) at
    // mid-scale, like a real speedometer's arc. This uniform upload is the
    // one thing that legitimately must happen every frame — it's the only
    // GL state that actually changes frame to frame.
    float fraction = sample.speedKph / kMaxSpeedForSweepKph;
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    float angle = (fraction - 0.5f) * kSweepRadians;
    glUniform1f(angleHandle_, angle);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    const auto swapStart = std::chrono::steady_clock::now();
    ATrace_endSection();

    ATrace_beginSection("VehicleHud::eglSwapBuffers");
    eglSwapBuffers(display_, surface_);
    ATrace_endSection();
    const auto swapEnd = std::chrono::steady_clock::now();

    lastDrawMicros_ =
            std::chrono::duration_cast<std::chrono::microseconds>(swapStart - drawStart).count();
    lastSwapMicros_ =
            std::chrono::duration_cast<std::chrono::microseconds>(swapEnd - swapStart).count();
}

void RenderThread::run() {
    if (!initEgl()) {
        LOGE("EGL init failed; render thread exiting without a loop");
        teardownEgl();
        return;
    }

    // Lightweight, always-on frame profiling: average CPU time inside
    // renderFrame() (GL calls + swap) plus the real frame rate over each
    // window of kProfileWindowFrames frames.
    constexpr int kProfileWindowFrames = 300; // ~5s at 60fps
    long long profileDrawSumMicros = 0;
    long long profileSwapSumMicros = 0;
    int profileFrameCount = 0;
    auto windowStart = std::chrono::steady_clock::now();

    // No sleep in this loop: eglSwapBuffers already blocks until the
    // compositor can take the next buffer (swap interval 1), so vsync paces
    // the loop. A fixed 16 ms sleep on top of that wait made every frame
    // ~19 ms and capped the HUD at ~52 fps on a 60 Hz display.
    while (running_.load()) {
        // latest() locks only long enough to copy two floats out; every GL
        // call below runs unlocked.
        renderFrame(telemetry_.latest());

        profileDrawSumMicros += lastDrawMicros_;
        profileSwapSumMicros += lastSwapMicros_;
        if (++profileFrameCount >= kProfileWindowFrames) {
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - windowStart).count();
            LOGI("draw avg: %lld us, swap avg: %lld us, %.1f fps over %d frames",
                 profileDrawSumMicros / profileFrameCount,
                 profileSwapSumMicros / profileFrameCount,
                 profileFrameCount / seconds, profileFrameCount);
            profileDrawSumMicros = 0;
            profileSwapSumMicros = 0;
            profileFrameCount = 0;
            windowStart = now;
        }
    }

    teardownEgl();
}
