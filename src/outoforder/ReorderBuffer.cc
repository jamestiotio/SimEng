#include "ReorderBuffer.hh"

#include <algorithm>
#include <cassert>

namespace simeng {
namespace outoforder {

ReorderBuffer::ReorderBuffer(
    unsigned int maxSize, RegisterAliasTable& rat, LoadStoreQueue& lsq,
    std::function<void(std::shared_ptr<Instruction>)> raiseException)
    : rat(rat), lsq(lsq), maxSize(maxSize), raiseException(raiseException) {}

void ReorderBuffer::reserve(std::shared_ptr<Instruction> insn) {
  assert(buffer.size() < maxSize &&
         "Attempted to reserve entry in reorder buffer when already full");
  insn->setSequenceId(seqId);
  seqId++;
  buffer.push_back(insn);
}

unsigned int ReorderBuffer::commit(unsigned int maxCommitSize) {
  shouldFlush_ = false;
  size_t maxCommits =
      std::min(static_cast<size_t>(maxCommitSize), buffer.size());

  unsigned int n;
  for (n = 0; n < maxCommits; n++) {
    auto& uop = buffer[0];
    if (!uop->canCommit()) {
      break;
    }

    if (uop->exceptionEncountered()) {
      raiseException(uop);
      buffer.pop_front();
      return n + 1;
    }

    const auto& destinations = uop->getDestinationRegisters();
    for (const auto& reg : destinations) {
      rat.commit(reg);
    }

    // If it's a memory op, commit the entry at the head of the respective queue
    if (uop->isStore()) {
      bool violationFound = lsq.commitStore(uop);
      if (violationFound) {
        // Memory order violation found; aborting commits and flushing
        auto load = lsq.getViolatingLoad();
        shouldFlush_ = true;
        flushAfter = load->getSequenceId() - 1;
        pc = load->getInstructionAddress();

        buffer.pop_front();
        return n + 1;
      }
    } else if (uop->isLoad()) {
      lsq.commitLoad(uop);
    }
    buffer.pop_front();
  }

  return n;
}

void ReorderBuffer::flush(uint64_t afterSeqId) {
  // Iterate backwards from the tail of the queue to find and remove ops newer
  // than `afterSeqId`
  while (!buffer.empty()) {
    auto& uop = buffer.back();
    if (uop->getSequenceId() <= afterSeqId) {
      break;
    }

    for (const auto& reg : uop->getDestinationRegisters()) {
      rat.rewind(reg);
    }
    uop->setFlushed();
    buffer.pop_back();
  }
}

unsigned int ReorderBuffer::size() const { return buffer.size(); }

unsigned int ReorderBuffer::getFreeSpace() const {
  return maxSize - buffer.size();
}

bool ReorderBuffer::shouldFlush() const { return shouldFlush_; }
uint64_t ReorderBuffer::getFlushAddress() const { return pc; }
uint64_t ReorderBuffer::getFlushSeqId() const { return flushAfter; }

}  // namespace outoforder
}  // namespace simeng
