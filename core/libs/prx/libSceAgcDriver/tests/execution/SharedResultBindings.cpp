#include "Recompiler.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ShaderRecompiler;

namespace {

void Require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

DescriptorBinding binding(std::uint32_t slot, std::vector<std::uint32_t> words) {
    DescriptorBinding made{};
    made.kind = DescriptorKind::StorageBuffer;
    made.role = DescriptorRole::GuestBuffers;
    made.binding = slot;
    made.count = 1;
    made.guestDescriptor = std::move(words);
    made.bufferWritten = {true};
    return made;
}

void copyShares() {
    RecompileResult donor;
    donor.bindings = {binding(0, {1, 2}), binding(1, {3}), binding(2, {4})};
    donor.parameterExports = {7u, 8u};
    donor.pushConstants.resize(8);
    auto patched = std::make_shared<RecompileResult>(donor);
    patched->bindings.Patch(1).guestDescriptor = {30, 31};
    patched->bindings.Patch(2).guestDescriptor[0] = 40;
    patched->bindings.Patch(1).guestDescriptor[0] = 33;
    const auto& reader = *patched;
    Require(reader.bindings.size() == 3 && reader.bindings[0].guestDescriptor == std::vector<std::uint32_t>{1, 2}, "an untouched binding reads through");
    Require(reader.bindings[1].guestDescriptor == std::vector<std::uint32_t>{33, 31} && reader.bindings.at(2).guestDescriptor[0] == 40, "a patched binding reads its own words");
    Require(reader.bindings[1].bufferWritten == std::vector<bool>{true} && reader.bindings[1].binding == 1, "a patched binding keeps its shape");
    const auto& held = donor;
    Require(held.bindings[1].guestDescriptor == std::vector<std::uint32_t>{3} && held.bindings[2].guestDescriptor[0] == 4, "a patch leaves the donor as it was");
    std::vector<std::uint32_t> slots;
    for (const auto& item : reader.bindings) slots.push_back(item.binding);
    Require(slots == std::vector<std::uint32_t>{0, 1, 2}, "iteration keeps the order");
    Require(std::ranges::find_if(reader.bindings, [](const DescriptorBinding& item) { return item.binding == 2; }) - reader.bindings.begin() == 2, "the list is a random access range");
    static_assert(std::ranges::random_access_range<const BindingList>);
    RecompileResult copied = reader;
    Require(std::as_const(copied).bindings[1].guestDescriptor[1] == 31, "a copy of a patched result keeps its patches");
    copied.bindings[0].guestDescriptor[0] = 100;
    Require(std::as_const(copied).bindings[1].guestDescriptor[1] == 31 && std::as_const(copied).bindings[0].guestDescriptor[0] == 100, "a mutable access flattens the patches into its own copy");
    Require(reader.bindings[0].guestDescriptor[0] == 1 && held.bindings[0].guestDescriptor[0] == 1, "a mutable access reaches no other result");
    copied.parameterExports.Mutable()[0] = 9;
    Require(reader.parameterExports[0] == 7u && copied.parameterExports[0] == 9u, "a shared vector clones at its writer");
    RecompileResult only;
    only.bindings = {binding(0, {1})};
    only.bindings.Patch(0).guestDescriptor[0] = 5;
    Require(std::as_const(only).bindings[0].guestDescriptor[0] == 5, "patching an unshared list writes it in place");
    bool threw = false;
    try {
        only.bindings.Patch(3);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    Require(threw, "a patch past the end throws");
    only.bindings.clear();
    Require(only.bindings.empty(), "a cleared list is empty");
}

}

int main() {
    try {
        copyShares();
        std::cout << "shared result bindings ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
