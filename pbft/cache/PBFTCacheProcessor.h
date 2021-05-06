/**
 *  Copyright (C) 2021 FISCO BCOS.
 *  SPDX-License-Identifier: Apache-2.0
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * @brief cache processor for the PBFTReq
 * @file PBFTCacheProcessor.h
 * @author: yujiechen
 * @date 2021-04-21
 */
#pragma once
#include "pbft/cache/PBFTCache.h"
#include "pbft/config/PBFTConfig.h"
#include "pbft/interfaces/PBFTMessageFactory.h"
#include "pbft/interfaces/PBFTMessageInterface.h"
#include "pbft/interfaces/ViewChangeMsgInterface.h"
#include <queue>
namespace bcos
{
namespace consensus
{
class PBFTCacheProcessor
{
public:
    using Ptr = std::shared_ptr<PBFTCacheProcessor>;
    explicit PBFTCacheProcessor(PBFTConfig::Ptr _config) : m_config(_config) {}

    virtual ~PBFTCacheProcessor() {}

    virtual void addPrePrepareCache(PBFTMessageInterface::Ptr _prePrepareMsg);
    virtual bool existPrePrepare(PBFTMessageInterface::Ptr _prePrepareMsg);

    virtual bool tryToFillProposal(PBFTMessageInterface::Ptr _prePrepareMsg);

    virtual bool conflictWithProcessedReq(PBFTMessageInterface::Ptr _msg);
    virtual bool conflictWithPrecommitReq(PBFTMessageInterface::Ptr _prePrepareMsg);
    virtual void addPrepareCache(PBFTMessageInterface::Ptr _prepareReq)
    {
        addCache(m_caches, _prepareReq,
            [](PBFTCache::Ptr _pbftCache, PBFTMessageInterface::Ptr _prepareReq) {
                _pbftCache->addPrepareCache(_prepareReq);
            });
    }

    virtual void addCommitReq(PBFTMessageInterface::Ptr _commitReq)
    {
        addCache(m_caches, _commitReq,
            [](PBFTCache::Ptr _pbftCache, PBFTMessageInterface::Ptr _commitReq) {
                _pbftCache->addCommitCache(_commitReq);
            });
    }

    PBFTMessageList preCommitCachesWithData()
    {
        PBFTMessageList precommitCacheList;
        for (auto const& it : m_caches)
        {
            auto precommitCache = it.second->preCommitCache();
            if (precommitCache != nullptr)
            {
                precommitCacheList.push_back(precommitCache);
            }
        }
        return precommitCacheList;
    }

    PBFTMessageList preCommitCachesWithoutData()
    {
        PBFTMessageList precommitCacheList;
        for (auto const& it : m_caches)
        {
            auto precommitCache = it.second->preCommitWithoutData();
            if (precommitCache != nullptr)
            {
                precommitCacheList.push_back(precommitCache);
            }
        }
        return precommitCacheList;
    }

    virtual bool checkAndPreCommit();
    virtual bool checkAndCommit();

    virtual void addViewChangeReq(ViewChangeMsgInterface::Ptr _viewChange);
    virtual NewViewMsgInterface::Ptr checkAndTryIntoNewView();

    // TODO: clear the expired cache periodically
    virtual void clearExpiredCache() {}
    // TODO: remove invalid viewchange
    virtual void removeInvalidViewChange() {}
    virtual bytesPointer fetchPrecommitData(ViewChangeMsgInterface::Ptr _pbftMessage,
        bcos::protocol::BlockNumber _index, bcos::crypto::HashType const& _hash);

    bool checkPrecommitMsg(PBFTMessageInterface::Ptr _precommitMsg);

private:
    using PBFTCachesType = std::map<bcos::protocol::BlockNumber, PBFTCache::Ptr>;

    using UpdateCacheHandler =
        std::function<void(PBFTCache::Ptr _pbftCache, PBFTMessageInterface::Ptr _pbftMessage)>;
    void addCache(PBFTCachesType& _pbftCache, PBFTMessageInterface::Ptr _pbftReq,
        UpdateCacheHandler _handler);

    PBFTMessageList generatePrePrepareMsg(
        std::map<IndexType, ViewChangeMsgInterface::Ptr> _viewChangeCache);

private:
    PBFTConfig::Ptr m_config;
    PBFTCachesType m_caches;

    // viewchange caches
    using ViewChangeCacheType =
        std::map<ViewType, std::map<IndexType, ViewChangeMsgInterface::Ptr>>;
    ViewChangeCacheType m_viewChangeCache;
    std::map<ViewType, uint64_t> m_viewChangeWeight;
    std::map<ViewType, int64_t> m_maxCommittedIndex;
    std::map<ViewType, int64_t> m_maxPrecommitIndex;
};
}  // namespace consensus
}  // namespace bcos