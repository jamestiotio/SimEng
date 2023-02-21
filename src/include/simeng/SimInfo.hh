#pragma once

#include <string>

#include "simeng/Config.hh"
#include "simeng/RegisterFileSet.hh"

namespace simeng {
/** Enum representing the possible simulation modes. */
enum simMode { emulation, inorder, outoforder };

/** Enum representing the possible ISAs. */
enum ISA { AArch64, RV64 };

/** A SimInfo class to hold values specific to the current simulation. */
class SimInfo {
 public:
  /** Returns the simulation mode of the current SimEng instance. */
  static simMode getSimMode() { return getInstance()->mode_; }

  /** Returns the simulation mode of the current SimEng instance as a string. */
  static std::string getSimModeStr() { return getInstance()->modeStr_; }

  /** Returns which ISA the current simulation is using. */
  static ISA getISA() { return getInstance()->isa_; }

  /** Returns a vector of {size, number} pairs describing the available
   * architectural registers. */
  static const std::vector<simeng::RegisterFileStructure>& getArchRegStruct() {
    return getInstance()->archRegStruct_;
  }

  /** Returns a vector of Capstone arm64_sysreg enums for all the system
   * registers that should be utilised in simulation. */
  static const std::vector<arm64_sysreg>& getSysRegVec() {
    return getInstance()->sysRegisterEnums_;
  }

  /** Returns whether or not the special files directories should be generated.
   */
  static const bool getGenSpecFiles() {
    return getInstance()->genSpecialFiles_;
  }

  /** Public function used to reset the architectural register file structure.
   */
  static void resetArchRegs() { getInstance()->resetArchRegStruct(); }

 private:
  SimInfo() {
    // Get Config File
    YAML::Node& config = Config::get();
    // Get ISA type
    if (config["Core"]["ISA"].as<std::string>() == "AArch64") {
      isa_ = ISA::AArch64;
      // Define system registers
      sysRegisterEnums_ = {arm64_sysreg::ARM64_SYSREG_DCZID_EL0,
                           arm64_sysreg::ARM64_SYSREG_FPCR,
                           arm64_sysreg::ARM64_SYSREG_FPSR,
                           arm64_sysreg::ARM64_SYSREG_TPIDR_EL0,
                           arm64_sysreg::ARM64_SYSREG_MIDR_EL1,
                           arm64_sysreg::ARM64_SYSREG_CNTVCT_EL0,
                           arm64_sysreg::ARM64_SYSREG_PMCCNTR_EL0,
                           arm64_sysreg::ARM64_SYSREG_SVCR};
      // Initialise architectural reg structures
      uint16_t numSysRegs = static_cast<uint16_t>(sysRegisterEnums_.size());
      const uint16_t ZAsize = static_cast<uint16_t>(
          config["Core"]["Streaming-Vector-Length"].as<uint64_t>() /
          8);  // Convert to bytes
      archRegStruct_ = {
          {8, 32},          // General purpose
          {256, 32},        // Vector
          {32, 17},         // Predicate
          {1, 1},           // NZCV
          {8, numSysRegs},  // System
          {256, ZAsize},    // Matrix (Each row is a register)
      };
    } else if (config["Core"]["ISA"].as<std::string>() == "rv64") {
      isa_ = ISA::RV64;
      // Define system registers
      sysRegisterEnums_ = {};
      // Initialise architectural reg structures
      uint16_t numSysRegs = static_cast<uint16_t>(sysRegisterEnums_.size());
      archRegStruct_ = {
          {8, 32},          // General purpose
          {8, 32},          // Floating Point
          {8, numSysRegs},  // System
      };
    }

    // Get Simulation mode
    if (config["Core"]["Simulation-Mode"].as<std::string>() == "emulation") {
      mode_ = simMode::emulation;
      modeStr_ = "Emulation";
    } else if (config["Core"]["Simulation-Mode"].as<std::string>() ==
               "inorderpipelined") {
      mode_ = simMode::inorder;
      modeStr_ = "In-Order Pipelined";
    } else if (config["Core"]["Simulation-Mode"].as<std::string>() ==
               "outoforder") {
      mode_ = simMode::outoforder;
      modeStr_ = "Out-of-Order";
    }

    // Get if special files directory should be created
    genSpecialFiles_ = config["CPU-Info"]["Generate-Special-Dir"].as<bool>();
  }

  /** Gets the static instance of the SimInfo class. */
  static std::unique_ptr<SimInfo>& getInstance() {
    static std::unique_ptr<SimInfo> SimInfoClass = nullptr;
    if (SimInfoClass == nullptr) {
      SimInfoClass = std::unique_ptr<SimInfo>(new SimInfo());
    }
    return SimInfoClass;
  }

  /** Function used to reset the architectural register file structure. */
  void resetArchRegStruct() {
    // Given some register quantities rely on Config file arguments (SME relies
    // on SVL), it is possible that if the config was to change the register
    // quantities would be incorrect. This function provides a way to reset the
    // Architectural register structure.
    YAML::Node& config = Config::get();
    if (isa_ == ISA::AArch64) {
      uint16_t numSysRegs = static_cast<uint16_t>(sysRegisterEnums_.size());
      const uint16_t ZAsize = static_cast<uint16_t>(
          config["Core"]["Streaming-Vector-Length"].as<uint64_t>() /
          8);  // Convert to bytes
      archRegStruct_ = {
          {8, 32},          // General purpose
          {256, 32},        // Vector
          {32, 17},         // Predicate
          {1, 1},           // NZCV
          {8, numSysRegs},  // System
          {256, ZAsize},    // Matrix (Each row is a register)
      };
    } else if (isa_ == ISA::RV64) {
      uint16_t numSysRegs = static_cast<uint16_t>(sysRegisterEnums_.size());
      archRegStruct_ = {
          {8, 32},          // General purpose
          {8, 32},          // Floating Point
          {8, numSysRegs},  // System
      };
    }
  }

  /** The simulation mode of current execution of SimEng. */
  simMode mode_;

  /** The simulation mode String of current execution of SimEng. */
  std::string modeStr_;

  /** Architecture type of the current execution of SimEng. */
  ISA isa_;

  /** Architectural Register Structure of the current execution of SimEng. */
  std::vector<simeng::RegisterFileStructure> archRegStruct_;

  /** Vector of all system register Capsone enum values used in Architecture. */
  std::vector<arm64_sysreg> sysRegisterEnums_;

  /** Bool representing if the special file directory should be created. */
  bool genSpecialFiles_;
};
}  // namespace simeng