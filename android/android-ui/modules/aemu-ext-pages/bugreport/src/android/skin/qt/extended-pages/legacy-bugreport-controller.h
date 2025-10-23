// Copyright 2025 The Android Open Source Project
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

#pragma once

#include "android/skin/qt/extended-pages/bugreport-controller.h"

// LegacyBugreportController is the implementation of the BugreportController
// that uses the traditional console agents and global functions to interact
// with the emulator.
class LegacyBugreportController : public BugreportController {
public:
    LegacyBugreportController();
    ~LegacyBugreportController() override = default;

    void takeScreenshot(std::function<void(const std::string&)> callback) override;
    void isBootCompleted(std::function<void(bool)> callback) override;
    BugreportInfo getSystemInfo() override;
};
