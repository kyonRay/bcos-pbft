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
 * @brief implementation for PBFTNewViewMsg
 * @file PBFTNewViewMsg.h
 * @author: yujiechen
 * @date 2021-04-16
 */
#pragma once
#include "pbft/protocol/PB/PBFTBaseMessage.h"
#include "pbft/protocol/interfaces/NewViewMsgInterface.h"
#include "pbft/protocol/interfaces/ViewChangeMsgInterface.h"

namespace bcos
{
namespace consensus
{
class PBFTNewViewMsg : public PBFTBaseMessage, NewViewMsgInterface
{
public:
    using Ptr = std::shared_ptr<PBFTNewViewMsg>;
    PBFTNewViewMsg() : PBFTBaseMessage()
    {
        m_rawNewView = std::make_shared<RawNewViewMessage>();
        m_rawNewView->set_allocated_message(PBFTBaseMessage::baseMessage().get());
        m_viewChangeList = std::make_shared<ViewChangeMsgList>();
        m_packetType = PacketType::NewViewPacket;
    }
    explicit PBFTNewViewMsg(bytesConstRef _data) : PBFTBaseMessage()
    {
        m_rawNewView = std::make_shared<RawNewViewMessage>();
        m_viewChangeList = std::make_shared<ViewChangeMsgList>();
        m_packetType = PacketType::NewViewPacket;
        decode(_data);
    }

    ~PBFTNewViewMsg() override
    {
        // return back the ownership of message to the PBFTBaseMessage
        m_rawNewView->unsafe_arena_release_message();
        // return back the ownership to m_generatedPreprepare
        m_rawNewView->unsafe_arena_release_generatedpreprepare();
        // return back the ownership to m_viewChangeList
        auto viewChangeSize = m_rawNewView->viewchangemsglist_size();
        for (auto i = 0; i < viewChangeSize; i++)
        {
            m_rawNewView->mutable_viewchangemsglist()->UnsafeArenaReleaseLast();
        }
    }

    bytesPointer encode(bcos::crypto::CryptoSuite::Ptr _cryptoSuite,
        bcos::crypto::KeyPairInterface::Ptr _keyPair) const override;
    void decode(bytesConstRef _data) override;

    void setViewChangeMsgList(ViewChangeMsgList const& _viewChangeMsgList) override;
    void setGeneratedPrePrepare(PBFTBaseMessageInterface::Ptr _prePreparedMsg) override;

    ViewChangeMsgList const& viewChangeMsgList() const override { return *m_viewChangeList; }
    PBFTBaseMessageInterface::Ptr generatedPrePrepare() override { return m_generatedPreprepare; }

protected:
    void deserializeToObject() override;

private:
    std::shared_ptr<RawNewViewMessage> m_rawNewView;
    // required and need to be verified
    ViewChangeMsgListPtr m_viewChangeList;
    // optional
    PBFTBaseMessageInterface::Ptr m_generatedPreprepare = nullptr;
};
}  // namespace consensus
}  // namespace bcos