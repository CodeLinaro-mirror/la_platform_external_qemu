#include "android/skin/qt/extended-pages/grpc_snapshot_controller.h"

#include <chrono>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "android/emulation/control/utils/SnapshotClient.h"
#include "google/protobuf/util/time_util.h"
#include "snapshot_service.pb.h"

namespace android {
namespace emulation {
namespace control {

using google::protobuf::util::TimeUtil;

namespace {

SnapshotStatus toSnapshotStatus(
        android::emulation::control::SnapshotDetails_LoadStatus status) {
    switch (status) {
        case android::emulation::control::SnapshotDetails_LoadStatus_Compatible:
            return SnapshotStatus::Compatible;
        case android::emulation::control::
                SnapshotDetails_LoadStatus_Incompatible:
            return SnapshotStatus::Incompatible;
        case android::emulation::control::SnapshotDetails_LoadStatus_Loaded:
            return SnapshotStatus::Loaded;
        default:

            return SnapshotStatus::Unknown;    }
}

}  // namespace

class GrpcSnapshotController::Impl {
public:
    Impl() : mSnapshotService(EmulatorGrpcClient::me()) {}

    SnapshotServiceClient mSnapshotService;
};

GrpcSnapshotController::GrpcSnapshotController()
    : mImpl(std::make_unique<Impl>()) {}

GrpcSnapshotController::~GrpcSnapshotController() = default;

void GrpcSnapshotController::listSnapshots(
        std::function<void(absl::StatusOr<std::vector<SnapshotInfo>>)>
                callback) {
    mImpl->mSnapshotService.ListSnapshotsAsync(
            SnapshotFilter(),
            [callback](absl::StatusOr<SnapshotList*> result) {
                if (!result.ok()) {
                    callback(result.status());
                    return;
                }

                std::vector<SnapshotInfo> details;
                for (const auto& protoDetails : result.value()->snapshots()) {
                    google::protobuf::Timestamp creation_time_proto;
                    creation_time_proto.set_seconds(
                            protoDetails.details().creation_time());
                    creation_time_proto.set_nanos(
                            0);  // Assuming nanos are not available or 0

                    details.push_back(SnapshotInfo{
                            .snapshot_id = protoDetails.snapshot_id(),
                            .status = toSnapshotStatus(protoDetails.status()),
                            .creation_time =
                                    std::chrono::system_clock::from_time_t(
                                            TimeUtil::TimestampToTimeT(
                                                    creation_time_proto)),
                            .logical_name =
                                    protoDetails.details().logical_name(),
                            .description = protoDetails.details().description(),
                            .size = protoDetails.size()});
                }

                callback(details);
            });
}

void GrpcSnapshotController::loadSnapshot(
        const std::string& snapshotId,
        const std::string& destination,
        std::function<void(absl::Status)> callback) {
    mImpl->mSnapshotService.LoadSnapshotAsync(
            snapshotId, [callback](absl::StatusOr<SnapshotPackage*> result) {
                callback(result.status());
            });
}

void GrpcSnapshotController::saveSnapshot(
        const std::string& snapshotId,
        std::function<void(absl::Status)> callback) {
    mImpl->mSnapshotService.SaveSnapshotAsync(
            snapshotId, [callback](absl::StatusOr<SnapshotPackage*> result) {
                callback(result.status());
            });
}

void GrpcSnapshotController::deleteSnapshot(
        const std::string& snapshotId,
        std::function<void(absl::Status)> callback) {
    mImpl->mSnapshotService.DeleteSnapshotAsync(
            snapshotId, [callback](absl::StatusOr<SnapshotPackage*> result) {
                callback(result.status());
            });
}

void GrpcSnapshotController::updateSnapshot(
        const SnapshotInfo& details,
        std::function<void(absl::Status)> callback) {
    SnapshotUpdateDescription update;
    update.set_snapshot_id(details.snapshot_id);
    update.mutable_logical_name()->set_value(details.logical_name);
    update.mutable_description()->set_value(details.description);

    mImpl->mSnapshotService.UpdateSnapshotAsync(
            update,
            [callback](
                    absl::StatusOr<
                            android::emulation::control::SnapshotDetails*>
                            result) { callback(result.status()); });
}

void GrpcSnapshotController::getScreenshot(
        const std::string& snapshotId,
        std::function<void(absl::StatusOr<SnapshotScreenshot>)> callback) {
    mImpl->mSnapshotService.GetScreenshotAsync(
            snapshotId,
            [callback](absl::StatusOr<SnapshotScreenshotFile*> result) {
                if (!result.ok()) {
                    callback(result.status());
                    return;
                }

                const std::string& imageData = result.value()->data();
                callback(SnapshotScreenshot{
                        .format = result.value()->format() ==
                                                  SnapshotScreenshotFile::
                                                          FORMAT_PNG
                                          ? SnapshotScreenshot::Format::PNG
                                          : SnapshotScreenshot::Format::
                                                    UNSPECIFIED,
                        .data = std::vector<char>(imageData.begin(),
                                                  imageData.end())});
            });
}

}  // namespace control
}  // namespace emulation
}  // namespace android
