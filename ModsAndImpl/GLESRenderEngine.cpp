/*
 * Copyright 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "RenderEngine"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <sched.h>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android-base/stringprintf.h>
#include <cutils/compiler.h>
#include <cutils/properties.h>
#include <gui/DebugEGLImageTracker.h>
#include <gui/ISurfaceComposer.h>
#include <renderengine/Mesh.h>
#include <renderengine/Texture.h>
#include <renderengine/private/Description.h>
#include <sync/sync.h>
#include <ui/ColorSpace.h>
#include <ui/DebugUtils.h>
#include <ui/GraphicBuffer.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <utils/KeyedVector.h>
#include <utils/Trace.h>
#include "GLESRenderEngine.h"
#include "GLExtensions.h"
#include "GLFramebuffer.h"
#include "GLImage.h"
#include "Program.h"
#include "ProgramCache.h"

#if RK_NV12_10_TO_NV12_BY_RGA
#define UN_NEED_GL
#include <RockchipRga.h>
#endif

#if (RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
#include <dlfcn.h>
#endif

extern "C" EGLAPI const char* eglQueryStringImplementationANDROID(EGLDisplay dpy, EGLint name);

bool checkGlError(const char* op, int lineNumber) {
    bool errorFound = false;
    GLint error = glGetError();
    while (error != GL_NO_ERROR) {
        errorFound = true;
        error = glGetError();
        ALOGV("after %s() (line # %d) glError (0x%x)\n", op, lineNumber, error);
    }
    return errorFound;
}

static constexpr bool outputDebugPPMs = false;

void writePPM(const char* basename, GLuint width, GLuint height) {
    //ALOGE("writePPM #%s: %d x %d", basename, width, height);

    std::vector<GLubyte> pixels(width * height * 4);
    std::vector<GLubyte> outBuffer(width * height * 3);

    // TODO(courtneygo): We can now have float formats, need
    // to remove this code or update to support.
    // Make returned pixels fit in uint32_t, one byte per component
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    if (checkGlError(__FUNCTION__, __LINE__)) {
        return;
    }

    std::string filename(basename);
    filename.append(".ppm");
    std::ofstream file(filename.c_str(), std::ios::binary);
    if (!file.is_open()) {
        ALOGE("Unable to open file: %s", filename.c_str());
        ALOGE("You may need to do: \"adb shell setenforce 0\" to enable "
              "surfaceflinger to write debug images");
        return;
    }

    file << "P6\n";
    file << width << "\n";
    file << height << "\n";
    file << 255 << "\n";

    auto ptr = reinterpret_cast<char*>(pixels.data());
    auto outPtr = reinterpret_cast<char*>(outBuffer.data());
    for (int y = height - 1; y >= 0; y--) {
        char* data = ptr + y * width * sizeof(uint32_t);

        for (GLuint x = 0; x < width; x++) {
            // Only copy R, G and B components
            outPtr[0] = data[0];
            outPtr[1] = data[1];
            outPtr[2] = data[2];
            data += sizeof(uint32_t);
            outPtr += 3;
        }
    }
    file.write(reinterpret_cast<char*>(outBuffer.data()), outBuffer.size());
}

namespace android {

#if (RK_NV12_10_TO_NV12_BY_RGA | RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
typedef struct
{
     sp<GraphicBuffer> yuvTexBuffer;
     EGLImageKHR img;
} TexBufferImag;

#define TexBufferMax  2
#define TexKey 0x524f434b
static TexBufferImag yuvTeximg[TexBufferMax] = {{NULL,EGL_NO_IMAGE_KHR},{NULL,EGL_NO_IMAGE_KHR}};
#endif

namespace renderengine {
namespace gl {

using base::StringAppendF;
using ui::Dataspace;

static status_t selectConfigForAttribute(EGLDisplay dpy, EGLint const* attrs, EGLint attribute,
                                         EGLint wanted, EGLConfig* outConfig) {
    EGLint numConfigs = -1, n = 0;
    eglGetConfigs(dpy, nullptr, 0, &numConfigs);
    std::vector<EGLConfig> configs(numConfigs, EGL_NO_CONFIG_KHR);
    eglChooseConfig(dpy, attrs, configs.data(), configs.size(), &n);
    configs.resize(n);

    if (!configs.empty()) {
        if (attribute != EGL_NONE) {
            for (EGLConfig config : configs) {
                EGLint value = 0;
                eglGetConfigAttrib(dpy, config, attribute, &value);
                if (wanted == value) {
                    *outConfig = config;
                    return NO_ERROR;
                }
            }
        } else {
            // just pick the first one
            *outConfig = configs[0];
            return NO_ERROR;
        }
    }

    return NAME_NOT_FOUND;
}

class EGLAttributeVector {
    struct Attribute;
    class Adder;
    friend class Adder;
    KeyedVector<Attribute, EGLint> mList;
    struct Attribute {
        Attribute() : v(0){};
        explicit Attribute(EGLint v) : v(v) {}
        EGLint v;
        bool operator<(const Attribute& other) const {
            // this places EGL_NONE at the end
            EGLint lhs(v);
            EGLint rhs(other.v);
            if (lhs == EGL_NONE) lhs = 0x7FFFFFFF;
            if (rhs == EGL_NONE) rhs = 0x7FFFFFFF;
            return lhs < rhs;
        }
    };
    class Adder {
        friend class EGLAttributeVector;
        EGLAttributeVector& v;
        EGLint attribute;
        Adder(EGLAttributeVector& v, EGLint attribute) : v(v), attribute(attribute) {}

    public:
        void operator=(EGLint value) {
            if (attribute != EGL_NONE) {
                v.mList.add(Attribute(attribute), value);
            }
        }
        operator EGLint() const { return v.mList[attribute]; }
    };

public:
    EGLAttributeVector() { mList.add(Attribute(EGL_NONE), EGL_NONE); }
    void remove(EGLint attribute) {
        if (attribute != EGL_NONE) {
            mList.removeItem(Attribute(attribute));
        }
    }
    Adder operator[](EGLint attribute) { return Adder(*this, attribute); }
    EGLint operator[](EGLint attribute) const { return mList[attribute]; }
    // cast-operator to (EGLint const*)
    operator EGLint const*() const { return &mList.keyAt(0).v; }
};

static status_t selectEGLConfig(EGLDisplay display, EGLint format, EGLint renderableType,
                                EGLConfig* config) {
    // select our EGLConfig. It must support EGL_RECORDABLE_ANDROID if
    // it is to be used with WIFI displays
    status_t err;
    EGLint wantedAttribute;
    EGLint wantedAttributeValue;

    EGLAttributeVector attribs;
    if (renderableType) {
        attribs[EGL_RENDERABLE_TYPE] = renderableType;
        attribs[EGL_RECORDABLE_ANDROID] = EGL_TRUE;
        attribs[EGL_SURFACE_TYPE] = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
        attribs[EGL_FRAMEBUFFER_TARGET_ANDROID] = EGL_TRUE;
        attribs[EGL_RED_SIZE] = 8;
        attribs[EGL_GREEN_SIZE] = 8;
        attribs[EGL_BLUE_SIZE] = 8;
        attribs[EGL_ALPHA_SIZE] = 8;
        wantedAttribute = EGL_NONE;
        wantedAttributeValue = EGL_NONE;
    } else {
        // if no renderable type specified, fallback to a simplified query
        wantedAttribute = EGL_NATIVE_VISUAL_ID;
        wantedAttributeValue = format;
    }

    err = selectConfigForAttribute(display, attribs, wantedAttribute, wantedAttributeValue, config);
    if (err == NO_ERROR) {
        EGLint caveat;
        if (eglGetConfigAttrib(display, *config, EGL_CONFIG_CAVEAT, &caveat))
            ALOGW_IF(caveat == EGL_SLOW_CONFIG, "EGL_SLOW_CONFIG selected!");
    }

    return err;
}

std::unique_ptr<GLESRenderEngine> GLESRenderEngine::create(int hwcFormat, uint32_t featureFlags,
                                                           uint32_t imageCacheSize) {
    // initialize EGL for the default display
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(display, nullptr, nullptr)) {
        LOG_ALWAYS_FATAL("failed to initialize EGL");
    }

    GLExtensions& extensions = GLExtensions::getInstance();
    extensions.initWithEGLStrings(eglQueryStringImplementationANDROID(display, EGL_VERSION),
                                  eglQueryStringImplementationANDROID(display, EGL_EXTENSIONS));

    // The code assumes that ES2 or later is available if this extension is
    // supported.
    EGLConfig config = EGL_NO_CONFIG;
    if (!extensions.hasNoConfigContext()) {
        config = chooseEglConfig(display, hwcFormat, /*logConfig*/ true);
    }

    bool useContextPriority = extensions.hasContextPriority() &&
            (featureFlags & RenderEngine::USE_HIGH_PRIORITY_CONTEXT);
    EGLContext protectedContext = EGL_NO_CONTEXT;
    if ((featureFlags & RenderEngine::ENABLE_PROTECTED_CONTEXT) &&
        extensions.hasProtectedContent()) {
        protectedContext = createEglContext(display, config, nullptr, useContextPriority,
                                            Protection::PROTECTED);
        ALOGE_IF(protectedContext == EGL_NO_CONTEXT, "Can't create protected context");
    }

    EGLContext ctxt = createEglContext(display, config, protectedContext, useContextPriority,
                                       Protection::UNPROTECTED);

    // if can't create a GL context, we can only abort.
    LOG_ALWAYS_FATAL_IF(ctxt == EGL_NO_CONTEXT, "EGLContext creation failed");

    EGLSurface dummy = EGL_NO_SURFACE;
    if (!extensions.hasSurfacelessContext()) {
        dummy = createDummyEglPbufferSurface(display, config, hwcFormat, Protection::UNPROTECTED);
        LOG_ALWAYS_FATAL_IF(dummy == EGL_NO_SURFACE, "can't create dummy pbuffer");
    }
    EGLBoolean success = eglMakeCurrent(display, dummy, dummy, ctxt);
    LOG_ALWAYS_FATAL_IF(!success, "can't make dummy pbuffer current");
    extensions.initWithGLStrings(glGetString(GL_VENDOR), glGetString(GL_RENDERER),
                                 glGetString(GL_VERSION), glGetString(GL_EXTENSIONS));

    EGLSurface protectedDummy = EGL_NO_SURFACE;
    if (protectedContext != EGL_NO_CONTEXT && !extensions.hasSurfacelessContext()) {
        protectedDummy =
                createDummyEglPbufferSurface(display, config, hwcFormat, Protection::PROTECTED);
        ALOGE_IF(protectedDummy == EGL_NO_SURFACE, "can't create protected dummy pbuffer");
    }

    // now figure out what version of GL did we actually get
    GlesVersion version = parseGlesVersion(extensions.getVersion());

    // initialize the renderer while GL is current
    std::unique_ptr<GLESRenderEngine> engine;
    switch (version) {
        case GLES_VERSION_1_0:
        case GLES_VERSION_1_1:
            LOG_ALWAYS_FATAL("SurfaceFlinger requires OpenGL ES 2.0 minimum to run.");
            break;
        case GLES_VERSION_2_0:
        case GLES_VERSION_3_0:
            engine = std::make_unique<GLESRenderEngine>(featureFlags, display, config, ctxt, dummy,
                                                        protectedContext, protectedDummy,
                                                        imageCacheSize);
            break;
    }

    ALOGI("OpenGL ES informations:");
    ALOGI("vendor    : %s", extensions.getVendor());
    ALOGI("renderer  : %s", extensions.getRenderer());
    ALOGI("version   : %s", extensions.getVersion());
    ALOGI("extensions: %s", extensions.getExtensions());
    ALOGI("GL_MAX_TEXTURE_SIZE = %zu", engine->getMaxTextureSize());
    ALOGI("GL_MAX_VIEWPORT_DIMS = %zu", engine->getMaxViewportDims());

    return engine;
}

EGLConfig GLESRenderEngine::chooseEglConfig(EGLDisplay display, int format, bool logConfig) {
    status_t err;
    EGLConfig config;

    // First try to get an ES3 config
    err = selectEGLConfig(display, format, EGL_OPENGL_ES3_BIT, &config);
    if (err != NO_ERROR) {
        // If ES3 fails, try to get an ES2 config
        err = selectEGLConfig(display, format, EGL_OPENGL_ES2_BIT, &config);
        if (err != NO_ERROR) {
            // If ES2 still doesn't work, probably because we're on the emulator.
            // try a simplified query
            ALOGW("no suitable EGLConfig found, trying a simpler query");
            err = selectEGLConfig(display, format, 0, &config);
            if (err != NO_ERROR) {
                // this EGL is too lame for android
                LOG_ALWAYS_FATAL("no suitable EGLConfig found, giving up");
            }
        }
    }

    if (logConfig) {
        // print some debugging info
        EGLint r, g, b, a;
        eglGetConfigAttrib(display, config, EGL_RED_SIZE, &r);
        eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &a);
        ALOGI("EGL information:");
        ALOGI("vendor    : %s", eglQueryString(display, EGL_VENDOR));
        ALOGI("version   : %s", eglQueryString(display, EGL_VERSION));
        ALOGI("extensions: %s", eglQueryString(display, EGL_EXTENSIONS));
        ALOGI("Client API: %s", eglQueryString(display, EGL_CLIENT_APIS) ?: "Not Supported");
        ALOGI("EGLSurface: %d-%d-%d-%d, config=%p", r, g, b, a, config);
    }

    return config;
}

GLESRenderEngine::GLESRenderEngine(uint32_t featureFlags, EGLDisplay display, EGLConfig config,
                                   EGLContext ctxt, EGLSurface dummy, EGLContext protectedContext,
                                   EGLSurface protectedDummy, uint32_t imageCacheSize)
      : renderengine::impl::RenderEngine(featureFlags),
        mEGLDisplay(display),
        mEGLConfig(config),
        mEGLContext(ctxt),
        mDummySurface(dummy),
        mProtectedEGLContext(protectedContext),
        mProtectedDummySurface(protectedDummy),
        mVpWidth(0),
        mVpHeight(0),
        mFramebufferImageCacheSize(imageCacheSize),
        mUseColorManagement(featureFlags & USE_COLOR_MANAGEMENT) {
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mMaxTextureSize);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, mMaxViewportDims);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    // Initialize protected EGL Context.
    if (mProtectedEGLContext != EGL_NO_CONTEXT) {
        EGLBoolean success = eglMakeCurrent(display, mProtectedDummySurface, mProtectedDummySurface,
                                            mProtectedEGLContext);
        ALOGE_IF(!success, "can't make protected context current");
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        success = eglMakeCurrent(display, mDummySurface, mDummySurface, mEGLContext);
        LOG_ALWAYS_FATAL_IF(!success, "can't make default context current");
    }

    const uint16_t protTexData[] = {0};
    glGenTextures(1, &mProtectedTexName);
    glBindTexture(GL_TEXTURE_2D, mProtectedTexName);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, protTexData);

    // mColorBlindnessCorrection = M;

    if (mUseColorManagement) {
        const ColorSpace srgb(ColorSpace::sRGB());
        const ColorSpace displayP3(ColorSpace::DisplayP3());
        const ColorSpace bt2020(ColorSpace::BT2020());

        // no chromatic adaptation needed since all color spaces use D65 for their white points.
        mSrgbToXyz = mat4(srgb.getRGBtoXYZ());
        mDisplayP3ToXyz = mat4(displayP3.getRGBtoXYZ());
        mBt2020ToXyz = mat4(bt2020.getRGBtoXYZ());
        mXyzToSrgb = mat4(srgb.getXYZtoRGB());
        mXyzToDisplayP3 = mat4(displayP3.getXYZtoRGB());
        mXyzToBt2020 = mat4(bt2020.getXYZtoRGB());

        // Compute sRGB to Display P3 and BT2020 transform matrix.
        // NOTE: For now, we are limiting output wide color space support to
        // Display-P3 and BT2020 only.
        mSrgbToDisplayP3 = mXyzToDisplayP3 * mSrgbToXyz;
        mSrgbToBt2020 = mXyzToBt2020 * mSrgbToXyz;

        // Compute Display P3 to sRGB and BT2020 transform matrix.
        mDisplayP3ToSrgb = mXyzToSrgb * mDisplayP3ToXyz;
        mDisplayP3ToBt2020 = mXyzToBt2020 * mDisplayP3ToXyz;

        // Compute BT2020 to sRGB and Display P3 transform matrix
        mBt2020ToSrgb = mXyzToSrgb * mBt2020ToXyz;
        mBt2020ToDisplayP3 = mXyzToDisplayP3 * mBt2020ToXyz;
    }

    char value[PROPERTY_VALUE_MAX];
    property_get("debug.egl.traceGpuCompletion", value, "0");
    if (atoi(value)) {
        mTraceGpuCompletion = true;
        mFlushTracer = std::make_unique<FlushTracer>(this);
    }
    mImageManager = std::make_unique<ImageManager>(this);
    mImageManager->initThread();
    mDrawingBuffer = createFramebuffer();

    shaderProgram = 0;

    glGenTextures(1, &fbTexName);
    glBindTexture(GL_TEXTURE_2D, fbTexName);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    //ALOGE("[zizhan] place 3");
}

GLESRenderEngine::~GLESRenderEngine() {
    // Destroy the image manager first.
    mImageManager = nullptr;
    std::lock_guard<std::mutex> lock(mRenderingMutex);
    unbindFrameBuffer(mDrawingBuffer.get());
    mDrawingBuffer = nullptr;
    while (!mFramebufferImageCache.empty()) {
        EGLImageKHR expired = mFramebufferImageCache.front().second;
        mFramebufferImageCache.pop_front();
        eglDestroyImageKHR(mEGLDisplay, expired);
        DEBUG_EGL_IMAGE_TRACKER_DESTROY();
    }
    mImageCache.clear();
    eglMakeCurrent(mEGLDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(mEGLDisplay);
}

std::unique_ptr<Framebuffer> GLESRenderEngine::createFramebuffer() {
    return std::make_unique<GLFramebuffer>(*this);
}

std::unique_ptr<Image> GLESRenderEngine::createImage() {
    return std::make_unique<GLImage>(*this);
}

Framebuffer* GLESRenderEngine::getFramebufferForDrawing() {
    return mDrawingBuffer.get();
}

void GLESRenderEngine::primeCache() const {
    ProgramCache::getInstance().primeCache(mInProtectedContext ? mProtectedEGLContext : mEGLContext,
                                           mFeatureFlags & USE_COLOR_MANAGEMENT);
}

bool GLESRenderEngine::isCurrent() const {
    return mEGLDisplay == eglGetCurrentDisplay() && mEGLContext == eglGetCurrentContext();
}

base::unique_fd GLESRenderEngine::flush() {
    ATRACE_CALL();
    if (!GLExtensions::getInstance().hasNativeFenceSync()) {
        return base::unique_fd();
    }

    EGLSyncKHR sync = eglCreateSyncKHR(mEGLDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR) {
        ALOGW("failed to create EGL native fence sync: %#x", eglGetError());
        return base::unique_fd();
    }

    // native fence fd will not be populated until flush() is done.
    glFlush();

    // get the fence fd
    base::unique_fd fenceFd(eglDupNativeFenceFDANDROID(mEGLDisplay, sync));
    eglDestroySyncKHR(mEGLDisplay, sync);
    if (fenceFd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
        ALOGW("failed to dup EGL native fence sync: %#x", eglGetError());
    }

    // Only trace if we have a valid fence, as current usage falls back to
    // calling finish() if the fence fd is invalid.
    if (CC_UNLIKELY(mTraceGpuCompletion && mFlushTracer) && fenceFd.get() >= 0) {
        mFlushTracer->queueSync(eglCreateSyncKHR(mEGLDisplay, EGL_SYNC_FENCE_KHR, nullptr));
    }

    return fenceFd;
}

bool GLESRenderEngine::finish() {
    ATRACE_CALL();
    if (!GLExtensions::getInstance().hasFenceSync()) {
        ALOGW("no synchronization support");
        return false;
    }

    EGLSyncKHR sync = eglCreateSyncKHR(mEGLDisplay, EGL_SYNC_FENCE_KHR, nullptr);
    if (sync == EGL_NO_SYNC_KHR) {
        ALOGW("failed to create EGL fence sync: %#x", eglGetError());
        return false;
    }

    if (CC_UNLIKELY(mTraceGpuCompletion && mFlushTracer)) {
        mFlushTracer->queueSync(eglCreateSyncKHR(mEGLDisplay, EGL_SYNC_FENCE_KHR, nullptr));
    }

    return waitSync(sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR);
}

bool GLESRenderEngine::waitSync(EGLSyncKHR sync, EGLint flags) {
    EGLint result = eglClientWaitSyncKHR(mEGLDisplay, sync, flags, 2000000000 /*2 sec*/);
    EGLint error = eglGetError();
    eglDestroySyncKHR(mEGLDisplay, sync);
    if (result != EGL_CONDITION_SATISFIED_KHR) {
        if (result == EGL_TIMEOUT_EXPIRED_KHR) {
            ALOGW("fence wait timed out");
        } else {
            ALOGW("error waiting on EGL fence: %#x", error);
        }
        return false;
    }

    return true;
}

bool GLESRenderEngine::waitFence(base::unique_fd fenceFd) {
    if (!GLExtensions::getInstance().hasNativeFenceSync() ||
        !GLExtensions::getInstance().hasWaitSync()) {
        return false;
    }

    // release the fd and transfer the ownership to EGLSync
    EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fenceFd.release(), EGL_NONE};
    EGLSyncKHR sync = eglCreateSyncKHR(mEGLDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    if (sync == EGL_NO_SYNC_KHR) {
        ALOGE("failed to create EGL native fence sync: %#x", eglGetError());
        return false;
    }

    // XXX: The spec draft is inconsistent as to whether this should return an
    // EGLint or void.  Ignore the return value for now, as it's not strictly
    // needed.
    eglWaitSyncKHR(mEGLDisplay, sync, 0);
    EGLint error = eglGetError();
    eglDestroySyncKHR(mEGLDisplay, sync);
    if (error != EGL_SUCCESS) {
        ALOGE("failed to wait for EGL native fence sync: %#x", error);
        return false;
    }

    return true;
}

void GLESRenderEngine::clearWithColor(float red, float green, float blue, float alpha) {
    ATRACE_CALL();
    glDisable(GL_BLEND);
    glClearColor(red, green, blue, alpha);
    glClear(GL_COLOR_BUFFER_BIT);
}

void GLESRenderEngine::fillRegionWithColor(const Region& region, float red, float green, float blue,
                                           float alpha) {
    size_t c;
    Rect const* r = region.getArray(&c);
    Mesh mesh(Mesh::TRIANGLES, c * 6, 2);
    Mesh::VertexArray<vec2> position(mesh.getPositionArray<vec2>());
    for (size_t i = 0; i < c; i++, r++) {
        position[i * 6 + 0].x = r->left;
        position[i * 6 + 0].y = r->top;
        position[i * 6 + 1].x = r->left;
        position[i * 6 + 1].y = r->bottom;
        position[i * 6 + 2].x = r->right;
        position[i * 6 + 2].y = r->bottom;
        position[i * 6 + 3].x = r->left;
        position[i * 6 + 3].y = r->top;
        position[i * 6 + 4].x = r->right;
        position[i * 6 + 4].y = r->bottom;
        position[i * 6 + 5].x = r->right;
        position[i * 6 + 5].y = r->top;
    }
    setupFillWithColor(red, green, blue, alpha);
    drawMesh(mesh);
}

void GLESRenderEngine::setScissor(const Rect& region) {
    glScissor(region.left, region.top, region.getWidth(), region.getHeight());
    glEnable(GL_SCISSOR_TEST);
}

void GLESRenderEngine::disableScissor() {
    glDisable(GL_SCISSOR_TEST);
}

void GLESRenderEngine::genTextures(size_t count, uint32_t* names) {
    glGenTextures(count, names);
}

void GLESRenderEngine::deleteTextures(size_t count, uint32_t const* names) {
    glDeleteTextures(count, names);
}

void GLESRenderEngine::bindExternalTextureImage(uint32_t texName, const Image& image) {
    ATRACE_CALL();
    const GLImage& glImage = static_cast<const GLImage&>(image);
    const GLenum target = GL_TEXTURE_EXTERNAL_OES;

    glBindTexture(target, texName);
    if (glImage.getEGLImage() != EGL_NO_IMAGE_KHR) {
        glEGLImageTargetTexture2DOES(target, static_cast<GLeglImageOES>(glImage.getEGLImage()));
    }
}

status_t GLESRenderEngine::bindExternalTextureBuffer(uint32_t texName,
                                                     const sp<GraphicBuffer>& buffer,
                                                     const sp<Fence>& bufferFence) {
    if (buffer == nullptr) {
        return BAD_VALUE;
    }

    ATRACE_CALL();

    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mRenderingMutex);
        auto cachedImage = mImageCache.find(buffer->getId());
        found = (cachedImage != mImageCache.end());
    }

    // If we couldn't find the image in the cache at this time, then either
    // SurfaceFlinger messed up registering the buffer ahead of time or we got
    // backed up creating other EGLImages.
    if (!found) {
        status_t cacheResult = mImageManager->cache(buffer);
        if (cacheResult != NO_ERROR) {
            return cacheResult;
        }
    }

    // Whether or not we needed to cache, re-check mImageCache to make sure that
    // there's an EGLImage. The current threading model guarantees that we don't
    // destroy a cached image until it's really not needed anymore (i.e. this
    // function should not be called), so the only possibility is that something
    // terrible went wrong and we should just bind something and move on.
    {
        std::lock_guard<std::mutex> lock(mRenderingMutex);
        auto cachedImage = mImageCache.find(buffer->getId());

        if (cachedImage == mImageCache.end()) {
            // We failed creating the image if we got here, so bail out.
            ALOGE("Failed to create an EGLImage when rendering");
            bindExternalTextureImage(texName, *createImage());
            return NO_INIT;
        }

        bindExternalTextureImage(texName, *cachedImage->second);
    }

    // Wait for the new buffer to be ready.
    if (bufferFence != nullptr && bufferFence->isValid()) {
        if (GLExtensions::getInstance().hasWaitSync()) {
            base::unique_fd fenceFd(bufferFence->dup());
            if (fenceFd == -1) {
                ALOGE("error dup'ing fence fd: %d", errno);
                return -errno;
            }
            if (!waitFence(std::move(fenceFd))) {
                ALOGE("failed to wait on fence fd");
                return UNKNOWN_ERROR;
            }
        } else {
            status_t err = bufferFence->waitForever("RenderEngine::bindExternalTextureBuffer");
            if (err != NO_ERROR) {
                ALOGE("error waiting for fence: %d", err);
                return err;
            }
        }
    }

    return NO_ERROR;
}

void GLESRenderEngine::cacheExternalTextureBuffer(const sp<GraphicBuffer>& buffer) {
    mImageManager->cacheAsync(buffer, nullptr);
}

std::shared_ptr<ImageManager::Barrier> GLESRenderEngine::cacheExternalTextureBufferForTesting(
        const sp<GraphicBuffer>& buffer) {
    auto barrier = std::make_shared<ImageManager::Barrier>();
    mImageManager->cacheAsync(buffer, barrier);
    return barrier;
}

status_t GLESRenderEngine::cacheExternalTextureBufferInternal(const sp<GraphicBuffer>& buffer) {
    if (buffer == nullptr) {
        return BAD_VALUE;
    }

    {
        std::lock_guard<std::mutex> lock(mRenderingMutex);
        if (mImageCache.count(buffer->getId()) > 0) {
            // If there's already an image then fail fast here.
            return NO_ERROR;
        }
    }
    ATRACE_CALL();

    // Create the image without holding a lock so that we don't block anything.
    std::unique_ptr<Image> newImage = createImage();

    bool created = newImage->setNativeWindowBuffer(buffer->getNativeBuffer(),
                                                   buffer->getUsage() & GRALLOC_USAGE_PROTECTED);
    if (!created) {
        ALOGE("Failed to create image. size=%ux%u st=%u usage=%#" PRIx64 " fmt=%d",
              buffer->getWidth(), buffer->getHeight(), buffer->getStride(), buffer->getUsage(),
              buffer->getPixelFormat());
        return NO_INIT;
    }

    {
        std::lock_guard<std::mutex> lock(mRenderingMutex);
        if (mImageCache.count(buffer->getId()) > 0) {
            // In theory it's possible for another thread to recache the image,
            // so bail out if another thread won.
            return NO_ERROR;
        }
        mImageCache.insert(std::make_pair(buffer->getId(), std::move(newImage)));
    }

    return NO_ERROR;
}

void GLESRenderEngine::unbindExternalTextureBuffer(uint64_t bufferId) {
    mImageManager->releaseAsync(bufferId, nullptr);
}

std::shared_ptr<ImageManager::Barrier> GLESRenderEngine::unbindExternalTextureBufferForTesting(
        uint64_t bufferId) {
    auto barrier = std::make_shared<ImageManager::Barrier>();
    mImageManager->releaseAsync(bufferId, barrier);
    return barrier;
}

void GLESRenderEngine::unbindExternalTextureBufferInternal(uint64_t bufferId) {
    std::unique_ptr<Image> image;
    {
        std::lock_guard<std::mutex> lock(mRenderingMutex);
        const auto& cachedImage = mImageCache.find(bufferId);

        if (cachedImage != mImageCache.end()) {
            ALOGV("Destroying image for buffer: %" PRIu64, bufferId);
            // Move the buffer out of cache first, so that we can destroy
            // without holding the cache's lock.
            image = std::move(cachedImage->second);
            mImageCache.erase(bufferId);
            return;
        }
    }
    ALOGV("Failed to find image for buffer: %" PRIu64, bufferId);
}

FloatRect GLESRenderEngine::setupLayerCropping(const LayerSettings& layer, Mesh& mesh) {
    // Translate win by the rounded corners rect coordinates, to have all values in
    // layer coordinate space.
    FloatRect cropWin = layer.geometry.boundaries;
    const FloatRect& roundedCornersCrop = layer.geometry.roundedCornersCrop;
    cropWin.left -= roundedCornersCrop.left;
    cropWin.right -= roundedCornersCrop.left;
    cropWin.top -= roundedCornersCrop.top;
    cropWin.bottom -= roundedCornersCrop.top;
    Mesh::VertexArray<vec2> cropCoords(mesh.getCropCoordArray<vec2>());
    cropCoords[0] = vec2(cropWin.left, cropWin.top);
    cropCoords[1] = vec2(cropWin.left, cropWin.top + cropWin.getHeight());
    cropCoords[2] = vec2(cropWin.right, cropWin.top + cropWin.getHeight());
    cropCoords[3] = vec2(cropWin.right, cropWin.top);

    setupCornerRadiusCropSize(roundedCornersCrop.getWidth(), roundedCornersCrop.getHeight());
    return cropWin;
}

void GLESRenderEngine::handleRoundedCorners(const DisplaySettings& display,
                                            const LayerSettings& layer, const Mesh& mesh) {
    // We separate the layer into 3 parts essentially, such that we only turn on blending for the
    // top rectangle and the bottom rectangle, and turn off blending for the middle rectangle.
    FloatRect bounds = layer.geometry.roundedCornersCrop;

    // Firstly, we need to convert the coordination from layer native coordination space to
    // device coordination space.
    const auto transformMatrix = display.globalTransform * layer.geometry.positionTransform;
    const vec4 leftTopCoordinate(bounds.left, bounds.top, 1.0, 1.0);
    const vec4 rightBottomCoordinate(bounds.right, bounds.bottom, 1.0, 1.0);
    const vec4 leftTopCoordinateInBuffer = transformMatrix * leftTopCoordinate;
    const vec4 rightBottomCoordinateInBuffer = transformMatrix * rightBottomCoordinate;
    bounds = FloatRect(leftTopCoordinateInBuffer[0], leftTopCoordinateInBuffer[1],
                       rightBottomCoordinateInBuffer[0], rightBottomCoordinateInBuffer[1]);

    /*-------------------------------------------------------*/

    // Secondly, if the display is rotated, we need to undo the rotation on coordination and
    // align the (left, top) and (right, bottom) coordination with the device coordination
    // space.

    ISurfaceComposer::Rotation orientationInRotation;

    switch (display.orientation)
    {
        case ui::Transform::ROT_0:
            orientationInRotation = ISurfaceComposer::eRotateNone;
            break;
        case ui::Transform::ROT_90:
            orientationInRotation = ISurfaceComposer::eRotate90;
            break;
        case ui::Transform::ROT_180:
            orientationInRotation = ISurfaceComposer::eRotate180;
            break;
        case ui::Transform::ROT_270:
            orientationInRotation = ISurfaceComposer::eRotate270;
            break;
        default:
            ALOGE("unexpect 'display.orientation' : 0x%x", display.orientation);
            orientationInRotation = ISurfaceComposer::eRotateNone;
            break;
    }

    int finalOrientation = ( (int)orientationInRotation + display.displayInstallOrientation) % ( ISurfaceComposer::eRotate270 + 1);

    switch (finalOrientation)
    {
        case ISurfaceComposer::eRotate90:
            std::swap(bounds.left, bounds.right);
            break;
        case ISurfaceComposer::eRotate180:
            std::swap(bounds.left, bounds.right);
            std::swap(bounds.top, bounds.bottom);
            break;
        case ISurfaceComposer::eRotate270:
            std::swap(bounds.top, bounds.bottom);
            break;
        default:
            break;
    }

    /*-------------------------------------------------------*/

    // Finally, we cut the layer into 3 parts, with top and bottom parts having rounded corners
    // and the middle part without rounded corners.
    const int32_t radius = ceil(layer.geometry.roundedCornersRadius);
    const Rect topRect(bounds.left, bounds.top, bounds.right, bounds.top + radius);
    setScissor(topRect);
    drawMesh(mesh);
    const Rect bottomRect(bounds.left, bounds.bottom - radius, bounds.right, bounds.bottom);
    setScissor(bottomRect);
    drawMesh(mesh);

    // The middle part of the layer can turn off blending.
    const Rect middleRect(bounds.left, bounds.top + radius, bounds.right, bounds.bottom - radius);
    if ( middleRect.bottom < middleRect.top )
    {
        return;
    }
    setScissor(middleRect);
    mState.cornerRadius = 0.0;
    disableBlending();
    drawMesh(mesh);
    disableScissor();
}

status_t GLESRenderEngine::bindFrameBuffer(Framebuffer* framebuffer) {
    ATRACE_CALL();
    GLFramebuffer* glFramebuffer = static_cast<GLFramebuffer*>(framebuffer);
    EGLImageKHR eglImage = glFramebuffer->getEGLImage();
    uint32_t textureName = glFramebuffer->getTextureName();
    uint32_t framebufferName = glFramebuffer->getFramebufferName();

    // Bind the texture and turn our EGLImage into a texture
    glBindTexture(GL_TEXTURE_2D, textureName);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)eglImage);

    // Bind the Framebuffer to render into
    glBindFramebuffer(GL_FRAMEBUFFER, framebufferName);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureName, 0);

    uint32_t glStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    ALOGE_IF(glStatus != GL_FRAMEBUFFER_COMPLETE_OES, "glCheckFramebufferStatusOES error %d",
             glStatus);

    return glStatus == GL_FRAMEBUFFER_COMPLETE_OES ? NO_ERROR : BAD_VALUE;
}

void GLESRenderEngine::unbindFrameBuffer(Framebuffer* /* framebuffer */) {
    ATRACE_CALL();

    // back to main framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLESRenderEngine::checkErrors() const {
    do {
        // there could be more than one error flag
        GLenum error = glGetError();
        if (error == GL_NO_ERROR) break;
        ALOGE("GL error 0x%04x", int(error));
    } while (true);
}

bool GLESRenderEngine::supportsProtectedContent() const {
    return mProtectedEGLContext != EGL_NO_CONTEXT;
}

bool GLESRenderEngine::useProtectedContext(bool useProtectedContext) {
    if (useProtectedContext == mInProtectedContext) {
        return true;
    }
    if (useProtectedContext && mProtectedEGLContext == EGL_NO_CONTEXT) {
        return false;
    }
    const EGLSurface surface = useProtectedContext ? mProtectedDummySurface : mDummySurface;
    const EGLContext context = useProtectedContext ? mProtectedEGLContext : mEGLContext;
    const bool success = eglMakeCurrent(mEGLDisplay, surface, surface, context) == EGL_TRUE;
    if (success) {
        mInProtectedContext = useProtectedContext;
    }
    return success;
}
EGLImageKHR GLESRenderEngine::createFramebufferImageIfNeeded(ANativeWindowBuffer* nativeBuffer,
                                                             bool isProtected,
                                                             bool useFramebufferCache) {
    sp<GraphicBuffer> graphicBuffer = GraphicBuffer::from(nativeBuffer);
    if (useFramebufferCache) {
        std::lock_guard<std::mutex> lock(mFramebufferImageCacheMutex);
        for (const auto& image : mFramebufferImageCache) {
            if (image.first == graphicBuffer->getId()) {
                return image.second;
            }
        }
    }
    EGLint attributes[] = {
            isProtected ? EGL_PROTECTED_CONTENT_EXT : EGL_NONE,
            isProtected ? EGL_TRUE : EGL_NONE,
            EGL_NONE,
    };
    EGLImageKHR image = eglCreateImageKHR(mEGLDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                          nativeBuffer, attributes);
    if (useFramebufferCache) {
        if (image != EGL_NO_IMAGE_KHR) {
            std::lock_guard<std::mutex> lock(mFramebufferImageCacheMutex);
            if (mFramebufferImageCache.size() >= mFramebufferImageCacheSize) {
                EGLImageKHR expired = mFramebufferImageCache.front().second;
                mFramebufferImageCache.pop_front();
                eglDestroyImageKHR(mEGLDisplay, expired);
                DEBUG_EGL_IMAGE_TRACKER_DESTROY();
            }
            mFramebufferImageCache.push_back({graphicBuffer->getId(), image});
        }
    }

    if (image != EGL_NO_IMAGE_KHR) {
        DEBUG_EGL_IMAGE_TRACKER_CREATE();
    }
    return image;
}

#if (RK_NV12_10_TO_NV12_BY_RGA | RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
  /* print time macros. */
#define PRINT_TIME_START        \
    struct timeval tpend1, tpend2;\
    long usec1 = 0;\
    gettimeofday(&tpend1,NULL);\

#define PRINT_TIME_END(tag)        \
    gettimeofday(&tpend2,NULL);\
    usec1 = 1000*(tpend2.tv_sec - tpend1.tv_sec) + (tpend2.tv_usec- tpend1.tv_usec)/1000;\
    if (property_get_bool("sys.hwc.time", 1)) \
    ALOGD_IF(1,"%s use time=%ld ms",tag,usec1);\

#if RK_NV12_10_TO_NV12_BY_RGA
static int rgaCopyBit(sp<GraphicBuffer> src_buf, sp<GraphicBuffer> dst_buf, const Rect& rect)
{
    rga_info_t src, dst;
    int src_l,src_t,src_r,src_b,src_h,src_stride,src_format;
    int dst_l,dst_t,dst_r,dst_b,dst_h,dst_stride,dst_format;
    RockchipRga& mRga = RockchipRga::get();
    int ret = 0;

    memset(&src, 0, sizeof(rga_info_t));
    memset(&dst, 0, sizeof(rga_info_t));
    src.fd = -1;
    dst.fd = -1;

    src_stride = src_buf->getStride();
    src_format = src_buf->getPixelFormat();
    src_h = src_buf->getHeight();

    dst_stride = dst_buf->getStride();
    dst_format = dst_buf->getPixelFormat();
    dst_h = dst_buf->getHeight();

    dst_l = src_l = rect.left;
    dst_t = src_t = rect.top;
    dst_r = src_r = rect.right;
    dst_b = src_b = rect.bottom;
    rga_set_rect(&src.rect, src_l, src_t, src_r - src_l, src_b - src_t, src_stride, src_h, src_format);
    rga_set_rect(&dst.rect, dst_l, dst_t, dst_buf->getWidth(), dst_buf->getHeight(), dst_stride, dst_h, dst_format);

    src.hnd = src_buf->handle;
    dst.hnd = dst_buf->handle;
//    mRga.RkRgaGetBufferFd(src_buf->handle, &src.fd);
//    mRga.RkRgaGetBufferFd(dst_buf->handle, &dst.fd);
//  src.rotation = rga_transform;
//PRINT_TIME_START
    ret = mRga.RkRgaBlit(&src, &dst, NULL);
//PRINT_TIME_END("rgaCopyBit")
    if(ret) {
        ALOGD_IF(1,"rgaCopyBit  : src[x=%d,y=%d,w=%d,h=%d,ws=%d,hs=%d,format=0x%x],dst[x=%d,y=%d,w=%d,h=%d,ws=%d,hs=%d,format=0x%x]",
            src.rect.xoffset, src.rect.yoffset, src.rect.width, src.rect.height, src.rect.wstride, src.rect.hstride, src.rect.format,
            dst.rect.xoffset, dst.rect.yoffset, dst.rect.width, dst.rect.height, dst.rect.wstride, dst.rect.hstride, dst.rect.format);
        ALOGD_IF(1,"rgaCopyBit : src hnd=%p,dst hnd=%p, src_format=0x%x ==> dst_format=0x%x\n",
            (void*)src_buf->handle, (void*)(dst_buf->handle), src_format, dst_format);
        return ret;
    }

    return ret;
}
#endif

#if RK_HDR
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
#define ARM_P010            0x4000000
#define HDRUSAGE            0x3000000
#define RK_XXX_PATH         "/system/lib64/librockchipxxx.so"
typedef void (*__rockchipxxx)(u8 *src, u8 *dst, int w, int h, int srcStride, int dstStride, int area);

static void* dso = NULL;
static __rockchipxxx rockchipxxx = NULL;

#define ALIGN(val, align) (((val) + ((align) - 1)) & ~((align) - 1))

#elif RK_NV12_10_TO_NV12_BY_NENO

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
#define RK_XXX_PATH         "/system/lib/librockchipxxx.so"
typedef void (*__rockchipxxx3288)(u8 *src, u8 *dst, int w, int h, int srcStride, int dstStride, int area);

static void* dso = NULL;
static __rockchipxxx3288 rockchipxxx3288 = NULL;
#endif

#endif

status_t GLESRenderEngine::drawLayers(const DisplaySettings& display,
                                      const std::vector<LayerSettings>& layers,
                                      ANativeWindowBuffer* const buffer,
                                      ANativeWindowBuffer* const buffer2,
                                      const bool useFramebufferCache, base::unique_fd&& bufferFence,
                                      base::unique_fd* drawFence) {
    ATRACE_CALL();
    //ALOGE("[zizhan] draw begin--------------------------------------------------------------------------------");
    if (layers.empty()) {
        ALOGE("[zizhan] Drawing empty layer stack");
        return NO_ERROR;
    }
#if MALI_PRODUCT_ID_450 || MALI_PRODUCT_ID_400
        if(bufferFence.get() >= 0){     //.RK ext: don't use waitFence before solve the problem(eglWaitSyncKHR didn't wait release fence) 
#else
        if (bufferFence.get() >= 0 && !waitFence(std::move(bufferFence))) {
#endif
        ATRACE_NAME("Waiting before draw");
        //ALOGE("before wait, bufferFence is %d", bufferFence.get());
        sync_wait(bufferFence.get(), -1);
        //ALOGE("after wait, bufferFence is %d", bufferFence.get());
    }

    if (buffer == nullptr) {
        ALOGE("No output buffer provided. Aborting GPU composition.");
        return BAD_VALUE;
    }

{
    if (shaderProgram == 0) {
    ALOGE("[zizhan] enter shader program");
    static const GLchar* vertexShaderSource =  "attribute vec3 aPosition;\n"
    "attribute vec2 aTexcoords;\n"
    "varying vec2 vTexcoords;\n"
    "void main()\n"
    "{\n"
    "vTexcoords = aTexcoords;\n"
    "gl_Position = vec4(aPosition.x, aPosition.y, aPosition.z, 1.0);\n"
    "}\0";
    static const GLchar* fragmentShaderSource = "precision mediump float;\n"
    "varying vec2 vTexcoords;\n"
    "uniform sampler2D uTexture;\n"
    "void main()\n"
    "{\n"
    "vec3 color = texture2D(uTexture, vTexcoords).rgb;\n"
    "gl_FragColor = vec4(color.r, color.g, color.b, 1.0);\n"
    "}\n\0";

    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderSource, NULL);
    glCompileShader(vertexShader);
    // Check for compile time errors
    GLint success;
    GLchar infoLog[512];
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
        ALOGE("ERROR::SHADER::VERTEX::COMPILATION_FAILED %s", infoLog);
    }
    // Fragment shader
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
    glCompileShader(fragmentShader);
    // Check for compile time errors
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    //ALOGE("[zizhan] place 1");
    if (!success)
    {
        glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
        ALOGE("ERROR::SHADER::FRAGMENT::COMPILATION_FAILED %s\n", infoLog);
    }
    // Link shaders
    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);
    glLinkProgram(shaderProgram);
    // Check for linking errors
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
        ALOGE("ERROR::SHADER::PROGRAM::LINKING_FAILED %s\n", infoLog);
        glDeleteProgram(shaderProgram);
        shaderProgram = 0;
    }
    //ALOGE("[zizhan] place 2");
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    if (shaderProgram) {
        texLocation = glGetUniformLocation(shaderProgram, "uTexture");
    }
    }

    BindNativeBufferAsFramebuffer fbo(*this, buffer, useFramebufferCache);
    //ALOGE("[zizhan] gpu buffer is [%p]", buffer->handle);

    if (fbo.getStatus() != NO_ERROR) {
        ALOGE("Failed to bind framebuffer! Aborting GPU composition for buffer (%p).",
              buffer->handle);
        //ALOGE("[zizhan] place 1");
        checkErrors();
        return fbo.getStatus();
    }

    // clear the entire buffer, sometimes when we reuse buffers we'd persist
    // ghost images otherwise.
    // we also require a full transparent framebuffer for overlays. This is
    // probably not quite efficient on all GPUs, since we could filter out
    // opaque layers.
    clearWithColor(0.0, 0.0, 0.0, 0.0);

    setViewportAndProjection(display.physicalDisplay, display.clip);

    setOutputDataSpace(display.outputDataspace);
    setDisplayMaxLuminance(display.maxLuminance);

    mat4 projectionMatrix = mState.projectionMatrix * display.globalTransform;
    mState.projectionMatrix = projectionMatrix;
    if (!display.clearRegion.isEmpty()) {
        glDisable(GL_BLEND);
        fillRegionWithColor(display.clearRegion, 0.0, 0.0, 0.0, 1.0);
    }

    Mesh mesh(Mesh::TRIANGLE_FAN, 4, 2, 2);
    for (auto layer : layers) {
        mState.projectionMatrix = projectionMatrix * layer.geometry.positionTransform;

        const FloatRect bounds = layer.geometry.boundaries;
        Mesh::VertexArray<vec2> position(mesh.getPositionArray<vec2>());
        position[0] = vec2(bounds.left, bounds.top);
        position[1] = vec2(bounds.left, bounds.bottom);
        position[2] = vec2(bounds.right, bounds.bottom);
        position[3] = vec2(bounds.right, bounds.top);

        setupLayerCropping(layer, mesh);
        setColorTransform(display.colorTransform * layer.colorTransform);

        bool usePremultipliedAlpha = true;
        bool disableTexture = true;
        bool isOpaque = false;

        if (layer.source.buffer.buffer != nullptr) {
            disableTexture = false;
            isOpaque = layer.source.buffer.isOpaque;

            sp<GraphicBuffer> gBuf = layer.source.buffer.buffer;
#if (RK_NV12_10_TO_NV12_BY_RGA | RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
            if(gBuf != NULL &&
               gBuf->getPixelFormat() == HAL_PIXEL_FORMAT_YCrCb_NV12_10 )
            {
                //Rect CurrentCrop(0,0,3840,2160);
                Rect CurrentCrop(layer.source.buffer.currentcrop);
#if RK_HDR
                const int yuvTexUsage = GraphicBuffer::USAGE_HW_TEXTURE | GRALLOC_USAGE_TO_USE_ARM_P010;
                const int yuvTexFormat = HAL_PIXEL_FORMAT_YCrCb_NV12_10;
#elif (RK_NV12_10_TO_NV12_BY_NENO | RK_NV12_10_TO_NV12_BY_RGA)
                const int yuvTexUsage = GraphicBuffer::USAGE_HW_TEXTURE /*| HDRUSAGE*/;
                //GraphicBuffer::USAGE_SW_WRITE_RARELY;
                const int yuvTexFormat = HAL_PIXEL_FORMAT_YCrCb_NV12;
#endif
                static int yuvcnt;
                int yuvIndex ;

                yuvcnt ++;
                yuvIndex = yuvcnt%2;
#if (RK_HDR | RK_NV12_10_TO_NV12_BY_NENO)
                int src_l,src_t,src_r,src_b,src_stride;
                void *src_vaddr;
                void *dst_vaddr;
                src_l = CurrentCrop.left;
                src_t = CurrentCrop.top;
                src_r = CurrentCrop.right;
                src_b = CurrentCrop.bottom;
                src_stride = gBuf->getStride();
                uint32_t w = src_r - src_l;
#elif RK_NV12_10_TO_NV12_BY_RGA
                //Since rga cann't support scalet to bigger than 4096 limit to 4096
                uint32_t w = (CurrentCrop.getWidth() + 31) & (~31);
                //ALOGD("10to8[%s %d] f:%x w:%d gH:%d\n",__FUNCTION__,__LINE__,gBuf->getPixelFormat(),w,gBuf->getHeight());
#endif
                if((yuvTeximg[yuvIndex].yuvTexBuffer != NULL) &&
                   (yuvTeximg[yuvIndex].yuvTexBuffer->getWidth() != w ||
                    yuvTeximg[yuvIndex].yuvTexBuffer->getHeight() != gBuf->getHeight()))
                {
                    yuvTeximg[yuvIndex].yuvTexBuffer = NULL;
                }
                if(yuvTeximg[yuvIndex].yuvTexBuffer == NULL)
                {
                    yuvTeximg[yuvIndex].yuvTexBuffer = new GraphicBuffer(w, gBuf->getHeight(),yuvTexFormat, yuvTexUsage);
                }

#if (RK_HDR | RK_NV12_10_TO_NV12_BY_NENO)
                gBuf->lock(GRALLOC_USAGE_SW_READ_OFTEN,&src_vaddr);
                yuvTeximg[yuvIndex].yuvTexBuffer->lock(GRALLOC_USAGE_SW_WRITE_OFTEN,&dst_vaddr);

                //PRINT_TIME_START
                if(dso == NULL)
                    dso = dlopen(RK_XXX_PATH, RTLD_NOW | RTLD_LOCAL);

                if (dso == 0) {
                    ALOGE("rk_debug can't not find /system/lib64/librockchipxxx.so ! error=%s \n",
                        dlerror());
                    return BAD_VALUE;
                }
#if RK_HDR
                if(rockchipxxx == NULL)
                    rockchipxxx = (__rockchipxxx)dlsym(dso, "_Z11rockchipxxxPhS_iiiii");
                if(rockchipxxx == NULL)
                {
                    ALOGE("rk_debug can't not find target function in /system/lib64/librockchipxxx.so ! \n");
                    dlclose(dso);
                    return BAD_VALUE;
                }
                /* align w to 64 */
                w = ALIGN(w, 64);
                ALOGD("DEBUG_lb Stride=%d",yuvTeximg[yuvIndex].yuvTexBuffer->getStride());
                if(w <= yuvTeximg[yuvIndex].yuvTexBuffer->getStride()/2)
                {
                    rockchipxxx((u8*)src_vaddr, (u8*)dst_vaddr, w, src_b - src_t, src_stride, yuvTeximg[yuvIndex].yuvTexBuffer->getStride(), 0);
                }else
                    ALOGE("%s(%d):unsupport resolution for 4k", __FUNCTION__, __LINE__);
#elif RK_NV12_10_TO_NV12_BY_NENO
                if(rockchipxxx3288 == NULL)
                    rockchipxxx3288 = (__rockchipxxx3288)dlsym(dso, "_Z15rockchipxxx3288PhS_iiiii");
                if(rockchipxxx3288 == NULL)
                {
                    ALOGE("rk_debug can't not find target function in /system/lib64/librockchipxxx.so ! \n");
                    dlclose(dso);
                    return BAD_VALUE;
                }
                rockchipxxx3288((u8*)src_vaddr, (u8*)dst_vaddr, src_r - src_l, src_b - src_t, src_stride, (src_r - src_l), 0);
#endif
                //PRINT_TIME_END("convert10to16_highbit_arm64_neon")
                ALOGD("src_vaddr=%p,dst_vaddr=%p,crop_w=%d,crop_h=%d,stride=%f, src_stride=%d,raw_w=%d,raw_h=%d",
                        src_vaddr, dst_vaddr, src_r - src_l,src_b - src_t,
                        (src_r - src_l)*1.25+64,src_stride,gBuf->getWidth(),gBuf->getHeight());
                //dump data
                static int i =0;
                char pro_value[PROPERTY_VALUE_MAX];

                property_get("sys.dump_out_neon",pro_value,0);
                if(i<10 && !strcmp(pro_value,"true"))
                {
                    char data_name[100];

                    sprintf(data_name,"/data/dump/dmlayer%d_%d_%d.bin", i,
                            yuvTeximg[yuvIndex].yuvTexBuffer->getWidth(),yuvTeximg[yuvIndex].yuvTexBuffer->getHeight());
#if RK_HDR
                    int n = yuvTeximg[yuvIndex].yuvTexBuffer->getHeight() * yuvTeximg[yuvIndex].yuvTexBuffer->getStride();
#else
                    int n = yuvTeximg[yuvIndex].yuvTexBuffer->getHeight() * yuvTeximg[yuvIndex].yuvTexBuffer->getStride() * 1.5;
#endif
                    ALOGD("dump %s size=%d", data_name, n );
                    FILE *fp;
                    if ((fp = fopen(data_name, "w+")) == NULL)
                    {
                        printf("can't open output.bin!!!!!\n");
                    }
                    fwrite(dst_vaddr, n, 1, fp);
                    fclose(fp);
                    i++;
                }

#elif RK_NV12_10_TO_NV12_BY_RGA
                rgaCopyBit(gBuf, yuvTeximg[yuvIndex].yuvTexBuffer, CurrentCrop);
#endif
                bindExternalTextureBuffer(layer.source.buffer.textureName,
                                yuvTeximg[yuvIndex].yuvTexBuffer, layer.source.buffer.fence);

            }
            else
#endif
            {
                bindExternalTextureBuffer(layer.source.buffer.textureName, gBuf, layer.source.buffer.fence);
            }

            usePremultipliedAlpha = layer.source.buffer.usePremultipliedAlpha;
            Texture texture(Texture::TEXTURE_EXTERNAL, layer.source.buffer.textureName);
            mat4 texMatrix = layer.source.buffer.textureTransform;

            texture.setMatrix(texMatrix.asArray());
            texture.setFiltering(layer.source.buffer.useTextureFiltering);

            texture.setDimensions(gBuf->getWidth(), gBuf->getHeight());
            setSourceY410BT2020(layer.source.buffer.isY410BT2020);

            renderengine::Mesh::VertexArray<vec2> texCoords(mesh.getTexCoordArray<vec2>());
            texCoords[0] = vec2(0.0, 0.0);
            texCoords[1] = vec2(0.0, 1.0);
            texCoords[2] = vec2(1.0, 1.0);
            texCoords[3] = vec2(1.0, 0.0);
            setupLayerTexturing(texture);
        }

        const half3 solidColor = layer.source.solidColor;
        const half4 color = half4(solidColor.r, solidColor.g, solidColor.b, layer.alpha);
        // Buffer sources will have a black solid color ignored in the shader,
        // so in that scenario the solid color passed here is arbitrary.
        setupLayerBlending(usePremultipliedAlpha, isOpaque, disableTexture, color,
                           layer.geometry.roundedCornersRadius);
        if (layer.disableBlending) {
            glDisable(GL_BLEND);
        }
        setSourceDataSpace(layer.sourceDataspace);

        // We only want to do a special handling for rounded corners when having rounded corners
        // is the only reason it needs to turn on blending, otherwise, we handle it like the
        // usual way since it needs to turn on blending anyway.
        if (layer.geometry.roundedCornersRadius > 0.0 && color.a >= 1.0f && isOpaque) {
            handleRoundedCorners(display, layer, mesh);
        } else {
            //ALOGE("[zizhan] place 3");
            checkErrors();
            drawMesh(mesh);
            //ALOGE("[zizhan] place 4");
            checkErrors();
        }

        // Cleanup if there's a buffer source
        if (layer.source.buffer.buffer != nullptr) {
            disableBlending();
            setSourceY410BT2020(false);
            disableTexturing();
        }
    }

    Texture fbTexture(Texture::TEXTURE_2D, fbTexName);

    GLFramebuffer* glFramebuffer = static_cast<GLFramebuffer*>(fbo.getFramebuffer());
    EGLImageKHR eglImage = glFramebuffer->getEGLImage();

    setupLayerTexturing(fbTexture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)eglImage);
    glBindTexture(GL_TEXTURE_2D, 0);
    disableTexturing();
}

{
    //ALOGE("[zizhan] enter triangle"); 
    BindNativeBufferAsFramebuffer fbo(*this, buffer2, useFramebufferCache);
    //ALOGE("[zizhan] gpu buffer is [%p]", buffer->handle);

    if (fbo.getStatus() != NO_ERROR) {
        ALOGE("Failed to bind framebuffer! Aborting GPU composition for buffer (%p).",
              buffer2->handle);
        //ALOGE("[zizhan] place 1");
        checkErrors();
        return fbo.getStatus();
    }

    // clear the entire buffer, sometimes when we reuse buffers we'd persist
    // ghost images otherwise.
    // we also require a full transparent framebuffer for overlays. This is
    // probably not quite efficient on all GPUs, since we could filter out
    // opaque layers.
    clearWithColor(0.0, 0.0, 0.0, 0.0);

    setViewportAndProjection(display.physicalDisplay, display.clip);

    setOutputDataSpace(display.outputDataspace);
    setDisplayMaxLuminance(display.maxLuminance);

    static GLfloat vertices[] = {
         1.0f,  1.0f, 0.0f,  
         1.0f, -1.0f, 0.0f,
         -1.0f,  1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f
    };

    static GLfloat texcoords[] = {
        1.0f, 1.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 0.0f,
        0.0f, 0.0f,
        0.0f, 1.0f,
    };

    checkErrors();
    glUseProgram(shaderProgram);
    checkErrors();

    Texture fbTexture(Texture::TEXTURE_2D, fbTexName);

    glActiveTexture(GL_TEXTURE0);
    setupLayerTexturing(fbTexture);
    glUniform1i(texLocation, 0);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, vertices);
    checkErrors();
    glEnableVertexAttribArray(0);
    checkErrors();

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    checkErrors();
    glEnableVertexAttribArray(1);
    checkErrors();

    glDrawArrays(GL_TRIANGLES, 0, 6);
    checkErrors();

    glBindTexture(GL_TEXTURE_2D, 0);
    disableTexturing();
}
    /*
    //zizhan------------------------------------------------
    static uint64_t managedColorFrameCount = 0;
    std::ostringstream out;
    out << "/data/texture_out" << managedColorFrameCount++;
    writePPM(out.str().c_str(), mVpWidth, mVpHeight);
    //zizhan------------------------------------------------
    */

    if (drawFence != nullptr) {
        //ALOGE("[zizhan] before flush, drawFence is [%d]", drawFence->get());
        *drawFence = flush();
        //ALOGE("[zizhan] after flush, drawFence is [%d]", drawFence->get());
    }
    // If flush failed or we don't support native fences, we need to force the
    // gl command stream to be executed.
    if (drawFence == nullptr || drawFence->get() < 0) {
        //ALOGE("[zizhan] place 5");
        checkErrors();
        bool success = finish();
        //ALOGE("[zizhan] place 6");
        checkErrors();
        if (!success) {
            ALOGE("Failed to flush RenderEngine commands");
            //ALOGE("[zizhan] place 2");
            checkErrors();
            // Chances are, something illegal happened (either the caller passed
            // us bad parameters, or we messed up our shader generation).
            return INVALID_OPERATION;
        }
    }

    //zizhan
    //ALOGE("[zizhan] place 5");
    checkErrors();
    glFinish();
    //ALOGE("[zizhan] place 6");
    checkErrors();

    //ALOGE("[zizhan] place 7");
    checkErrors();
    //ALOGE("[zizhan] draw end--------------------------------------------------------------------------------");
    return NO_ERROR;
}

status_t GLESRenderEngine::drawLayers(const DisplaySettings& display,
                                      const std::vector<LayerSettings>& layers,
                                      ANativeWindowBuffer* const buffer,
                                      const bool useFramebufferCache, base::unique_fd&& bufferFence,
                                      base::unique_fd* drawFence) {
    ATRACE_CALL();
    //ALOGE("[zizhan] draw begin--------------------------------------------------------------------------------");
    if (layers.empty()) {
        ALOGE("[zizhan] Drawing empty layer stack");
        return NO_ERROR;
    }
#if MALI_PRODUCT_ID_450 || MALI_PRODUCT_ID_400
        if(bufferFence.get() >= 0){     //.RK ext: don't use waitFence before solve the problem(eglWaitSyncKHR didn't wait release fence) 
#else
        if (bufferFence.get() >= 0 && !waitFence(std::move(bufferFence))) {
#endif
        ATRACE_NAME("Waiting before draw");
        //ALOGE("before wait, bufferFence is %d", bufferFence.get());
        sync_wait(bufferFence.get(), -1);
        //ALOGE("after wait, bufferFence is %d", bufferFence.get());
    }

    if (buffer == nullptr) {
        ALOGE("No output buffer provided. Aborting GPU composition.");
        return BAD_VALUE;
    }

    BindNativeBufferAsFramebuffer fbo(*this, buffer, useFramebufferCache);
    //ALOGE("[zizhan] gpu buffer is [%p]", buffer->handle);

    if (fbo.getStatus() != NO_ERROR) {
        ALOGE("Failed to bind framebuffer! Aborting GPU composition for buffer (%p).",
              buffer->handle);
        //ALOGE("[zizhan] place 1");
        checkErrors();
        return fbo.getStatus();
    }

    // clear the entire buffer, sometimes when we reuse buffers we'd persist
    // ghost images otherwise.
    // we also require a full transparent framebuffer for overlays. This is
    // probably not quite efficient on all GPUs, since we could filter out
    // opaque layers.
    clearWithColor(0.0, 0.0, 0.0, 0.0);

    setViewportAndProjection(display.physicalDisplay, display.clip);

    setOutputDataSpace(display.outputDataspace);
    setDisplayMaxLuminance(display.maxLuminance);

    mat4 projectionMatrix = mState.projectionMatrix * display.globalTransform;
    mState.projectionMatrix = projectionMatrix;
    if (!display.clearRegion.isEmpty()) {
        glDisable(GL_BLEND);
        fillRegionWithColor(display.clearRegion, 0.0, 0.0, 0.0, 1.0);
    }

    Mesh mesh(Mesh::TRIANGLE_FAN, 4, 2, 2);
    for (auto layer : layers) {
        mState.projectionMatrix = projectionMatrix * layer.geometry.positionTransform;

        const FloatRect bounds = layer.geometry.boundaries;
        Mesh::VertexArray<vec2> position(mesh.getPositionArray<vec2>());
        position[0] = vec2(bounds.left, bounds.top);
        position[1] = vec2(bounds.left, bounds.bottom);
        position[2] = vec2(bounds.right, bounds.bottom);
        position[3] = vec2(bounds.right, bounds.top);

        setupLayerCropping(layer, mesh);
        setColorTransform(display.colorTransform * layer.colorTransform);

        bool usePremultipliedAlpha = true;
        bool disableTexture = true;
        bool isOpaque = false;

        if (layer.source.buffer.buffer != nullptr) {
            disableTexture = false;
            isOpaque = layer.source.buffer.isOpaque;

            sp<GraphicBuffer> gBuf = layer.source.buffer.buffer;
#if (RK_NV12_10_TO_NV12_BY_RGA | RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
            if(gBuf != NULL &&
               gBuf->getPixelFormat() == HAL_PIXEL_FORMAT_YCrCb_NV12_10 )
            {
                //Rect CurrentCrop(0,0,3840,2160);
                Rect CurrentCrop(layer.source.buffer.currentcrop);
#if RK_HDR
                const int yuvTexUsage = GraphicBuffer::USAGE_HW_TEXTURE | GRALLOC_USAGE_TO_USE_ARM_P010;
                const int yuvTexFormat = HAL_PIXEL_FORMAT_YCrCb_NV12_10;
#elif (RK_NV12_10_TO_NV12_BY_NENO | RK_NV12_10_TO_NV12_BY_RGA)
                const int yuvTexUsage = GraphicBuffer::USAGE_HW_TEXTURE /*| HDRUSAGE*/;
                //GraphicBuffer::USAGE_SW_WRITE_RARELY;
                const int yuvTexFormat = HAL_PIXEL_FORMAT_YCrCb_NV12;
#endif
                static int yuvcnt;
                int yuvIndex ;

                yuvcnt ++;
                yuvIndex = yuvcnt%2;
#if (RK_HDR | RK_NV12_10_TO_NV12_BY_NENO)
                int src_l,src_t,src_r,src_b,src_stride;
                void *src_vaddr;
                void *dst_vaddr;
                src_l = CurrentCrop.left;
                src_t = CurrentCrop.top;
                src_r = CurrentCrop.right;
                src_b = CurrentCrop.bottom;
                src_stride = gBuf->getStride();
                uint32_t w = src_r - src_l;
#elif RK_NV12_10_TO_NV12_BY_RGA
                //Since rga cann't support scalet to bigger than 4096 limit to 4096
                uint32_t w = (CurrentCrop.getWidth() + 31) & (~31);
                //ALOGD("10to8[%s %d] f:%x w:%d gH:%d\n",__FUNCTION__,__LINE__,gBuf->getPixelFormat(),w,gBuf->getHeight());
#endif
                if((yuvTeximg[yuvIndex].yuvTexBuffer != NULL) &&
                   (yuvTeximg[yuvIndex].yuvTexBuffer->getWidth() != w ||
                    yuvTeximg[yuvIndex].yuvTexBuffer->getHeight() != gBuf->getHeight()))
                {
                    yuvTeximg[yuvIndex].yuvTexBuffer = NULL;
                }
                if(yuvTeximg[yuvIndex].yuvTexBuffer == NULL)
                {
                    yuvTeximg[yuvIndex].yuvTexBuffer = new GraphicBuffer(w, gBuf->getHeight(),yuvTexFormat, yuvTexUsage);
                }

#if (RK_HDR | RK_NV12_10_TO_NV12_BY_NENO)
                gBuf->lock(GRALLOC_USAGE_SW_READ_OFTEN,&src_vaddr);
                yuvTeximg[yuvIndex].yuvTexBuffer->lock(GRALLOC_USAGE_SW_WRITE_OFTEN,&dst_vaddr);

                //PRINT_TIME_START
                if(dso == NULL)
                    dso = dlopen(RK_XXX_PATH, RTLD_NOW | RTLD_LOCAL);

                if (dso == 0) {
                    ALOGE("rk_debug can't not find /system/lib64/librockchipxxx.so ! error=%s \n",
                        dlerror());
                    return BAD_VALUE;
                }
#if RK_HDR
                if(rockchipxxx == NULL)
                    rockchipxxx = (__rockchipxxx)dlsym(dso, "_Z11rockchipxxxPhS_iiiii");
                if(rockchipxxx == NULL)
                {
                    ALOGE("rk_debug can't not find target function in /system/lib64/librockchipxxx.so ! \n");
                    dlclose(dso);
                    return BAD_VALUE;
                }
                /* align w to 64 */
                w = ALIGN(w, 64);
                ALOGD("DEBUG_lb Stride=%d",yuvTeximg[yuvIndex].yuvTexBuffer->getStride());
                if(w <= yuvTeximg[yuvIndex].yuvTexBuffer->getStride()/2)
                {
                    rockchipxxx((u8*)src_vaddr, (u8*)dst_vaddr, w, src_b - src_t, src_stride, yuvTeximg[yuvIndex].yuvTexBuffer->getStride(), 0);
                }else
                    ALOGE("%s(%d):unsupport resolution for 4k", __FUNCTION__, __LINE__);
#elif RK_NV12_10_TO_NV12_BY_NENO
                if(rockchipxxx3288 == NULL)
                    ALOGE("rk_debug can't not find target function in /system/lib64/librockchipxxx.so ! \n");
                    dlclose(dso);
                    return BAD_VALUE;
                }
                rockchipxxx3288((u8*)src_vaddr, (u8*)dst_vaddr, src_r - src_l, src_b - src_t, src_stride, (src_r - src_l), 0);
#endif
                //PRINT_TIME_END("convert10to16_highbit_arm64_neon")
                ALOGD("src_vaddr=%p,dst_vaddr=%p,crop_w=%d,crop_h=%d,stride=%f, src_stride=%d,raw_w=%d,raw_h=%d",
                        src_vaddr, dst_vaddr, src_r - src_l,src_b - src_t,
                        (src_r - src_l)*1.25+64,src_stride,gBuf->getWidth(),gBuf->getHeight());
                //dump data
                static int i =0;
                char pro_value[PROPERTY_VALUE_MAX];

                property_get("sys.dump_out_neon",pro_value,0);
                if(i<10 && !strcmp(pro_value,"true"))
                {
                    char data_name[100];

                    sprintf(data_name,"/data/dump/dmlayer%d_%d_%d.bin", i,
                            yuvTeximg[yuvIndex].yuvTexBuffer->getWidth(),yuvTeximg[yuvIndex].yuvTexBuffer->getHeight());
#if RK_HDR
                    int n = yuvTeximg[yuvIndex].yuvTexBuffer->getHeight() * yuvTeximg[yuvIndex].yuvTexBuffer->getStride();
#else
                    int n = yuvTeximg[yuvIndex].yuvTexBuffer->getHeight() * yuvTeximg[yuvIndex].yuvTexBuffer->getStride() * 1.5;
#endif
                    ALOGD("dump %s size=%d", data_name, n );
                    FILE *fp;
                    if ((fp = fopen(data_name, "w+")) == NULL)
                    {
                        printf("can't open output.bin!!!!!\n");
                    }
                    fwrite(dst_vaddr, n, 1, fp);
                    fclose(fp);
                    i++;
                }

#elif RK_NV12_10_TO_NV12_BY_RGA
                rgaCopyBit(gBuf, yuvTeximg[yuvIndex].yuvTexBuffer, CurrentCrop);
#endif
                bindExternalTextureBuffer(layer.source.buffer.textureName,
                                yuvTeximg[yuvIndex].yuvTexBuffer, layer.source.buffer.fence);

            }
            else
#endif
            {
                bindExternalTextureBuffer(layer.source.buffer.textureName, gBuf, layer.source.buffer.fence);
            }

            usePremultipliedAlpha = layer.source.buffer.usePremultipliedAlpha;
            Texture texture(Texture::TEXTURE_EXTERNAL, layer.source.buffer.textureName);
            mat4 texMatrix = layer.source.buffer.textureTransform;

            texture.setMatrix(texMatrix.asArray());
            texture.setFiltering(layer.source.buffer.useTextureFiltering);

            texture.setDimensions(gBuf->getWidth(), gBuf->getHeight());
            setSourceY410BT2020(layer.source.buffer.isY410BT2020);

            renderengine::Mesh::VertexArray<vec2> texCoords(mesh.getTexCoordArray<vec2>());
            texCoords[0] = vec2(0.0, 0.0);
            texCoords[1] = vec2(0.0, 1.0);
            texCoords[2] = vec2(1.0, 1.0);
            texCoords[3] = vec2(1.0, 0.0);
            setupLayerTexturing(texture);
        }

        const half3 solidColor = layer.source.solidColor;
        const half4 color = half4(solidColor.r, solidColor.g, solidColor.b, layer.alpha);
        // Buffer sources will have a black solid color ignored in the shader,
        // so in that scenario the solid color passed here is arbitrary.
        setupLayerBlending(usePremultipliedAlpha, isOpaque, disableTexture, color,
                           layer.geometry.roundedCornersRadius);
        if (layer.disableBlending) {
            glDisable(GL_BLEND);
        }
        setSourceDataSpace(layer.sourceDataspace);

        // We only want to do a special handling for rounded corners when having rounded corners
        // is the only reason it needs to turn on blending, otherwise, we handle it like the
        // usual way since it needs to turn on blending anyway.
        if (layer.geometry.roundedCornersRadius > 0.0 && color.a >= 1.0f && isOpaque) {
            handleRoundedCorners(display, layer, mesh);
        } else {
            //ALOGE("[zizhan] place 3");
            checkErrors();
            drawMesh(mesh);
            //ALOGE("[zizhan] place 4");
            checkErrors();
        }

        // Cleanup if there's a buffer source
        if (layer.source.buffer.buffer != nullptr) {
            disableBlending();
            setSourceY410BT2020(false);
            disableTexturing();
        }
    }

    if (drawFence != nullptr) {
        //ALOGE("[zizhan] before flush, drawFence is [%d]", drawFence->get());
        *drawFence = flush();
        //ALOGE("[zizhan] after flush, drawFence is [%d]", drawFence->get());
    }
    // If flush failed or we don't support native fences, we need to force the
    // gl command stream to be executed.
    if (drawFence == nullptr || drawFence->get() < 0) {
        //ALOGE("[zizhan] place 5");
        checkErrors();
        bool success = finish();
        //ALOGE("[zizhan] place 6");
        checkErrors();
        if (!success) {
            ALOGE("Failed to flush RenderEngine commands");
            //ALOGE("[zizhan] place 2");
            checkErrors();
            // Chances are, something illegal happened (either the caller passed
            // us bad parameters, or we messed up our shader generation).
            return INVALID_OPERATION;
        }
    }

    //zizhan
    //ALOGE("[zizhan] place 5");
    checkErrors();
    glFinish();
    //ALOGE("[zizhan] place 6");
    checkErrors();

    //ALOGE("[zizhan] place 7");
    checkErrors();
    //ALOGE("[zizhan] draw end--------------------------------------------------------------------------------");
    return NO_ERROR;
}

void GLESRenderEngine::setViewportAndProjection(size_t vpw, size_t vph, Rect sourceCrop,
                                                ui::Transform::orientation_flags rotation) {
    setViewportAndProjection(Rect(vpw, vph), sourceCrop);

    if (rotation == ui::Transform::ROT_0) {
        return;
    }

    // Apply custom rotation to the projection.
    float rot90InRadians = 2.0f * static_cast<float>(M_PI) / 4.0f;
    mat4 m = mState.projectionMatrix;
    switch (rotation) {
        case ui::Transform::ROT_90:
            m = mat4::rotate(rot90InRadians, vec3(0, 0, 1)) * m;
            break;
        case ui::Transform::ROT_180:
            m = mat4::rotate(rot90InRadians * 2.0f, vec3(0, 0, 1)) * m;
            break;
        case ui::Transform::ROT_270:
            m = mat4::rotate(rot90InRadians * 3.0f, vec3(0, 0, 1)) * m;
            break;
        default:
            break;
    }
    mState.projectionMatrix = m;
}

void GLESRenderEngine::setViewportAndProjection(Rect viewport, Rect clip) {
    ATRACE_CALL();
    mVpWidth = viewport.getWidth();
    mVpHeight = viewport.getHeight();

    // We pass the the top left corner instead of the bottom left corner,
    // because since we're rendering off-screen first.
    glViewport(viewport.left, viewport.top, mVpWidth, mVpHeight);

    mState.projectionMatrix = mat4::ortho(clip.left, clip.right, clip.top, clip.bottom, 0, 1);
}

void GLESRenderEngine::setupLayerBlending(bool premultipliedAlpha, bool opaque, bool disableTexture,
                                          const half4& color, float cornerRadius) {
    mState.isPremultipliedAlpha = premultipliedAlpha;
    mState.isOpaque = opaque;
    mState.color = color;
    mState.cornerRadius = cornerRadius;

    if (disableTexture) {
        mState.textureEnabled = false;
    }

    if (color.a < 1.0f || !opaque || cornerRadius > 0.0f) {
        glEnable(GL_BLEND);
        glBlendFunc(premultipliedAlpha ? GL_ONE : GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
}

void GLESRenderEngine::setSourceY410BT2020(bool enable) {
    mState.isY410BT2020 = enable;
}

void GLESRenderEngine::setSourceDataSpace(Dataspace source) {
    mDataSpace = source;
}

void GLESRenderEngine::setOutputDataSpace(Dataspace dataspace) {
    mOutputDataSpace = dataspace;
}

void GLESRenderEngine::setDisplayMaxLuminance(const float maxLuminance) {
    mState.displayMaxLuminance = maxLuminance;
}

void GLESRenderEngine::setupLayerTexturing(const Texture& texture) {
    GLuint target = texture.getTextureTarget();
    glBindTexture(target, texture.getTextureName());
    GLenum filter = GL_NEAREST;
    if (texture.getFiltering()) {
        filter = GL_LINEAR;
    }
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);

    mState.texture = texture;
    mState.textureEnabled = true;
}

void GLESRenderEngine::setupLayerBlackedOut() {
    glBindTexture(GL_TEXTURE_2D, mProtectedTexName);
    Texture texture(Texture::TEXTURE_2D, mProtectedTexName);
    texture.setDimensions(1, 1); // FIXME: we should get that from somewhere
    mState.texture = texture;
    mState.textureEnabled = true;
}

void GLESRenderEngine::setColorTransform(const mat4& colorTransform) {
    mState.colorMatrix = colorTransform;
}

void GLESRenderEngine::disableTexturing() {
    mState.textureEnabled = false;
}

void GLESRenderEngine::disableBlending() {
    glDisable(GL_BLEND);
}

void GLESRenderEngine::setupFillWithColor(float r, float g, float b, float a) {
    mState.isPremultipliedAlpha = true;
    mState.isOpaque = false;
    mState.color = half4(r, g, b, a);
    mState.textureEnabled = false;
    glDisable(GL_BLEND);
}

void GLESRenderEngine::setupCornerRadiusCropSize(float width, float height) {
    mState.cropSize = half2(width, height);
}

void GLESRenderEngine::drawMesh(const Mesh& mesh) {
    ATRACE_CALL();
    if (mesh.getTexCoordsSize()) {
        glEnableVertexAttribArray(Program::texCoords);
        glVertexAttribPointer(Program::texCoords, mesh.getTexCoordsSize(), GL_FLOAT, GL_FALSE,
                              mesh.getByteStride(), mesh.getTexCoords());
    }

    glVertexAttribPointer(Program::position, mesh.getVertexSize(), GL_FLOAT, GL_FALSE,
                          mesh.getByteStride(), mesh.getPositions());

    if (mState.cornerRadius > 0.0f) {
        glEnableVertexAttribArray(Program::cropCoords);
        glVertexAttribPointer(Program::cropCoords, mesh.getVertexSize(), GL_FLOAT, GL_FALSE,
                              mesh.getByteStride(), mesh.getCropCoords());
    }

    // By default, DISPLAY_P3 is the only supported wide color output. However,
    // when HDR content is present, hardware composer may be able to handle
    // BT2020 data space, in that case, the output data space is set to be
    // BT2020_HLG or BT2020_PQ respectively. In GPU fall back we need
    // to respect this and convert non-HDR content to HDR format.
    if (mUseColorManagement) {
        Description managedState = mState;
        Dataspace inputStandard = static_cast<Dataspace>(mDataSpace & Dataspace::STANDARD_MASK);
        Dataspace inputTransfer = static_cast<Dataspace>(mDataSpace & Dataspace::TRANSFER_MASK);
        Dataspace outputStandard =
                static_cast<Dataspace>(mOutputDataSpace & Dataspace::STANDARD_MASK);
        Dataspace outputTransfer =
                static_cast<Dataspace>(mOutputDataSpace & Dataspace::TRANSFER_MASK);
        bool needsXYZConversion = needsXYZTransformMatrix();

        // NOTE: if the input standard of the input dataspace is not STANDARD_DCI_P3 or
        // STANDARD_BT2020, it will be  treated as STANDARD_BT709
        if (inputStandard != Dataspace::STANDARD_DCI_P3 &&
            inputStandard != Dataspace::STANDARD_BT2020) {
            inputStandard = Dataspace::STANDARD_BT709;
        }

        if (needsXYZConversion) {
            // The supported input color spaces are standard RGB, Display P3 and BT2020.
            switch (inputStandard) {
                case Dataspace::STANDARD_DCI_P3:
                    managedState.inputTransformMatrix = mDisplayP3ToXyz;
                    break;
                case Dataspace::STANDARD_BT2020:
                    managedState.inputTransformMatrix = mBt2020ToXyz;
                    break;
                default:
                    managedState.inputTransformMatrix = mSrgbToXyz;
                    break;
            }

            // The supported output color spaces are BT2020, Display P3 and standard RGB.
            switch (outputStandard) {
                case Dataspace::STANDARD_BT2020:
                    managedState.outputTransformMatrix = mXyzToBt2020;
                    break;
                case Dataspace::STANDARD_DCI_P3:
                    managedState.outputTransformMatrix = mXyzToDisplayP3;
                    break;
                default:
                    managedState.outputTransformMatrix = mXyzToSrgb;
                    break;
            }
        } else if (inputStandard != outputStandard) {
            // At this point, the input data space and output data space could be both
            // HDR data spaces, but they match each other, we do nothing in this case.
            // In addition to the case above, the input data space could be
            // - scRGB linear
            // - scRGB non-linear
            // - sRGB
            // - Display P3
            // - BT2020
            // The output data spaces could be
            // - sRGB
            // - Display P3
            // - BT2020
            switch (outputStandard) {
                case Dataspace::STANDARD_BT2020:
                    if (inputStandard == Dataspace::STANDARD_BT709) {
                        managedState.outputTransformMatrix = mSrgbToBt2020;
                    } else if (inputStandard == Dataspace::STANDARD_DCI_P3) {
                        managedState.outputTransformMatrix = mDisplayP3ToBt2020;
                    }
                    break;
                case Dataspace::STANDARD_DCI_P3:
                    if (inputStandard == Dataspace::STANDARD_BT709) {
                        managedState.outputTransformMatrix = mSrgbToDisplayP3;
                    } else if (inputStandard == Dataspace::STANDARD_BT2020) {
                        managedState.outputTransformMatrix = mBt2020ToDisplayP3;
                    }
                    break;
                default:
                    if (inputStandard == Dataspace::STANDARD_DCI_P3) {
                        managedState.outputTransformMatrix = mDisplayP3ToSrgb;
                    } else if (inputStandard == Dataspace::STANDARD_BT2020) {
                        managedState.outputTransformMatrix = mBt2020ToSrgb;
                    }
                    break;
            }
        }

        // we need to convert the RGB value to linear space and convert it back when:
        // - there is a color matrix that is not an identity matrix, or
        // - there is an output transform matrix that is not an identity matrix, or
        // - the input transfer function doesn't match the output transfer function.
        if (managedState.hasColorMatrix() || managedState.hasOutputTransformMatrix() ||
            inputTransfer != outputTransfer) {
            managedState.inputTransferFunction =
                    Description::dataSpaceToTransferFunction(inputTransfer);
            managedState.outputTransferFunction =
                    Description::dataSpaceToTransferFunction(outputTransfer);
        }

        ProgramCache::getInstance().useProgram(mInProtectedContext ? mProtectedEGLContext
                                                                   : mEGLContext,
                                               managedState);

        glDrawArrays(mesh.getPrimitive(), 0, mesh.getVertexCount());
        //ALOGE("[zizhan] glDrawArrays is called, mesh promitive is [%u], mesh vertex count is [%zu]", 
                //mesh.getPrimitive(), mesh.getVertexCount());

        if (outputDebugPPMs) {
            static uint64_t managedColorFrameCount = 0;
            std::ostringstream out;
            out << "/data/texture_out" << managedColorFrameCount++;
            writePPM(out.str().c_str(), mVpWidth, mVpHeight);
        }
    } else {
        ProgramCache::getInstance().useProgram(mInProtectedContext ? mProtectedEGLContext
                                                                   : mEGLContext,
                                               mState);

        glDrawArrays(mesh.getPrimitive(), 0, mesh.getVertexCount());
    }

    if (mesh.getTexCoordsSize()) {
        glDisableVertexAttribArray(Program::texCoords);
    }

    if (mState.cornerRadius > 0.0f) {
        glDisableVertexAttribArray(Program::cropCoords);
    }
}

size_t GLESRenderEngine::getMaxTextureSize() const {
    return mMaxTextureSize;
}

size_t GLESRenderEngine::getMaxViewportDims() const {
    return mMaxViewportDims[0] < mMaxViewportDims[1] ? mMaxViewportDims[0] : mMaxViewportDims[1];
}

void GLESRenderEngine::dump(std::string& result) {
    const GLExtensions& extensions = GLExtensions::getInstance();
    ProgramCache& cache = ProgramCache::getInstance();

    StringAppendF(&result, "EGL implementation : %s\n", extensions.getEGLVersion());
    StringAppendF(&result, "%s\n", extensions.getEGLExtensions());
    StringAppendF(&result, "GLES: %s, %s, %s\n", extensions.getVendor(), extensions.getRenderer(),
                  extensions.getVersion());
    StringAppendF(&result, "%s\n", extensions.getExtensions());
    StringAppendF(&result, "RenderEngine supports protected context: %d\n",
                  supportsProtectedContent());
    StringAppendF(&result, "RenderEngine is in protected context: %d\n", mInProtectedContext);
    StringAppendF(&result, "RenderEngine program cache size for unprotected context: %zu\n",
                  cache.getSize(mEGLContext));
    StringAppendF(&result, "RenderEngine program cache size for protected context: %zu\n",
                  cache.getSize(mProtectedEGLContext));
    StringAppendF(&result, "RenderEngine last dataspace conversion: (%s) to (%s)\n",
                  dataspaceDetails(static_cast<android_dataspace>(mDataSpace)).c_str(),
                  dataspaceDetails(static_cast<android_dataspace>(mOutputDataSpace)).c_str());
    {
        std::lock_guard<std::mutex> lock(mRenderingMutex);
        StringAppendF(&result, "RenderEngine image cache size: %zu\n", mImageCache.size());
        StringAppendF(&result, "Dumping buffer ids...\n");
        for (const auto& [id, unused] : mImageCache) {
            StringAppendF(&result, "0x%" PRIx64 "\n", id);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mFramebufferImageCacheMutex);
        StringAppendF(&result, "RenderEngine framebuffer image cache size: %zu\n",
                      mFramebufferImageCache.size());
        StringAppendF(&result, "Dumping buffer ids...\n");
        for (const auto& [id, unused] : mFramebufferImageCache) {
            StringAppendF(&result, "0x%" PRIx64 "\n", id);
        }
    }
}

GLESRenderEngine::GlesVersion GLESRenderEngine::parseGlesVersion(const char* str) {
    int major, minor;
    if (sscanf(str, "OpenGL ES-CM %d.%d", &major, &minor) != 2) {
        if (sscanf(str, "OpenGL ES %d.%d", &major, &minor) != 2) {
            ALOGW("Unable to parse GL_VERSION string: \"%s\"", str);
            return GLES_VERSION_1_0;
        }
    }

    if (major == 1 && minor == 0) return GLES_VERSION_1_0;
    if (major == 1 && minor >= 1) return GLES_VERSION_1_1;
    if (major == 2 && minor >= 0) return GLES_VERSION_2_0;
    if (major == 3 && minor >= 0) return GLES_VERSION_3_0;

    ALOGW("Unrecognized OpenGL ES version: %d.%d", major, minor);
    return GLES_VERSION_1_0;
}

EGLContext GLESRenderEngine::createEglContext(EGLDisplay display, EGLConfig config,
                                              EGLContext shareContext, bool useContextPriority,
                                              Protection protection) {
    EGLint renderableType = 0;
    if (config == EGL_NO_CONFIG) {
        renderableType = EGL_OPENGL_ES3_BIT;
    } else if (!eglGetConfigAttrib(display, config, EGL_RENDERABLE_TYPE, &renderableType)) {
        LOG_ALWAYS_FATAL("can't query EGLConfig RENDERABLE_TYPE");
    }
    EGLint contextClientVersion = 0;
    if (renderableType & EGL_OPENGL_ES3_BIT) {
        contextClientVersion = 3;
    } else if (renderableType & EGL_OPENGL_ES2_BIT) {
        contextClientVersion = 2;
    } else if (renderableType & EGL_OPENGL_ES_BIT) {
        contextClientVersion = 1;
    } else {
        LOG_ALWAYS_FATAL("no supported EGL_RENDERABLE_TYPEs");
    }

    std::vector<EGLint> contextAttributes;
    contextAttributes.reserve(7);
    contextAttributes.push_back(EGL_CONTEXT_CLIENT_VERSION);
    contextAttributes.push_back(contextClientVersion);
    if (useContextPriority) {
        contextAttributes.push_back(EGL_CONTEXT_PRIORITY_LEVEL_IMG);
        contextAttributes.push_back(EGL_CONTEXT_PRIORITY_HIGH_IMG);
    }
    if (protection == Protection::PROTECTED) {
        contextAttributes.push_back(EGL_PROTECTED_CONTENT_EXT);
        contextAttributes.push_back(EGL_TRUE);
    }
    contextAttributes.push_back(EGL_NONE);

    EGLContext context = eglCreateContext(display, config, shareContext, contextAttributes.data());

    if (contextClientVersion == 3 && context == EGL_NO_CONTEXT) {
        // eglGetConfigAttrib indicated we can create GLES 3 context, but we failed, thus
        // EGL_NO_CONTEXT so that we can abort.
        if (config != EGL_NO_CONFIG) {
            return context;
        }
        // If |config| is EGL_NO_CONFIG, we speculatively try to create GLES 3 context, so we should
        // try to fall back to GLES 2.
        contextAttributes[1] = 2;
        context = eglCreateContext(display, config, shareContext, contextAttributes.data());
    }

    return context;
}

EGLSurface GLESRenderEngine::createDummyEglPbufferSurface(EGLDisplay display, EGLConfig config,
                                                          int hwcFormat, Protection protection) {
    EGLConfig dummyConfig = config;
    if (dummyConfig == EGL_NO_CONFIG) {
        dummyConfig = chooseEglConfig(display, hwcFormat, /*logConfig*/ true);
    }
    std::vector<EGLint> attributes;
    attributes.reserve(7);
    attributes.push_back(EGL_WIDTH);
    attributes.push_back(1);
    attributes.push_back(EGL_HEIGHT);
    attributes.push_back(1);
    if (protection == Protection::PROTECTED) {
        attributes.push_back(EGL_PROTECTED_CONTENT_EXT);
        attributes.push_back(EGL_TRUE);
    }
    attributes.push_back(EGL_NONE);

    return eglCreatePbufferSurface(display, dummyConfig, attributes.data());
}

bool GLESRenderEngine::isHdrDataSpace(const Dataspace dataSpace) const {
    const Dataspace standard = static_cast<Dataspace>(dataSpace & Dataspace::STANDARD_MASK);
    const Dataspace transfer = static_cast<Dataspace>(dataSpace & Dataspace::TRANSFER_MASK);
    return standard == Dataspace::STANDARD_BT2020 &&
            (transfer == Dataspace::TRANSFER_ST2084 || transfer == Dataspace::TRANSFER_HLG);
}

// For convenience, we want to convert the input color space to XYZ color space first,
// and then convert from XYZ color space to output color space when
// - SDR and HDR contents are mixed, either SDR content will be converted to HDR or
//   HDR content will be tone-mapped to SDR; Or,
// - there are HDR PQ and HLG contents presented at the same time, where we want to convert
//   HLG content to PQ content.
// In either case above, we need to operate the Y value in XYZ color space. Thus, when either
// input data space or output data space is HDR data space, and the input transfer function
// doesn't match the output transfer function, we would enable an intermediate transfrom to
// XYZ color space.
bool GLESRenderEngine::needsXYZTransformMatrix() const {
    const bool isInputHdrDataSpace = isHdrDataSpace(mDataSpace);
    const bool isOutputHdrDataSpace = isHdrDataSpace(mOutputDataSpace);
    const Dataspace inputTransfer = static_cast<Dataspace>(mDataSpace & Dataspace::TRANSFER_MASK);
    const Dataspace outputTransfer =
            static_cast<Dataspace>(mOutputDataSpace & Dataspace::TRANSFER_MASK);

    return (isInputHdrDataSpace || isOutputHdrDataSpace) && inputTransfer != outputTransfer;
}

bool GLESRenderEngine::isImageCachedForTesting(uint64_t bufferId) {
    std::lock_guard<std::mutex> lock(mRenderingMutex);
    const auto& cachedImage = mImageCache.find(bufferId);
    return cachedImage != mImageCache.end();
}

bool GLESRenderEngine::isFramebufferImageCachedForTesting(uint64_t bufferId) {
    std::lock_guard<std::mutex> lock(mFramebufferImageCacheMutex);
    return std::any_of(mFramebufferImageCache.cbegin(), mFramebufferImageCache.cend(),
                       [=](std::pair<uint64_t, EGLImageKHR> image) {
                           return image.first == bufferId;
                       });
}

// FlushTracer implementation
GLESRenderEngine::FlushTracer::FlushTracer(GLESRenderEngine* engine) : mEngine(engine) {
    mThread = std::thread(&GLESRenderEngine::FlushTracer::loop, this);
}

GLESRenderEngine::FlushTracer::~FlushTracer() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mRunning = false;
    }
    mCondition.notify_all();
    if (mThread.joinable()) {
        mThread.join();
    }
}

void GLESRenderEngine::FlushTracer::queueSync(EGLSyncKHR sync) {
    std::lock_guard<std::mutex> lock(mMutex);
    char name[64];
    const uint64_t frameNum = mFramesQueued++;
    snprintf(name, sizeof(name), "Queueing sync for frame: %lu",
             static_cast<unsigned long>(frameNum));
    ATRACE_NAME(name);
    mQueue.push({sync, frameNum});
    ATRACE_INT("GPU Frames Outstanding", mQueue.size());
    mCondition.notify_one();
}

void GLESRenderEngine::FlushTracer::loop() {
    while (mRunning) {
        QueueEntry entry;
        {
            std::lock_guard<std::mutex> lock(mMutex);

            mCondition.wait(mMutex,
                            [&]() REQUIRES(mMutex) { return !mQueue.empty() || !mRunning; });

            if (!mRunning) {
                // if mRunning is false, then FlushTracer is being destroyed, so
                // bail out now.
                break;
            }
            entry = mQueue.front();
            mQueue.pop();
        }
        {
            char name[64];
            snprintf(name, sizeof(name), "waiting for frame %lu",
                     static_cast<unsigned long>(entry.mFrameNum));
            ATRACE_NAME(name);
            mEngine->waitSync(entry.mSync, 0);
        }
    }
}

} // namespace gl
} // namespace renderengine
} // namespace android
