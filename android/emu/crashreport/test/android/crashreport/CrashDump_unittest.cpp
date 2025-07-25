// Copyright 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <chrono>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "aemu/base/files/PathUtils.h"
#include "aemu/base/logging/Log.h"
#include "aemu/base/process/Command.h"
#include "android/base/system/System.h"
#include "android/crashreport/CrashReporter.h"
#include "android/crashreport/CrashpadLogSink.h"
#include "client/annotation.h"
#include "client/crash_report_database.h"
#include "client/settings.h"
#include "nlohmann/json.hpp"
#include "processor/stackwalk_common.h"
#include "snapshot/minidump/process_snapshot_minidump.h"

using android::base::Command;
using android::base::PathUtils;
using android::base::System;
using crashpad::CrashReportDatabase;
using namespace std::chrono_literals;
using json = nlohmann::json;
using crashpad::FileReader;
using crashpad::ModuleSnapshot;
using crashpad::ProcessSnapshotMinidump;

const constexpr char kCrashpadDatabase[] = "emu-crash.db";

// The crashpad handler binary, as shipped with the emulator.
const constexpr char kCrashpadHandler[] = "crashpad_handler";
const std::string kCrashMe = "crash-me";
const std::string kReport = "crashreport";
using android::crashreport::CrashReporter;

class CrashTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_configure_logs(LoggingFlags::kLogDefaultOptions);
        mCrashdatabase = InitializeCrashDatabase();
        deleteReports();
    }

    void TearDown() override { deleteReports(); }

    std::vector<CrashReportDatabase::Report> get_all_reports() {
        std::vector<crashpad::CrashReportDatabase::Report> reports;
        std::vector<crashpad::CrashReportDatabase::Report> pendingReports;
        mCrashdatabase->GetCompletedReports(&reports);
        mCrashdatabase->GetPendingReports(&pendingReports);
        reports.insert(reports.end(), pendingReports.begin(),
                       pendingReports.end());
        return reports;
    }

    void deleteReports() {
        for (const auto& report : get_all_reports()) {
            dinfo("Erasing %s\n", report.uuid.ToString());
            mCrashdatabase->DeleteReport(report.uuid);
        }
    }

    // Translate vector to hex string.
    std::string vectorToHexString(const std::vector<uint8_t>& data) {
        std::stringstream ss;
        ss << "[";
        for (int i = 0; i < data.size(); i++) {
            ss << "0x" << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(data[i]);
        }
        ss << "]";
        return ss.str();
    }

    std::unique_ptr<CrashReportDatabase> InitializeCrashDatabase() {
        auto crashDatabasePath = android::base::pj(System::get()->getTempDir(),
                                                   kCrashpadDatabase);
        auto handler_path = ::base::FilePath(
                PathUtils::asUnicodePath(
                        System::get()
                                ->findBundledExecutable(kCrashpadHandler)
                                .data())
                        .c_str());
        auto database_path = CrashReporter::databaseDirectory();
        auto crashDatabase =
                crashpad::CrashReportDatabase::Initialize(database_path);

        crashDatabase->GetSettings()->SetUploadsEnabled(false);

        return crashDatabase;
    }

    void crash() {
        std::string executable = System::get()->findBundledExecutable(kCrashMe);
        dinfo("Running %s", executable);
        auto proc = Command::create({executable}).inherit().execute();
        proc->wait_for(500ms);
    }

    // Returns a json object with all the module annotations.
    json GetAnnotationsAsJson(const base::FilePath& minidump_file) {
        FileReader reader;
        if (!reader.Open(minidump_file)) {
            return json();
        }

        // Next we are going to dump the annotations
        ProcessSnapshotMinidump snapshot;
        if (!snapshot.Initialize(&reader)) {
            return json();
        }

        // Let's do json style output

        json modules;
        for (const crashpad::ModuleSnapshot* module : snapshot.Modules()) {
            // Let's not print empty records
            if (module->AnnotationsSimpleMap().empty() &&
                module->AnnotationsVector().empty() &&
                module->AnnotationObjects().empty()) {
                continue;
            }
            json json_module;
            json_module["name"] = module->Name();
            if (!module->AnnotationsSimpleMap().empty()) {
                json_module["simple_annotations"] = std::vector<json>();
                for (const auto& kv : module->AnnotationsSimpleMap()) {
                    json_module["simple_annotations"].push_back(
                            {"name", kv.first, "value", kv.second});
                }
            }

            if (!module->AnnotationsVector().empty()) {
                json_module["vectored_annotations"] =
                        std::vector<std::string>();
                for (const std::string& annotation :
                     module->AnnotationsVector()) {
                    json_module["vectored_annotations"].push_back(annotation);
                }
            }
            if (!module->AnnotationObjects().empty()) {
                json_module["annotation_objects"] = std::vector<json>();
                for (const crashpad::AnnotationSnapshot& annotation :
                     module->AnnotationObjects()) {
                    json json_annotation;
                    json_annotation["name"] = annotation.name;
                    if (annotation.type !=
                        static_cast<uint16_t>(
                                crashpad::Annotation::Type::kString)) {
                        json_annotation["value"] =
                                vectorToHexString(annotation.value);
                    } else {
                        std::string value(reinterpret_cast<const char*>(
                                                  annotation.value.data()),
                                          annotation.value.size());
                        json_annotation["value"] = value;
                    }
                    json_module["annotation_objects"].push_back(
                            json_annotation);
                }
            }
            modules.push_back(json_module);
        }
        return modules;
    }

    std::string dump_report(std::string path) {
        std::string executable = System::get()->findBundledExecutable(kReport);
        dinfo("Running %s", executable);
        auto proc = Command::create({executable, "-d", path})
                            .withStdoutBuffer(128 * 1024)
                            .inherit()
                            .execute();

        proc->wait_for(1500ms);
        return proc->out()->asString();
    }

    std::vector<CrashReportDatabase::Report> crash_reports() {
        std::vector<CrashReportDatabase::Report> reports = get_all_reports();

        std::set<crashpad::UUID> before_uuids;
        for (const auto& report : reports) {
            before_uuids.insert(report.uuid);
        }

        crash();

        std::vector<CrashReportDatabase::Report> all_reports;
        auto start = std::chrono::high_resolution_clock::now();
        do {
            // Let's give the crash handler some time to write to the database.
            std::this_thread::sleep_for(50ms);
            all_reports = get_all_reports();
        } while (all_reports.size() <= before_uuids.size() &&
                 std::chrono::high_resolution_clock::now() - start < 10s);

        std::vector<CrashReportDatabase::Report> new_reports;
        for (const auto& report : all_reports) {
            if (before_uuids.find(report.uuid) == before_uuids.end()) {
                new_reports.push_back(report);
            }
        }
        return new_reports;
    }

    std::unique_ptr<CrashReportDatabase> mCrashdatabase;
};

TEST_F(CrashTest, crash_generates_minidump) {
    auto new_reports = crash_reports();
    EXPECT_EQ(new_reports.size(), 1)
            << "The database should have recorded an additional crash!";
}
TEST_F(CrashTest, minidump_has_error_log) {
    auto new_reports = crash_reports();
    ASSERT_EQ(new_reports.size(), 1);

    std::stringstream ss;
    auto report = GetAnnotationsAsJson(new_reports.at(0).file_path);
    auto dump = report.dump();
    EXPECT_THAT(dump,
                testing::HasSubstr("Crash message should appear on the log."));
}

TEST_F(CrashTest, minidump_is_usable) {
    // TODO: This requires us to binplace all the .sym files ourselves.
}
