/* Copyright (C) 2026 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

#include "android/opengles-overrides.h"
#include "OpenGLESDispatch/OpenGLDispatchLoader.h"
#include "render-utils/virtio_gpu_ops.h"
#include "aemu/base/Log.h"

#include <string.h>
#include <stdlib.h>

using namespace gfxstream::host::gl;

static int prepareOpenglesEmulation(void) { return 0; }
static int setOpenglesEmulation(void* rl, void* e, void* g) { return 0; }
static int initOpenglesEmulation(void) { return 0; }
static int startOpenglesRenderer(int width, int height,
                                 bool isPhone, int guestApiLevel,
                                 const QAndroidVmOperations *vm_operations,
                                 const QAndroidEmulatorWindowAgent *window_agent,
                                 const QAndroidMultiDisplayAgent *multi_display_agent,
                                 const void* gfxstreamFeatures,
                                 int* glesMajorVersion_out,
                                 int* glesMinorVersion_out) {
    if (glesMajorVersion_out) *glesMajorVersion_out = 2;
    if (glesMinorVersion_out) *glesMinorVersion_out = 0;
    return 0;
}
static bool asyncReadbackSupported() { return false; }
static void setPostCallback(OnPostFunc onPost, void* context, bool bgra, uint32_t id) {}
static ReadPixelsFunc getReadPixelsFunc() { return nullptr; }
static FlushReadPixelPipeline getFlushReadPixelPipeline() { return nullptr; }
static void getOpenglesHardwareStrings(char** v, char** r, char** ver) {
    *v = strdup("Fishtank");
    *r = strdup("Fishtank Minimal Renderer");
    *ver = strdup("OpenGL ES 2.0");
}
static void getOpenglesVersion(int* maj, int* min) { *maj = 2; *min = 0; }
static int showOpenglesWindow(void* window, int wx, int wy, int ww, int wh, int fbw, int fbh, float dpr, float rotation, bool deleteExisting, bool hideWindow) { return 0; }
static int hideOpenglesWindow(void) { return 0; }
static void setOpenglesTranslation(float px, float py) {}
static void setOpenglesScreenMask(int width, int height, const uint8_t* rgbaData) {}
static void setOpenglesScreenBackground(int width, int height, const uint8_t* rgbaData) {}
static void setOpenglesDisplayLayout(int sw, int sh, int dpx, int dpy, int dw, int dh) {}
static void redrawOpenglesWindow(void) {}
static void setShouldSkipDraw(bool skip) {}
static bool getShouldSkipDraw(void) { return false; }
static bool hasGuestPostedAFrame(void) { return false; }
static void resetGuestPostedAFrame(void) {}
static void registerScreenshotFunc(ScreenshotFunc f) {}
static bool screenShot(const char* dirname, uint32_t displayId) { return false; }
static void stopOpenglesRenderer(bool wait) {}
static void finishOpenglesRenderer(void) {}
static void onGuestGraphicsProcessCreate(uint64_t puid) {}
static void cleanupProcGLObjects(uint64_t puid) {}
static void waitForOpenglesProcessCleanup(void) {}
static struct AndroidVirtioGpuOps* getVirtioGpuOps(void) { return nullptr; }
static const void* getEGLDispatch(void) { return (const void*)LazyLoadedEGLDispatch::get(); }
static const void* getGLESv2Dispatch(void) { return (const void*)LazyLoadedGLESv2Dispatch::get(); }
static void setVsyncHz(int vsyncHz) {}
static void setOpenglesDisplayConfigs(int configId, int w, int h, int dpiX, int dpiY) {}
static void setOpenglesDisplayActiveConfig(int configId) {}

/**
 * @struct sFishtankFuncs
 * @brief OpenGLES overrides for the fishtank backend.
 *
 * This table provides minimal or no-op implementations for OpenGLES
 * operations that are not used by the fishtank UI, but are required by
 * the common emulator glue code.
 */
static struct AndroidOpenglesFuncs sFishtankFuncs = {
    .prepareOpenglesEmulation = prepareOpenglesEmulation,
    .setOpenglesEmulation = setOpenglesEmulation,
    .initOpenglesEmulation = initOpenglesEmulation,
    .startOpenglesRenderer = startOpenglesRenderer,
    .asyncReadbackSupported = asyncReadbackSupported,
    .setPostCallback = setPostCallback,
    .getReadPixelsFunc = getReadPixelsFunc,
    .getFlushReadPixelPipeline = getFlushReadPixelPipeline,
    .getOpenglesHardwareStrings = getOpenglesHardwareStrings,
    .getOpenglesVersion = getOpenglesVersion,
    .showOpenglesWindow = showOpenglesWindow,
    .hideOpenglesWindow = hideOpenglesWindow,
    .setOpenglesTranslation = setOpenglesTranslation,
    .setOpenglesScreenMask = setOpenglesScreenMask,
    .setOpenglesScreenBackground = setOpenglesScreenBackground,
    .setOpenglesDisplayLayout = setOpenglesDisplayLayout,
    .redrawOpenglesWindow = redrawOpenglesWindow,
    .setShouldSkipDraw = setShouldSkipDraw,
    .getShouldSkipDraw = getShouldSkipDraw,
    .hasGuestPostedAFrame = hasGuestPostedAFrame,
    .resetGuestPostedAFrame = resetGuestPostedAFrame,
    .registerScreenshotFunc = registerScreenshotFunc,
    .screenShot = screenShot,
    .stopOpenglesRenderer = stopOpenglesRenderer,
    .finishOpenglesRenderer = finishOpenglesRenderer,
    .onGuestGraphicsProcessCreate = onGuestGraphicsProcessCreate,
    .cleanupProcGLObjects = cleanupProcGLObjects,
    .waitForOpenglesProcessCleanup = waitForOpenglesProcessCleanup,
    .getVirtioGpuOps = getVirtioGpuOps,
    .getEGLDispatch = getEGLDispatch,
    .getGLESv2Dispatch = getGLESv2Dispatch,
    .setVsyncHz = setVsyncHz,
    .setOpenglesDisplayConfigs = setOpenglesDisplayConfigs,
    .setOpenglesDisplayActiveConfig = setOpenglesDisplayActiveConfig,
};

extern "C" {
void injectFishtankOpenglesFuncs() {
    android_setOpenglesFuncs(&sFishtankFuncs);
}
}
