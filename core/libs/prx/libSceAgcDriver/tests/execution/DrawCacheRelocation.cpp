#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "Optimization/SrtWalker.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgc/Shader/include/ShaderConstants.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DrawMiss;

alignas(256) constexpr std::array<std::uint32_t, 8> VertexCode{0x7e000208u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> SecondVertexCode = VertexCode;
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0xf4000100u, 0xfa000000u, 0x7e000204u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 16> TriangleVertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 16> ForwardedPixelCode{
    0xf4040080u, 0xfa000000u, 0xbf8cc07fu, 0xf4000101u, 0xfa000000u, 0xbf8cc07fu, 0x7e000204u, 0xd5480001u,
    0x02050800u, 0x7e020d01u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};
// s_swappc_b64 s[10:11], s[8:9] (the fetch call, through user words 0 and 1), exp pos0 v8..v11: the position is
// the vertex attribute the header's one input semantic places in v8.
alignas(256) constexpr std::array<std::uint32_t, 4> FetchVertexCode{0xbe8a2108u, 0xf80008cfu, 0x0b0a0908u, 0xbf810000u};
alignas(16) std::array<float, 4> Other{0.5f, 0.0f, 0.0f, 0.0f};
alignas(16) std::array<float, 4> ForwardedData{};
alignas(32) std::array<std::array<std::uint64_t, 4>, 6> AlignedTables{};
alignas(32) std::array<std::uint64_t, 4> OffsetTable{};
alignas(256) std::array<std::byte, 1024> ColorMemory{};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};
alignas(8) Header SecondVertexHeader{};
alignas(8) Header TriangleVertexHeader{};
alignas(8) Header ForwardedPixelHeader{};

struct FetchHeader {
    Shader shader{};
    ShaderUserData userData{};
    std::array<std::uint16_t, ShaderRegs::AGC_DIRECT_RESOURCE_TYPE_COUNT> direct{};
    ShaderSemantic semantic{};
};

alignas(8) FetchHeader FetchVertexHeader{};

// The vertex buffer table pointer is in user words 4 and 5, the attribute table pointer in 6 and 7.
constexpr std::uint32_t VertexTableWord = 4;
constexpr std::uint32_t AttributeTableWord = 6;

const Shader* Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
    return &header.shader;
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void RegisterFetchVertex() {
    auto& header = FetchVertexHeader;
    header.direct.fill(ShaderRegs::AGC_ILLEGAL_DIRECT_OFFSET);
    header.direct[static_cast<std::size_t>(ShaderRegs::AgcDirectResourceType::PtrVertexBufferTable)] = VertexTableWord;
    header.direct[static_cast<std::size_t>(ShaderRegs::AgcDirectResourceType::PtrVertexAttribDescTable)] = AttributeTableWord;
    header.userData.direct_resource_offset = header.direct.data();
    header.userData.direct_resource_count = static_cast<std::uint16_t>(header.direct.size());
    header.semantic = {};
    header.semantic.hardware_mapping = 8;
    header.semantic.size_in_elements = 4;
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(FetchHeader);
    header.shader.shader_size = sizeof(FetchVertexCode);
    header.shader.code = FetchVertexCode.data();
    header.shader.type = 2;
    header.shader.user_data = &header.userData;
    header.shader.input_semantics = &header.semantic;
    header.shader.num_input_semantics = 1;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

std::vector<std::uint32_t> DrawFrom(std::initializer_list<const void*> sources, const std::uint32_t* vertexCode = VertexCode.data(), const std::uint32_t* pixelCode = PixelCode.data(), std::span<const std::uint32_t> vertexWords = {}) {
    const auto color = reinterpret_cast<std::uintptr_t>(ColorMemory.data());
    const auto vertex = reinterpret_cast<std::uintptr_t>(vertexCode);
    const auto pixel = reinterpret_cast<std::uintptr_t>(pixelCode);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(vertexWords.size())) << 1u}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 2u << 1u},
    };
    for (std::uint32_t word = 0; word < vertexWords.size(); ++word) shader.emplace_back(0x8cu + word, vertexWords[word]);
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    for (const auto* source : sources) {
        const auto address = reinterpret_cast<std::uintptr_t>(source);
        Append(commands, 0x76, {0x00c, static_cast<std::uint32_t>(address)});
        Append(commands, 0x76, {0x00d, static_cast<std::uint32_t>(address >> 32u)});
        Append(commands, 0x2d, {3, 2});
    }
    return commands;
}

void MovedDataHitsTheCache(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto draw = [&](std::initializer_list<const void*> sources) {
        auto commands = DrawFrom(sources);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    static std::array<std::array<float, 4>, 8> same{};
    for (auto& entry : same) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (std::size_t i = 0; i < 4; ++i) counters = draw({same[i].data()});
    Require(counters.lookups == 4 && counters.hits == 0 && counters.inserts == 2, mode + ": the draws that teach the moved pointer and its contents made " + std::to_string(counters.hits) + " hits and " + std::to_string(counters.inserts) + " inserts, expected 0 and 2");
    for (std::size_t i = 4; i < same.size(); ++i) counters = draw({same[i].data()});
    Require(counters.lookups == 8 && counters.hits == 4 && counters.inserts == 2, mode + ": draws whose data moved again did not reuse the cached draw (" + std::to_string(counters.hits) + " hits in " + std::to_string(counters.lookups) + " lookups)");
    const auto other = draw({Other.data()});
    Require(other.lookups == 9 && other.hits == 4, mode + ": a draw whose moved data holds another value reused the cached draw");
    const auto again = draw({same[0].data()});
    Require(again.lookups == 10 && again.hits == 5, mode + ": a draw at an address seen before did not reuse the cached draw (" + std::to_string(again.hits) + " hits)");
}

void EvictedEntriesServeNoRelocation(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    Registered(SecondVertexHeader, SecondVertexCode.data(), sizeof(SecondVertexCode), 2);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto draw = [&](const float* source, const std::uint32_t* vertexCode) {
        auto commands = DrawFrom({source}, vertexCode);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    static std::array<std::array<float, 4>, 6> same{};
    for (auto& entry : same) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    alignas(16) static std::array<std::array<float, 4>, 3> others{};
    for (std::size_t i = 0; i < others.size(); ++i) others[i] = {0.25f + 0.03125f * static_cast<float>(i), 0.0f, 0.0f, 0.0f};
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (std::size_t i = 0; i < 4; ++i) counters = draw(same[i].data(), VertexCode.data());
    Require(counters.lookups == 4 && counters.hits == 0 && counters.inserts == 2, mode + ": the draws that teach the moved pointer and its contents made " + std::to_string(counters.hits) + " hits and " + std::to_string(counters.inserts) + " inserts, expected 0 and 2");
    const auto relocated = draw(same[4].data(), VertexCode.data());
    Require(relocated.hits == 1, mode + ": a draw whose data moved after the teaching draws did not reuse the cached draw (" + std::to_string(relocated.hits) + " hits)");
    auto evicted = relocated;
    for (const auto& entry : others) {
        draw(entry.data(), SecondVertexCode.data());
        evicted = draw(entry.data(), SecondVertexCode.data());
    }
    Require(evicted.evictions - relocated.evictions >= 2 && evicted.inserts - relocated.inserts >= 2, mode + ": draws of another vertex shader did not evict the two taught entries from the draw cache (" + std::to_string(evicted.inserts - relocated.inserts) + " inserts, " + std::to_string(evicted.evictions - relocated.evictions) + " evictions; run with APS5_DRAW_CACHE_ENTRIES=2)");
    const auto after = draw(same[5].data(), VertexCode.data());
    Require(after.lookups == evicted.lookups + 1 && after.hits == evicted.hits, mode + ": a draw whose data moved reused a draw the cache had evicted, through its relocation alias (" + std::to_string(after.hits - evicted.hits) + " hits)");
}

void RelocatingDrawsKeepTheirDonor(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    Registered(SecondVertexHeader, SecondVertexCode.data(), sizeof(SecondVertexCode), 2);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto draw = [&](const float* source, const std::uint32_t* vertexCode) {
        auto commands = DrawFrom({source}, vertexCode);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    constexpr std::size_t Rounds = 3;
    alignas(16) static std::array<std::array<float, 4>, Rounds> others{};
    for (std::size_t i = 0; i < others.size(); ++i) others[i] = {0.25f + 0.03125f * static_cast<float>(i), 0.0f, 0.0f, 0.0f};
    static std::array<std::array<float, 4>, 5 + Rounds> same{};
    for (auto& entry : same) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (std::size_t i = 0; i < 4; ++i) counters = draw(same[i].data(), VertexCode.data());
    Require(counters.lookups == 4 && counters.hits == 0 && counters.inserts == 2, mode + ": the draws that teach the moved pointer and its contents made " + std::to_string(counters.hits) + " hits and " + std::to_string(counters.inserts) + " inserts, expected 0 and 2");
    const auto start = draw(same[4].data(), VertexCode.data());
    Require(start.hits == 1, mode + ": a draw whose data moved after the teaching draws did not reuse the cached draw (" + std::to_string(start.hits) + " hits)");
    auto relocated = start;
    for (std::size_t i = 0; i < others.size(); ++i) {
        draw(others[i].data(), SecondVertexCode.data());
        const auto inserted = draw(others[i].data(), SecondVertexCode.data());
        relocated = draw(same[5 + i].data(), VertexCode.data());
        Require(relocated.hits == inserted.hits + 1, mode + ": a draw whose data moved again lost the cached draw it reused before each of " + std::to_string(i + 1) + " rounds of another vertex shader's draws (" + std::to_string(inserted.inserts - start.inserts) + " inserts, " + std::to_string(inserted.evictions - start.evictions) + " evictions)");
    }
    Require(relocated.evictions - start.evictions >= 2, mode + ": draws of another vertex shader evicted " + std::to_string(relocated.evictions - start.evictions) + " entries, expected at least 2 (run with APS5_DRAW_CACHE_ENTRIES=2)");
}

void SplitRunsNameTheirSharedWords() {
    alignas(16) static std::array<std::uint32_t, 4> data{1u, 2u, 3u, 4u};
    auto table = std::make_shared<ShaderRecompiler::SrtGuestWords>();
    table->words = {5u, 6u, 7u, 8u};
    const auto dataAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data.data()));
    const auto tableAddress = dataAddress + 0x1000u;
    const std::array<ShaderRecompiler::MemoryRegion, 2> regions{{{dataAddress, std::as_bytes(std::span(data))}, {tableAddress, std::as_bytes(std::span(table->words))}}};
    ShaderRecompiler::SrtUserDataUse use;
    use.asBase = 0x3u;
    use.reads = {{dataAddress, 0}, {dataAddress + 8u, 1}};
    const std::array<std::shared_ptr<const ShaderRecompiler::SrtGuestWords>, 1> supplied{table};
    AgcDriver::DriverDetail::DispatchVariant variant;
    AgcDriver::DriverDetail::Driver::tagVariantRuns(variant, regions, use, true, 0, supplied);
    Require(variant.runs.size() > regions.size(), "two user-data bases into one region did not split it into several runs");
    Require(variant.shared.size() == 1u && variant.shared.front().regions == 1u, "the table region's words were not recorded as one shared run");
    const auto& shared = variant.shared.front();
    std::size_t first = 0;
    for (std::size_t run = 0; run < shared.region && run < variant.runs.size(); ++run) first += static_cast<std::size_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t));
    Require(shared.region < variant.runs.size() && variant.runs[shared.region].first == tableAddress && first == shared.first, "the shared words of a variant whose runs were split for relocation name run " + std::to_string(shared.region) + " of " + std::to_string(variant.runs.size()) + ", not the table's run " + std::to_string(variant.runs.size() - 1u));
}

std::uint32_t Low(const void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer) >> 32u);
}

std::array<std::uint32_t, 4> VertexBufferSharp(const void* data, std::uint32_t stride = 16u, std::uint32_t records = 3u, std::uint32_t format = 77u) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), records, 0x01000facu | (format << 12u)};
}

void VertexTableRunsMoveWithTheirPointer() {
    using namespace AgcDriver::DriverDetail;
    alignas(16) static std::array<float, 12> data{};
    alignas(16) static std::array<std::uint32_t, 4> table = VertexBufferSharp(data.data());
    alignas(16) static std::array<std::uint32_t, 1> attributes{0u};
    DrawProgram program{};
    program.binary.stage = ShaderRecompiler::ShaderStage::Vertex;
    program.userData = {0x10000000u, 1u, 0u, 0u, Low(table.data()), High(table.data()), Low(attributes.data()), High(attributes.data())};
    ShaderRecompiler::ShaderVertexStageInfo info{};
    info.fetchEmbedded = true;
    info.resourcesNum = 1;
    info.fetchBufferReg = VertexTableWord;
    info.fetchAttribReg = AttributeTableWord;
    info.resources[0].fields = table;
    std::vector<AgcDriver::Graphics::DecodeRead> reads(2);
    reads[0].address = reinterpret_cast<std::uintptr_t>(attributes.data());
    reads[0].size = 4;
    reads[1].address = reinterpret_cast<std::uintptr_t>(table.data());
    reads[1].size = 16;
    std::memcpy(reads[1].data.data(), table.data(), 16);
    ShaderRecompiler::RecompileResult compiled;
    compiled.vertexAttributes = {{0u, 4u, {table}, 0u}};
    const ShaderRecompiler::SrtUserDataUse use;
    PoolRelocation pool;
    Require(Driver::vertexTableRelocation(program, info, reads, compiled, use, pool), "a vertex stage whose decode read only its two tables was not relocatable");
    Require(pool.words == std::uint64_t {1} << VertexTableWord && pool.movable.size() == 1u && pool.rebases.size() == 1u && pool.reads.size() == 4u, "the vertex buffer table pointer did not move with its V#'s four words, one base read again and one vertex attribute rebased");
    std::vector<ShaderRecompiler::MemoryRegion> regions{{reads[0].address, reads[0].Bytes()}, {reads[1].address, reads[1].Bytes()}};
    std::stable_sort(regions.begin(), regions.end(), [](const auto& a, const auto& b) { return a.guestAddress < b.guestAddress; });
    DispatchVariant variant;
    Driver::tagVariantRuns(variant, regions, use, true, 0, {}, &pool);
    Require(variant.relocatableWords == std::uint64_t {1} << VertexTableWord, "the tagged vertex stage does not relocate exactly its vertex buffer table pointer");
    std::size_t position = 0, sharpPosition = ~std::size_t {0};
    for (std::size_t run = 0; run < variant.runs.size(); ++run) {
        const bool sharp = variant.runs[run].first == reads[1].address;
        Require(run < variant.runBases.size() && variant.runBases[run] == (sharp ? static_cast<std::int8_t>(VertexTableWord) : std::int8_t {-1}), "a run of the vertex stage has the wrong relocation base");
        if (sharp) sharpPosition = position;
        position += static_cast<std::size_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t));
    }
    Require(variant.movableWords == std::vector<std::uint32_t>{static_cast<std::uint32_t>(sharpPosition)}, "the V#'s base word is not the one word a relocated hit reads again");
    Require(variant.rebases.size() == 1u && variant.rebases[0].kind == VariantRebase::Kind::VertexBase && variant.rebases[0].target == 0u && variant.rebases[0].source == sharpPosition, "the vertex attribute is not rebased from the V#'s base word");

    auto other = compiled;
    other.vertexAttributes[0].resource.fields[2] = 4u;
    PoolRelocation refused;
    Require(!Driver::vertexTableRelocation(program, info, reads, other, use, refused), "a vertex stage whose result was built from another V# was relocatable");
    auto outside = reads;
    outside[1].address += 8u;
    Require(!Driver::vertexTableRelocation(program, info, outside, compiled, use, refused), "a vertex stage whose V# read is not a table entry was relocatable");
    ShaderRecompiler::SrtUserDataUse valued;
    valued.asValue = std::uint64_t {1} << VertexTableWord;
    PoolRelocation kept;
    Require(Driver::vertexTableRelocation(program, info, reads, compiled, valued, kept) && kept.words == 0u && kept.movable.empty(), "a vertex buffer table pointer the SRT walk used as a value moved");
}

void MovedVertexTableHitsTheCache(bool libraries) {
    RegisterFetchVertex();
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    alignas(16) static std::array<float, 4> color{1.0f, 0.0f, 0.0f, 0.0f};
    alignas(256) static std::array<float, 32> whole{-1.0f, -1.0f, 0.0f, 1.0f, 3.0f, -1.0f, 0.0f, 1.0f, -1.0f, 3.0f, 0.0f, 1.0f};
    alignas(256) static std::array<float, 32> left{-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 1.0f, -1.0f, 3.0f, 0.0f, 1.0f};
    alignas(16) static std::array<std::uint32_t, 1> attributes{0u};
    alignas(16) static std::array<std::array<std::uint32_t, 4>, 12> tables{};
    const auto draw = [&](std::size_t index, std::array<std::uint32_t, 4> sharp) {
        tables[index] = sharp;
        const std::array<std::uint32_t, 8> words{0x10000000u, 1u, 0u, 0u, Low(tables[index].data()), High(tables[index].data()), Low(attributes.data()), High(attributes.data())};
        ColorMemory.fill(std::byte {0x40});
        auto commands = DrawFrom({color.data()}, FetchVertexCode.data(), PixelCode.data(), words);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(ColorMemory.data()), ColorMemory.size());
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    const auto drawn = [&] {
        std::size_t pixels = 0;
        for (std::size_t at = 0; at < ColorMemory.size(); at += 4) pixels += ColorMemory[at] != std::byte {0x40} ? 1u : 0u;
        return pixels;
    };
    const auto all = ColorMemory.size() / 4u;
    const auto start = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (std::size_t i = 0; i < 4; ++i) counters = draw(i, VertexBufferSharp(whole.data()));
    Require(counters.lookups == start.lookups + 4 && counters.hits == start.hits && counters.inserts == start.inserts + 2, mode + ": the draws that teach the moved vertex buffer table made " + std::to_string(counters.hits - start.hits) + " hits and " + std::to_string(counters.inserts - start.inserts) + " inserts, expected 0 and 2");
    Require(drawn() == all, mode + ": the taught draw did not cover the target");
    auto before = counters;
    counters = draw(4, VertexBufferSharp(left.data()));
    Require(counters.hits == before.hits + 1, mode + ": a draw whose vertex buffer table moved, with another vertex buffer in it, did not reuse the cached draw (" + std::to_string(counters.hits - before.hits) + " hits)");
    const auto half = drawn();
    Require(half != 0 && half < all, mode + ": a relocated draw did not draw from the vertex buffer of its moved table (" + std::to_string(half) + " of " + std::to_string(all) + " pixels)");
    before = counters;
    counters = draw(5, VertexBufferSharp(whole.data()));
    Require(counters.hits == before.hits + 1 && drawn() == all, mode + ": a draw whose table moved back to the first vertex buffer did not reuse the cached draw with it");
    const std::array<std::pair<std::array<std::uint32_t, 4>, const char*>, 3> changed{{
        {VertexBufferSharp(whole.data(), 32u), "stride"},
        {VertexBufferSharp(whole.data(), 16u, 2u), "record count"},
        {VertexBufferSharp(whole.data(), 16u, 3u, 74u), "format"},
    }};
    for (std::size_t i = 0; i < changed.size(); ++i) {
        before = counters;
        counters = draw(6 + i, changed[i].first);
        Require(counters.lookups == before.lookups + 1 && counters.hits == before.hits, mode + ": a draw whose moved vertex buffer table has another V# " + changed[i].second + " reused the cached draw");
    }
    before = counters;
    counters = draw(9, VertexBufferSharp(left.data()));
    Require(counters.hits == before.hits + 1, mode + ": a draw whose table moved again after V#s that differed did not reuse the cached draw");
    attributes[0] = 16u << 14u;
    before = counters;
    counters = draw(10, VertexBufferSharp(whole.data()));
    attributes[0] = 0u;
    Require(counters.lookups == before.lookups + 1 && counters.hits == before.hits, mode + ": a draw whose attribute word changed where its table stayed reused the cached draw");
}

void UnrelocatedAliasesStayOut(bool libraries) {
    Registered(SecondVertexHeader, SecondVertexCode.data(), sizeof(SecondVertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto draw = [&](const float* source) {
        auto commands = DrawFrom({source}, SecondVertexCode.data());
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    alignas(16) static std::array<float, 4> repeated{1.0f, 0.0f, 0.0f, 0.0f};
    alignas(16) static std::array<std::array<std::array<float, 4>, 2>, 12> twice{};
    for (std::size_t i = 0; i < twice.size(); ++i) {
        for (auto& entry : twice[i]) entry = {0.25f + 0.03125f * static_cast<float>(i), 0.0f, 0.0f, 0.0f};
    }
    draw(repeated.data());
    const auto start = draw(repeated.data());
    AgcDriver::DriverDetail::DrawEntryCounters counters = start;
    for (const auto& pair : twice) {
        for (const auto& entry : pair) draw(entry.data());
        counters = draw(repeated.data());
    }
    const auto inserts = counters.inserts - start.inserts;
    Require(inserts <= AgcDriver::DriverDetail::DrawRelocationTrial::ProbationAdmissions, mode + ": draws whose alias repeats but never relocates were cached (" + std::to_string(inserts) + " inserts for " + std::to_string(twice.size()) + " aliases)");
    Require(counters.hits - start.hits == twice.size(), mode + ": the exact key of a repeated draw lost its hits next to draws whose alias never relocates (" + std::to_string(counters.hits - start.hits) + " hits in " + std::to_string(twice.size()) + " draws)");
}

void MovedForwardedWordReachesStageDataHit(bool libraries) {
    Registered(TriangleVertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
    Registered(ForwardedPixelHeader, ForwardedPixelCode.data(), sizeof(ForwardedPixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    constexpr std::byte Kept{0x40};
    const auto draw = [&](const void* table) {
        ColorMemory.fill(Kept);
        auto commands = DrawFrom({table}, TriangleVertexCode.data(), ForwardedPixelCode.data());
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(ColorMemory.data()), ColorMemory.size());
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    const auto pixels = [&](std::array<std::uint8_t, 4> expected) {
        std::size_t drawn = 0;
        for (std::size_t at = 0; at < ColorMemory.size(); at += 4) {
            const std::array<std::uint8_t, 4> pixel{static_cast<std::uint8_t>(ColorMemory[at]), static_cast<std::uint8_t>(ColorMemory[at + 1]), static_cast<std::uint8_t>(ColorMemory[at + 2]), static_cast<std::uint8_t>(ColorMemory[at + 3])};
            if (pixel == std::array<std::uint8_t, 4>{0x40, 0x40, 0x40, 0x40}) continue;
            if (pixel != expected) return "pixel " + std::to_string(at / 4) + " is " + std::to_string(pixel[0]) + "," + std::to_string(pixel[1]) + "," + std::to_string(pixel[2]) + "," + std::to_string(pixel[3]) + ", expected " + std::to_string(expected[0]) + "," + std::to_string(expected[1]) + "," + std::to_string(expected[2]) + "," + std::to_string(expected[3]);
            ++drawn;
        }
        return drawn == 0 ? std::string("the draw wrote no pixel") : std::string();
    };
    const auto data = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ForwardedData.data()));
    for (auto& table : AlignedTables) table[0] = data;
    OffsetTable[2] = data;
    ForwardedData = {1.0f, 0.0f, 0.0f, 0.0f};
    const auto start = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (const auto& table : AlignedTables) counters = draw(table.data());
    Require(counters.hits != start.hits, mode + ": draws whose pointer table moved to identical contents never reused the cached draw (" + std::to_string(counters.lookups - start.lookups) + " lookups, " + std::to_string(counters.inserts - start.inserts) + " inserts)");
    const auto taught = pixels({255, 0, 0, 255});
    Require(taught.empty(), mode + ": a draw through a pointer table with bit 4 of its address clear: " + taught);
    ForwardedData[0] = 0.25f;
    const auto before = counters;
    counters = draw(&OffsetTable[2]);
    Require(counters.lookups == before.lookups + 1 && counters.hits == before.hits + 1 && counters.stageDataHits == before.stageDataHits + 1, mode + ": a draw whose pointer table moved and whose data changed in place was not a relocated stage data hit (" + std::to_string(counters.hits - before.hits) + " hits, " + std::to_string(counters.stageDataHits - before.stageDataHits) + " stage data hits)");
    const auto moved = pixels({64, 255, 255, 64});
    Require(moved.empty(), mode + ": a relocated stage data hit did not draw with the moved user word: " + moved);
}

}

int main(int argc, char** argv) {
    try {
        const bool evicted = argc == 2 && std::string(argv[1]) == "--evicted";
        const bool kept = argc == 2 && std::string(argv[1]) == "--kept";
        const bool pool = argc == 2 && std::string(argv[1]) == "--pool";
        if (!evicted && !kept && !pool) {
            SplitRunsNameTheirSharedWords();
            VertexTableRunsMoveWithTheirPointer();
        }
        bool libraries = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            libraries = device->GraphicsPipelineLibraries();
        }
        if (!libraries && std::getenv("APS5_NO_GPL") == nullptr) return VulkanTestSkipped;
        if (evicted) {
            EvictedEntriesServeNoRelocation(libraries);
            AgcDriverShutdown_nid_postfix();
            std::cout << "An evicted draw served no relocation " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
            return 0;
        }
        if (pool) {
            Require(AgcDriver::DriverDetail::Driver::poolRelocationEnabled(), "run with APS5_POOL_RELOCATION=1");
            MovedVertexTableHitsTheCache(libraries);
            AgcDriverShutdown_nid_postfix();
            std::cout << "Draws whose guest pool addresses moved reused the cached draw " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
            return 0;
        }
        if (kept) {
            RelocatingDrawsKeepTheirDonor(libraries);
            AgcDriverShutdown_nid_postfix();
            std::cout << "A draw relocated from a cached draw kept it cached " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
            return 0;
        }
        MovedDataHitsTheCache(libraries);
        UnrelocatedAliasesStayOut(libraries);
        MovedForwardedWordReachesStageDataHit(libraries);
        AgcDriverShutdown_nid_postfix();
        std::cout << "A draw whose data moved reused the cached draw " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
