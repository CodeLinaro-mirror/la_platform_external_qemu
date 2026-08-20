// Copyright 2014 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/avd/util.h"
#include "android/base/testing/TestSystem.h"
#include "android/utils/file_data.h"
#include "aemu/base/ArraySize.h"
#include "aemu/base/files/PathUtils.h"
#include "aemu/base/memory/ScopedPtr.h"
#include "android/base/testing/TestSystem.h"

#include <iostream>
#include <fstream>
#include <memory>
#include <gtest/gtest.h>
#include "android/base/testing/TestTempDir.h"

using android::base::pj;
using android::base::ScopedCPtr;
using android::base::TestSystem;
using android::base::TestTempDir;

static void writeToFile(std::string path, std::string text) {
    std::ofstream iniFile(path, std::ios::trunc);
    iniFile << text;
    iniFile.close();
}

TEST(AvdUtil, path_getAvdSystemPath) {
    TestSystem sys("/home", 64, "/");
    TestTempDir* tmp = sys.getTempRoot();
    tmp->makeSubDir("android_home");
    tmp->makeSubDir(pj("android_home", "sysimg"));
    tmp->makeSubDir(pj("android_home", "avd"));
    tmp->makeSubDir("nothome");

    std::string sdkRoot =
        pj(tmp->pathString(), "android_home");
    std::string avdConfig =
        pj({sdkRoot, "avd", "config.ini"});
    sys.envSet("ANDROID_AVD_HOME", sdkRoot);

    // Create an in file for the @q avd.
    writeToFile(pj(sdkRoot, "q.ini"),
                "path=" +
                    pj(sdkRoot, "avd"));

    // A relative path should be resolved from ANRDOID_AVD_HOME
    writeToFile(avdConfig, "image.sysdir.1=sysimg");

    ScopedCPtr<char> path(path_getAvdSystemPath("q", sdkRoot.c_str(), false));

    auto sysimgPath = pj(sdkRoot, "sysimg");
    EXPECT_STREQ(sysimgPath.c_str(), path.get());

    // An absolute path should be usuable as well
    writeToFile(avdConfig,
                "image.sysdir.1=" +
                pj(
                    tmp->pathString(),
                    "nothome"));

    path.reset(path_getAvdSystemPath("q", sdkRoot.c_str(), false));

    auto notHomePath = pj(tmp->pathString(), "nothome");
    EXPECT_STREQ(notHomePath.c_str(), path.get());

    std::string noBufferOverflow(MAX_PATH * 2, 'Z');
    writeToFile(avdConfig, "image.sysdir.1=" + noBufferOverflow);

    path.reset(path_getAvdSystemPath("q", sdkRoot.c_str(), false));

    EXPECT_EQ(nullptr, path.get());
}

TEST(AvdUtil, emulator_getBackendSuffix) {
  EXPECT_STREQ("arm", emulator_getBackendSuffix("arm"));
  EXPECT_STREQ("x86", emulator_getBackendSuffix("x86"));
  EXPECT_STREQ("x86", emulator_getBackendSuffix("x86_64"));
  EXPECT_STREQ("mips", emulator_getBackendSuffix("mips"));
  EXPECT_STREQ("arm64", emulator_getBackendSuffix("arm64"));
  EXPECT_STREQ("mips64", emulator_getBackendSuffix("mips64"));

  EXPECT_FALSE(emulator_getBackendSuffix(NULL));
  EXPECT_FALSE(emulator_getBackendSuffix("dummy"));
}

TEST(AvdUtil, propertyFile_getInt) {
  FileData fd;

  const char* testFile =
    "nineteen=19\n"
    "int_min=-2147483648\n"
    "int_max=2147483647\n"
    "invalid=2147483648\n"
    "invalid2=-2147483649\n"
    "invalid3=bar\n"
    "empty=\n";

  EXPECT_EQ(0,fileData_initFromMemory(&fd, testFile, strlen(testFile)));

  const int kDefault = 1138;
  SearchResult kSearchResultGarbage = (SearchResult)0xdeadbeef;
  SearchResult searchResult = kSearchResultGarbage;

  EXPECT_EQ(kDefault,propertyFile_getInt(&fd, "invalid", kDefault, &searchResult));
  EXPECT_EQ(RESULT_INVALID,searchResult);

  searchResult = kSearchResultGarbage;
  EXPECT_EQ(kDefault,propertyFile_getInt(&fd, "invalid2", kDefault, &searchResult));
  EXPECT_EQ(RESULT_INVALID,searchResult);

  searchResult = kSearchResultGarbage;
  EXPECT_EQ(kDefault,propertyFile_getInt(&fd, "invalid3", kDefault, &searchResult));
  EXPECT_EQ(RESULT_INVALID,searchResult);

  searchResult = kSearchResultGarbage;
  EXPECT_EQ(kDefault,propertyFile_getInt(&fd, "bar", kDefault, &searchResult));
  EXPECT_EQ(RESULT_NOT_FOUND,searchResult);

  searchResult = kSearchResultGarbage;
  EXPECT_EQ(kDefault,propertyFile_getInt(&fd, "empty", kDefault, &searchResult));
  EXPECT_EQ(RESULT_INVALID,searchResult);

  searchResult = kSearchResultGarbage;
  EXPECT_EQ(19,propertyFile_getInt(&fd, "nineteen", kDefault, &searchResult));
  EXPECT_EQ(RESULT_FOUND,searchResult);

  // check that null "searchResult" parameter is supported
  EXPECT_EQ(kDefault,propertyFile_getInt(&fd, "bar", kDefault, NULL));
  EXPECT_EQ(kDefault,propertyFile_getInt(&fd, "invalid", kDefault, NULL));
  EXPECT_EQ(19,propertyFile_getInt(&fd, "nineteen", kDefault, NULL));
}

TEST(AvdUtil, propertyFile_getApiLevel) {
  FileData fd;

  const char* emptyFile =
    "\n";

  const char* testFile19 =
    "ro.build.version.sdk=19\n";

  const char* testFileBogus =
    "ro.build.version.sdk=bogus\n";

  EXPECT_EQ(0,fileData_initFromMemory(&fd, emptyFile, strlen(emptyFile)));
  EXPECT_EQ(10000,propertyFile_getApiLevel(&fd));

  EXPECT_EQ(0,fileData_initFromMemory(&fd, testFile19, strlen(testFile19)));
  EXPECT_EQ(19,propertyFile_getApiLevel(&fd));

  EXPECT_EQ(0,fileData_initFromMemory(&fd, testFileBogus, strlen(testFileBogus)));
  EXPECT_EQ(3,propertyFile_getApiLevel(&fd));
}

TEST(AvdUtil, propertyFile_findProductName) {
  FileData fd;

  const char* testFileGoogle =
    "ro.product.name=sdk_google_x86-eng\n";

  const char* testFileGoogle2 =
      "ro.product.name=google_sdk_x86-eng\n";

  const char* testFilePhone =
    "ro.product.name=abc_phone_x86-eng\n";

  const char* testFileAndroidAuto =
      "ro.product.name=car_emu_x86_64-userdebug\n";

  const char* testFileRandom =
      "ro.product.name=bat_land-userdebug\n";

  EXPECT_EQ(0, fileData_initFromMemory(&fd, testFileGoogle, strlen(testFileGoogle)));
  const char *google_names[] = {"sdk_google", "google_sdk"};
  EXPECT_TRUE(propertyFile_findProductName(&fd, google_names, ARRAY_SIZE(google_names), false));

  EXPECT_EQ(0, fileData_initFromMemory(&fd, testFileGoogle2, strlen(testFileGoogle2)));
  EXPECT_TRUE(propertyFile_findProductName(&fd, google_names, ARRAY_SIZE(google_names), false));

  EXPECT_EQ(0, fileData_initFromMemory(&fd, testFilePhone, strlen(testFilePhone)));
  const char *phone_names[] = {"phone"};
  EXPECT_TRUE(propertyFile_findProductName(&fd, phone_names, ARRAY_SIZE(phone_names), false));

  EXPECT_EQ(0, fileData_initFromMemory(&fd, testFileAndroidAuto, strlen(testFileAndroidAuto)));
  const char *car_names[] = {"car_emu"};
  EXPECT_TRUE(propertyFile_findProductName(&fd, car_names, ARRAY_SIZE(car_names), true));

  EXPECT_EQ(0, fileData_initFromMemory(&fd, testFileRandom, strlen(testFileRandom)));
  EXPECT_FALSE(propertyFile_findProductName(&fd, google_names, ARRAY_SIZE(google_names), false));
  EXPECT_FALSE(propertyFile_findProductName(&fd, phone_names, ARRAY_SIZE(phone_names), false));
  EXPECT_FALSE(propertyFile_findProductName(&fd, car_names, ARRAY_SIZE(car_names), true));
}

#include "android/avd/info.h"

TEST(AvdInfoTest, api_level_parsing) {
    TestSystem sys("/home", 64, "/");
    TestTempDir* tmp = sys.getTempRoot();
    tmp->makeSubDir("android_home");
    tmp->makeSubDir(pj("android_home", "avd"));
    tmp->makeSubDir(pj("android_home", "platform-tools"));
    tmp->makeSubDir(pj("android_home", "platforms"));
    tmp->makeSubDir(pj("android_home", "tools"));

    std::string sdkRoot = pj(tmp->pathString(), "android_home");
    sys.envSet("ANDROID_AVD_HOME", sdkRoot);
    sys.setLauncherDirectory(".");
    sys.setCurrentDirectory(pj(sdkRoot, "tools"));

    // Helper to create a mock AVD config
    auto create_mock_avd = [&](const std::string& name, const std::string& target) {
        std::string iniPath = pj(sdkRoot, name + ".ini");
        std::string avdDir = pj({sdkRoot, "avd", name + ".avd"});
        tmp->makeSubDir(pj({"android_home", "avd", name + ".avd"}));

        writeToFile(iniPath,
                    "path=" + avdDir + "\n"
                    "target=" + target + "\n");

        // config.ini inside the avd dir
        writeToFile(pj(avdDir, "config.ini"),
                    "target=" + target + "\n"
                    "image.sysdir.1=dummy\n");
    };

    char fullNameBuf[128];

    // Test case 1: Decimal version 36.1
    create_mock_avd("avd_36_1", "android-36.1");
    AvdInfo* info_36_1 = avdInfo_new("avd_36_1", NULL, "dummy_sysdir");
    ASSERT_NE(nullptr, info_36_1);
    EXPECT_EQ(36, avdInfo_getApiLevel(info_36_1));
    EXPECT_STREQ("36.1", avdInfo_getApiLevelStr(info_36_1));
    avdInfo_getFullApiNameFromAvd(info_36_1, fullNameBuf, sizeof(fullNameBuf));
    EXPECT_STREQ("16 (B) - API 36.1", fullNameBuf);
    avdInfo_free(info_36_1);

    // Test case 2: Decimal version 37.0
    create_mock_avd("avd_37_0", "android-37.0");
    AvdInfo* info_37_0 = avdInfo_new("avd_37_0", NULL, "dummy_sysdir");
    ASSERT_NE(nullptr, info_37_0);
    EXPECT_EQ(37, avdInfo_getApiLevel(info_37_0));
    EXPECT_STREQ("37.0", avdInfo_getApiLevelStr(info_37_0));
    avdInfo_getFullApiNameFromAvd(info_37_0, fullNameBuf, sizeof(fullNameBuf));
    EXPECT_STREQ("17 (C) - API 37.0", fullNameBuf);
    avdInfo_free(info_37_0);

    // Test case 3: Decimal version 37.1
    create_mock_avd("avd_37_1", "android-37.1");
    AvdInfo* info_37_1 = avdInfo_new("avd_37_1", NULL, "dummy_sysdir");
    ASSERT_NE(nullptr, info_37_1);
    EXPECT_EQ(37, avdInfo_getApiLevel(info_37_1));
    EXPECT_STREQ("37.1", avdInfo_getApiLevelStr(info_37_1));
    avdInfo_getFullApiNameFromAvd(info_37_1, fullNameBuf, sizeof(fullNameBuf));
    EXPECT_STREQ("17 (C) - API 37.1", fullNameBuf);
    avdInfo_free(info_37_1);

    // Test case 3b: Decimal version with beta suffix 37.2-beta1
    create_mock_avd("avd_37_2_beta1", "android-37.2-beta1");
    AvdInfo* info_37_2_beta1 = avdInfo_new("avd_37_2_beta1", NULL, "dummy_sysdir");
    ASSERT_NE(nullptr, info_37_2_beta1);
    EXPECT_EQ(37, avdInfo_getApiLevel(info_37_2_beta1));
    EXPECT_STREQ("37.2-beta1", avdInfo_getApiLevelStr(info_37_2_beta1));
    avdInfo_getFullApiNameFromAvd(info_37_2_beta1, fullNameBuf, sizeof(fullNameBuf));
    EXPECT_STREQ("17 (C) - API 37.2-beta1", fullNameBuf);
    avdInfo_free(info_37_2_beta1);

    // Test case 4: Preview dessert Baklava (API 36)
    create_mock_avd("avd_baklava", "android-Baklava");
    AvdInfo* info_baklava = avdInfo_new("avd_baklava", NULL, "dummy_sysdir");
    ASSERT_NE(nullptr, info_baklava);
    EXPECT_EQ(36, avdInfo_getApiLevel(info_baklava));
    EXPECT_STREQ("Baklava", avdInfo_getApiLevelStr(info_baklava));
    avdInfo_getFullApiNameFromAvd(info_baklava, fullNameBuf, sizeof(fullNameBuf));
    EXPECT_STREQ("16 (B) - API Baklava", fullNameBuf);
    avdInfo_free(info_baklava);

    // Test case 5: Preview dessert CinnamonBun (API 37)
    create_mock_avd("avd_cinnamon", "android-CinnamonBun");
    AvdInfo* info_cinnamon = avdInfo_new("avd_cinnamon", NULL, "dummy_sysdir");
    ASSERT_NE(nullptr, info_cinnamon);
    EXPECT_EQ(37, avdInfo_getApiLevel(info_cinnamon));
    EXPECT_STREQ("CinnamonBun", avdInfo_getApiLevelStr(info_cinnamon));
    avdInfo_getFullApiNameFromAvd(info_cinnamon, fullNameBuf, sizeof(fullNameBuf));
    EXPECT_STREQ("17 (C) - API CinnamonBun", fullNameBuf);
    avdInfo_free(info_cinnamon);

    // Test case 6: Standard integer 35
    create_mock_avd("avd_35", "android-35");
    AvdInfo* info_35 = avdInfo_new("avd_35", NULL, "dummy_sysdir");
    ASSERT_NE(nullptr, info_35);
    EXPECT_EQ(35, avdInfo_getApiLevel(info_35));
    EXPECT_STREQ("35", avdInfo_getApiLevelStr(info_35));
    avdInfo_getFullApiNameFromAvd(info_35, fullNameBuf, sizeof(fullNameBuf));
    EXPECT_STREQ("15.0 (V) - API 35", fullNameBuf);
    avdInfo_free(info_35);

    // Test case 7: NULL AvdInfo
    avdInfo_getFullApiNameFromAvd(nullptr, fullNameBuf, sizeof(fullNameBuf));
    EXPECT_STREQ("Unknown API version", fullNameBuf);
}

TEST(AvdUtil, path_getBuildVendorProp) {
    TestSystem sys("/home", 64, "/");
    TestTempDir* tmp = sys.getTempRoot();
    tmp->makeSubDir("out");
    std::string outDir = pj(tmp->pathString(), "out");

    // Initially vendor/build.prop does not exist
    EXPECT_EQ(nullptr, path_getBuildVendorProp(outDir.c_str()));

    // Create vendor/build.prop
    tmp->makeSubDir(pj("out", "vendor"));
    std::string vendorBuildProp = pj({outDir, "vendor", "build.prop"});
    writeToFile(vendorBuildProp, "ro.vendor.uwb.dev=/dev/uwb0\n");

    ScopedCPtr<char> propPath(path_getBuildVendorProp(outDir.c_str()));
    ASSERT_NE(nullptr, propPath.get());
    EXPECT_STREQ(vendorBuildProp.c_str(), propPath.get());
}

TEST(AvdInfoTest, build_and_vendor_properties_merged) {
    TestSystem sys("/home", 64, "/");
    TestTempDir* tmp = sys.getTempRoot();
    tmp->makeSubDir("out");
    tmp->makeSubDir(pj("out", "system"));
    tmp->makeSubDir(pj("out", "vendor"));
    std::string outDir = pj(tmp->pathString(), "out");

    writeToFile(pj({outDir, "system", "build.prop"}),
                "ro.product.cpu.abi=x86_64\n"
                "ro.build.version.sdk=34\n");
    writeToFile(pj({outDir, "vendor", "build.prop"}),
                "ro.vendor.uwb.dev=/dev/uwb0\n");

    AvdInfo* avd = avdInfo_newForAndroidBuild(tmp->pathString().c_str(), outDir.c_str(), nullptr, nullptr);
    ASSERT_NE(nullptr, avd);

    ScopedCPtr<char> arch(avdInfo_getTargetCpuArch(avd));
    EXPECT_STREQ("x86_64", arch.get());
    EXPECT_EQ(34, avdInfo_getApiLevel(avd));

    ScopedCPtr<char> uwbDev(avdInfo_getVendorBuildPropertyString(avd, "ro.vendor.uwb.dev"));
    ASSERT_NE(nullptr, uwbDev.get());
    EXPECT_STREQ("/dev/uwb0", uwbDev.get());

    const FileData* vendorProps = avdInfo_getVendorBuildProperties(avd);
    ASSERT_NE(nullptr, vendorProps);
    EXPECT_FALSE(fileData_isEmpty(vendorProps));

    avdInfo_free(avd);
}

