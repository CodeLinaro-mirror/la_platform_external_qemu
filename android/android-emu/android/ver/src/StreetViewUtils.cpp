/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "StreetViewUtils.h"

#include <setjmp.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

extern "C" {
typedef size_t (*CurlWriteCallback)(char* ptr,
                                    size_t size,
                                    size_t nmemb,
                                    void* userdata);

bool curl_download(const char* url,
                   const char* post_fields,
                   CurlWriteCallback callback_func,
                   void* callback_userdata,
                   char** error);

#include <jpeglib.h>
}

#include "TextureUtils.h"
#include "aemu/base/StringFormat.h"
#include "aemu/base/files/PathUtils.h"
#include "aemu/base/files/ScopedStdioFile.h"
#include "aemu/base/logging/Log.h"
#include "android/base/system/System.h"
#include "android/utils/file_io.h"

using android::base::PathUtils;
using android::base::ScopedStdioFile;
using android::base::StringFormat;
using android::base::System;

namespace android {
namespace ver {

namespace {

size_t curlWriteCallback(char* contents,
                         size_t size,
                         size_t nmemb,
                         void* userp) {
    size_t total = size * nmemb;
    auto& res = *static_cast<std::string*>(userp);
    res.append(contents, contents + total);
    return total;
}

std::string redactApiKey(const std::string& url) {
    std::string result = url;
    size_t pos = 0;
    while ((pos = result.find("key=", pos)) != std::string::npos) {
        size_t start = pos + 4;
        size_t end = result.find('&', start);
        if (end == std::string::npos) {
            result.replace(start, std::string::npos, "<redacted>");
            break;
        } else {
            result.replace(start, end - start, "<redacted>");
            pos = start + 10;
        }
    }
    return result;
}

std::string downloadUrl(const std::string& requestUrl,
                        const std::string& postFields = "") {
    char* curlError = nullptr;
    std::string res;
    const char* postData = postFields.empty() ? nullptr : postFields.c_str();
    bool curlOk = curl_download(requestUrl.c_str(), postData, curlWriteCallback, &res,
                      &curlError);
    if (curlError) {
        if (!curlOk && strcmp(curlError, "No error") != 0) {
            dprint("StreetView: downloadUrl failed for %s: %s",
                   redactApiKey(requestUrl).c_str(), curlError);
        }
        free(curlError);
    }
    return curlOk ? res : "";
}

std::string parseJsonField(const std::string& jsonStr,
                           const std::string& fieldName) {
    std::string pattern = "\"" + fieldName + "\"";
    size_t pos = jsonStr.find(pattern);
    if (pos == std::string::npos)
        return "";
    pos += pattern.length();
    pos = jsonStr.find(':', pos);
    if (pos == std::string::npos)
        return "";
    pos = jsonStr.find('"', pos + 1);
    if (pos == std::string::npos)
        return "";
    size_t endPos = jsonStr.find('"', pos + 1);
    if (endPos == std::string::npos)
        return "";
    return jsonStr.substr(pos + 1, endPos - pos - 1);
}

std::string fetchPanoId(double latitude,
                        double longitude,
                        const std::string& mapsKey,
                        std::string* outSessionToken) {
    std::string sessionToken;
    if (!mapsKey.empty()) {
        std::string createSessionUrl =
                "https://tile.googleapis.com/v1/createSession?key=" + mapsKey;
        std::string postBody =
                "{\"mapType\": \"streetview\", \"language\": \"en-US\"}";
        std::string sessionResp = downloadUrl(createSessionUrl, postBody);
        sessionToken = parseJsonField(sessionResp, "session");
        if (!sessionToken.empty()) {
            dprint("StreetView: obtained Map Tiles API session token");
        }
    }
    if (outSessionToken) {
        *outSessionToken = sessionToken;
    }

    std::string panoId;
    if (!sessionToken.empty()) {
        std::string metaUrl = StringFormat(
                "https://tile.googleapis.com/v1/streetview/"
                "metadata?session=%s&key=%s&lat=%.6f&lng=%.6f",
                sessionToken, mapsKey, latitude, longitude);
        std::string metaResp = downloadUrl(metaUrl);
        panoId = parseJsonField(metaResp, "panoId");
        if (panoId.empty()) {
            panoId = parseJsonField(metaResp, "pano_id");
        }
    }

    if (panoId.empty()) {
        std::string metaUrl;
        if (!mapsKey.empty()) {
            metaUrl = StringFormat(
                    "https://maps.googleapis.com/maps/api/streetview/"
                    "metadata?location=%.6f,%.6f&key=%s",
                    latitude, longitude, mapsKey);
        } else {
            metaUrl = StringFormat(
                    "https://maps.googleapis.com/maps/api/streetview/"
                    "metadata?location=%.6f,%.6f",
                    latitude, longitude);
        }
        std::string metaResp = downloadUrl(metaUrl);
        panoId = parseJsonField(metaResp, "pano_id");
        if (panoId.empty()) {
            panoId = parseJsonField(metaResp, "panoId");
        }
    }
    return panoId;
}

std::string downloadTile(int zoom,
                         int x,
                         int y,
                         const std::string& panoId,
                         const std::string& sessionToken,
                         const std::string& mapsKey) {
    std::string tileData;
    if (!sessionToken.empty() && !mapsKey.empty()) {
        char tileUrl[1024];
        snprintf(
                tileUrl, sizeof(tileUrl),
                "https://tile.googleapis.com/v1/streetview/tiles/%d/%d/%d?session=%s&key=%s&panoId=%s",
                zoom, x, y, sessionToken.c_str(), mapsKey.c_str(),
                panoId.c_str());
        tileData = downloadUrl(tileUrl);
    }
    if (tileData.empty() && !mapsKey.empty()) {
        char tileUrl[1024];
        snprintf(tileUrl, sizeof(tileUrl),
                 "https://tile.googleapis.com/v1/streetview/"
                 "tiles/%d/%d/%d?key=%s&panoId=%s",
                 zoom, x, y, mapsKey.c_str(), panoId.c_str());
        tileData = downloadUrl(tileUrl);
    }
    if (tileData.empty()) {
        char tileUrl[1024];
        snprintf(
                tileUrl, sizeof(tileUrl),
                "https://streetviewpixels-pa.googleapis.com/v1/tile?cb_client=maps_sv.tactile&panoid=%s&x=%d&y=%d&zoom=%d",
                panoId.c_str(), x, y, zoom);
        tileData = downloadUrl(tileUrl);
    }
    if (tileData.empty()) {
        char tileUrl[1024];
        snprintf(
                tileUrl, sizeof(tileUrl),
                "https://cbk0.google.com/cbk?output=tile&panoid=%s&zoom=%d&x=%d&y=%d",
                panoId.c_str(), zoom, x, y);
        tileData = downloadUrl(tileUrl);
    }
    return tileData;
}

std::vector<uint8_t> stitchTilesToRGBA(
        const std::vector<std::vector<android::ver::TextureUtils::Result>>&
                tiles,
        uint32_t numTilesX,
        uint32_t numTilesY,
        uint32_t* outTotalW,
        uint32_t* outTotalH) {
    uint32_t tileW = tiles[0][0].mWidth;
    uint32_t tileH = tiles[0][0].mHeight;
    uint32_t totalW = tileW * numTilesX;
    uint32_t totalH = tileH * numTilesY;

    if (outTotalW)
        *outTotalW = totalW;
    if (outTotalH)
        *outTotalH = totalH;

    std::vector<uint8_t> stitchedBuffer(totalW * totalH * 4, 255);

    for (uint32_t y = 0; y < numTilesY; ++y) {
        for (uint32_t x = 0; x < numTilesX; ++x) {
            const auto& tile = tiles[y][x];
            uint32_t srcW = tile.mWidth;
            uint32_t srcH = tile.mHeight;
            const uint8_t* srcData = tile.mBuffer.data();
            size_t srcStride = (srcW * 3 + 3) / 4 * 4;

            for (uint32_t ty = 0; ty < srcH && (y * tileH + ty) < totalH;
                 ++ty) {
                uint32_t destY = totalH - 1 - (y * tileH + ty);
                uint32_t destX = x * tileW;
                uint8_t* destRow =
                        &stitchedBuffer[(destY * totalW + destX) * 4];
                const uint8_t* srcRow = srcData + ty * srcStride;
                for (uint32_t tx = 0; tx < std::min(srcW, tileW); ++tx) {
                    destRow[tx * 4 + 0] = srcRow[tx * 3 + 0];
                    destRow[tx * 4 + 1] = srcRow[tx * 3 + 1];
                    destRow[tx * 4 + 2] = srcRow[tx * 3 + 2];
                    destRow[tx * 4 + 3] = 255;
                }
            }
        }
    }
    return stitchedBuffer;
}

struct ErrorManager {
    struct jpeg_error_mgr pub;  // Public fields.
    jmp_buf setjmp_buffer;
};

bool saveRGBAToJPEG(const char* filename,
                    const uint8_t* rgbaData,
                    int width,
                    int height,
                    int quality = 90) {
    ScopedStdioFile fp(android_fopen(filename, "wb"));
    if (!fp) {
        return false;
    }

    struct jpeg_compress_struct cinfo;
    ErrorManager jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = [](j_common_ptr cinfoPtr) {
        ErrorManager* err = reinterpret_cast<ErrorManager*>(cinfoPtr->err);
        longjmp(err->setjmp_buffer, 1);
    };

    std::vector<uint8_t> rowRgb(width * 3);
    if (setjmp(jerr.setjmp_buffer)) {
        derror("StreetView: JPEG compression error");
        jpeg_destroy_compress(&cinfo);
        return false;
    }

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp.get());

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = rgbaData + (height - 1 - y) * width * 4;
        for (int x = 0; x < width; ++x) {
            rowRgb[x * 3 + 0] = srcRow[x * 4 + 0];
            rowRgb[x * 3 + 1] = srcRow[x * 4 + 1];
            rowRgb[x * 3 + 2] = srcRow[x * 4 + 2];
        }
        JSAMPROW row_pointer[1] = {rowRgb.data()};
        (void)jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return true;
}

}  // namespace

std::optional<TextureUtils::Result> StreetViewUtils::fetch360Panorama(
        double latitude,
        double longitude,
        const std::string& mapsKey) {
    std::string sessionToken;
    std::string panoId =
            fetchPanoId(latitude, longitude, mapsKey, &sessionToken);
    if (panoId.empty()) {
        return std::nullopt;
    }

    dprint("StreetView: using pano_id: %s to download tiles via Map Tiles API",
           panoId.c_str());
    int zoom = 2;
    int numTilesX = 4;
    int numTilesY = 2;

    std::vector<std::vector<android::ver::TextureUtils::Result>> tiles(
            numTilesY);
    std::string tempDir = System::get()->getTempDir();

    const std::string saveImagesVar =
            System::get()->envGet("ANDROID_EMU_SAVE_STREETVIEW_IMAGES");
    const bool saveImages = (saveImagesVar == "1");

    for (int y = 0; y < numTilesY; ++y) {
        tiles[y].resize(numTilesX);
        for (int x = 0; x < numTilesX; ++x) {
            std::string tileData =
                    downloadTile(zoom, x, y, panoId, sessionToken, mapsKey);
            if (tileData.empty()) {
                derror("StreetView: failed to download tile (%d, %d)", x, y);
                return std::nullopt;
            }

            if (saveImages) {
                std::string tempTilePath = PathUtils::join(
                        tempDir, "sv_tile_" + std::to_string(x) + "_" +
                                         std::to_string(y) + ".jpg");
                std::ofstream tileFile(
                        PathUtils::asUnicodePath(tempTilePath.c_str()).c_str(),
                        std::ios_base::binary | std::ios_base::trunc);
                if (tileFile.is_open()) {
                    tileFile.write(tileData.data(), tileData.size());
                    tileFile.close();
                }
            }

            auto tileRes = android::ver::TextureUtils::loadJPEGFromMemory(
                    reinterpret_cast<const uint8_t*>(tileData.data()),
                    tileData.size(),
                    android::ver::TextureUtils::Orientation::TopDown);
            if (tileRes) {
                tiles[y][x] = std::move(*tileRes);
            } else {
                derror("StreetView: failed to decode tile (%d, %d)", x, y);
                return std::nullopt;
            }
        }
    }

    uint32_t totalW = 0;
    uint32_t totalH = 0;
    std::vector<uint8_t> stitched =
            stitchTilesToRGBA(tiles, numTilesX, numTilesY, &totalW, &totalH);

    if (saveImages) {
        std::string filename = PathUtils::join(tempDir, "streetview.jpg");
        if (saveRGBAToJPEG(filename.c_str(), stitched.data(), totalW, totalH,
                           90)) {
            dprint("StreetView: saved stitched 360 equirectangular image (%dx%d) to %s",
                   totalW, totalH, filename.c_str());
        } else {
            derror("StreetView: failed to save stitched 360 image to %s",
                   filename.c_str());
        }
    }

    TextureUtils::Result result;
    result.mBuffer = std::move(stitched);
    result.mWidth = totalW;
    result.mHeight = totalH;
    result.mFormat = TextureUtils::Format::RGBA32;
    return result;
}

std::optional<TextureUtils::Result> StreetViewUtils::downloadStaticImage(
        double latitude,
        double longitude,
        const std::string& mapsKey) {
    char url[1024];
    if (!mapsKey.empty()) {
        snprintf(url, sizeof(url),
                 "https://maps.googleapis.com/maps/api/"
                 "streetview?size=1024x512&location=%.6f,%.6f&fov=120&key=%s",
                 latitude, longitude, mapsKey.c_str());
    } else {
        snprintf(url, sizeof(url),
                 "https://maps.googleapis.com/maps/api/"
                 "streetview?size=1024x512&location=%.6f,%.6f&fov=120",
                 latitude, longitude);
    }
    std::string res = downloadUrl(url);
    if (!res.empty()) {
        const std::string saveImagesVar =
                System::get()->envGet("ANDROID_EMU_SAVE_STREETVIEW_IMAGES");
        if (saveImagesVar == "1") {
            std::string filename = PathUtils::join(System::get()->getTempDir(),
                                                   "streetview.jpg");
            std::ofstream outFile(
                    PathUtils::asUnicodePath(filename.c_str()).c_str(),
                    std::ios_base::binary | std::ios_base::trunc);
            if (outFile.is_open()) {
                outFile.write(res.data(), res.size());
                outFile.close();
            }
        }

        auto result = TextureUtils::loadJPEGFromMemory(
                reinterpret_cast<const uint8_t*>(res.data()), res.size(),
                TextureUtils::Orientation::OpenGL);
        if (result && result->mFormat == TextureUtils::Format::RGB24) {
            std::vector<uint8_t> rgba(result->mWidth * result->mHeight * 4);
            size_t srcStride = (result->mWidth * 3 + 3) / 4 * 4;
            const uint8_t* src = result->mBuffer.data();
            for (uint32_t y = 0; y < result->mHeight; ++y) {
                const uint8_t* srcRow = src + y * srcStride;
                uint8_t* dstRow = rgba.data() + y * result->mWidth * 4;
                for (uint32_t x = 0; x < result->mWidth; ++x) {
                    dstRow[x * 4 + 0] = srcRow[x * 3 + 0];
                    dstRow[x * 4 + 1] = srcRow[x * 3 + 1];
                    dstRow[x * 4 + 2] = srcRow[x * 3 + 2];
                    dstRow[x * 4 + 3] = 255;
                }
            }
            result->mBuffer = std::move(rgba);
            result->mFormat = TextureUtils::Format::RGBA32;
        }
        return result;
    }
    return std::nullopt;
}

}  // namespace ver
}  // namespace android
