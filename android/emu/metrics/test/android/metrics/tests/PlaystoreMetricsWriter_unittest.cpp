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

#include "android/metrics/PlaystoreMetricsWriter.h"

#include <gtest/gtest.h>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#else
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <algorithm>
#include "aemu/base/files/GzipStreambuf.h"
#include "aemu/base/files/PathUtils.h"
#include "aemu/base/sockets/ScopedSocket.h"
#include "aemu/base/sockets/SocketUtils.h"
#include "android/base/testing/TestSystem.h"
#include "android/curl-support.h"
#include "android/metrics/FileMetricsWriter.h"
#include "android/metrics/MetricsLogging.h"
#include "android/metrics/studio_stats_wrapper.pb.h"
#include "google_logs_publishing.pb.h"

using namespace android;
using namespace android::base;
using namespace android::metrics;

static const int SOCK_WAIT = 200;  // Increase if you see test failures.

bool waitForData(int socket, int timeoutms) {
    timeval timeout;
    fd_set fds;
    timeout.tv_sec = 0;
    timeout.tv_usec = timeoutms * 1000;
    FD_ZERO(&fds);
    FD_SET(socket, &fds);
    int available = select(socket + 1, &fds, 0, 0, &timeout);
    if (available > 0)
        return true;

    if (available == 0) {
        //  D("Timeout. %d. %d", socket, timeoutms);
    } else {
        D("Error waiting for %d, %d", socket, errno);
    }

    return false;
}

// returns count of non-overlapping occurrences of 'sub' in 'str'
int countSubstring(const std::string& str, const std::string& sub) {
    if (sub.length() == 0) {
        return 0;
    }
    int cnt = 0;
    auto o = str.find(sub);
    while (o != std::string::npos) {
        cnt++;
        o = str.find(sub, o + sub.length());
    }
    return cnt;
}

class PlaystoreMetricsWriterTest : public ::testing::Test {
public:
    void SetUp() override {
        // Clean up mess and start up the fake http server.
        // android_verbose = -1; // Uncomment for lots of debug info.
        auto tmp = mTestSystem.getTempRoot();
        mCookie = base::pj(tmp->pathString(), "sam_cookie");
        System::get()->deleteFile(mCookie);
        curl_init(nullptr);
        mRunning = true;
        startMockHttpServer();
        mServer = std::thread([this]() { this->runMockHttpServer(); });
        System::get()->sleepMs(100);
    }

    void TearDown() override {
        // Clean up the mess.
        System::get()->deleteFile(mCookie);
        mRunning = false;
        socketClose(mFd);
        curl_cleanup();
    }

protected:
    bool runMockHttpServer() {
        // A fake http server, right now it doesn't respond,
        // which is good enough for our unit tests.
        while (mRunning) {
            D("Wait for data..");
            while (mRunning && !waitForData(mFd, SOCK_WAIT)) {
                if (!mRunning)
                    return true;
            }
            D("accept");
            ScopedSocket s(socketAcceptAny(mFd));
            if (!s.valid()) {
                D("Could not accept connection!?\n");
                return false;
            }
            socketSetNonBlocking(s.get());
            mRequestsMade++;
            mOpenMarkersAtRequest = countOpenMarkers();
            int rec = 1;
            while (rec > 0 && waitForData(s.get(), SOCK_WAIT)) {
                char buffer[512];
                rec = socketRecv(s.get(), buffer, sizeof(buffer));
                if (rec > 0) {
                    mHttpRequest.append(buffer, rec);
                }
                D("Rec %d", rec);
            }
        }
        return true;
    }

    void startMockHttpServer() {
        mFd = socketTcp4LoopbackServer(0);
        if (mFd < 0) {
            // No ports??
            FAIL();
            return;
        }
        socketSetNonBlocking(mFd);
        // Get socket port now.
        mPort = socketGetPort(mFd);
        D("Using TCP loopback port %d\n", mPort);
    }

    void waitForServerThread() {
        mRunning = false;
        mServer.join();
    }

    // Counts the PlaystoreMetricsWriter session markers (.open files) in the
    // spool directory, which is the directory of |mCookie|.
    int countOpenMarkers() const {
        const auto files = mTestSystem.scanDirEntries(
                mTestSystem.getTempRoot()->pathString());
        return static_cast<int>(std::count_if(
                files.begin(), files.end(), [](const std::string& name) {
                    return PathUtils::extension(name) == ".open";
                }));
    }

    std::string mHttpRequest;
    std::thread mServer;
    int mFd{0};
    int mPort{0};
    int mRequestsMade{0};
    // Number of .open session markers present when the last request arrived.
    int mOpenMarkersAtRequest{-1};
    bool mRunning{true};
    TestSystem mTestSystem{"/", System::kProgramBitness};
    std::string mCookie;
};

static wireless_android_play_playlog::LogEvent addFakeMetricToWriter(
        PlaystoreMetricsWriter& metrics,
        std::string project_id = "An example project_id") {
    android_studio::AndroidStudioEvent ase;
    ase.set_project_id(project_id);
    wireless_android_play_playlog::LogEvent logEvent;
    logEvent.set_event_time_ms(1234);
    logEvent.set_source_extension("Nonsense data that take up bytes..");
    metrics.write(ase, &logEvent);
    return logEvent;
}

TEST_F(PlaystoreMetricsWriterTest, commitOnClose) {
    // Check that we commit the data when we go out of scope,
    // we rely on this to send metrics!
    {
        PlaystoreMetricsWriter metrics(
                "hello-emu", mCookie,
                "http://localhost:" + std::to_string(mPort));
        addFakeMetricToWriter(metrics);
    }

    waitForServerThread();
    EXPECT_NE(mHttpRequest.find("Content-Length: "), std::string::npos);
    EXPECT_NE(mHttpRequest.find("Content-Encoding: gzip"), std::string::npos);
}

TEST_F(PlaystoreMetricsWriterTest, commitOnlyWhenData) {
    // We should only send data when we have something. In this case we are
    // running commit 2x so we should get only one incoming request.
    {
        PlaystoreMetricsWriter metrics(
                "hello-emu", mCookie,
                "http://localhost:" + std::to_string(mPort));
        addFakeMetricToWriter(metrics);
        metrics.commit();
    }

    waitForServerThread();
    EXPECT_EQ(mRequestsMade, 1);
    // Content-Length only occurs once!
    EXPECT_EQ(countSubstring(mHttpRequest, "Content-Length"), 1);
}

static std::string messageBody(std::string httpMessage) {
    /*
         Request       = Request-Line              ; Section 5.1
                         *(( general-header        ; Section 4.5
                          | request-header         ; Section 5.3
                          | entity-header ) CRLF)  ; Section 7.1
                         CRLF
                         [ message-body ]          ; Section 4.3
     */
    const std::string crlfs = "\r\n\r\n";
    auto idx = httpMessage.find(crlfs);

    // We ignore content-lenght header..
    return httpMessage.substr(idx + crlfs.size());
}

TEST_F(PlaystoreMetricsWriterTest, commitSomethingSensible) {
    // Check that we actually shipped a gzipped event with our data.
    PlaystoreMetricsWriter metrics("hello-emu", mCookie,
                                   "http://localhost:" + std::to_string(mPort));
    auto event = addFakeMetricToWriter(metrics);
    metrics.commit();
    waitForServerThread();

    std::stringbuf buffer(messageBody(mHttpRequest));
    GzipInputStream gis(&buffer);
    wireless_android_play_playlog::LogRequest incoming;
    incoming.ParseFromIstream(&gis);

    EXPECT_TRUE(incoming.has_client_info());
    EXPECT_EQ(incoming.log_event_size(), 1);
}

TEST_F(PlaystoreMetricsWriterTest, commitWhenData) {
    // We should only send data when we have something.
    // Here we are adding data 2x, so 2 commits.
    {
        PlaystoreMetricsWriter metrics(
                "hello-emu", mCookie,
                "http://localhost:" + std::to_string(mPort));
        addFakeMetricToWriter(metrics);
        metrics.commit();
        addFakeMetricToWriter(metrics);
    }
    waitForServerThread();
    EXPECT_EQ(mRequestsMade, 2);
}

TEST_F(PlaystoreMetricsWriterTest, noDataNoCall) {
    {
        PlaystoreMetricsWriter metrics(
                "hello-emu", mCookie,
                "http://localhost:" + std::to_string(mPort));
    }
    waitForServerThread();
    EXPECT_EQ(mRequestsMade, 0);
    EXPECT_EQ(mHttpRequest.size(), 0);
}

TEST_F(PlaystoreMetricsWriterTest, dropWhenOOM) {
    // We don't collect events when we are consuming too much memory.
    {
        PlaystoreMetricsWriter metrics(
                "hello-emu", mCookie,
                "http://localhost:" + std::to_string(mPort));
        std::string largeMsg = std::string(256 * 1024, 'x');
        addFakeMetricToWriter(metrics, largeMsg);
    }
    waitForServerThread();
    EXPECT_EQ(mRequestsMade, 0);
    EXPECT_EQ(mHttpRequest.size(), 0);
}

static void writeFakeCookie(std::string fname, int64_t waitUntil) {
    wireless_android_play_playlog::LogResponse response;
    response.set_next_request_wait_millis(waitUntil);
    std::ofstream cookieResponse(
            fname, std::ios::out | std::ios::binary | std::ios::trunc);
    response.SerializeToOstream(&cookieResponse);
    cookieResponse.close();

    // Make sure cookie was written.
    System::FileSize fsize;
    EXPECT_TRUE(System::get()->pathFileSize(fname, &fsize));
    EXPECT_NE(0, fsize);
}

// sdk_tools_windows currently fails this test.
// We don't have TestSystem available in the shared library, so skip it.
TEST_F(PlaystoreMetricsWriterTest, noCallBeforeTime) {
    // This validates that we respect the timing in the cookie file.
    // We set the cookie to somewhere far in the future.
    writeFakeCookie(mCookie, std::numeric_limits<int64_t>::max());
    {
        PlaystoreMetricsWriter metrics(
                "hello-emu", mCookie,
                "http://localhost:" + std::to_string(mPort));
        addFakeMetricToWriter(metrics);
        metrics.commit();
    }

    waitForServerThread();
    EXPECT_EQ(mRequestsMade, 0);
    EXPECT_EQ(mHttpRequest.size(), 0);
}

TEST_F(PlaystoreMetricsWriterTest, doCallAfterTime) {
    // This validates that we respect the timing in the cookie file.
    // Since this time is in the past, we should send.
    writeFakeCookie(mCookie, System::get()->getUnixTime() - 100);
    {
        PlaystoreMetricsWriter metrics(
                "hello-emu", mCookie,
                "http://localhost:" + std::to_string(mPort));
        addFakeMetricToWriter(metrics);

        // System time = 101ms.
        mTestSystem.setUnixTimeUs(101 * 1000);
        metrics.commit();
    }

    waitForServerThread();
    EXPECT_EQ(mRequestsMade, 1);
}

TEST_F(PlaystoreMetricsWriterTest, sessionMarkerCreatedAndRemovedOnCleanClose) {
    const std::string spoolDir = mTestSystem.getTempRoot()->pathString();
    const std::string sessionId = "11111111-2222-3333-4444-555555555555";
    {
        PlaystoreMetricsWriter metrics(
                sessionId, mCookie,
                "http://localhost:" + std::to_string(mPort));
        auto files = mTestSystem.scanDirEntries(spoolDir);
        EXPECT_EQ(1, std::count_if(files.begin(), files.end(),
                                   [](const std::string& name) {
                                       return PathUtils::extension(name) ==
                                              ".open";
                                   }));
        EXPECT_EQ(1, std::count_if(files.begin(), files.end(),
                                   [](const std::string& name) {
                                       return PathUtils::extension(name) ==
                                              ".lock";
                                   }));

        // Active session must not be treated as abandoned while locked.
        auto abandoned =
                FileMetricsWriter::finalizeAbandonedSessionFiles(spoolDir);
        EXPECT_TRUE(abandoned.empty());

        // Mid-session commit() must keep the session marker open.
        metrics.commit();
        files = mTestSystem.scanDirEntries(spoolDir);
        EXPECT_EQ(1, std::count_if(files.begin(), files.end(),
                                   [](const std::string& name) {
                                       return PathUtils::extension(name) ==
                                              ".open";
                                   }));
    }

    waitForServerThread();

    // Clean destruction must remove both .open and .open.lock without .trk.
    auto filesAfter = mTestSystem.scanDirEntries(spoolDir);
    EXPECT_EQ(0,
              std::count_if(filesAfter.begin(), filesAfter.end(),
                            [](const std::string& name) {
                                return PathUtils::extension(name) == ".open" ||
                                       PathUtils::extension(name) == ".lock" ||
                                       PathUtils::extension(name) == ".trk";
                            }));
    auto abandonedAfter =
            FileMetricsWriter::finalizeAbandonedSessionFiles(spoolDir);
    EXPECT_TRUE(abandonedAfter.empty());
}

TEST_F(PlaystoreMetricsWriterTest, sessionMarkerRemovedBeforeFinalUpload) {
    {
        PlaystoreMetricsWriter metrics(
                "33333333-4444-5555-6666-777777777777", mCookie,
                "http://localhost:" + std::to_string(mPort));
        addFakeMetricToWriter(metrics);
    }

    waitForServerThread();

    // The destructor uploads the pending event only after removing the
    // session marker, so a kill during a slow final upload isn't reported as
    // a crash by the next session.
    EXPECT_EQ(mRequestsMade, 1);
    EXPECT_EQ(mOpenMarkersAtRequest, 0);
}

#ifndef _WIN32
TEST_F(PlaystoreMetricsWriterTest, abandonedSessionMarkerFinalizedAfterCrash) {
    waitForServerThread();

    const std::string spoolDir = mTestSystem.getTempRoot()->pathString();
    const std::string crashedSessionId = "22222222-3333-4444-5555-666666666666";

    int pipeFds[2] = {-1, -1};
    ASSERT_EQ(0, pipe(pipeFds));
    const pid_t childPid = fork();
    ASSERT_GE(childPid, 0);
    if (childPid == 0) {
        close(pipeFds[0]);
        filelock_init();
        PlaystoreMetricsWriter metrics(
                crashedSessionId, mCookie,
                "http://localhost:" + std::to_string(mPort));
        uint8_t ready = 1;
        (void)write(pipeFds[1], &ready, sizeof(ready));
        close(pipeFds[1]);
        pause();
        _exit(0);
    }
    close(pipeFds[1]);
    uint8_t ready = 0;
    ASSERT_EQ(static_cast<ssize_t>(sizeof(ready)),
              read(pipeFds[0], &ready, sizeof(ready)));
    close(pipeFds[0]);
    ASSERT_EQ(1U, ready);

    // Simulate an unclean crash (SIGKILL) of the PlaystoreMetricsWriter run.
    ASSERT_EQ(0, kill(childPid, SIGKILL));
    ASSERT_EQ(childPid, waitpid(childPid, nullptr, 0));

    // A single finalizeAbandonedSessionFiles call in the next session must
    // detect the crashed session and delete the 0-byte .open marker.
    auto abandoned = FileMetricsWriter::finalizeAbandonedSessionFiles(spoolDir);
    ASSERT_EQ(1U, abandoned.size());
    EXPECT_NE(abandoned.end(), abandoned.find(crashedSessionId));

    auto filesAfter = mTestSystem.scanDirEntries(spoolDir);
    EXPECT_EQ(0,
              std::count_if(filesAfter.begin(), filesAfter.end(),
                            [](const std::string& name) {
                                return PathUtils::extension(name) == ".open" ||
                                       PathUtils::extension(name) == ".lock" ||
                                       PathUtils::extension(name) == ".trk";
                            }));
}
#endif
