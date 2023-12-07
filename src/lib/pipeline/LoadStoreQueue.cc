#include "simeng/pipeline/LoadStoreQueue.hh"

#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <list>

namespace simeng {
namespace pipeline {

/** Check whether requests `a` and `b` overlap. */
bool requestsOverlap(memory::MemoryAccessTarget a,
                     memory::MemoryAccessTarget b) {
  // Check whether one region ends before the other begins, implying no overlap,
  // and negate
  return !(a.vaddr + a.size <= b.vaddr || b.vaddr + b.size <= a.vaddr);
}

LoadStoreQueue::LoadStoreQueue(
    unsigned int maxCombinedSpace, std::shared_ptr<memory::MMU> mmu,
    span<PipelineBuffer<std::shared_ptr<Instruction>>> completionSlots,
    std::function<void(span<Register>, span<RegisterValue>)> forwardOperands,
    CompletionOrder completionOrder)
    : completionSlots_(completionSlots),
      forwardOperands_(forwardOperands),
      maxCombinedSpace_(maxCombinedSpace),
      combined_(true),
      mmu_(mmu),
      completionOrder_(completionOrder){};

LoadStoreQueue::LoadStoreQueue(
    unsigned int maxLoadQueueSpace, unsigned int maxStoreQueueSpace,
    std::shared_ptr<memory::MMU> mmu,
    span<PipelineBuffer<std::shared_ptr<Instruction>>> completionSlots,
    std::function<void(span<Register>, span<RegisterValue>)> forwardOperands,
    CompletionOrder completionOrder)
    : completionSlots_(completionSlots),
      forwardOperands_(forwardOperands),
      maxLoadQueueSpace_(maxLoadQueueSpace),
      maxStoreQueueSpace_(maxStoreQueueSpace),
      combined_(false),
      mmu_(mmu),
      completionOrder_(completionOrder){};

unsigned int LoadStoreQueue::getLoadQueueSpace() const {
  if (combined_) {
    return getCombinedSpace();
  } else {
    return getLoadQueueSplitSpace();
  }
}
unsigned int LoadStoreQueue::getStoreQueueSpace() const {
  if (combined_) {
    return getCombinedSpace();
  } else {
    return getStoreQueueSplitSpace();
  }
}
unsigned int LoadStoreQueue::getTotalSpace() const {
  if (combined_) {
    return getCombinedSpace();
  } else {
    return getLoadQueueSplitSpace() + getStoreQueueSplitSpace();
  }
}

unsigned int LoadStoreQueue::getLoadQueueSplitSpace() const {
  return maxLoadQueueSpace_ - loadQueue_.size();
}
unsigned int LoadStoreQueue::getStoreQueueSplitSpace() const {
  return maxStoreQueueSpace_ - storeQueue_.size();
}
unsigned int LoadStoreQueue::getCombinedSpace() const {
  return maxCombinedSpace_ - loadQueue_.size() - storeQueue_.size();
}

void LoadStoreQueue::addLoad(const std::shared_ptr<Instruction>& insn) {
  loadQueue_.push_back(insn);
}
void LoadStoreQueue::addStore(const std::shared_ptr<Instruction>& insn) {
  storeQueue_.push_back({insn, {}});
}

void LoadStoreQueue::startLoad(const std::shared_ptr<Instruction>& insn) {
  const auto& ld_addresses = insn->getGeneratedAddresses();
  if (ld_addresses.size() == 0) {
    // Early execution if not addresses need to be accessed
    insn->execute();
    completedRequests_.push(insn);
  } else {
    // If the completion order is inorder, reserve an entry in
    // completedRequests_ now
    if (completionOrder_ == CompletionOrder::INORDER)
      completedRequests_.push(insn);

    // Detect reordering conflicts
    if (storeQueue_.size() > 0) {
      uint64_t seqId = insn->getSequenceId();
      for (auto itSt = storeQueue_.rbegin(); itSt != storeQueue_.rend();
           itSt++) {
        const auto& store = itSt->first;
        // If entry is earlier in the program order than load, detect conflicts
        if (store->getSequenceId() < seqId) {
          const auto& str_addresses = store->getGeneratedAddresses();
          // Iterate over possible overlaps between store and load addresses
          for (const auto& strAddr : str_addresses) {
            for (const auto& ldAddr : ld_addresses) {
              if (requestsOverlap(strAddr, ldAddr)) {
                // Conflict exists, add load instruction to conflictionMap_ and
                // delay until store retires
                conflictionMap_[store->getSequenceId()].push_back(insn);
                return;
              }
            }
          }
        }
      }
    }
    // No conflict found, process load
    requestLoadQueue_[tickCounter_ + insn->getLSQLatency()].push_back(insn);
    // Register active load
    // if (tid_ == 3)
    //   std::cerr << tickCounter_ << "|LSQ Register 0x" << std::hex
    //             << insn->getSequenceId() << std::dec << ":0x" << std::hex
    //             << insn->getInstructionId() << std::dec << std::endl;
    requestedLoads_.emplace(
        insn->getSequenceId(),
        std::pair<std::shared_ptr<Instruction>, uint64_t>({insn, 0}));
  }
}

void LoadStoreQueue::supplyStoreData(const std::shared_ptr<Instruction>& insn) {
  if (!insn->isStoreData()) return;
  // Get identifier values
  const uint64_t macroOpNum = insn->getInstructionId();
  const int microOpNum = insn->getMicroOpIndex();

  // Get data
  const auto& data = insn->getData();

  // Find storeQueue_ entry which is linked to the store data operation
  auto itSt = storeQueue_.begin();
  while (itSt != storeQueue_.end()) {
    auto& entry = itSt->first;
    // Pair entry and incoming store data operation with macroOp identifier and
    // microOp index value pre-detemined in microDecoder
    if (entry->getInstructionId() == macroOpNum &&
        entry->getMicroOpIndex() == microOpNum) {
      // Supply data to be stored by operations
      itSt->second = data;
      break;
    } else {
      itSt++;
    }
  }
}

void LoadStoreQueue::startStore(const std::shared_ptr<Instruction>& uop) {
  const auto& addresses = uop->getGeneratedAddresses();
  const auto& data = storeQueue_.front().second;

  // Early exit if there's no addresses to process
  if (addresses.size() == 0) {
    // TODO: Check if atomic lock needs to be released (not LL/SC monitor)
    return;
  }

  // Reset store's commit ready status as we need to determine any
  // post-memory-request values to be committed
  uop->setCommitReady(false);

  // Supply the data to store to the instruction. Can't be done in
  // `supplyStoreData` as addresses may not have been calculated
  assert(addresses.size() == data.size() &&
         "[SimEng:LoadStoreQueue] Tried to supply data to an store instruction "
         "with un-equal addresses and data items.");
  for (int i = 0; i < data.size(); i++) {
    uop->supplyData(addresses[i].vaddr, data[i]);
  }

  requestStoreQueue_[tickCounter_].push_back(uop);

  // If this instruction is a store conditional operation, track it
  if (uop->isStoreCond() && !uop->isCondResultReady()) {
    assert(requestedCondStore_ == nullptr &&
           "[SimEng:LoadStoreQueue] Tried to issue a second conditional store "
           "whilst one is already in flight.");
    requestedCondStore_ = uop;
    // If the completion order is inorder, reserve an entry in
    // completedRequests_ now
    if (completionOrder_ == CompletionOrder::INORDER)
      completedRequests_.push(uop);
  }
}

bool LoadStoreQueue::commitStore(const std::shared_ptr<Instruction>& uop) {
  assert(storeQueue_.size() > 0 &&
         "Attempted to commit a store from an empty queue");
  assert(storeQueue_.front().first->getSequenceId() == uop->getSequenceId() &&
         "Attempted to commit a store that wasn't present at the front of the "
         "store queue");

  const auto& addresses = uop->getGeneratedAddresses();

  // Early exit if there's no addresses to process
  if (addresses.size() == 0) {
    // TODO: Check if atomic lock needs to be released (not LL/SC monitor)
    storeQueue_.pop_front();
    return false;
  }

  // Check all loads that have requested memory
  violatingLoad_ = nullptr;
  for (const auto& load : requestedLoads_) {
    // Skip loads that are younger than the oldest violating load
    if (violatingLoad_ &&
        load.second.first->getSequenceId() > violatingLoad_->getSequenceId())
      continue;
    // Violation invalid if the load and store entries are generated by the same
    // instruction
    if (load.second.first->getSequenceId() != uop->getSequenceId()) {
      const auto& loadedAddresses = load.second.first->getGeneratedAddresses();
      // Iterate over store addresses
      for (const auto& storeReq : addresses) {
        // Iterate over load addresses
        for (const auto& loadReq : loadedAddresses) {
          // Check for overlapping requests, and flush if discovered
          if (requestsOverlap(storeReq, loadReq)) {
            violatingLoad_ = load.second.first;
          }
        }
      }
    }
  }

  // Resolve any conflictions on this store
  auto itr = conflictionMap_.find(uop->getSequenceId());
  if (itr != conflictionMap_.end()) {
    // For each load, we can now execute them given the conflicting
    // store has now been triggered
    auto ldVec = itr->second;
    for (auto load : ldVec) {
      requestLoadQueue_[tickCounter_ + 1 + load->getLSQLatency()].push_back(
          load);
      // if (tid_ == 3)
      //   std::cerr << tickCounter_ << "|LSQ conflict 0x" << std::hex
      //             << load->getSequenceId() << std::dec << ":0x" << std::hex
      //             << load->getInstructionId() << std::dec << std::endl;
      requestedLoads_.emplace(
          load->getSequenceId(),
          std::pair<std::shared_ptr<Instruction>, uint64_t>({load, 0}));
    }
    // Remove all entries for this store from conflictionMap_
    conflictionMap_.erase(itr);
  }

  storeQueue_.pop_front();
  return violatingLoad_ != nullptr;
}

void LoadStoreQueue::commitLoad(const std::shared_ptr<Instruction>& uop) {
  assert(loadQueue_.size() > 0 &&
         "Attempted to commit a load from an empty queue");
  assert(loadQueue_.front()->getSequenceId() == uop->getSequenceId() &&
         "Attempted to commit a load that wasn't present at the front of the "
         "load queue");

  auto it = loadQueue_.begin();
  while (it != loadQueue_.end()) {
    const auto& entry = *it;
    if (entry->isLoad()) {
      // if (tid_ == 3)
      //   std::cerr << tickCounter_ << "|LSQ Commit 0x" << std::hex
      //             << entry->getSequenceId() << std::dec << ":0x" << std::hex
      //             << entry->getInstructionId() << std::dec << std::endl;
      requestedLoads_.erase(entry->getSequenceId());
      it = loadQueue_.erase(it);
      break;
    } else {
      it++;
    }
  }
}

void LoadStoreQueue::purgeFlushed() {
  // Remove flushed loads from load queue
  auto itLd = loadQueue_.begin();
  while (itLd != loadQueue_.end()) {
    const auto& entry = *itLd;
    if (entry->isFlushed()) {
      // if (tid_ == 3)
      //   std::cerr << tickCounter_ << "|LSQ PURGED 0x" << std::hex
      //             << entry->getSequenceId() << std::dec << ":0x" << std::hex
      //             << entry->getInstructionId() << std::dec << std::endl;
      requestedLoads_.erase(entry->getSequenceId());
      itLd = loadQueue_.erase(itLd);
    } else {
      itLd++;
    }
  }

  // Remove flushed stores from store queue and confliction queue if an entry
  // exists
  auto itSt = storeQueue_.begin();
  while (itSt != storeQueue_.end()) {
    const auto& entry = itSt->first;
    if (entry->isFlushed()) {
      // Can erase all load entries as they must be younger than flushed store
      conflictionMap_.erase(entry->getSequenceId());
      itSt = storeQueue_.erase(itSt);
    } else {
      itSt++;
    }
  }

  // Remove flushed loads from confliction queue
  for (auto itCnflct = conflictionMap_.begin();
       itCnflct != conflictionMap_.end(); itCnflct++) {
    auto ldItr = itCnflct->second.begin();
    while (ldItr != itCnflct->second.end()) {
      if ((*ldItr)->isFlushed()) {
        ldItr = itCnflct->second.erase(ldItr);
      } else {
        ldItr++;
      }
    }
  }

  // Remove flushed loads and stores from request queues
  auto itLdReq = requestLoadQueue_.begin();
  while (itLdReq != requestLoadQueue_.end()) {
    auto itInsn = itLdReq->second.begin();
    while (itInsn != itLdReq->second.end()) {
      if ((*itInsn)->isFlushed()) {
        itInsn = itLdReq->second.erase(itInsn);
      } else {
        itInsn++;
      }
    }
    if (itLdReq->second.size() == 0) {
      itLdReq = requestLoadQueue_.erase(itLdReq);
    } else {
      itLdReq++;
    }
  }

  // Don't need to purge stores as they will only be sent at commit
}

void LoadStoreQueue::tick() {
  tickCounter_++;
  // Send memory requests
  // Index 0: loads, index 1: stores
  std::array<bool, 2> exceededLimits = {false, false};
  auto itLoad = requestLoadQueue_.begin();
  auto itStore = requestStoreQueue_.begin();
  while (requestLoadQueue_.size() + requestStoreQueue_.size() > 0) {
    // Choose which request type to schedule next
    bool chooseLoad = false;
    std::pair<bool, uint64_t> earliestLoad;
    std::pair<bool, uint64_t> earliestStore;
    // Determine if a load request can be scheduled
    if (requestLoadQueue_.size() == 0 || exceededLimits[accessType::LOAD]) {
      earliestLoad = {false, 0};
    } else {
      earliestLoad = {true, itLoad->first};
    }
    // Determine if a store request can be scheduled
    if (requestStoreQueue_.size() == 0 || exceededLimits[accessType::STORE]) {
      earliestStore = {false, 0};
    } else {
      earliestStore = {true, itStore->first};
    }
    // Choose between available requests favouring those constructed earlier
    // (store requests on a tie)
    if (earliestLoad.first) {
      chooseLoad = !(earliestStore.first &&
                     (earliestLoad.second >= earliestStore.second));
    } else if (!earliestStore.first) {
      break;
    }

    // Get next request to schedule
    auto& itReq = chooseLoad ? itLoad : itStore;

    // Check if earliest request is ready
    if (itReq->first <= tickCounter_) {
      // Identify request type
      uint8_t isStore = 0;
      if (!chooseLoad) {
        isStore = 1;
      }
      // Iterate over requests ready this cycle
      auto itInsn = itReq->second.begin();
      while (itInsn != itReq->second.end()) {
        // Schedule requests from the queue of addresses in
        // request[Load|Store]Queue_ entry
        bool accepted = false;
        if (isStore) {
          accepted = mmu_->requestWrite((*itInsn), (*itInsn)->getData());
        } else {
          accepted = mmu_->requestRead((*itInsn));
        }
        // if (accepted) {
        //   if (tid_ == 24)
        //     std::cerr << tickCounter_ << "|LSQ REQ accepted for 0x" <<
        //     std::hex
        //               << (*itInsn)->getSequenceId() << std::dec << ":0x"
        //               << std::hex << (*itInsn)->getInstructionId() <<
        //               std::dec
        //               << " at 0x" << std::hex
        //               << (*itInsn)->getGeneratedAddresses()[0].vaddr <<
        //               std::dec
        //               << std::endl;
        // } else {
        //   if (tid_ == 24)
        //     std::cerr << tickCounter_ << "|LSQ REQ rejected for 0x" <<
        //     std::hex
        //               << (*itInsn)->getSequenceId() << std::dec << ":0x"
        //               << std::hex << (*itInsn)->getInstructionId() <<
        //               std::dec
        //               << " at 0x" << std::hex
        //               << (*itInsn)->getGeneratedAddresses()[0].vaddr <<
        //               std::dec
        //               << std::endl;
        // }
        // requestedLoads_[(*itInsn)->getSequenceId()].second = tickCounter_;
        // Remove entry from vector if accepted (available bandwidth this
        // cycle)
        if (accepted) {
          itInsn = itReq->second.erase(itInsn);
        } else {
          // No more requests of this type can be scheduled this cycle
          exceededLimits[isStore] = true;
          break;
        }
      }

      // If all instructions for currently selected cycle in
      // request[Load|Store]Queue_ have been scheduled, erase entry
      if (itReq->second.size() == 0) {
        if (chooseLoad) {
          itReq = requestLoadQueue_.erase(itReq);
        } else {
          itReq = requestStoreQueue_.erase(itReq);
        }
      }
    } else {
      break;
    }
  }

  // Initialise completion counter
  size_t count = 0;

  // Process completed conditional store request
  // This only applies to a completion order of OoO
  // There's no need to check if it has been flushed as a conditional store must
  // be the next-to-retire instruction
  if (completionOrder_ == CompletionOrder::OUTOFORDER &&
      (requestedCondStore_ != nullptr)) {
    // Check to see if conditional store is ready, if yes then add to
    // completedRequests_ for result forwarding and passing to writeback
    if (requestedCondStore_->isCondResultReady()) {
      completedRequests_.push(requestedCondStore_);
      requestedCondStore_ = nullptr;
    }
  }

  // Process completed read requests
  // bool found = false;
  auto load = requestedLoads_.begin();
  while (load != requestedLoads_.end()) {
    // if (load->second.first->getInstructionId() == 0x58d3) found = true;
    if (load->second.first->hasAllData() &&
        !load->second.first->hasExecuted()) {
      // if (tid_ == 3)
      //   std::cerr << tickCounter_ << "|LSQ RET for 0x" << std::hex
      //             << load->second.first->getSequenceId() << std::dec << ":0x"
      //             << std::hex << load->second.first->getInstructionId()
      //             << std::dec << " at 0x" << std::hex
      //             << load->second.first->getGeneratedAddresses()[0].vaddr
      //             << std::dec << std::endl;
      // uint64_t lat = tickCounter_ - load->second.second;
      // if (latencies_.find(lat) == latencies_.end())
      //   latencies_[lat] = 1;
      // else
      //   latencies_[lat]++;

      // This load has completed
      load->second.first->execute();
      if (load->second.first->isStoreData()) {
        supplyStoreData(load->second.first);
      }
      // If the completion order is OoO, add entry to completedRequests_
      if (completionOrder_ == CompletionOrder::OUTOFORDER)
        completedRequests_.push(load->second.first);
    } else {
      // if (tid_ == 3)
      //   std::cerr << tickCounter_ << "|LSQ WAITING for 0x" << std::hex
      //             << load->second.first->getSequenceId() << std::dec << ":0x"
      //             << std::hex << load->second.first->getInstructionId()
      //             << std::dec << " at 0x" << std::hex
      //             << load->second.first->getGeneratedAddresses()[0].vaddr
      //             << std::dec
      //             << ", hasAllData: " << load->second.first->hasAllData()
      //             << ", hasExecuted: " << load->second.first->hasExecuted()
      //             << std::endl;
    }
    load++;
  }

  // if (tid_ == 3 && found)
  //   std::cerr << tickCounter_ << "|LSQ FOUND 0x58d3" << std::endl;
  // else if (tid_ == 3)
  //   std::cerr << tickCounter_ << "|LSQ NOT FOUND 0x58d3" << std::endl;

  // Pop from the front of the completed loads queue and send to writeback
  while (completedRequests_.size() > 0 && count < completionSlots_.size()) {
    // Skip a completion slot if stalled
    if (completionSlots_[count].isStalled()) {
      count++;
      continue;
    }

    auto& insn = completedRequests_.front();

    // Don't process load instruction if it has been flushed
    if (insn->isFlushed()) {
      completedRequests_.pop();
      continue;
    }

    // If the load at the front of the queue is yet to execute, continue
    // processing next cycle
    if (insn->isLoad() && !insn->hasExecuted()) {
      break;
    }

    if (insn->isStoreCond() && !insn->isCondResultReady()) {
      break;
    }

    // if (tid_ == 24 && insn->getGeneratedAddresses().size())
    //   std::cerr << tickCounter_ << "|LSQ RET for 0x" << std::hex
    //             << insn->getSequenceId() << std::dec << ":0x" << std::hex
    //             << insn->getInstructionId() << std::dec << " at 0x" <<
    //             std::hex
    //             << insn->getGeneratedAddresses()[0].vaddr << std::dec
    //             << std::endl;

    // Forward the results
    forwardOperands_(insn->getDestinationRegisters(), insn->getResults());

    completionSlots_[count].getTailSlots()[0] = std::move(insn);

    completedRequests_.pop();

    count++;
  }
}

std::shared_ptr<Instruction> LoadStoreQueue::getViolatingLoad() const {
  return violatingLoad_;
}

bool LoadStoreQueue::isCombined() const { return combined_; }

std::map<uint64_t, uint64_t> LoadStoreQueue::getLatencies() const {
  return latencies_;
}
void LoadStoreQueue::setTid(uint64_t tid) { tid_ = tid; }
uint64_t LoadStoreQueue::getTid() { return tid_; }

}  // namespace pipeline
}  // namespace simeng
