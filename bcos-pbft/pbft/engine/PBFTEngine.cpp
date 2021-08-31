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
 * @brief implementation for PBFTEngine
 * @file PBFTEngine.cpp
 * @author: yujiechen
 * @date 2021-04-20
 */
#include "PBFTEngine.h"
#include "../cache/PBFTCacheFactory.h"
#include "../cache/PBFTCacheProcessor.h"
#include <bcos-framework/interfaces/ledger/LedgerConfig.h>
#include <bcos-framework/interfaces/protocol/Protocol.h>
#include <bcos-framework/libutilities/ThreadPool.h>
#include <boost/bind/bind.hpp>
using namespace bcos;
using namespace bcos::consensus;
using namespace bcos::ledger;
using namespace bcos::front;
using namespace bcos::crypto;
using namespace bcos::protocol;

PBFTEngine::PBFTEngine(PBFTConfig::Ptr _config)
  : ConsensusEngine("pbft", 0),
    m_config(_config),
    m_worker(std::make_shared<ThreadPool>("pbftWorker", 1)),
    m_msgQueue(std::make_shared<PBFTMsgQueue>())
{
    auto cacheFactory = std::make_shared<PBFTCacheFactory>();
    m_cacheProcessor = std::make_shared<PBFTCacheProcessor>(cacheFactory, _config);
    m_logSync = std::make_shared<PBFTLogSync>(m_config, m_cacheProcessor);
    // register the timeout function
    m_config->timer()->registerTimeoutHandler(boost::bind(&PBFTEngine::onTimeout, this));
    m_config->storage()->registerFinalizeHandler(boost::bind(
        &PBFTEngine::finalizeConsensus, this, boost::placeholders::_1, boost::placeholders::_2));
    m_cacheProcessor->registerProposalAppliedHandler(boost::bind(&PBFTEngine::onProposalApplied,
        this, boost::placeholders::_1, boost::placeholders::_2, boost::placeholders::_3));

    m_cacheProcessor->registerOnLoadAndVerifyProposalSucc(
        boost::bind(&PBFTEngine::onLoadAndVerifyProposalSucc, this, boost::placeholders::_1));
    initSendResponseHandler();
    // when the node first setup, set timeout to be true for view recovery
    // set timeout to be true to in case of notify-seal before the PBFTEngine started
    m_config->setTimeoutState(true);
}

void PBFTEngine::initSendResponseHandler()
{
    // set the sendResponse callback
    std::weak_ptr<FrontServiceInterface> weakFrontService = m_config->frontService();
    m_sendResponseHandler = [weakFrontService](std::string const& _id, int _moduleID,
                                NodeIDPtr _dstNode, bytesConstRef _data) {
        try
        {
            auto frontService = weakFrontService.lock();
            if (!frontService)
            {
                return;
            }
            frontService->asyncSendResponse(
                _id, _moduleID, _dstNode, _data, [_id, _moduleID, _dstNode](Error::Ptr _error) {
                    if (_error)
                    {
                        PBFT_LOG(WARNING) << LOG_DESC("sendResonse failed") << LOG_KV("uuid", _id)
                                          << LOG_KV("module", std::to_string(_moduleID))
                                          << LOG_KV("dst", _dstNode->shortHex())
                                          << LOG_KV("code", _error->errorCode())
                                          << LOG_KV("msg", _error->errorMessage());
                    }
                });
        }
        catch (std::exception const& e)
        {
            PBFT_LOG(WARNING) << LOG_DESC("sendResonse exception")
                              << LOG_KV("error", boost::diagnostic_information(e))
                              << LOG_KV("uuid", _id) << LOG_KV("moduleID", _moduleID)
                              << LOG_KV("peer", _dstNode->shortHex());
        }
    };
}

void PBFTEngine::start()
{
    ConsensusEngine::start();
    // when the node setup, start the timer for view recovery
    m_config->timer()->start();
}

void PBFTEngine::stop()
{
    ConsensusEngine::stop();
    if (m_worker)
    {
        m_worker->stop();
    }
    if (m_logSync)
    {
        m_logSync->stop();
    }
    if (m_config)
    {
        m_config->stop();
    }
    m_cacheProcessor->clearAll();
}

void PBFTEngine::onLoadAndVerifyProposalSucc(PBFTProposalInterface::Ptr _proposal)
{
    // must add lock here to ensure thread-safe
    Guard l(m_mutex);
    m_cacheProcessor->updateCommitQueue(_proposal);
}

void PBFTEngine::onProposalApplyFailed(PBFTProposalInterface::Ptr _proposal)
{
    PBFT_LOG(WARNING) << LOG_DESC("proposal execute failed") << printPBFTProposal(_proposal)
                      << m_config->printCurrentState();
    // Note: must add lock here to ensure thread-safe
    Guard l(m_mutex);
    // re-push the proposal into the queue
    if (_proposal->index() >= m_config->committedProposal()->index() ||
        _proposal->index() >= m_config->syncingHighestNumber())
    {
        if (m_config->timer()->running())
        {
            m_config->timer()->restart();
        }
        PBFT_LOG(INFO) << LOG_DESC(
                              "proposal execute failed and re-push the proposal "
                              "into the cache")
                       << printPBFTProposal(_proposal);
        m_cacheProcessor->updateCommitQueue(_proposal);
    }
    m_config->setExpectedCheckPoint(m_config->committedProposal()->index() + 1);
    m_cacheProcessor->eraseExecutedProposal(_proposal->hash());
    return;
}

void PBFTEngine::onProposalApplySuccess(
    PBFTProposalInterface::Ptr _proposal, PBFTProposalInterface::Ptr _executedProposal)
{
    // commit the proposal when execute success
    m_config->storage()->asyncCommitProposal(_proposal);

    // broadcast checkpoint message
    auto checkPointMsg = m_config->pbftMessageFactory()->populateFrom(PacketType::CheckPoint,
        m_config->pbftMsgDefaultVersion(), m_config->view(), utcTime(), m_config->nodeIndex(),
        _executedProposal, m_config->cryptoSuite(), m_config->keyPair(), true);

    auto encodedData = m_config->codec()->encode(checkPointMsg);
    m_config->frontService()->asyncSendMessageByNodeIDs(
        ModuleID::PBFT, m_config->consensusNodeIDList(), ref(*encodedData));
    // Note: must lock here to ensure thread safe
    Guard l(m_mutex);
    // restart the timer when proposal execute finished to in case of timeout
    if (m_config->timer()->running())
    {
        m_config->timer()->restart();
    }
    m_cacheProcessor->addCheckPointMsg(checkPointMsg);
    m_cacheProcessor->setCheckPointProposal(_executedProposal);
    m_config->setExpectedCheckPoint(_executedProposal->index() + 1);
    m_cacheProcessor->checkAndCommitStableCheckPoint();
    m_cacheProcessor->tryToApplyCommitQueue();
    m_cacheProcessor->eraseExecutedProposal(_proposal->hash());
}

// called after proposal executed successfully
void PBFTEngine::onProposalApplied(bool _execSuccess, PBFTProposalInterface::Ptr _proposal,
    PBFTProposalInterface::Ptr _executedProposal)
{
    auto self = std::weak_ptr<PBFTEngine>(shared_from_this());
    m_worker->enqueue([self, _execSuccess, _proposal, _executedProposal]() {
        try
        {
            auto engine = self.lock();
            if (!engine)
            {
                return;
            }
            if (!_execSuccess)
            {
                engine->onProposalApplyFailed(_proposal);
                return;
            }
            engine->onProposalApplySuccess(_proposal, _executedProposal);
        }
        catch (std::exception const& e)
        {
            PBFT_LOG(WARNING) << LOG_DESC("onProposalApplied exception")
                              << printPBFTProposal(_executedProposal)
                              << LOG_KV("error", boost::diagnostic_information(e));
        }
    });
}

void PBFTEngine::asyncSubmitProposal(bytesConstRef _proposalData, BlockNumber _proposalIndex,
    HashType const& _proposalHash, std::function<void(Error::Ptr)> _onProposalSubmitted)
{
    if (_onProposalSubmitted)
    {
        _onProposalSubmitted(nullptr);
    }
    onRecvProposal(_proposalData, _proposalIndex, _proposalHash);
}

void PBFTEngine::onRecvProposal(
    bytesConstRef _proposalData, BlockNumber _proposalIndex, HashType const& _proposalHash)
{
    if (_proposalData.size() == 0)
    {
        return;
    }
    // expired proposal
    auto consProposalIndex = m_config->committedProposal()->index() + 1;
    if (_proposalIndex <= m_config->syncingHighestNumber())
    {
        PBFT_LOG(WARNING) << LOG_DESC("asyncSubmitProposal failed for expired index")
                          << LOG_KV("index", _proposalIndex)
                          << LOG_KV("hash", _proposalHash.abridged())
                          << m_config->printCurrentState()
                          << LOG_KV("syncingHighestNumber", m_config->syncingHighestNumber());
        m_config->notifyResetSealing(consProposalIndex);
        return;
    }
    if (_proposalIndex <= m_config->committedProposal()->index() ||
        _proposalIndex < m_config->expectedCheckPoint() ||
        _proposalIndex < m_config->lowWaterMark())
    {
        PBFT_LOG(WARNING) << LOG_DESC("asyncSubmitProposal failed for invalid index")
                          << LOG_KV("index", _proposalIndex)
                          << LOG_KV("hash", _proposalHash.abridged())
                          << m_config->printCurrentState()
                          << LOG_KV("lowWaterMark", m_config->lowWaterMark());
        return;
    }
    auto leaderIndex = m_config->leaderIndex(_proposalIndex);
    if (leaderIndex != m_config->nodeIndex())
    {
        PBFT_LOG(WARNING) << LOG_DESC(
                                 "asyncSubmitProposal failed for the node-self is not the leader")
                          << LOG_KV("expectedLeader", leaderIndex)
                          << LOG_KV("index", _proposalIndex)
                          << LOG_KV("hash", _proposalHash.abridged())
                          << m_config->printCurrentState();
        m_config->notifyResetSealing(consProposalIndex);
        return;
    }
    if (m_config->timeout())
    {
        PBFT_LOG(WARNING) << LOG_DESC("onRecvProposal failed for timout now")
                          << LOG_KV("index", _proposalIndex)
                          << LOG_KV("hash", _proposalHash.abridged())
                          << m_config->printCurrentState();
        m_config->notifyResetSealing(m_config->committedProposal()->index() + 1);
        return;
    }
    PBFT_LOG(DEBUG) << LOG_DESC("asyncSubmitProposal") << LOG_KV("index", _proposalIndex)
                    << LOG_KV("hash", _proposalHash.abridged()) << m_config->printCurrentState();
    // generate the pre-prepare packet
    auto pbftProposal = m_config->pbftMessageFactory()->createPBFTProposal();
    pbftProposal->setData(_proposalData);
    pbftProposal->setIndex(_proposalIndex);
    pbftProposal->setHash(_proposalHash);
    pbftProposal->setSealerId(m_config->nodeIndex());

    auto pbftMessage =
        m_config->pbftMessageFactory()->populateFrom(PacketType::PrePreparePacket, pbftProposal,
            m_config->pbftMsgDefaultVersion(), m_config->view(), utcTime(), m_config->nodeIndex());

    // broadcast the pre-prepare packet
    auto encodedData = m_config->codec()->encode(pbftMessage);
    m_config->frontService()->asyncSendMessageByNodeIDs(
        ModuleID::PBFT, m_config->consensusNodeIDList(), ref(*encodedData));

    PBFT_LOG(INFO) << LOG_DESC("++++++++++++++++ Generating seal on")
                   << LOG_KV("index", pbftMessage->index()) << LOG_KV("Idx", m_config->nodeIndex())
                   << LOG_KV("hash", pbftMessage->hash().abridged());

    // handle the pre-prepare packet
    Guard l(m_mutex);
    auto ret = handlePrePrepareMsg(pbftMessage, false, false, false);
    if (!ret)
    {
        resetSealedTxs(pbftMessage);
    }
}

void PBFTEngine::resetSealedTxs(std::shared_ptr<PBFTMessageInterface> _prePrepareMsg)
{
    if (_prePrepareMsg->generatedFrom() != m_config->nodeIndex())
    {
        return;
    }
    m_config->notifyResetSealing(m_config->committedProposal()->index() + 1);
}

// receive the new block notification from the sync module
void PBFTEngine::asyncNotifyNewBlock(
    LedgerConfig::Ptr _ledgerConfig, std::function<void(Error::Ptr)> _onRecv)
{
    if (_onRecv)
    {
        _onRecv(nullptr);
    }
    if (m_config->shouldResetConfig(_ledgerConfig->blockNumber()))
    {
        PBFT_LOG(INFO) << LOG_DESC("The sync module notify the latestBlock")
                       << LOG_KV("index", _ledgerConfig->blockNumber())
                       << LOG_KV("hash", _ledgerConfig->hash().abridged());
        finalizeConsensus(_ledgerConfig, true);
    }
}

void PBFTEngine::onReceivePBFTMessage(
    bcos::Error::Ptr _error, std::string const& _id, NodeIDPtr _nodeID, bytesConstRef _data)
{
    auto self = std::weak_ptr<PBFTEngine>(shared_from_this());
    onReceivePBFTMessage(_error, _nodeID, _data, [_id, _nodeID, self](bytesConstRef _respData) {
        try
        {
            auto engine = self.lock();
            if (!engine)
            {
                return;
            }
            engine->m_sendResponseHandler(_id, ModuleID::PBFT, _nodeID, _respData);
        }
        catch (std::exception const& e)
        {
            PBFT_LOG(WARNING) << LOG_DESC("onReceivePBFTMessage exception")
                              << LOG_KV("fromNode", _nodeID->hex()) << LOG_KV("uuid", _id)
                              << LOG_KV("error", boost::diagnostic_information(e));
        }
    });
}

void PBFTEngine::onReceivePBFTMessage(Error::Ptr _error, NodeIDPtr _fromNode, bytesConstRef _data,
    std::function<void(bytesConstRef _respData)> _sendResponseCallback)
{
    try
    {
        if (_error != nullptr)
        {
            return;
        }
        // the node is not the consensusNode
        if (!m_config->isConsensusNode())
        {
            PBFT_LOG(TRACE) << LOG_DESC(
                "onReceivePBFTMessage: reject the message for the node is not the consensus "
                "node");
            return;
        }
        // decode the message and push the message into the queue
        auto pbftMsg = m_config->codec()->decode(_data);
        pbftMsg->setFrom(_fromNode);
        // the committed proposal request message
        if (pbftMsg->packetType() == PacketType::CommittedProposalRequest)
        {
            m_logSync->onReceiveCommittedProposalRequest(pbftMsg, _sendResponseCallback);
            return;
        }
        // the precommitted proposals request message
        if (pbftMsg->packetType() == PacketType::PreparedProposalRequest)
        {
            m_logSync->onReceivePrecommitRequest(pbftMsg, _sendResponseCallback);
            return;
        }
        m_msgQueue->push(pbftMsg);
    }
    catch (std::exception const& _e)
    {
        PBFT_LOG(WARNING) << LOG_DESC("onReceivePBFTMessage exception")
                          << LOG_KV("fromNode", _fromNode->hex())
                          << LOG_KV("Idx", m_config->nodeIndex())
                          << LOG_KV("nodeId", m_config->nodeID()->hex())
                          << LOG_KV("error", boost::diagnostic_information(_e));
    }
}

void PBFTEngine::executeWorker()
{
    // the node is not the consensusNode
    if (!m_config->isConsensusNode())
    {
        waitSignal();
        return;
    }
    // when the node is syncing, not handle the PBFT message
    if (m_config->committedProposal()->index() < m_config->syncingHighestNumber())
    {
        waitSignal();
        return;
    }
    // handle the PBFT message(here will wait when the msgQueue is empty)
    auto messageResult = m_msgQueue->tryPop(c_PopWaitSeconds);
    if (messageResult.first)
    {
        if (m_config->timeout() == true)
        {
            auto pbftMsg = messageResult.second;
            auto packetType = pbftMsg->packetType();
            // Pre-prepare, prepare and commit type message packets are not allowed to be processed
            // in the timeout state
            if (c_timeoutAllowedPacket.count(packetType))
            {
                handleMsg(pbftMsg);
            }
            // Re-insert unqualified messages into the queue
            else if (pbftMsg->index() > m_config->committedProposal()->index())
            {
                m_msgQueue->push(pbftMsg);
            }
        }
        else
        {
            handleMsg(messageResult.second);
        }
    }
    // wait for PBFTMsg
    else
    {
        waitSignal();
    }
}

void PBFTEngine::handleMsg(std::shared_ptr<PBFTBaseMessageInterface> _msg)
{
    Guard l(m_mutex);
    switch (_msg->packetType())
    {
    case PacketType::PrePreparePacket:
    {
        auto prePrepareMsg = std::dynamic_pointer_cast<PBFTMessageInterface>(_msg);
        handlePrePrepareMsg(prePrepareMsg, true);
        break;
    }
    case PacketType::PreparePacket:
    {
        auto prepareMsg = std::dynamic_pointer_cast<PBFTMessageInterface>(_msg);
        handlePrepareMsg(prepareMsg);
        break;
    }
    case PacketType::CommitPacket:
    {
        auto commitMsg = std::dynamic_pointer_cast<PBFTMessageInterface>(_msg);
        handleCommitMsg(commitMsg);
        break;
    }
    case PacketType::ViewChangePacket:
    {
        auto viewChangeMsg = std::dynamic_pointer_cast<ViewChangeMsgInterface>(_msg);
        handleViewChangeMsg(viewChangeMsg);
        break;
    }
    case PacketType::NewViewPacket:
    {
        auto newViewMsg = std::dynamic_pointer_cast<NewViewMsgInterface>(_msg);
        handleNewViewMsg(newViewMsg);
        break;
    }
    case PacketType::CheckPoint:
    {
        auto checkPointMsg = std::dynamic_pointer_cast<PBFTMessageInterface>(_msg);
        handleCheckPointMsg(checkPointMsg);
        break;
    }
    default:
    {
        PBFT_LOG(DEBUG) << LOG_DESC("handleMsg: unknown PBFT message")
                        << LOG_KV("type", std::to_string(_msg->packetType()))
                        << LOG_KV("genIdx", _msg->generatedFrom())
                        << LOG_KV("nodesef", m_config->nodeID()->hex());
        return;
    }
    }
}

CheckResult PBFTEngine::checkPBFTMsgState(PBFTMessageInterface::Ptr _pbftReq) const
{
    if (!_pbftReq->consensusProposal())
    {
        return CheckResult::INVALID;
    }
    if (_pbftReq->index() < m_config->lowWaterMark() ||
        _pbftReq->index() < m_config->expectedCheckPoint() ||
        _pbftReq->index() >= m_config->highWaterMark() ||
        _pbftReq->index() <= m_config->syncingHighestNumber())
    {
        PBFT_LOG(TRACE) << LOG_DESC("checkPBFTMsgState: invalid pbftMsg for invalid index")
                        << LOG_KV("highWaterMark", m_config->highWaterMark())
                        << printPBFTMsgInfo(_pbftReq) << m_config->printCurrentState()
                        << LOG_KV("syncingNumber", m_config->syncingHighestNumber());
        return CheckResult::INVALID;
    }
    // case index equal
    if (_pbftReq->view() != m_config->view())
    {
        PBFT_LOG(TRACE) << LOG_DESC("checkPBFTMsgState: invalid pbftMsg for invalid view")
                        << printPBFTMsgInfo(_pbftReq) << m_config->printCurrentState();
        return CheckResult::INVALID;
    }
    return CheckResult::VALID;
}

CheckResult PBFTEngine::checkPrePrepareMsg(std::shared_ptr<PBFTMessageInterface> _prePrepareMsg)
{
    if (checkPBFTMsgState(_prePrepareMsg) == CheckResult::INVALID)
    {
        return CheckResult::INVALID;
    }
    // check the existence of the msg
    if (m_cacheProcessor->existPrePrepare(_prePrepareMsg))
    {
        return CheckResult::INVALID;
    }
    // check conflict
    if (m_cacheProcessor->conflictWithPrecommitReq(_prePrepareMsg))
    {
        return CheckResult::INVALID;
    }
    return CheckResult::VALID;
}

CheckResult PBFTEngine::checkSignature(PBFTBaseMessageInterface::Ptr _req)
{
    // check the signature
    auto nodeInfo = m_config->getConsensusNodeByIndex(_req->generatedFrom());
    if (!nodeInfo)
    {
        PBFT_LOG(WARNING) << LOG_DESC("checkSignature failed for the node is not a consensus node")
                          << printPBFTMsgInfo(_req);
        return CheckResult::INVALID;
    }
    auto publicKey = nodeInfo->nodeID();
    if (!_req->verifySignature(m_config->cryptoSuite(), publicKey))
    {
        PBFT_LOG(WARNING) << LOG_DESC("checkSignature failed for invalid signature")
                          << printPBFTMsgInfo(_req);
        return CheckResult::INVALID;
    }
    return CheckResult::VALID;
}

bool PBFTEngine::checkProposalSignature(
    IndexType _generatedFrom, PBFTProposalInterface::Ptr _proposal)
{
    if (!_proposal || _proposal->signature().size() == 0)
    {
        return false;
    }
    auto nodeInfo = m_config->getConsensusNodeByIndex(_generatedFrom);
    if (!nodeInfo)
    {
        PBFT_LOG(WARNING)
            << LOG_DESC("checkProposalSignature failed for the node is not a consensus node")
            << printPBFTProposal(_proposal);
        return false;
    }

    return m_config->cryptoSuite()->signatureImpl()->verify(
        nodeInfo->nodeID(), _proposal->hash(), _proposal->signature());
}

bool PBFTEngine::handlePrePrepareMsg(PBFTMessageInterface::Ptr _prePrepareMsg,
    bool _needVerifyProposal, bool _generatedFromNewView, bool _needCheckSignature)
{
    PBFT_LOG(DEBUG) << LOG_DESC("handlePrePrepareMsg") << printPBFTMsgInfo(_prePrepareMsg)
                    << m_config->printCurrentState();

    if (m_config->committedProposal()->index() < m_config->syncingHighestNumber())
    {
        PBFT_LOG(WARNING)
            << LOG_DESC("handlePrePrepareMsg: reject the prePrepareMsg for the node is syncing")
            << LOG_KV("committedIndex", m_config->committedProposal()->index())
            << LOG_KV("recvIndex", _prePrepareMsg->index())
            << LOG_KV("hash", _prePrepareMsg->hash().abridged())
            << LOG_KV("syncingNum", m_config->syncingHighestNumber())
            << m_config->printCurrentState();
        return false;
    }
    auto result = checkPrePrepareMsg(_prePrepareMsg);
    if (result == CheckResult::INVALID)
    {
        return false;
    }
    if (!_generatedFromNewView)
    {
        // packet can be processed in this round of consensus
        // check the proposal is generated from the leader
        auto expectedLeader = m_config->leaderIndex(_prePrepareMsg->index());
        if (expectedLeader != _prePrepareMsg->generatedFrom())
        {
            PBFT_LOG(TRACE) << LOG_DESC(
                                   "handlePrePrepareMsg: invalid packet for not from the leader")
                            << printPBFTMsgInfo(_prePrepareMsg) << m_config->printCurrentState()
                            << LOG_KV("expectedLeader", expectedLeader);
            return false;
        }
        if (_needCheckSignature)
        {
            // check the signature
            result = checkSignature(_prePrepareMsg);
            if (result == CheckResult::INVALID)
            {
                m_config->notifySealer(_prePrepareMsg->index(), true);
                return false;
            }
        }
    }
    // add the prePrepareReq to the cache
    if (!_needVerifyProposal)
    {
        // Note: must reset the txs to be sealed no matter verify success or failed because
        // some nodes may verify failed for timeout, while other nodes may verify success
        m_config->validator()->asyncResetTxsFlag(_prePrepareMsg->consensusProposal()->data(), true);
        // add the pre-prepare packet into the cache
        m_cacheProcessor->addPrePrepareCache(_prePrepareMsg);
        m_config->timer()->restart();
        auto nextProposalIndex = _prePrepareMsg->index();
        if (nextProposalIndex <= m_config->highWaterMark() && !_generatedFromNewView)
        {
            m_config->notifySealer(nextProposalIndex);
        }
        // broadcast PrepareMsg the packet
        broadcastPrepareMsg(_prePrepareMsg);
        PBFT_LOG(DEBUG) << LOG_DESC("handlePrePrepareMsg and broadcast prepare packet")
                        << printPBFTMsgInfo(_prePrepareMsg) << m_config->printCurrentState();
        m_cacheProcessor->checkAndPreCommit();
        return true;
    }
    // verify the proposal
    auto self = std::weak_ptr<PBFTEngine>(shared_from_this());
    auto leaderNodeInfo = m_config->getConsensusNodeByIndex(_prePrepareMsg->generatedFrom());
    if (!leaderNodeInfo)
    {
        return false;
    }
    m_config->validator()->verifyProposal(leaderNodeInfo->nodeID(),
        _prePrepareMsg->consensusProposal(),
        [self, _prePrepareMsg, _generatedFromNewView](Error::Ptr _error, bool _verifyResult) {
            try
            {
                auto pbftEngine = self.lock();
                if (!pbftEngine)
                {
                    return;
                }
                // Note: must reset the txs to be sealed no matter verify success or failed because
                // some nodes may verify failed for timeout,  while other nodes may verify success
                pbftEngine->m_config->validator()->asyncResetTxsFlag(
                    _prePrepareMsg->consensusProposal()->data(), true);
                // verify exceptioned
                if (_error != nullptr)
                {
                    PBFT_LOG(WARNING) << LOG_DESC("verify proposal exceptioned")
                                      << printPBFTMsgInfo(_prePrepareMsg)
                                      << LOG_KV("errorCode", _error->errorCode())
                                      << LOG_KV("errorMsg", _error->errorMessage());
                    pbftEngine->m_config->notifySealer(_prePrepareMsg->index(), true);
                    return;
                }
                // verify failed
                if (!_verifyResult)
                {
                    PBFT_LOG(WARNING)
                        << LOG_DESC("verify proposal failed") << printPBFTMsgInfo(_prePrepareMsg);
                    pbftEngine->resetSealedTxs(_prePrepareMsg);
                    pbftEngine->m_config->notifySealer(_prePrepareMsg->index(), true);
                    return;
                }
                // verify success
                Guard l(pbftEngine->m_mutex);
                pbftEngine->handlePrePrepareMsg(
                    _prePrepareMsg, false, _generatedFromNewView, false);
            }
            catch (std::exception const& _e)
            {
                PBFT_LOG(WARNING) << LOG_DESC("exception when calls onVerifyFinishedHandler")
                                  << printPBFTMsgInfo(_prePrepareMsg)
                                  << LOG_KV("error", boost::diagnostic_information(_e));
            }
        });
    return true;
}

void PBFTEngine::broadcastPrepareMsg(PBFTMessageInterface::Ptr _prePrepareMsg)
{
    auto prepareMsg = m_config->pbftMessageFactory()->populateFrom(PacketType::PreparePacket,
        m_config->pbftMsgDefaultVersion(), m_config->view(), utcTime(), m_config->nodeIndex(),
        _prePrepareMsg->consensusProposal(), m_config->cryptoSuite(), m_config->keyPair());
    prepareMsg->setIndex(_prePrepareMsg->index());
    // add the message to local cache
    m_cacheProcessor->addPrepareCache(prepareMsg);

    auto encodedData = m_config->codec()->encode(prepareMsg, m_config->pbftMsgDefaultVersion());
    // only broadcast to the consensus nodes
    m_config->frontService()->asyncSendMessageByNodeIDs(
        ModuleID::PBFT, m_config->consensusNodeIDList(), ref(*encodedData));
    // try to precommit the message
    m_cacheProcessor->checkAndPreCommit();
}


CheckResult PBFTEngine::checkPBFTMsg(std::shared_ptr<PBFTMessageInterface> _prepareMsg)
{
    auto result = checkPBFTMsgState(_prepareMsg);
    if (result == CheckResult::INVALID)
    {
        return result;
    }
    if (_prepareMsg->generatedFrom() == m_config->nodeIndex())
    {
        PBFT_LOG(TRACE) << LOG_DESC("checkPrepareMsg: Recv own req")
                        << printPBFTMsgInfo(_prepareMsg);
        return CheckResult::INVALID;
    }
    // check the existence of Pre-Prepare request
    if (m_cacheProcessor->existPrePrepare(_prepareMsg))
    {
        // compare with the local pre-prepare cache
        if (m_cacheProcessor->conflictWithProcessedReq(_prepareMsg))
        {
            return CheckResult::INVALID;
        }
    }
    return checkSignature(_prepareMsg);
}

bool PBFTEngine::handlePrepareMsg(PBFTMessageInterface::Ptr _prepareMsg)
{
    PBFT_LOG(TRACE) << LOG_DESC("handlePrepareMsg") << printPBFTMsgInfo(_prepareMsg)
                    << m_config->printCurrentState();
    auto result = checkPBFTMsg(_prepareMsg);
    if (result == CheckResult::INVALID)
    {
        return false;
    }
    if (!checkProposalSignature(_prepareMsg->generatedFrom(), _prepareMsg->consensusProposal()))
    {
        return false;
    }
    m_cacheProcessor->addPrepareCache(_prepareMsg);
    m_cacheProcessor->checkAndPreCommit();
    return true;
}

bool PBFTEngine::handleCommitMsg(PBFTMessageInterface::Ptr _commitMsg)
{
    PBFT_LOG(TRACE) << LOG_DESC("handleCommitMsg") << printPBFTMsgInfo(_commitMsg)
                    << m_config->printCurrentState();
    auto result = checkPBFTMsg(_commitMsg);
    if (result == CheckResult::INVALID)
    {
        return false;
    }
    m_cacheProcessor->addCommitReq(_commitMsg);
    m_cacheProcessor->checkAndCommit();
    return true;
}

void PBFTEngine::onTimeout()
{
    Guard l(m_mutex);
    m_cacheProcessor->clearExpiredExecutingProposal();
    // when some proposals are executing, not trigger timeout
    auto executingProposalSize = m_cacheProcessor->executingProposalSize();
    if (executingProposalSize > 0)
    {
        PBFT_LOG(INFO) << LOG_DESC("onTimeout: Proposal is executing, resetart the timer")
                       << LOG_KV("executingProposalSize", executingProposalSize)
                       << m_config->printCurrentState();
        m_config->timer()->restart();
        return;
    }
    m_config->resetTimeoutState();
    // clear the viewchange cache
    m_cacheProcessor->removeInvalidViewChange(
        m_config->view(), m_config->committedProposal()->index());
    // notify the latest proposal index to the sync module when timeout to enable syncing
    m_cacheProcessor->notifyCommittedProposalIndex(m_config->committedProposal()->index());
    // broadcast viewchange and try to the new-view phase
    broadcastViewChangeReq();
    PBFT_LOG(WARNING) << LOG_DESC("onTimeout") << m_config->printCurrentState();
}

ViewChangeMsgInterface::Ptr PBFTEngine::generateViewChange()
{
    // broadcast the viewChangeReq
    auto committedProposal = m_config->populateCommittedProposal();
    if (committedProposal == nullptr)
    {
        PBFT_LOG(WARNING) << LOG_DESC(
            "broadcastViewChangeReq failed for the latest storage state has not been loaded.");
    }
    auto viewChangeReq = m_config->pbftMessageFactory()->createViewChangeMsg();
    viewChangeReq->setHash(m_config->committedProposal()->hash());
    viewChangeReq->setIndex(m_config->committedProposal()->index());
    viewChangeReq->setPacketType(PacketType::ViewChangePacket);
    viewChangeReq->setVersion(m_config->pbftMsgDefaultVersion());
    viewChangeReq->setView(m_config->toView());
    viewChangeReq->setTimestamp(utcTime());
    viewChangeReq->setGeneratedFrom(m_config->nodeIndex());
    // set the committed proposal
    viewChangeReq->setCommittedProposal(committedProposal);
    // set prepared proposals
    viewChangeReq->setPreparedProposals(m_cacheProcessor->preCommitCachesWithoutData());
    return viewChangeReq;
}

void PBFTEngine::sendViewChange(bcos::crypto::NodeIDPtr _dstNode)
{
    auto viewChangeReq = generateViewChange();
    // encode and broadcast the viewchangeReq
    auto encodedData = m_config->codec()->encode(viewChangeReq);
    // only broadcast to the consensus nodes
    m_config->frontService()->asyncSendMessageByNodeID(
        ModuleID::PBFT, _dstNode, ref(*encodedData), 0, nullptr);
    // collect the viewchangeReq
    m_cacheProcessor->addViewChangeReq(viewChangeReq);
    auto newViewMsg = m_cacheProcessor->checkAndTryIntoNewView();
    if (newViewMsg)
    {
        reHandlePrePrepareProposals(newViewMsg);
    }
}

void PBFTEngine::broadcastViewChangeReq()
{
    auto viewChangeReq = generateViewChange();
    // encode and broadcast the viewchangeReq
    auto encodedData = m_config->codec()->encode(viewChangeReq);
    // only broadcast to the consensus nodes
    m_config->frontService()->asyncSendMessageByNodeIDs(
        ModuleID::PBFT, m_config->consensusNodeIDList(), ref(*encodedData));
    PBFT_LOG(INFO) << LOG_DESC("broadcastViewChangeReq") << printPBFTMsgInfo(viewChangeReq);
    // collect the viewchangeReq
    m_cacheProcessor->addViewChangeReq(viewChangeReq);
    auto newViewMsg = m_cacheProcessor->checkAndTryIntoNewView();
    if (newViewMsg)
    {
        reHandlePrePrepareProposals(newViewMsg);
    }
}

bool PBFTEngine::isValidViewChangeMsg(
    std::shared_ptr<ViewChangeMsgInterface> _viewChangeMsg, bool _shouldCheckSig)
{
    // check the committed-proposal index
    if (_viewChangeMsg->committedProposal()->index() < m_config->committedProposal()->index())
    {
        PBFT_LOG(DEBUG) << LOG_DESC("InvalidViewChangeReq: invalid index")
                        << printPBFTMsgInfo(_viewChangeMsg) << m_config->printCurrentState();
        return false;
    }
    // check the view
    if (_viewChangeMsg->view() < m_config->view())
    {
        PBFT_LOG(DEBUG) << LOG_DESC("send viewchange to the node whose view falling behind")
                        << LOG_KV("dst", _viewChangeMsg->from()->shortHex())
                        << printPBFTMsgInfo(_viewChangeMsg) << m_config->printCurrentState();
        sendViewChange(_viewChangeMsg->from());
        return false;
    }
    // check the committed proposal hash
    if (_viewChangeMsg->committedProposal()->index() == m_config->committedProposal()->index() &&
        _viewChangeMsg->committedProposal()->hash() != m_config->committedProposal()->hash())
    {
        PBFT_LOG(DEBUG) << LOG_DESC("InvalidViewChangeReq: conflict with local committedProposal")
                        << LOG_DESC("received proposal")
                        << printPBFTProposal(_viewChangeMsg->committedProposal())
                        << LOG_DESC("local committedProposal")
                        << printPBFTProposal(m_config->committedProposal());
        return false;
    }
    // check the precommmitted proposals
    for (auto precommitMsg : _viewChangeMsg->preparedProposals())
    {
        if (!m_cacheProcessor->checkPrecommitMsg(precommitMsg))
        {
            PBFT_LOG(DEBUG) << LOG_DESC("InvalidViewChangeReq for invalid proposal")
                            << printPBFTMsgInfo(precommitMsg) << m_config->printCurrentState();
            return false;
        }
    }
    if (!_shouldCheckSig)
    {
        return true;
    }
    auto ret = checkSignature(_viewChangeMsg);
    if (ret == CheckResult::INVALID)
    {
        PBFT_LOG(DEBUG) << LOG_DESC("InvalidViewChangeReq: invalid signature")
                        << printPBFTMsgInfo(_viewChangeMsg) << m_config->printCurrentState();
        return false;
    }
    return true;
}

bool PBFTEngine::handleViewChangeMsg(ViewChangeMsgInterface::Ptr _viewChangeMsg)
{
    auto ret = checkSignature(_viewChangeMsg);
    if (ret == CheckResult::INVALID)
    {
        return false;
    }
    // receive the viewchange message from the leader
    if (!m_config->timeout() && _viewChangeMsg->view() > m_config->view() &&
        _viewChangeMsg->generatedFrom() == m_config->leaderIndex(m_config->progressedIndex()))
    {
        m_config->resetTimeoutState();
        // clear the viewchange cache
        m_cacheProcessor->removeInvalidViewChange(
            m_config->view(), m_config->committedProposal()->index());
        broadcastViewChangeReq();
        PBFT_LOG(INFO) << LOG_DESC(
                              "Receive the viewchange from the leader, try to trigger viewchange")
                       << m_config->printCurrentState();
    }
    if (!isValidViewChangeMsg(_viewChangeMsg, false))
    {
        return false;
    }
    // TODO: sync the proposal when the committedProposal is older than the index of the viewchange
    m_cacheProcessor->addViewChangeReq(_viewChangeMsg);
    // try to trigger fast view change if receive more than (f+1) valid view change messages whose
    // view is greater than the current view:
    // sends a view-change message for the smallest view in the set, even if its timer has not
    // expired
    auto view = m_cacheProcessor->tryToTriggerFastViewChange();
    if (view > 0)
    {
        broadcastViewChangeReq();
    }
    auto newViewMsg = m_cacheProcessor->checkAndTryIntoNewView();
    if (newViewMsg)
    {
        reHandlePrePrepareProposals(newViewMsg);
        return true;
    }
    return true;
}

bool PBFTEngine::isValidNewViewMsg(std::shared_ptr<NewViewMsgInterface> _newViewMsg)
{
    // check the newViewMsg
    auto expectedLeader = m_config->leaderIndexInNewViewPeriod(_newViewMsg->view());
    if (expectedLeader != _newViewMsg->generatedFrom())
    {
        PBFT_LOG(DEBUG) << LOG_DESC("InvalidNewViewMsg for invalid nextLeader")
                        << LOG_KV("expectedLeader", expectedLeader)
                        << LOG_KV("recvIdx", _newViewMsg->generatedFrom())
                        << m_config->printCurrentState();
        return false;
    }
    if (_newViewMsg->view() <= m_config->view())
    {
        PBFT_LOG(DEBUG) << LOG_DESC("InvalidNewViewMsg for invalid view")
                        << printPBFTMsgInfo(_newViewMsg) << m_config->printCurrentState();
        return false;
    }
    // check the viewchange
    uint64_t weight = 0;
    auto viewChangeList = _newViewMsg->viewChangeMsgList();
    for (auto viewChangeReq : viewChangeList)
    {
        if (!isValidViewChangeMsg(viewChangeReq))
        {
            PBFT_LOG(DEBUG) << LOG_DESC("InvalidNewViewMsg for viewChange check failed")
                            << printPBFTMsgInfo(viewChangeReq);
            return false;
        }
        auto nodeInfo = m_config->getConsensusNodeByIndex(viewChangeReq->generatedFrom());
        if (!nodeInfo)
        {
            continue;
        }
        weight += nodeInfo->weight();
    }
    // TODO: need to ensure the accuracy of local weight parameters
    if (weight < m_config->minRequiredQuorum())
    {
        PBFT_LOG(DEBUG) << LOG_DESC("InvalidNewViewMsg for unenough weight")
                        << LOG_KV("weight", weight)
                        << LOG_KV("minRequiredQuorum", m_config->minRequiredQuorum());
        return false;
    }
    // TODO: check the prePrepared message
    auto ret = checkSignature(_newViewMsg);
    if (ret == CheckResult::INVALID)
    {
        return false;
    }
    return true;
}

bool PBFTEngine::handleNewViewMsg(NewViewMsgInterface::Ptr _newViewMsg)
{
    PBFT_LOG(DEBUG) << LOG_DESC("handleNewViewMsg: receive newViewChangeMsg")
                    << printPBFTMsgInfo(_newViewMsg) << m_config->printCurrentState() << std::endl;
    if (!isValidNewViewMsg(_newViewMsg))
    {
        return false;
    }
    PBFT_LOG(DEBUG) << LOG_DESC("handleNewViewMsg success") << printPBFTMsgInfo(_newViewMsg)
                    << m_config->printCurrentState() << std::endl;
    reHandlePrePrepareProposals(_newViewMsg);
    return true;
}

void PBFTEngine::reachNewView(ViewType _view)
{
    m_config->resetNewViewState(_view);
    m_cacheProcessor->resetCacheAfterViewChange(
        m_config->view(), m_config->committedProposal()->index());
    PBFT_LOG(DEBUG) << LOG_DESC("reachNewView") << m_config->printCurrentState();
}

void PBFTEngine::reHandlePrePrepareProposals(NewViewMsgInterface::Ptr _newViewReq)
{
    reachNewView(_newViewReq->view());
    // note the sealer to reset after viewchange
    m_config->notifyResetSealing();
    auto const& prePrepareList = _newViewReq->prePrepareList();
    auto maxProposalIndex = m_config->committedProposal()->index();
    for (auto prePrepare : prePrepareList)
    {
        if (prePrepare->index() > maxProposalIndex)
        {
            maxProposalIndex = prePrepare->index();
        }
        // empty block proposal
        if (prePrepare->consensusProposal()->data().size() > 0)
        {
            PBFT_LOG(DEBUG) << LOG_DESC("reHandlePrePrepareProposals: hit the proposal")
                            << printPBFTMsgInfo(prePrepare) << m_config->printCurrentState();
            handlePrePrepareMsg(prePrepare, true, true, false);
            continue;
        }
        // hit the cache
        if (m_cacheProcessor->tryToFillProposal(prePrepare))
        {
            PBFT_LOG(DEBUG)
                << LOG_DESC(
                       "reHandlePrePrepareProposals: hit the cache, into prepare phase directly")
                << printPBFTMsgInfo(prePrepare) << m_config->printCurrentState();
            handlePrePrepareMsg(prePrepare, true, true, false);
            continue;
        }
        // miss the cache, request to from node
        auto from = m_config->getConsensusNodeByIndex(prePrepare->generatedFrom());
        m_logSync->requestPrecommitData(
            from->nodeID(), prePrepare, [this](PBFTMessageInterface::Ptr _prePrepare) {
                PBFT_LOG(DEBUG)
                    << LOG_DESC(
                           "reHandlePrePrepareProposals: get the missed proposal and handle now")
                    << printPBFTMsgInfo(_prePrepare) << m_config->printCurrentState();
                Guard l(m_mutex);
                handlePrePrepareMsg(_prePrepare, true, true, false);
            });
    }
    // re-notify the new leader to seal block
    m_config->reNotifySealer(maxProposalIndex + 1);
}

void PBFTEngine::finalizeConsensus(LedgerConfig::Ptr _ledgerConfig, bool _syncedBlock)
{
    Guard l(m_mutex);
    // resetConfig after submit the block to ledger
    m_config->resetConfig(_ledgerConfig, _syncedBlock);
    // tried to commit the stable checkpoint
    m_cacheProcessor->removeConsensusedCache(m_config->view(), _ledgerConfig->blockNumber());
    m_cacheProcessor->tryToApplyCommitQueue();
    m_cacheProcessor->tryToCommitStableCheckPoint();
    m_cacheProcessor->resetTimer();
    if (_syncedBlock)
    {
        // Note: should reNotifySealer or not?
        m_cacheProcessor->removeFutureProposals();
    }
}

bool PBFTEngine::handleCheckPointMsg(std::shared_ptr<PBFTMessageInterface> _checkPointMsg)
{
    // check index
    if (_checkPointMsg->index() <= m_config->committedProposal()->index())
    {
        PBFT_LOG(WARNING) << LOG_DESC("handleCheckPointMsg: Invalid expired checkpoint msg")
                          << LOG_KV("committedIndex", m_config->committedProposal()->index())
                          << LOG_KV("recvIndex", _checkPointMsg->index())
                          << LOG_KV("hash", _checkPointMsg->hash().abridged())
                          << m_config->printCurrentState();
        return false;
    }
    if (m_config->committedProposal()->index() < m_config->syncingHighestNumber())
    {
        PBFT_LOG(WARNING) << LOG_DESC(
                                 "handleCheckPointMsg: reject the checkPoint for the node is "
                                 "syncing higher block")
                          << LOG_KV("committedIndex", m_config->committedProposal()->index())
                          << LOG_KV("recvIndex", _checkPointMsg->index())
                          << LOG_KV("hash", _checkPointMsg->hash().abridged())
                          << LOG_KV("syncingNum", m_config->syncingHighestNumber())
                          << m_config->printCurrentState();
        return false;
    }
    // check signature
    auto result = checkSignature(_checkPointMsg);
    if (result == CheckResult::INVALID)
    {
        PBFT_LOG(WARNING) << LOG_DESC("handleCheckPointMsg: invalid signature")
                          << printPBFTMsgInfo(_checkPointMsg);
        return false;
    }
    // check the proposal signature
    if (!checkProposalSignature(
            _checkPointMsg->generatedFrom(), _checkPointMsg->consensusProposal()))
    {
        PBFT_LOG(WARNING) << LOG_DESC("handleCheckPointMsg: invalid  proposal signature")
                          << printPBFTMsgInfo(_checkPointMsg);
        return false;
    }
    PBFT_LOG(INFO) << LOG_DESC(
                          "handleCheckPointMsg: try to add the checkpoint message into the cache")
                   << printPBFTMsgInfo(_checkPointMsg) << m_config->printCurrentState();
    m_cacheProcessor->addCheckPointMsg(_checkPointMsg);
    m_cacheProcessor->checkAndCommitStableCheckPoint();
    if (m_cacheProcessor->shouldRequestCheckPoint(_checkPointMsg->index()))
    {
        PBFT_LOG(INFO) << LOG_DESC("request checkPoint proposal")
                       << LOG_KV("checkPointIndex", _checkPointMsg->index())
                       << LOG_KV("checkPointHash", _checkPointMsg->hash().abridged())
                       << m_config->printCurrentState();
        m_logSync->requestCommittedProposals(_checkPointMsg->from(), _checkPointMsg->index(), 1);
    }
    return true;
}