#include "simeng/memory/MMU.hh"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace simeng {
namespace memory {

MMU::MMU(VAddrTranslator fn)
    : cacheLineWidth_(config::SimInfo::getValue<uint64_t>(
          config::SimInfo::getConfig()["Memory-Hierarchy"]
                                      ["Cache-Line-Width"])),
      translate_(fn) {
  // Initialise Memory bandwidth and request limits
  // TODO: replace with cleaner solution in the ModelConfig itself
  ryml::ConstNodeRef config = config::SimInfo::getConfig();
  if (config::SimInfo::getValue<std::string>(
          config["Core"]["Simulation-Mode"]) != "emulation") {
    loadBandwidth_ = config::SimInfo::getValue<uint64_t>(
        config["LSQ-Memory-Interface"]["Load-Bandwidth"]);
    storeBandwidth_ = config::SimInfo::getValue<uint64_t>(
        config["LSQ-Memory-Interface"]["Store-Bandwidth"]);
    requestLimit_ = config::SimInfo::getValue<uint64_t>(
        config["LSQ-Memory-Interface"]["Permitted-Requests-Per-Cycle"]);
    loadRequestLimit_ = config::SimInfo::getValue<uint64_t>(
        config["LSQ-Memory-Interface"]["Permitted-Loads-Per-Cycle"]);
    storeRequestLimit_ = config::SimInfo::getValue<uint64_t>(
        config["LSQ-Memory-Interface"]["Permitted-Stores-Per-Cycle"]);
    exclusiveRequests_ = config::SimInfo::getValue<bool>(
        config["LSQ-Memory-Interface"]["Exclusive"]);
  } else {
    // If core model is emulation, remove all bandwidth and request limits. This
    // ensures single cycle processing of each instruction.
    loadBandwidth_ = UINT64_MAX;
    storeBandwidth_ = UINT64_MAX;
    requestLimit_ = UINT64_MAX;
    loadRequestLimit_ = UINT64_MAX;
    storeRequestLimit_ = UINT64_MAX;
    exclusiveRequests_ = true;
  }
}

void MMU::tick() {
  /** NOTE: The number of instructions present in each of the load / store
   * vectors is limited inside the `requestRead()` and `requestWrite()`
   * functions when we add to these vectors.
   * - Total instructions across loads and stores will equal (at most) to
   *   requestLimit_.
   * - Total loads will not exceed loadRequestLimit_.
   * - Total stores will not exceed storeRequestLimit_.
   * - If exclusiveRequests_ == true, then there will only be stores or loads
   *   at any one time. */
  if (exclusiveRequests_) {
    // If exclusive access, see which access type has available instructions.
    bool isStore = loadsStores_[STR].size() != 0;
    processRequests(isStore);
  } else {
    // Process Stores first (same as LSQ)
    processRequests(STR);
    processRequests(LD);
  }
}

void MMU::processRequests(const bool isStore) {
  uint64_t bandwidthLimit = isStore ? storeBandwidth_ : loadBandwidth_;
  uint64_t bandwidthUsed = 0;
  while (loadsStores_[isStore].size() > 0) {
    auto insn = loadsStores_[isStore].begin();
    // Process as many requests as possible within bandwidth limit
    auto pkt = insn->begin();
    while (pkt != insn->end()) {
      // Check that sending this packet won't exceed bandwidth
      if ((bandwidthUsed + (*pkt)->size_) <= bandwidthLimit) {
        // If the request is a store, and is the last packet associated with
        // this instruction, set the store to be ready to commit
        if (isStore) {
          if (pkt + 1 == insn->end()) {
            const auto& str = requestedStores_.find((*pkt)->insnSeqId_);
            assert(str != requestedStores_.end() &&
                   "[SimEng:MMU] Tried to process a store packet that has no "
                   "assocaited store instruction in the MMU's requestedStores_ "
                   "map.");
            // Set as ready to commit if the store is non-conditional. Store
            // conditional operations have to pass through the writeback unit
            // again before commitment (thus delay setting as ready to commit)
            if (!str->second.insn->isStoreCond()) {
              str->second.insn->setCommitReady();
              requestedStores_.erase(str);
            }
          }
        }
        bandwidthUsed += (*pkt)->size_;
        issueRequest(std::move(*pkt));
        pkt = insn->erase(pkt);
      } else {
        // Bandwidth will be exceeded. Stop sending instruction packets
        return;
      }
    }
    // If insn is now empty (all requests have been sent) then remove it
    // from the vector
    if (insn->size() == 0) {
      loadsStores_[isStore].erase(insn);
    }
  }
}

bool MMU::requestRead(const std::shared_ptr<Instruction>& uop) {
  // Check if space for instruction
  // If exclusive then no loads permitted if store still being processed
  if (exclusiveRequests_ && (loadsStores_[STR].size() != 0)) return false;
  // Check total limit isn't met if not exclusive
  if (!exclusiveRequests_ &&
      (loadsStores_[LD].size() + loadsStores_[STR].size() >= requestLimit_))
    return false;
  // Check space left for a load
  if (loadsStores_[LD].size() >= loadRequestLimit_) return false;

  // Initialise space in loads
  loadsStores_[LD].push_back({});
  uint64_t seqId = uop->getSequenceId();
  // Generate and fire off requests
  const auto& targets = uop->getGeneratedAddresses();
  for (int i = 0; i < targets.size(); i++) {
    createReadMemPackets(targets[i], loadsStores_[LD].back(), seqId, i);
  }
  if (uop->isLoadReserved()) {
    // Set MemPackets to be atomic if uop is an atomic operation
    for (int i = 0; i < loadsStores_[LD].back().size(); i++) {
      loadsStores_[LD].back()[i]->markAsAtomic();
    }
  }
  // Register load in map
  uint16_t totalReqs = static_cast<uint16_t>(loadsStores_[LD].back().size());
  pendingDataRequests_ += totalReqs;
  requestedLoads_[seqId] = {uop, totalReqs};
  return true;
}

bool MMU::requestWrite(const std::shared_ptr<Instruction>& uop,
                       const std::vector<RegisterValue>& data) {
  // Check if space for instruction
  // If exclusive then no stores permitted if load still being processed
  if (exclusiveRequests_ && (loadsStores_[LD].size() != 0)) return false;
  // Check total limit isn't met if not exclusive
  if (!exclusiveRequests_ &&
      (loadsStores_[LD].size() + loadsStores_[STR].size() >= requestLimit_))
    return false;
  // Check space left for a store
  if (loadsStores_[STR].size() >= storeRequestLimit_) return false;

  // Initialise space in stores
  loadsStores_[STR].push_back({});
  // Create and fire off requests
  const auto& targets = uop->getGeneratedAddresses();
  uint64_t seqId = uop->getSequenceId();
  assert(data.size() == targets.size() &&
         "[SimEng:MMU] Number of addresses does not match the number of data "
         "elements to write.");
  for (int i = 0; i < targets.size(); i++) {
    const auto& target = targets[i];
    // Format data
    const char* wdata = data[i].getAsVector<char>();
    std::vector<char> dt(wdata, wdata + target.size);
    // Create requests
    createWriteMemPackets(target, loadsStores_[STR].back(), dt, seqId, i);
  }
  if (uop->isStoreCond()) {
    // Set MemPackets to be atomic if uop is an atomic operation
    for (int i = 0; i < loadsStores_[STR].back().size(); i++) {
      loadsStores_[STR].back()[i]->markAsAtomic();
    }
  }
  uint16_t totalReqs = static_cast<uint16_t>(loadsStores_[STR].back().size());
  pendingDataRequests_ += totalReqs;
  requestedStores_[uop->getSequenceId()] = {uop, totalReqs};
  return true;
}

void MMU::requestWrite(const MemoryAccessTarget& target,
                       const RegisterValue& data) {
  // Format data
  const char* wdata = data.getAsVector<char>();
  std::vector<char> dt(wdata, wdata + target.size);

  // Create requests
  std::vector<std::unique_ptr<MemPacket>> pktVec = {};
  createWriteMemPackets(target, pktVec, dt, 0, 0);

  // Fire off requests
  uint16_t pktVecSize = static_cast<uint16_t>(pktVec.size());
  for (uint16_t i = 0; i < pktVecSize; i++) {
    issueRequest(std::move(pktVec[i]));
  }
  pendingDataRequests_ += pktVecSize;
}

void MMU::requestInstrRead(const MemoryAccessTarget& target) {
  assert(isAligned(target) &&
         "[SimEng:MMU] Unlaigned instruction read requests are not permitted.");
  // Create and fire off request
  std::unique_ptr<memory::MemPacket> insRequest =
      MemPacket::createReadRequest(target.vaddr, target.size, 0, 0, tid_);
  insRequest->markAsUntimed();
  insRequest->markAsInstrRead();
  issueRequest(std::move(insRequest));
}

const span<MemoryReadResult> MMU::getCompletedInstrReads() const {
  return {const_cast<MemoryReadResult*>(completedInstrReads_.data()),
          completedInstrReads_.size()};
}

void MMU::supplyDelayedTranslation(uint64_t vaddr, uint64_t paddr) {
  auto it = pendingRequests_.find(vaddr);
  if (it != pendingRequests_.end()) {
    // If a delayed virtual address translation exists, re-issue the request so
    // that the new translation can be supplied
    for (int i = 0; i < it->second.size(); i++) {
      issueRequest(std::move(it->second[i]));
    }
    pendingRequests_.erase(it);
  }
}

void MMU::clearCompletedIntrReads() { completedInstrReads_.clear(); }

bool MMU::hasPendingRequests() const { return pendingDataRequests_ != 0; }

void MMU::setTid(uint64_t tid) { tid_ = tid; }
uint64_t MMU::getTid() { return tid_; }

std::shared_ptr<Port<std::unique_ptr<MemPacket>>> MMU::initPort() {
  port_ = std::make_shared<Port<std::unique_ptr<MemPacket>>>();
  auto fn = [this](std::unique_ptr<MemPacket> packet) -> void {
    if (packet->isInstrRead()) {
      if (packet->isFaulty() || packet->ignore()) {
        // If faulty or ignored, return no data. This signals a data abort.
        completedInstrReads_.push_back({{packet->vaddr_, packet->size_},
                                        RegisterValue(),
                                        packet->insnSeqId_});
        return;
      }
      completedInstrReads_.push_back(
          {{packet->vaddr_, packet->size_},
           RegisterValue(packet->payload().data(), packet->size_),
           packet->insnSeqId_});
      return;
    }

    pendingDataRequests_--;
    uint64_t seqId = packet->insnSeqId_;
    if (packet->isRead()) {
      assert(requestedLoads_.find(seqId) != requestedLoads_.end() &&
             "[SimEng:MMU] Read response packet recieved for instruction that "
             "does not exist.");
      readResponses_[seqId][packet->packetOrderId_][packet->packetSplitId_] =
          std::move(packet);
      requestedLoads_.find(seqId)->second.totalPacketsRemaining--;
      if (requestedLoads_.find(seqId)->second.totalPacketsRemaining == 0) {
        // All packets have come back, supply load instruction all data
        supplyLoadInsnData(seqId);
      }
    } else if (packet->isWrite()) {
      const auto& str = requestedStores_.find(seqId);
      if (str == requestedStores_.end()) return;
      str->second.totalPacketsRemaining--;

      // If anyone packet belonging to the uop fails, currently mark is as
      // failed
      if (packet->hasFailed()) str->second.failed = true;

      if (str->second.totalPacketsRemaining == 0) {
        str->second.insn->updateCondStoreResult(!str->second.failed);
      }
    }
  };
  port_->registerReceiver(fn);
  return port_;
}

void MMU::issueRequest(std::unique_ptr<MemPacket> request) {
  // Since we don't have a TLB yet, treat every memory request as a TLB miss and
  // consult the page table.
  uint64_t paddr = translate_(request->vaddr_, tid_);
  uint64_t faultCode = simeng::OS::masks::faults::getFaultCode(paddr);

  if (faultCode == simeng::OS::masks::faults::pagetable::DATA_ABORT) {
    request->markAsFaulty();
    port_->recieve(std::move(request));
    return;
  }

  if (faultCode == simeng::OS::masks::faults::pagetable::PENDING) {
    // Record the wanted translation if it is currently bein resolved
    // asynchronously
    pendingRequests_[request->vaddr_].push_back(std::move(request));
    return;
  }

  if (faultCode == simeng::OS::masks::faults::pagetable::IGNORED) {
    request->markAsIgnored();
  } else {
    request->paddr_ = paddr;
  }

  if (request->isInstrRead())
    numInsnReads_++;
  else if (request->isRead())
    numDataReads_++;
  else if (request->isWrite())
    numDataWrites_++;
  port_->send(std::move(request));
}

bool MMU::isAligned(const MemoryAccessTarget& target) const {
  assert(target.size != 0 &&
         "[SimEng:MMU] Cannot have a memory target size of 0.");
  uint64_t startAddr = target.vaddr;
  // Must -1 from end address as vaddr + size will give the address at end of
  // region, but this address is not written to.
  // i.e. vaddr = 0, size = 4 :  | | | | | | | |
  //                      Addr:  0 1 2 3 4 5 6 7
  //                             ^-------^
  //                              Payload
  // End address is 4, but we do not write to address 4 hence this is allowed
  // to be a cache line boundary.
  uint64_t endAddr = target.vaddr + target.size - 1;
  // If start and end address down align to same value (w.r.t cache line
  // width), then memory target is aligned.
  return (downAlign(startAddr, cacheLineWidth_) ==
          downAlign(endAddr, cacheLineWidth_));
}

void MMU::createReadMemPackets(
    const MemoryAccessTarget& target,
    std::vector<std::unique_ptr<MemPacket>>& outputVec,
    const uint64_t insnSeqId, const uint16_t pktOrderId) {
  if (isAligned(target)) {
    std::unique_ptr<memory::MemPacket> req = MemPacket::createReadRequest(
        target.vaddr, target.size, insnSeqId, pktOrderId, tid_);
    outputVec.push_back(std::move(req));
    // Resize response data structure to equal the number of packets created
    readResponses_[insnSeqId][pktOrderId].resize(1);
  } else {
    uint64_t nextAddr = target.vaddr;
    uint64_t remSize = static_cast<uint64_t>(target.size);
    uint16_t nextSplitId = 0;
    while (remSize != 0) {
      // Get size of next target region
      uint16_t regSize = std::min(
          (downAlign(nextAddr, cacheLineWidth_) + cacheLineWidth_) - nextAddr,
          remSize);
      // Create MemPacket
      auto req = MemPacket::createReadRequest(nextAddr, regSize, insnSeqId,
                                              pktOrderId, tid_);
      req->packetSplitId_ = nextSplitId;
      outputVec.push_back(std::move(req));
      // Update vars
      nextAddr += regSize;
      remSize -= regSize;
      nextSplitId++;
    }
    // Resize response data structure to equal the number of packets created
    readResponses_[insnSeqId][pktOrderId].resize(nextSplitId);
  }
}

void MMU::createWriteMemPackets(
    const MemoryAccessTarget& target,
    std::vector<std::unique_ptr<MemPacket>>& outputVec,
    const std::vector<char>& data, const uint64_t insnSeqId,
    const uint16_t pktOrderId) {
  if (isAligned(target)) {
    std::unique_ptr<MemPacket> req = MemPacket::createWriteRequest(
        target.vaddr, target.size, insnSeqId, pktOrderId, tid_, data);
    outputVec.push_back(std::move(req));
  } else {
    uint64_t nextAddr = target.vaddr;
    uint64_t remSize = static_cast<uint64_t>(target.size);
    uint16_t nextSplitId = 0;
    std::vector<char> remData = data;
    while (remSize != 0) {
      // Get size of next target region
      uint16_t regSize = std::min(
          (downAlign(nextAddr, cacheLineWidth_) + cacheLineWidth_) - nextAddr,
          remSize);
      // Get data for this region
      auto regData =
          std::vector<char>(remData.begin(), remData.begin() + regSize);
      // Create MemPacket
      auto req = MemPacket::createWriteRequest(nextAddr, regSize, insnSeqId,
                                               pktOrderId, tid_, regData);
      req->packetSplitId_ = nextSplitId;
      outputVec.push_back(std::move(req));
      // Update vars
      nextAddr += regSize;
      remSize -= regSize;
      nextSplitId++;
      remData = std::vector<char>(remData.begin() + regSize, remData.end());
    }
  }
}

void MMU::supplyLoadInsnData(const uint64_t insnSeqId) {
  auto itr = requestedLoads_.find(insnSeqId);
  assert(itr != requestedLoads_.end() &&
         "[SimEng:MMU] Tried to supply data to a load instruction that does "
         "not exist in the requestedLoads_ map.");
  // Get reference to instruction for easier access
  auto& insn = itr->second.insn;
  // Get map of all packets, grouped by packetOrderId
  auto& packets = readResponses_.find(insnSeqId)->second;
  for (int i = 0; i < packets.size(); i++) {
    // Get vector containing all packets associated to a single target
    auto& pktVec = packets[i];
    assert(pktVec.size() > 0 &&
           "[SimEng:MMU] Empty read response packet vector.");

    uint64_t addr = pktVec[0]->vaddr_;
    // Do early check on first packet for data abort
    if (pktVec[0]->isFaulty()) {
      // If faulty, return no data. This signals a data abort.
      insn->supplyData(addr, RegisterValue());
      continue;
    }
    // Initialise values with first package
    std::vector<char> mergedData = pktVec[0]->payload();
    uint16_t mergedSize = pktVec[0]->size_;
    bool isFaulty = false;
    // Loop over any remaining packets due to a split
    for (int j = 1; j < pktVec.size(); j++) {
      if (pktVec[j]->isFaulty()) {
        // If faulty, return no data. This signals a data abort.
        insn->supplyData(addr, RegisterValue());
        isFaulty = true;
        break;
      }
      // Increase merged size
      mergedSize += pktVec[j]->size_;
      // Concatonate the payload data
      auto& tempData = pktVec[j]->payload();
      mergedData.insert(mergedData.end(), tempData.begin(), tempData.end());
    }
    // Supply data to instruction
    if (!isFaulty) insn->supplyData(addr, {mergedData.data(), mergedSize});
  }
  assert(insn->hasAllData() &&
         "[SimEng:MMU] Load instruction was supplied memory data but is still "
         "waiting on further data to be supplied.");
  // Instruction now has all data, remove entries from maps
  requestedLoads_.erase(insnSeqId);
  readResponses_.erase(insnSeqId);
}

}  // namespace memory
}  // namespace simeng
