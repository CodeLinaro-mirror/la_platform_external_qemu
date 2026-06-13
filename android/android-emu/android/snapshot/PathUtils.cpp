// Copyright 2017 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/snapshot/PathUtils.h"

#include "android/avd/info.h"
#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"
#include "android/console.h"

using android::base::System;

namespace android {
namespace snapshot {

std::vector<std::string> getQcow2Files(std::string avdDir) {
    std::vector<std::string> qcows{};
    const std::string ext = ".qcow2";

    auto list = System::get()->scanDirEntries(avdDir);

    // Copy all files that have a .qcow2 extension.
    std::copy_if(list.begin(), list.end(), std::back_inserter(qcows),
                 [ext](std::string fname) {
                     if (ext.size() > fname.size())
                         return false;
                     return std::equal(
                             fname.begin() + fname.size() - ext.size(),
                             fname.end(), ext.begin());
                 });
    return qcows;
}

std::string getAvdDir() {
    return avdInfo_getContentPath(getConsoleAgents()->settings->avdInfo());
}

std::string getSnapshotBaseDir() {
    auto avdDir = avdInfo_getContentPath(getConsoleAgents()->settings->avdInfo());
    auto path = base::PathUtils::join(avdDir ? avdDir : "", "snapshots");
    return base::PathUtils::canonicalPath(path);
}

std::string getSnapshotDir(const char* snapshotName) {
    auto baseDir = getSnapshotBaseDir();
    // SECURITY: snapshotName arrives verbatim from gRPC SnapshotService
    // (Push/Save/DeleteSnapshot). PathUtils::join() returns an absolute
    // second arg verbatim and does not strip "..". Callers immediately
    // path_delete_dir() / move() / mkdir the result, so a hostile name like
    // "/home/<u>" or "../../.." escapes the AVD. Force a single component.
    std::string safe(snapshotName ? snapshotName : "");
    std::replace(safe.begin(), safe.end(), '/',  '_');
    std::replace(safe.begin(), safe.end(), '\\', '_');
    return base::PathUtils::join(baseDir, safe);
}

std::string getSnapshotDepsFileName() {
    return base::PathUtils::join(getSnapshotBaseDir(), "snapshot_deps.pb");
}

std::vector<std::string> getSnapshotDirEntries() {
    return System::get()->scanDirEntries(getSnapshotBaseDir());
}

base::System::FileSize folderSize(const std::string& snapshotName) {
    return System::get()->recursiveSize(
            base::PathUtils::join(getSnapshotBaseDir(), snapshotName));
}

std::string getQuickbootChoiceIniPath() {
    auto avdDir = avdInfo_getContentPath(getConsoleAgents()->settings->avdInfo());
    auto path = base::PathUtils::join(avdDir ? avdDir : "", "quickbootChoice.ini");
    return path;
}

}  // namespace snapshot
}  // namespace android
