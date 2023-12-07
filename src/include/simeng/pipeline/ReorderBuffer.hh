#pragma once

#include <deque>
#include <functional>

#include "simeng/Instruction.hh"
#include "simeng/pipeline/LoadStoreQueue.hh"
#include "simeng/pipeline/RegisterAliasTable.hh"

namespace simeng {
namespace pipeline {

/** A branch prediction outcome with an associated instruction address. */
struct latestBranch {
  /** Branch instruction address. */
  uint64_t address;

  /** Outcome of the branch. */
  BranchPrediction outcome;

  /** The related instructionsCommitted_ value that this instruction was
   * committed on. */
  uint64_t commitNumber;
};

/** Check if the instruction ID is less/greater than a given value used by
 *  binary_search. */
struct idCompare {
  bool operator()(const std::shared_ptr<Instruction>& first,
                  const uint64_t second) {
    return first->getInstructionId() < second;
  }

  bool operator()(const uint64_t first,
                  const std::shared_ptr<Instruction>& second) {
    return first < second->getInstructionId();
  }
};

/** A Reorder Buffer (ROB) implementation. Contains an in-order queue of
 * in-flight instructions. */
class ReorderBuffer {
 public:
  /** Constructs a reorder buffer of maximum size `maxSize`, supplying a
   * reference to the register alias table. */
  ReorderBuffer(
      unsigned int maxSize, RegisterAliasTable& rat, LoadStoreQueue& lsq,
      std::function<void(const std::shared_ptr<Instruction>&)> raiseException,
      std::function<void(uint64_t branchAddress)> sendLoopBoundary,
      BranchPredictor& predictor, uint16_t loopBufSize,
      uint16_t loopDetectionThreshold);

  /** Add the provided instruction to the ROB. */
  void reserve(const std::shared_ptr<Instruction>& insn);

  void commitMicroOps(uint64_t insnId);

  /** Commit and remove up to `maxCommitSize` instructions. */
  unsigned int commit(unsigned int maxCommitSize);

  /** Flush all instructions with a instruction ID greater than `afterInsnId`.
   */
  void flush(uint64_t afterInsnId);

  /** Flush all instructions from ROB. Intended for use during context switch.
   */
  void flush();

  /** Retrieve the current size of the ROB. */
  unsigned int size() const;

  /** Retrieve the current amount of free space in the ROB. */
  unsigned int getFreeSpace() const;

  /** Query whether a memory order violation was discovered in the most recent
   * cycle. */
  bool shouldFlush() const;

  /** Retrieve the instruction address associated with the most recently
   * discovered memory order violation. */
  uint64_t getFlushAddress() const;

  /** Retrieve the instruction ID associated with the most recently discovered
   * memory order violation. */
  uint64_t getFlushInsnId() const;

  /** Get the number of instructions the ROB has committed. */
  uint64_t getInstructionsCommittedCount() const;

  /** Get the number of speculated loads which violated load-store ordering. */
  uint64_t getViolatingLoadsCount() const;

  void setTid(uint64_t tid);

  uint64_t getNumLoads() const { return numLoads_; };
  uint64_t getNumStores() const { return numStores_; };
  uint64_t getLastAddr() const { return lastAddr_; }
  uint64_t getHeadOfBuffer() const {
    if (buffer_.size())
      return buffer_.front()->getInstructionAddress();
    else
      return 0;
  }

 private:
  /** A reference to the register alias table. */
  RegisterAliasTable& rat_;

  /** A reference to the load/store queue. */
  LoadStoreQueue& lsq_;

  /** The maximum size of the ROB. */
  unsigned int maxSize_;

  /** A function to call upon exception generation. */
  std::function<void(std::shared_ptr<Instruction>)> raiseException_;

  /** A function to send an instruction at a detected loop boundary. */
  std::function<void(uint64_t branchAddress)> sendLoopBoundary_;

  /** Whether or not a loop has been detected. */
  bool loopDetected_ = false;

  /** A reference to the current branch predictor. */
  BranchPredictor& predictor_;

  /** The buffer containing in-flight instructions. */
  std::deque<std::shared_ptr<Instruction>> buffer_;

  /** Whether the core should be flushed after the most recent commit. */
  bool shouldFlush_ = false;

  /** The target instruction address the PC should be reset to after the most
   * recent commit.
   */
  uint64_t pc_;

  /** The instruction ID of the youngest instruction that should remain after
   * the current flush. */
  uint64_t flushAfter_;

  /** Latest retired branch outcome with a counter. */
  std::pair<latestBranch, uint64_t> branchCounter_ = {{0, {false, 0}, 0}, 0};

  /** Loop buffer size. */
  uint16_t loopBufSize_;

  /** Amount of times a branch must be seen without interruption for it to be
   * considered a loop. */
  uint16_t loopDetectionThreshold_;

  /** The number of instructions committed. */
  uint64_t instructionsCommitted_ = 0;

  /** The number of speculatived loads which violated load-store ordering. */
  uint64_t loadViolations_ = 0;

  /** Whether a store, at the front of the ROB, has memory accesses currently
   * being processed. */
  bool startedStore_ = false;

  /** Indicates whether the atomic (or load-reserved) at the front of the buffer
   * has been started. */
  bool sentAtomic_ = false;

  uint64_t tid_ = 0;

  uint64_t numStores_ = 0;
  uint64_t numLoads_ = 0;

  std::ofstream fileOut_;

  uint64_t lastAddr_ = 0;
};

}  // namespace pipeline
}  // namespace simeng
