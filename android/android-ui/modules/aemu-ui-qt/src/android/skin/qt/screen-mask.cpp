/* Copyright (C) 2018 The Android Open Source Project
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

#include "android/skin/qt/screen-mask.h"

#include <stddef.h>                                  // for NULL
#include <QImage>                                    // for QImage
#include <QImageReader>                              // for QImageReader
#include <QString>                                   // for QString
#include <string>                                    // for basic_string
                                                     //
#include "android/avd/info.h"                        // for avdInfo_getSkinInfo
#include "android/utils/system.h"
#include "aemu/base/files/PathUtils.h"            // for PathUtils
#include "aemu/base/memory/LazyInstance.h"        // for LazyInstance
#include "android/emulator-window.h"                 // for emulator_window_...
#include "android/console.h"                         // for getConsoleAgents()->settings->avdInfo()
#include "android/utils/aconfig-file.h"              // for aconfig_str, aco...

using android::base::LazyInstance;
using android::base::PathUtils;

struct ScreenMaskGlobals {
    QImage screenMaskImage;
};

static LazyInstance<ScreenMaskGlobals> sGlobals = LAZY_INSTANCE_INIT;

namespace ScreenMask {

// Force the rgb as '0' if alpha is '0', ensure RGBA layout
static void processImage(QImage& image, bool setAlphaToBlack) {
    // Convert the image to RGBA8888, which guarantees RGBA order needed
    image = image.convertToFormat(QImage::Format_RGBA8888);

    if (setAlphaToBlack) {
        for (int row = 0; row < image.height() - 1; row++) {
            for (int col = 0; col < image.width() - 1; col++) {
                QRgb p = image.pixel(col, row);
                if (qAlpha(p) == 0) {
                    image.setPixel(col, row, qRgba(0, 0, 0, 0));
                }
            }
        }
    }
}

// Load the image of the mask and set it for use on the
// AVD's display
static void loadMaskImage(AConfig* config, char* skinDir, char* skinName) {
    // Get the mask itself. The layout has the file name as
    // parts/portrait/foreground/mask.

    const char* maskFilename = aconfig_str(config, "mask", 0);
    if (!maskFilename || maskFilename[0] == '\0') {
        return;
    }

    QString maskPath =
            PathUtils::join(skinDir ? skinDir : "", skinName ? skinName : "", maskFilename).c_str();

    // Read and decode this file
    QImageReader imageReader(maskPath);
    sGlobals->screenMaskImage = imageReader.read();
    if (sGlobals->screenMaskImage.isNull()) {
        return;
    }
    processImage(sGlobals->screenMaskImage, true);
    emulator_window_set_screen_mask(sGlobals->screenMaskImage.width(),
                                    sGlobals->screenMaskImage.height(),
                                    sGlobals->screenMaskImage.bits());
}

AConfig* getPartsConfig(AConfig* rootConfig) {
    return aconfig_find(rootConfig, "parts");
}

AConfig* getPortraitConfig(AConfig* rootConfig) {
    AConfig* nextConfig = getPartsConfig(rootConfig);
    if (nextConfig == nullptr) {
        return nullptr;
    }
    return aconfig_find(nextConfig, "portrait");
}

AConfig* getForegroundConfig(AConfig* rootConfig) {
    // Look for parts/portrait/foreground
    AConfig* portraitConfig = getPortraitConfig(rootConfig);
    if (portraitConfig == nullptr) {
        return nullptr;
    }
    return aconfig_find(portraitConfig, "foreground");
}

// Handle the screen mask. This includes the mask image itself
// and any associated cutout and padding offset.
void loadMask() {
    char* skinName;
    char* skinDir;

    avdInfo_getSkinInfo(getConsoleAgents()->settings->avdInfo(), &skinName, &skinDir);
    QString layoutPath =
            PathUtils::join(skinDir ? skinDir : "", skinName ? skinName : "", "layout").c_str();

    AConfig* rootConfig = aconfig_node("", "");
    aconfig_load_file(rootConfig, layoutPath.toStdString().c_str());

    AConfig* foregroundConfig = getForegroundConfig(rootConfig);
    if (foregroundConfig != nullptr) {
        loadMaskImage(foregroundConfig, skinDir, skinName);
    }

    AFREE(skinName);
    AFREE(skinDir);
}

const QImage& getMaskImage() {
    return sGlobals->screenMaskImage;
}

} // namespace ScreenMask
