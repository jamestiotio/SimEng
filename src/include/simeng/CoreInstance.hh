#pragma once

#include <cstdint>
#include <string>

#include "simeng/AlwaysNotTakenPredictor.hh"
#include "simeng/Core.hh"
#include "simeng/Elf.hh"
#include "simeng/GenericPredictor.hh"
#include "simeng/OS/SimOS.hh"
#include "simeng/OS/SyscallHandler.hh"
#include "simeng/arch/Architecture.hh"
#include "simeng/arch/aarch64/Architecture.hh"
#include "simeng/arch/riscv/Architecture.hh"
#include "simeng/config/SimInfo.hh"
#include "simeng/memory/MMU.hh"
#include "simeng/models/emulation/Core.hh"
#include "simeng/models/inorder/Core.hh"
#include "simeng/models/outoforder/Core.hh"
#include "simeng/pipeline/A64FXPortAllocator.hh"
#include "simeng/pipeline/BalancedPortAllocator.hh"

namespace simeng {

/** The available modes of simulation. */
enum class SimulationMode { Emulation, InOrderPipelined, OutOfOrder };

/** A class to create a SimEng core instance from a supplied config. */
class CoreInstance {
 public:
  /** Constructor with an executable, its arguments, and a model configuration.
   */
  CoreInstance(
      std::shared_ptr<memory::MMU> mmu,
      arch::sendSyscallToHandler handleSyscall,
      std::function<void(OS::cpuContext, uint16_t, CoreStatus, uint64_t)>
          updateCoreDescInOS);

  ~CoreInstance(){};

  /** Construct the core and all its associated simulation objects after the
   * process and memory interfaces have been instantiated. */
  void createCore();

  /** Getter for the create core object. */
  std::shared_ptr<simeng::Core> getCore() const;

 private:
  /** The config file describing the modelled core to be created. */
  ryml::ConstNodeRef config_;

  /** Reference to the SimEng SimOS Process object. */
  std::shared_ptr<simeng::OS::Process> process_ = nullptr;

  /** Reference to the SimEng architecture object. */
  std::unique_ptr<simeng::arch::Architecture> arch_ = nullptr;

  /** Reference to the SimEng branch predictor object. */
  std::unique_ptr<simeng::BranchPredictor> predictor_ = nullptr;

  /** Reference to the SimEng port allocator object. */
  std::unique_ptr<simeng::pipeline::PortAllocator> portAllocator_ = nullptr;

  /** Reference to the SimEng core object. */
  std::shared_ptr<simeng::Core> core_ = nullptr;

  /** Reference to the MMU */
  std::shared_ptr<simeng::memory::MMU> mmu_ = nullptr;

  /** Callback function passed to the Core class to communicate a syscall
   * generated by the Core's exception handler to the simulated Operating
   * System's syscall handler. */
  arch::sendSyscallToHandler handleSyscall_;

  /** Callback function passed to OoO core so that core updates can
   * be sent to SimOS asynchronously. */
  std::function<void(OS::cpuContext, uint16_t, CoreStatus, uint64_t)>
      updateCoreDescInOS_;
};

}  // namespace simeng
