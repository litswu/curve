/*
 * Project: curve
 * Created Date: Tue Mar 26 2019
 * Author: xuchaojie
 * Copyright (c) 2018 netease
 */

#include "src/snapshotcloneserver/clone/clone_core.h"

#include <memory>
#include <string>
#include <vector>

#include "src/snapshotcloneserver/clone/clone_task.h"
#include "src/common/location_operator.h"
#include "src/common/uuid.h"

using ::curve::common::UUIDGenerator;
using ::curve::common::LocationOperator;

namespace curve {
namespace snapshotcloneserver {

int CloneCoreImpl::CloneOrRecoverPre(const UUID &source,
    const std::string &user,
    const std::string &destination,
    bool lazyFlag,
    CloneTaskType taskType,
    CloneInfo *cloneInfo) {
    std::vector<CloneInfo> list;
    metaStore_->GetCloneInfoList(&list);
    for (auto &info : list) {
        if (destination == info.GetDest() &&
            CloneStatus::error == info.GetStatus()) {
            LOG(ERROR) << "Can not clone/recover when has error,"
                      << " error taskid = " << info.GetTaskId()
                      << " destination fileName = " << info.GetDest();
            return kErrCodeSnapshotCannotCreateWhenError;
        }
    }
    //是否为快照
    SnapshotInfo snapInfo;
    CloneFileType fileType;
    int ret = metaStore_->GetSnapshotInfo(source, &snapInfo);
    if (ret < 0) {
        FInfo fInfo;
        ret = client_->GetFileInfo(source, user, &fInfo);
        switch (ret) {
            case LIBCURVE_ERROR::OK:
                fileType = CloneFileType::kFile;
                break;
            case LIBCURVE_ERROR::NOTEXIST:
                LOG(ERROR) << "Clone source file not exist"
                           << ", source = " << source
                           << ", user = " << user
                           << ", destination = " << destination;
                return kErrCodeFileNotExist;
            case LIBCURVE_ERROR::AUTHFAIL:
                LOG(ERROR) << "Clone file by invalid user"
                           << ", source = " << source
                           << ", user = " << user
                           << ", destination = " << destination;
                return kErrCodeInvalidUser;
            default:
                LOG(ERROR) << "GetFileInfo encounter an error"
                           << ", ret = " << ret
                           << ", source = " << source
                           << ", user = " << user;
                return kErrCodeInternalError;
        }
    } else {
        if (snapInfo.GetStatus() != Status::done) {
            LOG(ERROR) << "Can not clone by snapshot has status:"
                       << static_cast<int>(snapInfo.GetStatus());
            return kErrCodeInvalidSnapshot;
        }
        // TODO(xuchaojie): 后续考虑将鉴权逻辑独立，还需支持超级用户等复杂逻辑
        if (snapInfo.GetUser() != user) {
            LOG(ERROR) << "Clone snapshot by invalid user"
                       << ", source = " << source
                       << ", user = " << user
                       << ", destination = " << destination
                       << ", snapshot.user = " << snapInfo.GetUser();
            return kErrCodeInvalidUser;
        }
        fileType = CloneFileType::kSnapshot;
    }

    UUID uuid = UUIDGenerator().GenerateUUID();
    CloneInfo info(uuid, user, taskType,
        source, destination, fileType, lazyFlag);
    info.SetStatus(CloneStatus::cloning);
    ret = metaStore_->AddCloneInfo(info);
    if (ret < 0) {
        LOG(ERROR) << "AddCloneInfo error"
                   << ", ret = " << ret
                   << ", taskId = " << uuid
                   << ", user = " << user
                   << ", source = " << source
                   << ", destination = " << destination;
        return ret;
    }
    *cloneInfo = info;
    if (CloneFileType::kSnapshot == fileType) {
        snapshotRef_->IncrementSnapshotRef(source);
    } else {
        // TODO(xuchaojie): 非快照情况下，需提供措施防止被克隆文件被删除
    }
    return kErrCodeSuccess;
}

constexpr uint32_t kProgressCreateCloneFile = 10;
constexpr uint32_t kProgressCreateCloneMeta = 20;
constexpr uint32_t kProgressCreateCloneChunk = 50;
constexpr uint32_t kProgressRecoverChunkBegin = kProgressCreateCloneChunk;
constexpr uint32_t kProgressRecoverChunkEnd = 90;
constexpr uint32_t kProgressCloneComplete = 100;

void CloneCoreImpl::HandleCloneOrRecoverTask(
    std::shared_ptr<CloneTaskInfo> task) {
    int ret = kErrCodeSuccess;
    FInfo newFileInfo;
    CloneSegmentMap segInfos;
    if (IsSnapshot(task)) {
        ret = BuildFileInfoFromSnapshot(task, &newFileInfo, &segInfos);
        if (ret < 0) {
            HandleCloneError(task);
            return;
        }
    } else {
        ret = BuildFileInfoFromFile(task, &newFileInfo, &segInfos);
        if (ret < 0) {
            HandleCloneError(task);
            return;
        }
    }

    // 在kCreateCloneMeta以后的步骤还需更新CloneChunkInfo信息中的chunkIdInfo
    if (NeedUpdateCloneMeta(task)) {
        ret = CreateOrUpdateCloneMeta(task, &newFileInfo, &segInfos);
        if (ret < 0) {
            HandleCloneError(task);
            return;
        }
    }

    CloneStep step = task->GetCloneInfo().GetNextStep();
    while (step != CloneStep::kEnd) {
        switch (step) {
            case CloneStep::kCreateCloneFile:
                ret = CreateCloneFile(task, newFileInfo);
                if (ret < 0) {
                    HandleCloneError(task);
                    return;
                }
                task->SetProgress(kProgressCreateCloneFile);
                break;
            case CloneStep::kCreateCloneMeta:
                ret = CreateCloneMeta(task, &newFileInfo, &segInfos);
                if (ret < 0) {
                    HandleCloneError(task);
                    return;
                }
                task->SetProgress(kProgressCreateCloneMeta);
                break;
            case CloneStep::kCreateCloneChunk:
                ret = CreateCloneChunk(task, newFileInfo, segInfos);
                if (ret < 0) {
                    HandleCloneError(task);
                    return;
                }
                task->SetProgress(kProgressCreateCloneChunk);
                break;
            case CloneStep::kCompleteCloneMeta:
                ret = CompleteCloneMeta(task, newFileInfo, segInfos);
                if (ret < 0) {
                    HandleCloneError(task);
                    return;
                }
                break;
            case CloneStep::kRecoverChunk:
                ret = RecoverChunk(task, newFileInfo, segInfos);
                if (ret < 0) {
                    HandleCloneError(task);
                    return;
                }
                break;
            case CloneStep::kRenameCloneFile:
                ret = RenameCloneFile(task, newFileInfo);
                if (ret < 0) {
                    HandleCloneError(task);
                    return;
                }
                break;
            case CloneStep::kCompleteCloneFile:
                ret = CompleteCloneFile(task, newFileInfo, segInfos);
                if (ret < 0) {
                    HandleCloneError(task);
                    return;
                }
                break;
            default:
                LOG(ERROR) << "can not reach here.";
                HandleCloneError(task);
                return;
        }
        step = task->GetCloneInfo().GetNextStep();
    }
    HandleCloneSuccess(task);
}

int CloneCoreImpl::BuildFileInfoFromSnapshot(
    std::shared_ptr<CloneTaskInfo> task,
    FInfo *newFileInfo,
    CloneSegmentMap *segInfos) {
    segInfos->clear();
    UUID source = task->GetCloneInfo().GetSrc();

    SnapshotInfo snapInfo;
    int ret = metaStore_->GetSnapshotInfo(source, &snapInfo);
    if (ret < 0) {
        LOG(ERROR) << "GetSnapshotInfo error"
                   << ", source = " << source;
        return ret;
    }
    newFileInfo->chunksize = snapInfo.GetChunkSize();
    newFileInfo->segmentsize = snapInfo.GetSegmentSize();
    newFileInfo->length = snapInfo.GetFileLength();
    if (IsRecover(task)) {
        FInfo fInfo;
        std::string destination = task->GetCloneInfo().GetDest();
        std::string user = task->GetCloneInfo().GetUser();
        ret = client_->GetFileInfo(destination, user, &fInfo);
        if (ret != LIBCURVE_ERROR::OK) {
            LOG(ERROR) << "GetFileInfo fail"
                       << ", ret = " << ret
                       << ", destination = " << destination
                       << ", user = " << user;
            return kErrCodeInternalError;
        }
        // 从快照恢复的destinationId为目标文件的id
        task->GetCloneInfo().SetDestId(fInfo.id);
        // 从快照恢复seqnum+1
        newFileInfo->seqnum = fInfo.seqnum + 1;
    } else {
        newFileInfo->seqnum = kInitializeSeqNum;
    }
    newFileInfo->owner = task->GetCloneInfo().GetUser();

    ChunkIndexDataName indexName(snapInfo.GetFileName(),
         snapInfo.GetSeqNum());
    ChunkIndexData snapMeta;
    ret = dataStore_->GetChunkIndexData(indexName, &snapMeta);
    if (ret < 0) {
         LOG(ERROR) << "GetChunkIndexData error"
                    << ", fileName = " << snapInfo.GetFileName()
                    << ", seqNum = " << snapInfo.GetSeqNum();
         return ret;
    }

    uint64_t segmentSize = snapInfo.GetSegmentSize();
    uint64_t chunkSize = snapInfo.GetChunkSize();
    uint64_t chunkPerSegment = segmentSize / chunkSize;

    std::vector<ChunkIndexType> chunkIndexs =
        snapMeta.GetAllChunkIndex();
    for (auto &chunkIndex : chunkIndexs) {
        ChunkDataName chunkDataName;
        snapMeta.GetChunkDataName(chunkIndex, &chunkDataName);
        uint64_t segmentIndex = chunkIndex / chunkPerSegment;
        CloneChunkInfo info;
        info.location = chunkDataName.ToDataChunkKey();
        if (IsRecover(task)) {
            info.seqNum = chunkDataName.chunkSeqNum_;
        } else {
            info.seqNum = kInitializeSeqNum;
        }

        auto it = segInfos->find(segmentIndex);
        if (it == segInfos->end()) {
            CloneSegmentInfo segInfo;
            segInfo.emplace(chunkIndex, info);
            segInfos->emplace(segmentIndex, segInfo);
        } else {
            it->second.emplace(chunkIndex, info);
        }
    }
    return kErrCodeSuccess;
}

int CloneCoreImpl::BuildFileInfoFromFile(
    std::shared_ptr<CloneTaskInfo> task,
    FInfo *newFileInfo,
    CloneSegmentMap *segInfos) {
    segInfos->clear();
    UUID source = task->GetCloneInfo().GetSrc();
    std::string user = task->GetCloneInfo().GetUser();

    FInfo fInfo;
    // TODO(xuchaojie): 从文件克隆的source暂使用文件名，设计后续改为UUID
    int ret = client_->GetFileInfo(source, user, &fInfo);
    if (ret != LIBCURVE_ERROR::OK) {
        LOG(ERROR) << "GetFileInfo fail"
                   << ", ret = " << ret
                   << ", source = " << source
                   << ", user = " << user;
        return kErrCodeInternalError;
    }

    newFileInfo->chunksize = fInfo.chunksize;
    newFileInfo->segmentsize = fInfo.segmentsize;
    newFileInfo->length = fInfo.length;
    newFileInfo->seqnum = kInitializeSeqNum;
    newFileInfo->owner = task->GetCloneInfo().GetUser();

    uint64_t fileLength = fInfo.length;
    uint64_t segmentSize = fInfo.segmentsize;
    uint64_t chunkSize = fInfo.chunksize;

    if (0 == segmentSize) {
        LOG(ERROR) << "GetFileInfo return invalid fileInfo, segmentSize == 0";
        return kErrCodeInternalError;
    }
    if (fileLength%segmentSize != 0) {
        LOG(ERROR) << "GetFileInfo return invalid fileInfo, "
                   << "fileLength is not align to SegmentSize.";
        return kErrCodeInternalError;
    }

    for (uint64_t i = 0; i< fileLength/segmentSize; i++) {
        uint64_t offset = i * segmentSize;
        SegmentInfo segInfoOut;
        ret = client_->GetOrAllocateSegmentInfo(
                false, offset, &fInfo, user, &segInfoOut);
        if (ret != LIBCURVE_ERROR::OK &&
            ret != LIBCURVE_ERROR::NOT_ALLOCATE) {
            LOG(ERROR) << "GetOrAllocateSegmentInfo fail"
                       << ", ret = " << ret
                       << ", filename = " << source
                       << ", user = " << user
                       << ", offset = " << offset
                       << ", allocateIfNotExist = " << "false";
            return kErrCodeInternalError;
        }
        if (segInfoOut.chunkvec.size() != 0) {
            CloneSegmentInfo segInfo;
            for (std::vector<ChunkIDInfo>::size_type j = 0;
                    j < segInfoOut.chunkvec.size(); j++) {
                CloneChunkInfo info;
                info.location = offset + j * chunkSize;
                info.seqNum = kInitializeSeqNum;
                segInfo.emplace(j, info);
            }
            segInfos->emplace(i, segInfo);
        }
    }
    return kErrCodeSuccess;
}


int CloneCoreImpl::CreateCloneFile(
    std::shared_ptr<CloneTaskInfo> task,
    const FInfo &fInfo) {
    std::string fileName =
        cloneTempDir_ + "/" + task->GetCloneInfo().GetTaskId();
    std::string user = fInfo.owner;
    uint64_t fileLength = fInfo.length;
    uint64_t seqNum = fInfo.seqnum;
    uint32_t chunkSize = fInfo.chunksize;

    FInfo fInfoOut;
    int ret = client_->CreateCloneFile(fileName,
        user, fileLength, seqNum, chunkSize, &fInfoOut);
    if (ret != LIBCURVE_ERROR::OK &&
        ret != LIBCURVE_ERROR::EXISTS) {
        LOG(ERROR) << "CreateCloneFile file"
                   << ", ret = " << ret
                   << ", destination = " << fileName
                   << ", user = " << user
                   << ", fileLength = " << fileLength
                   << ", seqNum = " << seqNum
                   << ", chunkSize = " << chunkSize
                   << ", return fileId = " << fInfoOut.id;
        return kErrCodeInternalError;
    }
    task->GetCloneInfo().SetOriginId(fInfoOut.id);
    if (IsClone(task)) {
        // 克隆情况下destinationId = originId;
        task->GetCloneInfo().SetDestId(fInfoOut.id);
    }
    task->GetCloneInfo().SetNextStep(CloneStep::kCreateCloneMeta);
    ret = metaStore_->UpdateCloneInfo(task->GetCloneInfo());
    if (ret < 0) {
        LOG(ERROR) << "UpdateCloneInfo after CreateCloneFile error."
                   << " ret = " << ret;
        return ret;
    }
    return kErrCodeSuccess;
}

int CloneCoreImpl::CreateCloneMeta(
    std::shared_ptr<CloneTaskInfo> task,
    FInfo *fInfo,
    CloneSegmentMap *segInfos) {
    int ret = CreateOrUpdateCloneMeta(task, fInfo, segInfos);
    if (ret < 0) {
        return ret;
    }
    task->GetCloneInfo().SetNextStep(CloneStep::kCreateCloneChunk);
    ret = metaStore_->UpdateCloneInfo(task->GetCloneInfo());
    if (ret < 0) {
        LOG(ERROR) << "UpdateCloneInfo after CreateCloneMeta error."
                   << " ret = " << ret;
        return ret;
    }
    return kErrCodeSuccess;
}

int CloneCoreImpl::CreateCloneChunk(
    std::shared_ptr<CloneTaskInfo> task,
    const FInfo &fInfo,
    const CloneSegmentMap &segInfos) {
    int ret = kErrCodeSuccess;
    uint32_t chunkSize = fInfo.chunksize;
    uint32_t correctSn = fInfo.seqnum;
    for (auto & cloneSegmentInfo : segInfos) {
        for (auto & cloneChunkInfo : cloneSegmentInfo.second) {
            std::string location;
            if (IsSnapshot(task)) {
                location = LocationOperator::GenerateS3Location(
                    cloneChunkInfo.second.location);
            } else {
                location = LocationOperator::GenerateCurveLocation(
                    task->GetCloneInfo().GetDest(),
                    std::stoull(cloneChunkInfo.second.location));
            }
            ChunkIDInfo cidInfo = cloneChunkInfo.second.chunkIdInfo;
            ret = client_->CreateCloneChunk(location,
                cidInfo,
                cloneChunkInfo.second.seqNum,
                correctSn,
                chunkSize);
            if (ret != LIBCURVE_ERROR::OK) {
                LOG(ERROR) << "CreateCloneChunk fail"
                           << ", ret = " << ret
                           << ", location = " << location
                           << ", logicalPoolId = " << cidInfo.lpid_
                           << ", copysetId = " << cidInfo.cpid_
                           << ", chunkId = " << cidInfo.cid_
                           << ", seqNum = " << cloneChunkInfo.second.seqNum
                           << ", csn = " << correctSn;
                return kErrCodeInternalError;
            }
        }
    }
    task->GetCloneInfo().SetNextStep(CloneStep::kCompleteCloneMeta);
    ret = metaStore_->UpdateCloneInfo(task->GetCloneInfo());
    if (ret < 0) {
        LOG(ERROR) << "UpdateCloneInfo after CreateCloneChunk error."
                   << " ret = " << ret;
        return ret;
    }
    return kErrCodeSuccess;
}

int CloneCoreImpl::CompleteCloneMeta(
    std::shared_ptr<CloneTaskInfo> task,
    const FInfo &fInfo,
    const CloneSegmentMap &segInfos) {
    std::string origin =
        cloneTempDir_ + "/" + task->GetCloneInfo().GetTaskId();
    std::string user = task->GetCloneInfo().GetUser();
    int ret = client_->CompleteCloneMeta(origin, user);
    if (ret != LIBCURVE_ERROR::OK) {
        LOG(ERROR) << "CompleteCloneMeta fail"
                   << ", ret = " << ret
                   << ", filename = " << origin
                   << ", user = " << user;
        return kErrCodeInternalError;
    }
    if (IsLazy(task)) {
        task->GetCloneInfo().SetNextStep(
            CloneStep::kRenameCloneFile);
    } else {
        task->GetCloneInfo().SetNextStep(
            CloneStep::kRecoverChunk);
    }
    ret = metaStore_->UpdateCloneInfo(task->GetCloneInfo());
    if (ret < 0) {
        LOG(ERROR) << "UpdateCloneInfo after CompleteCloneMeta error."
                   << " ret = " << ret;
        return ret;
    }
    return kErrCodeSuccess;
}

int CloneCoreImpl::RecoverChunk(
    std::shared_ptr<CloneTaskInfo> task,
    const FInfo &fInfo,
    const CloneSegmentMap &segInfos) {
    int ret = kErrCodeSuccess;
    uint32_t chunkSize = fInfo.chunksize;

    uint32_t totalProgress =
        kProgressRecoverChunkEnd - kProgressRecoverChunkBegin;
    uint32_t segNum = segInfos.size();
    double progressPerData = static_cast<double>(totalProgress) / segNum;
    uint32_t index = 0;

    for (auto & cloneSegmentInfo : segInfos) {
        for (auto & cloneChunkInfo : cloneSegmentInfo.second) {
            ChunkIDInfo cidInfo = cloneChunkInfo.second.chunkIdInfo;

            if (0 == cloneChunkSplitSize_ ||
                chunkSize % cloneChunkSplitSize_ != 0) {
                LOG(ERROR) << "chunk is not align to cloneChunkSplitSize";
                return kErrCodeChunkSizeNotAligned;
            }
            uint64_t splitSize = chunkSize / cloneChunkSplitSize_;
            for (uint64_t i = 0; i < splitSize; i++) {
                uint64_t offset = i * cloneChunkSplitSize_;
                ret = client_->RecoverChunk(cidInfo,
                    offset,
                    cloneChunkSplitSize_);
                if (ret != LIBCURVE_ERROR::OK) {
                    LOG(ERROR) << "RecoverChunk fail"
                               << ", ret = " << ret
                               << ", logicalPoolId = " << cidInfo.lpid_
                               << ", copysetId = " << cidInfo.cpid_
                               << ", chunkId = " << cidInfo.cid_
                               << ", offset = " << offset
                               << ", len = " << cloneChunkSplitSize_;
                    return kErrCodeInternalError;
                }
            }
        }
        task->SetProgress(static_cast<uint32_t>(
            kProgressRecoverChunkBegin + index * progressPerData));
        index++;
    }
    task->GetCloneInfo().SetNextStep(CloneStep::kCompleteCloneFile);
    ret = metaStore_->UpdateCloneInfo(task->GetCloneInfo());
    if (ret < 0) {
        LOG(ERROR) << "UpdateCloneInfo after RecoverChunk error."
                   << " ret = " << ret;
        return ret;
    }
    return kErrCodeSuccess;
}

int CloneCoreImpl::RenameCloneFile(
    std::shared_ptr<CloneTaskInfo> task,
    const FInfo &fInfo) {
    std::string user = fInfo.owner;
    uint64_t originId = task->GetCloneInfo().GetOriginId();
    uint64_t destinationId = task->GetCloneInfo().GetDestId();
    std::string origin =
        cloneTempDir_ + "/" + task->GetCloneInfo().GetTaskId();
    std::string destination = task->GetCloneInfo().GetDest();

    // 判断原始文件存不存在
    FInfo oriFInfo;
    int ret = client_->GetFileInfo(origin, user, &oriFInfo);

    if (LIBCURVE_ERROR::OK == ret) {
        if (oriFInfo.id != originId) {
            LOG(ERROR) << "Origin File is missing, when rename clone file, "
                       << "exist file id = " << oriFInfo.id
                       << ", originId = " << originId;
            return kErrCodeInternalError;
        }
        ret = client_->RenameCloneFile(user,
            originId,
            destinationId,
            origin,
            destination);
        if (ret != LIBCURVE_ERROR::OK) {
            LOG(ERROR) << "RenameCloneFile fail"
                       << ", ret = " << ret
                       << ", user = " << user
                       << ", originId = " << originId
                       << ", origin = " << origin
                       << ", destination = " << destination;
            return kErrCodeInternalError;
        }
    } else if (LIBCURVE_ERROR::NOTEXIST == ret) {
        // 有可能是已经rename过了
        FInfo destFInfo;
        ret = client_->GetFileInfo(destination, user, &destFInfo);
        if (ret != LIBCURVE_ERROR::OK) {
            LOG(ERROR) << "Origin File is missing, when rename clone file,"
                       << "GetFileInfo fail, ret = " << ret
                       << ", destination filename = " << destination;
            return kErrCodeInternalError;
        }
        if (destFInfo.id != originId) {
            LOG(ERROR) << "Origin File is missing, when rename clone file, "
                       << "originId = " << originId;
            return kErrCodeInternalError;
        }
    } else {
            LOG(ERROR) << "GetFileInfo fail, ret = " << ret
                       << ", origin filename = " << origin;
            return ret;
    }

    if (IsLazy(task)) {
        task->GetCloneInfo().SetNextStep(CloneStep::kRecoverChunk);
    } else {
        task->GetCloneInfo().SetNextStep(CloneStep::kEnd);
    }
    ret = metaStore_->UpdateCloneInfo(task->GetCloneInfo());
    if (ret < 0) {
        LOG(ERROR) << "UpdateCloneInfo after RenameCloneFile error."
                   << " ret = " << ret;
        return ret;
    }
    return kErrCodeSuccess;
}

int CloneCoreImpl::CompleteCloneFile(
    std::shared_ptr<CloneTaskInfo> task,
    const FInfo &fInfo,
    const CloneSegmentMap &segInfos) {
    std::string fileName;
    if (IsLazy(task)) {
        fileName = task->GetCloneInfo().GetDest();
    } else {
        fileName =
            cloneTempDir_ + "/" + task->GetCloneInfo().GetTaskId();
    }
    std::string user = task->GetCloneInfo().GetUser();
    int ret = client_->CompleteCloneFile(fileName, user);
    if (ret != LIBCURVE_ERROR::OK) {
        LOG(ERROR) << "CompleteCloneFile fail"
                   << ", ret = " << ret
                   << ", fileName = " << fileName
                   << ", user = " << user;
        return kErrCodeInternalError;
    }
    if (IsLazy(task)) {
        task->GetCloneInfo().SetNextStep(CloneStep::kEnd);
    } else {
        task->GetCloneInfo().SetNextStep(CloneStep::kRenameCloneFile);
    }
    ret = metaStore_->UpdateCloneInfo(task->GetCloneInfo());
    if (ret < 0) {
        LOG(ERROR) << "UpdateCloneInfo after CompleteCloneFile error."
                   << " ret = " << ret;
        return ret;
    }
    return kErrCodeSuccess;
}

void CloneCoreImpl::HandleCloneSuccess(std::shared_ptr<CloneTaskInfo> task) {
    task->Lock();
    if (IsSnapshot(task)) {
        snapshotRef_->DecrementSnapshotRef(task->GetCloneInfo().GetSrc());
    }
    task->GetCloneInfo().SetStatus(CloneStatus::done);
    metaStore_->UpdateCloneInfo(task->GetCloneInfo());
    task->SetProgress(kProgressCloneComplete);

    task->Finish();
    task->UnLock();
    LOG(INFO) << "Task Success"
              << ", taskid = " << task->GetCloneInfo().GetTaskId();
    return;
}

void CloneCoreImpl::HandleCloneError(std::shared_ptr<CloneTaskInfo> task) {
    if (IsSnapshot(task)) {
        snapshotRef_->DecrementSnapshotRef(task->GetCloneInfo().GetSrc());
    }
    task->GetCloneInfo().SetStatus(CloneStatus::error);
    metaStore_->UpdateCloneInfo(task->GetCloneInfo());
    task->Finish();
    LOG(ERROR) << "Task Fail"
               << ", taskid = " << task->GetCloneInfo().GetTaskId()
               << ", step = "
               << static_cast<int>(task->GetCloneInfo().GetNextStep());
    return;
}

void CloneCoreImpl::HandleCleanSuccess(std::shared_ptr<CloneTaskInfo> task) {
    TaskIdType taskId = task->GetCloneInfo().GetTaskId();
    int ret = metaStore_->DeleteCloneInfo(taskId);
    if (ret < 0) {
        LOG(ERROR) << "DeleteCloneInfo failed"
                   << ", ret = " << ret
                   << ", taskId = " << taskId;
    } else {
        LOG(INFO) << "Clean Clone or Recover Task Success"
                  << ", taskid = " << task->GetCloneInfo().GetTaskId();
    }
    task->SetProgress(kProgressCloneComplete);

    task->Finish();
    return;
}

void CloneCoreImpl::HandleCleanError(std::shared_ptr<CloneTaskInfo> task) {
    task->GetCloneInfo().SetStatus(CloneStatus::error);
    metaStore_->UpdateCloneInfo(task->GetCloneInfo());
    task->Finish();
    LOG(ERROR) << "Clean Clone Or Recover Task Fail"
               << ", taskid = " << task->GetCloneInfo().GetTaskId();
    return;
}

int CloneCoreImpl::GetCloneInfoList(std::vector<CloneInfo> *taskList) {
    metaStore_->GetCloneInfoList(taskList);
    return kErrCodeSuccess;
}

int CloneCoreImpl::GetCloneInfo(TaskIdType taskId, CloneInfo *cloneInfo) {
    return metaStore_->GetCloneInfo(taskId, cloneInfo);
}

inline bool CloneCoreImpl::IsLazy(std::shared_ptr<CloneTaskInfo> task) {
    return task->GetCloneInfo().GetIsLazy();
}

inline bool CloneCoreImpl::IsSnapshot(std::shared_ptr<CloneTaskInfo> task) {
    return CloneFileType::kSnapshot == task->GetCloneInfo().GetFileType();
}

inline bool CloneCoreImpl::IsFile(std::shared_ptr<CloneTaskInfo> task) {
    return CloneFileType::kFile == task->GetCloneInfo().GetFileType();
}

inline bool CloneCoreImpl::IsRecover(std::shared_ptr<CloneTaskInfo> task) {
    return CloneTaskType::kRecover == task->GetCloneInfo().GetTaskType();
}

inline bool CloneCoreImpl::IsClone(std::shared_ptr<CloneTaskInfo> task) {
    return CloneTaskType::kClone == task->GetCloneInfo().GetTaskType();
}

bool CloneCoreImpl::NeedUpdateCloneMeta(
    std::shared_ptr<CloneTaskInfo> task) {
    bool ret = true;
    CloneStep step = task->GetCloneInfo().GetNextStep();
    if (CloneStep::kCreateCloneFile == step ||
        CloneStep::kCreateCloneMeta == step ||
        CloneStep::kEnd == step) {
        ret = false;
    }
    return ret;
}

int CloneCoreImpl::CreateOrUpdateCloneMeta(
    std::shared_ptr<CloneTaskInfo> task,
    FInfo *fInfo,
    CloneSegmentMap *segInfos) {
    std::string newFileName =
        cloneTempDir_ + "/" + task->GetCloneInfo().GetTaskId();
    std::string user = fInfo->owner;
    FInfo fInfoOut;
    int ret = client_->GetFileInfo(newFileName, user, &fInfoOut);
    if (ret != LIBCURVE_ERROR::OK) {
        LOG(ERROR) << "GetFileInfo fail"
                   << ", ret = " << ret
                   << ", filename = " << newFileName
                   << ", user = " << user;
        return kErrCodeInternalError;
    }
    // 更新fInfo
    *fInfo = fInfoOut;

    uint32_t segmentSize = fInfo->segmentsize;
    for (auto &segInfo : *segInfos) {
        SegmentInfo segInfoOut;
        uint64_t offset = segInfo.first * segmentSize;
        ret = client_->GetOrAllocateSegmentInfo(
            true, offset, fInfo, user, &segInfoOut);
        if (ret != LIBCURVE_ERROR::OK) {
            LOG(ERROR) << "GetOrAllocateSegmentInfo fail"
                       << ", newFileName = " << newFileName
                       << ", user = " << user
                       << ", offset = " << offset
                       << ", allocateIfNotExist = " << "true";
            return kErrCodeInternalError;
        }

        for (std::vector<ChunkIDInfo>::size_type i = 0;
             i < segInfoOut.chunkvec.size(); i++) {
            auto it = segInfo.second.find(i);
            if (it != segInfo.second.end()) {
                it->second.chunkIdInfo = segInfoOut.chunkvec[i];
            }
        }
    }
    return kErrCodeSuccess;
}

int CloneCoreImpl::CleanCloneOrRecoverTaskPre(const std::string &user,
    const TaskIdType &taskId,
    CloneInfo *cloneInfo) {
    int ret = metaStore_->GetCloneInfo(taskId, cloneInfo);
    if (ret < 0) {
        return kErrCodeFileNotExist;
    }
    if (cloneInfo->GetUser() != user) {
        LOG(ERROR) << "CleanCloneOrRecoverTaskPre by Invalid user";
        return kErrCodeInvalidUser;
    }
    switch (cloneInfo->GetStatus()) {
        case CloneStatus::cleaning:
            return kErrCodeTaskExist;
            break;
        case CloneStatus::error:
            cloneInfo->SetStatus(CloneStatus::cleaning);
            break;
        default:
            LOG(ERROR) << "Can not clean clone/recover task is not error.";
            return kErrCodeCannotCleanCloneNotError;
            break;
    }

    ret = metaStore_->UpdateCloneInfo(*cloneInfo);
    if (ret < 0) {
        LOG(ERROR) << "UpdateCloneInfo fail"
                   << ", ret = " << ret
                   << ", taskId = " << taskId;
        return ret;
    }
    return kErrCodeSuccess;
}

void CloneCoreImpl::HandleCleanCloneOrRecoverTask(
    std::shared_ptr<CloneTaskInfo> task) {
    std::string tempFileName =
        cloneTempDir_ + "/" + task->GetCloneInfo().GetTaskId();
    std::string destFileName =
        task->GetCloneInfo().GetDest();
    uint64_t fileId = task->GetCloneInfo().GetOriginId();
    std::string user =
        task->GetCloneInfo().GetUser();
    int ret = client_->DeleteFile(tempFileName, user, fileId);
    if (ret != LIBCURVE_ERROR::OK &&
        ret != LIBCURVE_ERROR::NOTEXIST) {
        LOG(ERROR) << "DeleteFile failed"
                   << ", ret = " << ret
                   << ", fileName = " << tempFileName
                   << ", user = " << user
                   << ", fileId = " << fileId;
        HandleCleanError(task);
        return;
    }
    // recover及lazy情况下不能删除destfile
    if (IsClone(task) && (!IsLazy(task))) {
        ret = client_->DeleteFile(destFileName, user, fileId);
        if (ret != LIBCURVE_ERROR::OK &&
            ret != LIBCURVE_ERROR::NOTEXIST) {
            LOG(ERROR) << "DeleteFile failed"
                       << ", ret = " << ret
                       << ", fileName = " << destFileName
                       << ", user = " << user
                       << ", fileId = " << fileId;
            HandleCleanError(task);
            return;
        }
    }
    // TODO(xuchaojie): lazy情况下，recoverchunk失败的情况
    // 需要考虑标坏文件等措施，详细方案待讨论

    HandleCleanSuccess(task);
    return;
}

}  // namespace snapshotcloneserver
}  // namespace curve
