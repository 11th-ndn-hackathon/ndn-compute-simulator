/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2019,  Regents of the University of California,
 *                           Arizona Board of Regents,
 *                           Colorado State University,
 *                           University Pierre & Marie Curie, Sorbonne University,
 *                           Washington University in St. Louis,
 *                           Beijing Institute of Technology,
 *                           The University of Memphis.
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "forwarder.hpp"

#include "algorithm.hpp"
#include "best-route-strategy2.hpp"
#include "strategy.hpp"
#include "common/global.hpp"
#include "common/logger.hpp"
#include "table/cleanup.hpp"

#include <ndn-cxx/lp/tags.hpp>

#include "face/null-face.hpp"

#include <ns3/node-list.h>
#include <ns3/node.h>

#include "ns3/core-module.h"

#include <boost/algorithm/string.hpp>

namespace nfd {

NFD_LOG_INIT(Forwarder);

static Name
getDefaultStrategyName()
{
  return fw::BestRouteStrategy2::getStrategyName();
}

Forwarder::Forwarder(FaceTable& faceTable)
  : m_faceTable(faceTable)
  , m_unsolicitedDataPolicy(make_unique<fw::DefaultUnsolicitedDataPolicy>())
  , m_fib(m_nameTree)
  , m_pit(m_nameTree)
  , m_measurements(m_nameTree)
  , m_strategyChoice(*this)
  , m_csFace(face::makeNullFace(FaceUri("contentstore://")))
{
  m_faceTable.addReserved(m_csFace, face::FACEID_CONTENT_STORE);

  m_faceTable.afterAdd.connect([this] (const Face& face) {
    face.afterReceiveInterest.connect(
      [this, &face] (const Interest& interest, const EndpointId& endpointId) {
        this->startProcessInterest(FaceEndpoint(face, endpointId), interest);
      });
    face.afterReceiveData.connect(
      [this, &face] (const Data& data, const EndpointId& endpointId) {
        this->startProcessData(FaceEndpoint(face, endpointId), data);
      });
    face.afterReceiveNack.connect(
      [this, &face] (const lp::Nack& nack, const EndpointId& endpointId) {
        this->startProcessNack(FaceEndpoint(face, endpointId), nack);
      });
    face.onDroppedInterest.connect(
      [this, &face] (const Interest& interest) {
        this->onDroppedInterest(FaceEndpoint(face, 0), interest);
      });
  });

  m_faceTable.beforeRemove.connect([this] (const Face& face) {
    cleanupOnFaceRemoval(m_nameTree, m_fib, m_pit, face);
  });

  m_fib.afterNewNextHop.connect([&] (const Name& prefix, const fib::NextHop& nextHop) {
    this->startProcessNewNextHop(prefix, nextHop);
  });

  m_strategyChoice.setDefaultStrategy(getDefaultStrategyName());
}

Forwarder::~Forwarder() = default;

void
Forwarder::onIncomingInterest(const FaceEndpoint& ingress, const Interest& interest)
{
  // receive Interest
  NFD_LOG_DEBUG("onIncomingInterest in=" << ingress << " interest=" << interest.getName());
  interest.setTag(make_shared<lp::IncomingFaceIdTag>(ingress.face.getId()));
  ++m_counters.nInInterests;

  // /localhost scope control
  bool isViolatingLocalhost = ingress.face.getScope() == ndn::nfd::FACE_SCOPE_NON_LOCAL &&
                              scope_prefix::LOCALHOST.isPrefixOf(interest.getName());
  if (isViolatingLocalhost) {
    NFD_LOG_DEBUG("onIncomingInterest in=" << ingress
                  << " interest=" << interest.getName() << " violates /localhost");
    // (drop)
    return;
  }

  // detect duplicate Nonce with Dead Nonce List
  bool hasDuplicateNonceInDnl = m_deadNonceList.has(interest.getName(), interest.getNonce());
  if (hasDuplicateNonceInDnl) {
    // goto Interest loop pipeline
    this->onInterestLoop(ingress, interest);
    return;
  }

  // strip forwarding hint if Interest has reached producer region
  if (!interest.getForwardingHint().empty() &&
      m_networkRegionTable.isInProducerRegion(interest.getForwardingHint())) {
    NFD_LOG_DEBUG("onIncomingInterest in=" << ingress
                  << " interest=" << interest.getName() << " reaching-producer-region");
    const_cast<Interest&>(interest).setForwardingHint({});
  }

  // PIT insert
  shared_ptr<pit::Entry> pitEntry = m_pit.insert(interest).first;

  // detect duplicate Nonce in PIT entry
  int dnw = fw::findDuplicateNonce(*pitEntry, interest.getNonce(), ingress.face);
  bool hasDuplicateNonceInPit = dnw != fw::DUPLICATE_NONCE_NONE;
  if (ingress.face.getLinkType() == ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
    // for p2p face: duplicate Nonce from same incoming face is not loop
    hasDuplicateNonceInPit = hasDuplicateNonceInPit && !(dnw & fw::DUPLICATE_NONCE_IN_SAME);
  }
  if (hasDuplicateNonceInPit) {
    // goto Interest loop pipeline
    this->onInterestLoop(ingress, interest);
    this->dispatchToStrategy(*pitEntry,
      [&] (fw::Strategy& strategy) { strategy.afterReceiveLoopedInterest(ingress, interest, *pitEntry); });
    return;
  }

  // is pending?
  if (!pitEntry->hasInRecords()) {
    m_cs.find(interest,
              bind(&Forwarder::onContentStoreHit, this, ingress, pitEntry, _1, _2),
              bind(&Forwarder::onContentStoreMiss, this, ingress, pitEntry, _1));
  }
  else {
    this->onContentStoreMiss(ingress, pitEntry, interest);
  }
}

void
Forwarder::onInterestLoop(const FaceEndpoint& ingress, const Interest& interest)
{
  // if multi-access or ad hoc face, drop
  if (ingress.face.getLinkType() != ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
    NFD_LOG_DEBUG("onInterestLoop in=" << ingress
                  << " interest=" << interest.getName() << " drop");
    return;
  }

  NFD_LOG_DEBUG("onInterestLoop in=" << ingress << " interest=" << interest.getName()
                << " send-Nack-duplicate");

  // send Nack with reason=DUPLICATE
  // note: Don't enter outgoing Nack pipeline because it needs an in-record.
  lp::Nack nack(interest);
  nack.setReason(lp::NackReason::DUPLICATE);
  ingress.face.sendNack(nack, ingress.endpoint);
}

void
Forwarder::onContentStoreMiss(const FaceEndpoint& ingress,
                              const shared_ptr<pit::Entry>& pitEntry, const Interest& interest)
{
  NFD_LOG_DEBUG("onContentStoreMiss interest=" << interest.getName());
  ++m_counters.nCsMisses;
  afterCsMiss(interest);

  // insert in-record
  pitEntry->insertOrUpdateInRecord(ingress.face, interest);

  // set PIT expiry timer to the time that the last PIT in-record expires
  auto lastExpiring = std::max_element(pitEntry->in_begin(), pitEntry->in_end(),
                                       [] (const auto& a, const auto& b) {
                                         return a.getExpiry() < b.getExpiry();
                                       });
  auto lastExpiryFromNow = lastExpiring->getExpiry() - time::steady_clock::now();
  this->setExpiryTimer(pitEntry, time::duration_cast<time::milliseconds>(lastExpiryFromNow));

  // has NextHopFaceId?
  auto nextHopTag = interest.getTag<lp::NextHopFaceIdTag>();
  if (nextHopTag != nullptr) {
    // chosen NextHop face exists?
    Face* nextHopFace = m_faceTable.get(*nextHopTag);
    if (nextHopFace != nullptr) {
      NFD_LOG_DEBUG("onContentStoreMiss interest=" << interest.getName()
                    << " nexthop-faceid=" << nextHopFace->getId());
      // go to outgoing Interest pipeline
      // scope control is unnecessary, because privileged app explicitly wants to forward
      this->onOutgoingInterest(pitEntry, FaceEndpoint(*nextHopFace, 0), interest);
    }
    return;
  }

  // dispatch to strategy: after incoming Interest
  this->dispatchToStrategy(*pitEntry,
    [&] (fw::Strategy& strategy) {
      strategy.afterReceiveInterest(FaceEndpoint(ingress.face, 0), interest, pitEntry);
    });
}

void
Forwarder::onContentStoreHit(const FaceEndpoint& ingress, const shared_ptr<pit::Entry>& pitEntry,
                             const Interest& interest, const Data& data)
{
  NFD_LOG_DEBUG("onContentStoreHit interest=" << interest.getName());

  int node_id=GetCurrentNode()->GetId();
 
  int  service_id=-1;
  int  service_type=-1;
  int  run_time=-1;
  int  resource_value=-1;

  if(node_id!=0 && node_id!=1 && node_id!=3) // service cannot run on consumer or producer nodes
  {
  //cout<<"Content Store hit: Data Name"<<data.getName().toUri()<<" Node Id:  "<<node_id<<endl;

  shared_ptr<const Interest> interest_ptr= make_shared<Interest>(interest);
  tuple<int,int,int,int> service_info=StartServiceExecution(interest_ptr);

  service_id=get<0>(service_info);
  service_type=get<1>(service_info);
  run_time=get<2>(service_info);
  resource_value=get<3>(service_info);
  ns3::Time time = ns3::MilliSeconds (run_time);

  ns3::Ptr<ns3::Node> currentNode=GetCurrentNode();
  cout<<currentNode->GetId()<<"\t"<<service_id<<"\t\t"<<service_type<<"\t\t"<<resource_value<<"\t\t"<<run_time<<"\t\t\t"<<currentNode->m_inital_compute_resources<<"\t\t\t\t"<<currentNode->m_compute_resources<<"\t\t\t"<<currentNode->m_compute_capacity<<endl;
  // set event setting for pause service execution
    
    ns3::Simulator::Schedule (time, &Forwarder::ProcessContentStoreHit, this, ingress,pitEntry,interest,data,service_id,resource_value);
    
  }
  else
  {
    ++m_counters.nCsHits;
    afterCsHit(interest, data);
  
    data.setTag(make_shared<lp::IncomingFaceIdTag>(face::FACEID_CONTENT_STORE));
    // FIXME Should we lookup PIT for other Interests that also match the data?

  pitEntry->isSatisfied = true;
  pitEntry->dataFreshnessPeriod = data.getFreshnessPeriod();

  // set PIT expiry timer to now
  this->setExpiryTimer(pitEntry, 0_ms);

  beforeSatisfyInterest(*pitEntry, *m_csFace, data);
  this->dispatchToStrategy(*pitEntry,
    [&] (fw::Strategy& strategy) { strategy.beforeSatisfyInterest(pitEntry, FaceEndpoint(*m_csFace, 0), data); });

  // dispatch to strategy: after Content Store hit
  this->dispatchToStrategy(*pitEntry,
    [&] (fw::Strategy& strategy) { strategy.afterContentStoreHit(pitEntry, ingress, data); });

  }
}

void 
Forwarder::ProcessContentStoreHit(const FaceEndpoint& ingress, const shared_ptr<pit::Entry>& pitEntry,
                             const Interest& interest, const Data& data,int service_id, int resource_value)
{
  ++m_counters.nCsHits;
  afterCsHit(interest, data);
  
  EndServiceExecution(service_id,resource_value);
  


  data.setTag(make_shared<lp::IncomingFaceIdTag>(face::FACEID_CONTENT_STORE));
  // FIXME Should we lookup PIT for other Interests that also match the data?

  pitEntry->isSatisfied = true;
  pitEntry->dataFreshnessPeriod = data.getFreshnessPeriod();

  // set PIT expiry timer to now
  this->setExpiryTimer(pitEntry, 0_ms);

  beforeSatisfyInterest(*pitEntry, *m_csFace, data);
  this->dispatchToStrategy(*pitEntry,
    [&] (fw::Strategy& strategy) { strategy.beforeSatisfyInterest(pitEntry, FaceEndpoint(*m_csFace, 0), data); });

  // dispatch to strategy: after Content Store hit
  this->dispatchToStrategy(*pitEntry,
    [&] (fw::Strategy& strategy) { strategy.afterContentStoreHit(pitEntry, ingress, data); });
}

void
Forwarder::onOutgoingInterest(const shared_ptr<pit::Entry>& pitEntry,
                              const FaceEndpoint& egress, const Interest& interest)
{
  NFD_LOG_DEBUG("onOutgoingInterest out=" << egress << " interest=" << pitEntry->getName());

  // insert out-record
  pitEntry->insertOrUpdateOutRecord(egress.face, interest);

  // send Interest
  egress.face.sendInterest(interest, egress.endpoint);
  ++m_counters.nOutInterests;
}

void
Forwarder::onInterestFinalize(const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_DEBUG("onInterestFinalize interest=" << pitEntry->getName()
                << (pitEntry->isSatisfied ? " satisfied" : " unsatisfied"));

  if (!pitEntry->isSatisfied) {
    beforeExpirePendingInterest(*pitEntry);
  }

  // Dead Nonce List insert if necessary
  this->insertDeadNonceList(*pitEntry, nullptr);

  // Increment satisfied/unsatisfied Interests counter
  if (pitEntry->isSatisfied) {
    ++m_counters.nSatisfiedInterests;
  }
  else {
    ++m_counters.nUnsatisfiedInterests;
  }

  // PIT delete
  pitEntry->expiryTimer.cancel();
  m_pit.erase(pitEntry.get());
}

void
Forwarder::onIncomingData(const FaceEndpoint& ingress, const Data& data)
{
  // receive Data
  NFD_LOG_DEBUG("onIncomingData in=" << ingress << " data=" << data.getName());
  data.setTag(make_shared<lp::IncomingFaceIdTag>(ingress.face.getId()));
  ++m_counters.nInData;

  // /localhost scope control
  bool isViolatingLocalhost = ingress.face.getScope() == ndn::nfd::FACE_SCOPE_NON_LOCAL &&
                              scope_prefix::LOCALHOST.isPrefixOf(data.getName());
  if (isViolatingLocalhost) {
    NFD_LOG_DEBUG("onIncomingData in=" << ingress << " data=" << data.getName() << " violates /localhost");
    // (drop)
    return;
  }

  // PIT match
  pit::DataMatchResult pitMatches = m_pit.findAllDataMatches(data);
  if (pitMatches.size() == 0) {
    // goto Data unsolicited pipeline
    this->onDataUnsolicited(ingress, data);
    return;
  }

  // CS insert
  m_cs.insert(data);

  // when only one PIT entry is matched, trigger strategy: after receive Data
  if (pitMatches.size() == 1) {
    auto& pitEntry = pitMatches.front();

    NFD_LOG_DEBUG("onIncomingData matching=" << pitEntry->getName());

    // set PIT expiry timer to now
    this->setExpiryTimer(pitEntry, 0_ms);

    beforeSatisfyInterest(*pitEntry, ingress.face, data);
    // trigger strategy: after receive Data
    this->dispatchToStrategy(*pitEntry,
      [&] (fw::Strategy& strategy) { strategy.afterReceiveData(pitEntry, ingress, data); });

    // mark PIT satisfied
    pitEntry->isSatisfied = true;
    pitEntry->dataFreshnessPeriod = data.getFreshnessPeriod();

    // Dead Nonce List insert if necessary (for out-record of inFace)
    this->insertDeadNonceList(*pitEntry, &ingress.face);

    // delete PIT entry's out-record
    pitEntry->deleteOutRecord(ingress.face);
  }
  // when more than one PIT entry is matched, trigger strategy: before satisfy Interest,
  // and send Data to all matched out faces
  else {
    std::set<std::pair<Face*, EndpointId>> pendingDownstreams;
    auto now = time::steady_clock::now();

    for (const auto& pitEntry : pitMatches) {
      NFD_LOG_DEBUG("onIncomingData matching=" << pitEntry->getName());

      // remember pending downstreams
      for (const pit::InRecord& inRecord : pitEntry->getInRecords()) {
        if (inRecord.getExpiry() > now) {
          pendingDownstreams.emplace(&inRecord.getFace(), 0);
        }
      }

      // set PIT expiry timer to now
      this->setExpiryTimer(pitEntry, 0_ms);

      // invoke PIT satisfy callback
      beforeSatisfyInterest(*pitEntry, ingress.face, data);
      this->dispatchToStrategy(*pitEntry,
        [&] (fw::Strategy& strategy) { strategy.beforeSatisfyInterest(pitEntry, ingress, data); });

      // mark PIT satisfied
      pitEntry->isSatisfied = true;
      pitEntry->dataFreshnessPeriod = data.getFreshnessPeriod();

      // Dead Nonce List insert if necessary (for out-record of inFace)
      this->insertDeadNonceList(*pitEntry, &ingress.face);

      // clear PIT entry's in and out records
      pitEntry->clearInRecords();
      pitEntry->deleteOutRecord(ingress.face);
    }

    // foreach pending downstream
    for (const auto& pendingDownstream : pendingDownstreams) {
      if (pendingDownstream.first->getId() == ingress.face.getId() &&
          pendingDownstream.second == ingress.endpoint &&
          pendingDownstream.first->getLinkType() != ndn::nfd::LINK_TYPE_AD_HOC) {
        continue;
      }
      // goto outgoing Data pipeline
      this->onOutgoingData(data, FaceEndpoint(*pendingDownstream.first, pendingDownstream.second));
    }
  }
}

void
Forwarder::onDataUnsolicited(const FaceEndpoint& ingress, const Data& data)
{
  // accept to cache?
  fw::UnsolicitedDataDecision decision = m_unsolicitedDataPolicy->decide(ingress.face, data);
  if (decision == fw::UnsolicitedDataDecision::CACHE) {
    // CS insert
    m_cs.insert(data, true);
  }

  NFD_LOG_DEBUG("onDataUnsolicited in=" << ingress << " data=" << data.getName() << " decision=" << decision);
}

void
Forwarder::onOutgoingData(const Data& data, const FaceEndpoint& egress)
{
  if (egress.face.getId() == face::INVALID_FACEID) {
    NFD_LOG_WARN("onOutgoingData out=(invalid) data=" << data.getName());
    return;
  }
  NFD_LOG_DEBUG("onOutgoingData out=" << egress << " data=" << data.getName());

  // /localhost scope control
  bool isViolatingLocalhost = egress.face.getScope() == ndn::nfd::FACE_SCOPE_NON_LOCAL &&
                              scope_prefix::LOCALHOST.isPrefixOf(data.getName());
  if (isViolatingLocalhost) {
    NFD_LOG_DEBUG("onOutgoingData out=" << egress << " data=" << data.getName() << " violates /localhost");
    // (drop)
    return;
  }

  // TODO traffic manager

  // send Data
  egress.face.sendData(data, egress.endpoint);
  ++m_counters.nOutData;
}

void
Forwarder::onIncomingNack(const FaceEndpoint& ingress, const lp::Nack& nack)
{
  // receive Nack
  nack.setTag(make_shared<lp::IncomingFaceIdTag>(ingress.face.getId()));
  ++m_counters.nInNacks;

  // if multi-access or ad hoc face, drop
  if (ingress.face.getLinkType() != ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
    NFD_LOG_DEBUG("onIncomingNack in=" << ingress
                  << " nack=" << nack.getInterest().getName() << "~" << nack.getReason()
                  << " link-type=" << ingress.face.getLinkType());
    return;
  }

  // PIT match
  shared_ptr<pit::Entry> pitEntry = m_pit.find(nack.getInterest());
  // if no PIT entry found, drop
  if (pitEntry == nullptr) {
    NFD_LOG_DEBUG("onIncomingNack in=" << ingress << " nack=" << nack.getInterest().getName()
                  << "~" << nack.getReason() << " no-PIT-entry");
    return;
  }

  // has out-record?
  auto outRecord = pitEntry->getOutRecord(ingress.face);
  // if no out-record found, drop
  if (outRecord == pitEntry->out_end()) {
    NFD_LOG_DEBUG("onIncomingNack in=" << ingress << " nack=" << nack.getInterest().getName()
                  << "~" << nack.getReason() << " no-out-record");
    return;
  }

  // if out-record has different Nonce, drop
  if (nack.getInterest().getNonce() != outRecord->getLastNonce()) {
    NFD_LOG_DEBUG("onIncomingNack in=" << ingress << " nack=" << nack.getInterest().getName()
                  << "~" << nack.getReason() << " wrong-Nonce " << nack.getInterest().getNonce()
                  << "!=" << outRecord->getLastNonce());
    return;
  }

  NFD_LOG_DEBUG("onIncomingNack in=" << ingress << " nack=" << nack.getInterest().getName()
                << "~" << nack.getReason() << " OK");

  // record Nack on out-record
  outRecord->setIncomingNack(nack);

  // set PIT expiry timer to now when all out-record receive Nack
  if (!fw::hasPendingOutRecords(*pitEntry)) {
    this->setExpiryTimer(pitEntry, 0_ms);
  }

  // trigger strategy: after receive NACK
  this->dispatchToStrategy(*pitEntry,
    [&] (fw::Strategy& strategy) { strategy.afterReceiveNack(ingress, nack, pitEntry); });
}

void
Forwarder::onOutgoingNack(const shared_ptr<pit::Entry>& pitEntry,
                          const FaceEndpoint& egress, const lp::NackHeader& nack)
{
  if (egress.face.getId() == face::INVALID_FACEID) {
    NFD_LOG_WARN("onOutgoingNack out=(invalid)"
                 << " nack=" << pitEntry->getInterest().getName() << "~" << nack.getReason());
    return;
  }

  // has in-record?
  auto inRecord = pitEntry->getInRecord(egress.face);

  // if no in-record found, drop
  if (inRecord == pitEntry->in_end()) {
    NFD_LOG_DEBUG("onOutgoingNack out=" << egress
                  << " nack=" << pitEntry->getInterest().getName()
                  << "~" << nack.getReason() << " no-in-record");
    return;
  }

  // if multi-access or ad hoc face, drop
  if (egress.face.getLinkType() != ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
    NFD_LOG_DEBUG("onOutgoingNack out=" << egress
                  << " nack=" << pitEntry->getInterest().getName() << "~" << nack.getReason()
                  << " link-type=" << egress.face.getLinkType());
    return;
  }

  NFD_LOG_DEBUG("onOutgoingNack out=" << egress
                << " nack=" << pitEntry->getInterest().getName()
                << "~" << nack.getReason() << " OK");

  // create Nack packet with the Interest from in-record
  lp::Nack nackPkt(inRecord->getInterest());
  nackPkt.setHeader(nack);

  // erase in-record
  pitEntry->deleteInRecord(egress.face);

  // send Nack on face
  egress.face.sendNack(nackPkt, egress.endpoint);
  ++m_counters.nOutNacks;
}

void
Forwarder::onDroppedInterest(const FaceEndpoint& egress, const Interest& interest)
{
  m_strategyChoice.findEffectiveStrategy(interest.getName()).onDroppedInterest(egress, interest);
}

void
Forwarder::onNewNextHop(const Name& prefix, const fib::NextHop& nextHop)
{
  const auto affectedEntries = this->getNameTree().partialEnumerate(prefix,
    [&] (const name_tree::Entry& nte) -> std::pair<bool, bool> {
      const fib::Entry* fibEntry = nte.getFibEntry();
      const fw::Strategy* strategy = nullptr;
      if (nte.getStrategyChoiceEntry() != nullptr) {
        strategy = &nte.getStrategyChoiceEntry()->getStrategy();
      }
      // current nte has buffered Interests but no fibEntry (except for the root nte) and the strategy
      // enables new nexthop behavior, we enumerate the current nte and keep visiting its children.
      if (nte.getName().size() == 0 ||
          (strategy != nullptr && strategy->wantNewNextHopTrigger() &&
          fibEntry == nullptr && nte.hasPitEntries())) {
        return {true, true};
      }
      // we don't need the current nte (no pitEntry or strategy doesn't support new nexthop), but
      // if the current nte has no fibEntry, it's still possible that its children are affected by
      // the new nexthop.
      else if (fibEntry == nullptr) {
        return {false, true};
      }
      // if the current nte has a fibEntry, we ignore the current nte and don't visit its
      // children because they are already covered by the current nte's fibEntry.
      else {
        return {false, false};
      }
    });

  for (const auto& nte : affectedEntries) {
    for (const auto& pitEntry : nte.getPitEntries()) {
      this->dispatchToStrategy(*pitEntry,
        [&] (fw::Strategy& strategy) {
          strategy.afterNewNextHop(nextHop, pitEntry);
        });
    }
  }
}

void
Forwarder::setExpiryTimer(const shared_ptr<pit::Entry>& pitEntry, time::milliseconds duration)
{
  BOOST_ASSERT(pitEntry);
  BOOST_ASSERT(duration >= 0_ms);

  pitEntry->expiryTimer.cancel();
  pitEntry->expiryTimer = getScheduler().schedule(duration, [=] { onInterestFinalize(pitEntry); });
}

void
Forwarder::insertDeadNonceList(pit::Entry& pitEntry, Face* upstream)
{
  // need Dead Nonce List insert?
  bool needDnl = true;
  if (pitEntry.isSatisfied) {
    BOOST_ASSERT(pitEntry.dataFreshnessPeriod >= 0_ms);
    needDnl = static_cast<bool>(pitEntry.getInterest().getMustBeFresh()) &&
              pitEntry.dataFreshnessPeriod < m_deadNonceList.getLifetime();
  }

  if (!needDnl) {
    return;
  }

  // Dead Nonce List insert
  if (upstream == nullptr) {
    // insert all outgoing Nonces
    const auto& outRecords = pitEntry.getOutRecords();
    std::for_each(outRecords.begin(), outRecords.end(), [&] (const auto& outRecord) {
      m_deadNonceList.add(pitEntry.getName(), outRecord.getLastNonce());
    });
  }
  else {
    // insert outgoing Nonce of a specific face
    auto outRecord = pitEntry.getOutRecord(*upstream);
    if (outRecord != pitEntry.getOutRecords().end()) {
      m_deadNonceList.add(pitEntry.getName(), outRecord->getLastNonce());
    }
  }
}










// Atif-Code: 

tuple<int,int,int,int> 
Forwarder::StartServiceExecution(shared_ptr<const Interest> interest)
{
  // Start Execution based on service name, type, and priority
  // Check if the resources are available or not

  tuple<string,string,string> service_info_tupple = GetServiceInformation(interest);
  string service_name=get<0>(service_info_tupple);
    
  ns3::Ptr<ns3::UniformRandomVariable> m_rand(ns3::CreateObject<ns3::UniformRandomVariable>());
  int service_type=m_rand->GetValue(1,5); //get<1>(service_info_tupple); TODO:
  
  string service_priority=get<2>(service_info_tupple);

  ns3::Ptr<ns3::Node> current_node=GetCurrentNode();
  int start_time=CurrentTime();

  
  int run_time=CurrentTime();
  int service_id=AddToServiceSatusCollection(service_name, start_time, current_node->GetId(),run_time, (ServiceStatus)1);
  int resource_value=OccupyNodeResources(service_type);
  
  tuple<int,int,int,int> service_info={service_id,service_type,run_time,resource_value};
  
  return service_info;
}

void 
Forwarder::PauseServiceExecution(int service_id)
{

}

void 
Forwarder::EndServiceExecution(int service_id,int resource_value)
{
    UpdateServiceStatus(service_id,(ServiceStatus)3);
    ReleaseNodeResources(resource_value);
}

int 
Forwarder::OccupyNodeResources(int service_type)
{
  ns3::Ptr<ns3::Node> current_node=GetCurrentNode();
  double resource_utilization_value=GetComputeResourceUtilizationValue(service_type);

  current_node->m_compute_resources=current_node->m_compute_resources-resource_utilization_value;
  current_node->m_compute_capacity=((double)current_node->m_compute_resources / (double)current_node->m_inital_compute_resources)*100.0;

 //cout<<"OccupyNodeResources(): Current Node Id: "<<current_node->GetId()<<" initial computational resources: "<<current_node->m_inital_compute_resources<<" remaining computation resources: "<<current_node->m_compute_resources<<" remaining computational capacity: "<<current_node->m_compute_capacity<<endl;

  return resource_utilization_value;
}

void 
Forwarder::ReleaseNodeResources(int resource_value)
{
  ns3::Ptr<ns3::Node> current_node=GetCurrentNode();
  current_node->m_compute_resources=current_node->m_compute_resources+resource_value;
    current_node->m_compute_capacity=((double)current_node->m_compute_resources / (double)current_node->m_inital_compute_resources)*100.0;

// cout<<"ReleaseNodeResources(): Current Node Id: "<<current_node->GetId()<<" initial computational resources: "<<current_node->m_inital_compute_resources<<" remaining computation resources: "<<current_node->m_compute_resources<<" remaining computational capacity: "<<current_node->m_compute_capacity<<endl;
}

tuple<string,string,string> 
Forwarder::GetServiceInformation(shared_ptr<const Interest> interest)
{

  string interest_name=interest->getName().toUri();
  vector<string> name_collection;
  boost::split(name_collection, interest_name, boost::is_any_of("7C"));
  string service_info=name_collection[2]; // service info will always be at index 1 (make sure that consumer application set service info after vertical pipe (|))
  vector<string> service_info_collection;
  boost::split(service_info_collection, service_info, boost::is_any_of("/")); // make sure that consumer application seperate the service info by "/"
  //cout<<"GetServiceInformation(): Service Name: "+service_info_collection[0]+" Service Tyype:  "+service_info_collection[1]+" Service Priority:  "+service_info_collection[2]<<endl;
  tuple<string,string,string> service_info_tupple= {service_info_collection[0],service_info_collection[1],service_info_collection[2]};
  
  return service_info_tupple;
}

double // the value will be in percentage
Forwarder::GetComputeResourceUtilizationValue(int service_type)
{
  ns3::Ptr<ns3::UniformRandomVariable> m_rand(ns3::CreateObject<ns3::UniformRandomVariable>());

  double resource_utilization_value=m_rand->GetValue((service_type*5), (service_type*10));

  return resource_utilization_value;
}

int // the value will be in millisecond
Forwarder::GetServiceExecutionTime(int service_type)
{
  ns3::Ptr<ns3::UniformRandomVariable> m_rand(ns3::CreateObject<ns3::UniformRandomVariable>());

  int resource_utilization_value=m_rand->GetValue((service_type*5), (service_type*10));

  return resource_utilization_value;
}



int
Forwarder::AddToServiceSatusCollection(string service_name, int start_time, int node_id,int run_time, ServiceStatus service_status)
{
  int service_id=m_service_id++;
  Forwarder::ServiceInfo service_info= {service_id, service_name,start_time,node_id,run_time,service_status};
  
  service_info_collection.push_back(service_info);
 
  return service_id;
}

Forwarder::ServiceInfo
Forwarder::FetchFromServiceCollectionByNameId(string service_name, int service_id)
{

  // TODO: improve further
    vector<ServiceInfo>::iterator service_info = find_if(service_info_collection.begin(), service_info_collection.end(), [&](const ServiceInfo & x) {
     return x.service_id == service_id && x.service_name==service_name;
   });
  return *service_info;
}
Forwarder::ServiceInfo
Forwarder::FetchFromServiceCollectionById(int service_id)
{
  // TODO: improve further
  vector<ServiceInfo>::iterator service_info =find_if(service_info_collection.begin(), service_info_collection.end(), [&](const ServiceInfo & x) {
    return x.service_id == service_id;
  });
  return *service_info;
}

vector<Forwarder::ServiceInfo>
Forwarder::FetchFromServiceCollectionByName(string service_name)
{
  // TODO: improve this searching mechanism
  vector<Forwarder::ServiceInfo> service_info_collection;
  for (vector<Forwarder::ServiceInfo>::iterator it = service_info_collection.begin(); it != service_info_collection.end(); ++it){
   if (it->service_name==service_name)
   {
     service_info_collection.push_back(*it);
   } 
  }
  return service_info_collection;
}


vector<Forwarder::ServiceInfo>
Forwarder::FetchFromServiceCollectionByStatus(Forwarder::ServiceStatus service_status)
{
  // TODO: improve this searching mechanism
  vector<Forwarder::ServiceInfo> service_info_collection;
  for (vector<Forwarder::ServiceInfo>::iterator it = service_info_collection.begin(); it != service_info_collection.end(); ++it){
   if (it->service_status==service_status)
   {
     service_info_collection.push_back(*it);
   } 
  }
  return service_info_collection;
}

Forwarder::ServiceInfo
Forwarder::UpdateServiceStatus(int service_id, Forwarder::ServiceStatus updated_service_status)
{
  
  Forwarder::ServiceInfo service_info=FetchFromServiceCollectionById(service_id);
  int service_index=GetServiceIndexInCollection(service_info.service_id);

  if(service_index!=-1)
  {
    service_info.service_status=updated_service_status; // set new status value
  
    service_info_collection.at(service_index)=service_info; // update in the list
      
  }
  return service_info; // TODO: return null in case of no update
}


int
Forwarder::GetServiceIndexInCollection(int service_id)
{
  int index=-1;
  auto  service_itr = find_if(service_info_collection.begin(), service_info_collection.end(), [&service_id](const ServiceInfo& x) {return x.service_id == service_id;});
  if(service_itr != service_info_collection.end())
  {
    index = distance(service_info_collection.begin(), service_itr);
  }
  return index;
}

int
Forwarder::CurrentTime()
{
  ndn::time::steady_clock::TimePoint now = ::ndn::time::steady_clock::now(); 
  ndn::time::milliseconds milliseconds = ::ndn::time::duration_cast<::ndn::time::milliseconds>(now.time_since_epoch());
  return milliseconds.count(); 
}

ns3::Ptr<ns3::Node> 
Forwarder::GetCurrentNode()
{
  ns3::Ptr<ns3::Node> currentNode;
  if (ns3::Simulator::GetContext() < 100000) 
  {
        currentNode = ns3::NodeList::GetNode(ns3::Simulator::GetContext());
  }
  return currentNode;
}

// Atif-Code: End





} // namespace nfd
