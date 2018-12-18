#include "registerFile.hh"

namespace simeng {

RegisterFile::RegisterFile(int registerCount) {
    registers = std::vector<RegisterValue>(registerCount);

    for (auto i = 0; i < registerCount; i++) {
        registers[i] = RegisterValue(0, 8);
    }
}

RegisterValue RegisterFile::get(Register reg) {
    return registers[reg];
}

void RegisterFile::set(Register reg, const RegisterValue &value) {
    registers[reg] = value;
}

}
