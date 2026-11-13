#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::ShaderResources;
using Count = ShaderResources::AddressCount;

constexpr std::uint32_t VertexBuffers = 3;
constexpr std::size_t VertexTableOffset = 0x300;
constexpr std::size_t VertexDataOffset = 0x800;
constexpr std::size_t ColorBytes = 65536;
constexpr std::uint32_t RenderTargetTileMode = 0x1b;
constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t VertexCodeOffset = 0x000;
constexpr std::size_t ReadCodeOffset = 0x200;
constexpr std::size_t StoreCodeOffset = 0x300;
constexpr std::size_t VertexHeaderOffset = 0x400;
constexpr std::size_t ReadHeaderOffset = 0x500;
constexpr std::size_t StoreHeaderOffset = 0x600;
constexpr std::size_t ImageCodeOffset = 0x800;
constexpr std::size_t ImageHeaderOffset = 0x900;
constexpr std::uint32_t TexelSide = 64;
constexpr std::size_t SparseCommitted = 32768;
constexpr std::size_t ImagePage = 4096;

alignas(ImagePage) std::array<float, ImagePage / sizeof(float)> ImageWords{1.0f};
alignas(ImagePage) const std::array<float, ImagePage / sizeof(float)> ImageConstants{0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

constexpr std::array<std::uint32_t, 40> VertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u,
    0xf4080104u, 0xfa000000u, 0xf4080204u, 0xfa000010u, 0xf4080304u, 0xfa000020u,
    0xbf8cc07fu,
    0xe00c2000u, 0x80010a05u, 0xe00c2000u, 0x80020e05u, 0xe00c2000u, 0x80031205u,
    0xbf8c3f70u,
    0x0602030au, 0x0604050bu, 0x0606070cu, 0x0608090du,
    0x0602030eu, 0x0604050fu, 0x06060710u, 0x06080911u,
    0x06020312u, 0x06040513u, 0x06060714u, 0x06080915u,
    0xf80008cfu, 0x04030201u,
    0xbf810000u, 0xbf810000u,
};

constexpr std::array<std::uint32_t, 18> PixelBufferRead{0x7e040280u, 0xe0301000u, 0x80010c02u, 0x7e1a0280u, 0xdc308000u, 0x0e02000du, 0xf4000201u, 0xfa000010u, 0xbf8cc07fu, 0xbf8c3f70u, 0x7e10030cu, 0x7e12030eu, 0x7e140208u, 0x7e160280u, 0xf800180fu, 0x0b0a0908u, 0xbf810000u, 0xbf810000u};
constexpr std::array<std::uint32_t, 22> PixelImageRead{0x7e000280u, 0x7e020280u, 0xf0000f08u, 0x00021000u, 0x7e040280u, 0xe0301000u, 0x80010c02u, 0x7e1a0280u, 0xdc308000u, 0x0e02000du, 0xf4000401u, 0xfa000010u, 0xbf8cc07fu, 0xbf8c3f70u, 0x7e10030cu, 0x7e12030eu, 0x7e140210u, 0x7e160310u, 0xf800180fu, 0x0b0a0908u, 0xbf810000u, 0xbf810000u};
constexpr std::array<std::uint32_t, 11> PixelStore{0x7e1a0280u, 0x7e1802f2u, 0xdc708000u, 0x00020c0du, 0x7e1002f2u, 0x7e1202f2u, 0x7e1402f2u, 0x7e1602f2u, 0xf800180fu, 0x0b0a0908u, 0xbf810000u};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

void Register(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

class GuestBlock {
public:
    explicit GuestBlock(std::size_t committed = BlockBytes) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, committed, PAGE_READWRITE, committed);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, committed == BlockBytes ? PROT_READ | PROT_WRITE : PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            if (committed != BlockBytes) Require(mprotect(block, committed, PROT_READ | PROT_WRITE) == 0, "BDA templates: cannot commit the sparse block's prefix");
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "BDA templates: cannot allocate a guest block");
        std::memset(block, 0, committed);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    void Map(bool writable) const { GuestAllocations::Mutation().Add(block, BlockBytes, true, writable, true); }
    std::uint64_t At(std::size_t offset) const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block + offset)); }
    std::uint8_t* Bytes(std::size_t offset) const { return block + offset; }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 4> BufferDescriptor(std::uint64_t address, std::uint32_t stride, std::uint32_t records, std::uint32_t word3) {
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), records, word3};
}

std::array<std::uint32_t, 4> DwordAt(std::uint64_t address) {
    return BufferDescriptor(address, 0, 4, 0x31016facu);
}

void FillVertices(const GuestBlock& block) {
    for (std::uint32_t buffer = 0; buffer < VertexBuffers; ++buffer) {
        const auto vertices = BufferDescriptor(block.At(VertexDataOffset + buffer * 0x100u), 16, 3, 0x0104dfacu);
        std::memcpy(block.Bytes(VertexTableOffset + buffer * 16u), vertices.data(), sizeof(vertices));
    }
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

struct Step {
    const std::uint32_t* pixel;
    std::uint64_t pointer;
    std::array<std::uint32_t, 4> descriptor{};
    std::array<std::uint32_t, 8> image{};
};

std::array<std::uint32_t, 8> TexelDescriptor(std::uint64_t address) {
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (56u << 20u) | (((TexelSide - 1u) & 3u) << 30u), ((TexelSide - 1u) >> 2u) | ((TexelSide - 1u) << 14u), 0xfacu | (9u << 28u), 0u, 0u, 0u, 0u};
}

template<std::size_t Words>
const std::uint32_t* Place(const GuestBlock& programs, std::size_t codeOffset, std::size_t headerOffset, const std::array<std::uint32_t, Words>& code, std::uint8_t type) {
    auto* placed = reinterpret_cast<std::uint32_t*>(programs.Bytes(codeOffset));
    std::memcpy(placed, code.data(), sizeof(code));
    Register(*new (programs.Bytes(headerOffset)) Header{}, placed, sizeof(code), type);
    return placed;
}

struct Fixture {
    GuestBlock programs;
    GuestBlock vertices;
    GuestBlock data;
    GuestBlock target;
    GuestBlock outside;
    GuestBlock added;
    GuestBlock sparse{SparseCommitted};
    GuestBlock texels;
    const std::uint32_t* vertexCode = Place(programs, VertexCodeOffset, VertexHeaderOffset, VertexCode, 2);
    const std::uint32_t* readCode = Place(programs, ReadCodeOffset, ReadHeaderOffset, PixelBufferRead, 1);
    const std::uint32_t* storeCode = Place(programs, StoreCodeOffset, StoreHeaderOffset, PixelStore, 1);
    const std::uint32_t* imageCode = Place(programs, ImageCodeOffset, ImageHeaderOffset, PixelImageRead, 1);
    bool off = std::getenv("APS5_NO_BDA_TEMPLATES") != nullptr;
    bool verify = std::getenv("APS5_VERIFY_BDA_TEMPLATES") != nullptr;
};

void Submit(const Fixture& fixture, std::initializer_list<Step> steps) {
    const auto& vertices = fixture.vertices;
    const auto& target = fixture.target;
    const auto color = target.At(0);
    const auto vertex = reinterpret_cast<std::uintptr_t>(fixture.vertexCode);
    const auto table = vertices.At(VertexTableOffset);
    const float scale = 32.0f;
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
        {0x3b8, 0x9000000u | (RenderTargetTileMode << 14u)}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(scale)}, {0x110, std::bit_cast<std::uint32_t>(scale)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{{0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 2u << 1u}, {0x8c, static_cast<std::uint32_t>(table)}, {0x8d, static_cast<std::uint32_t>(table >> 32u)}, {0x00b, 16u << 1u}, {0x00c, 0}, {0x00d, 0}}) Append(commands, 0x76, {offset, value});
    for (const auto& step : steps) {
        const auto pixel = reinterpret_cast<std::uintptr_t>(step.pixel);
        Append(commands, 0x76, {0x008, static_cast<std::uint32_t>(pixel >> 8u), static_cast<std::uint32_t>(pixel >> 40u)});
        Append(commands, 0x76, {0x00e, static_cast<std::uint32_t>(step.pointer), static_cast<std::uint32_t>(step.pointer >> 32u)});
        Append(commands, 0x76, {0x010, step.descriptor[0], step.descriptor[1], step.descriptor[2], step.descriptor[3]});
        Append(commands, 0x76, {0x014, step.image[0], step.image[1], step.image[2], step.image[3], step.image[4], step.image[5], step.image[6], step.image[7]});
        Append(commands, 0x2d, {3, 2});
    }
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "BDA templates: the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

struct Colors {
    unsigned red = 0;
    unsigned green = 0;
    unsigned blue = 0;
};

std::string Text(Colors colors) {
    return "red " + std::to_string(colors.red) + ", green " + std::to_string(colors.green) + ", blue " + std::to_string(colors.blue);
}

Colors Brightest(const Fixture& fixture) {
    AgcDriver::GuestMemory::FlushGpuWrites(static_cast<std::uintptr_t>(fixture.target.At(0)), ColorBytes);
    Colors colors;
    for (std::size_t pixel = 0; pixel + 4 <= ColorBytes; pixel += 4) {
        colors.red = std::max(colors.red, static_cast<unsigned>(fixture.target.Bytes(pixel)[0]));
        colors.green = std::max(colors.green, static_cast<unsigned>(fixture.target.Bytes(pixel)[1]));
        colors.blue = std::max(colors.blue, static_cast<unsigned>(fixture.target.Bytes(pixel)[2]));
    }
    return colors;
}

void WriteFloat(const GuestBlock& block, std::size_t offset, float value) {
    std::memcpy(block.Bytes(offset), &value, sizeof(value));
}

Colors DrawWith(const Fixture& fixture, const std::uint32_t* code, std::uint64_t buffer, std::uint64_t pointer) {
    std::memset(fixture.target.Bytes(0), 0, ColorBytes);
    Submit(fixture, {{code, pointer, DwordAt(buffer), TexelDescriptor(fixture.texels.At(0))}});
    return Brightest(fixture);
}

Colors DrawRead(const Fixture& fixture, std::uint64_t buffer, std::uint64_t pointer) {
    return DrawWith(fixture, fixture.readCode, buffer, pointer);
}

std::uint64_t Total(Count count) {
    return ShaderResources::AddressTemplateTotals()[static_cast<std::size_t>(count)];
}

struct ImportBudget {
    explicit ImportBudget(std::uint64_t bytes) { AgcDriver::Graphics::SetHostImportBudget(bytes); }
    ~ImportBudget() { AgcDriver::Graphics::SetHostImportBudget(0); }
    ImportBudget(const ImportBudget&) = delete;
    ImportBudget& operator=(const ImportBudget&) = delete;
};

std::uint64_t AddressOf(const void* pointer) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

void MapImagePages() {
    GuestAllocations::Mutation mutation;
    mutation.RegisterMainImage();
    mutation.Protect(ImageConstants.data(), sizeof(ImageConstants), true, false, true, [] {});
    mutation.Protect(ImageWords.data(), sizeof(ImageWords), true, true, true, [] {});
}

struct Counts {
    std::array<std::uint64_t, static_cast<std::size_t>(Count::Count)> at = ShaderResources::AddressTemplateTotals();
    std::uint64_t Since(Count count) const { return Total(count) - at[static_cast<std::size_t>(count)]; }
};

void Expect(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("BDA templates: " + what);
}

void ExpectColors(Colors seen, Colors wanted, const std::string& what) {
    Expect(seen.red == wanted.red && seen.green == wanted.green && seen.blue == wanted.blue, what + ": " + Text(seen) + ", not " + Text(wanted));
}

void ReuseBindsTheDrawsWords(Fixture& fixture) {
    const auto& data = fixture.data;
    WriteFloat(data, 0x100, 1.0f);
    WriteFloat(data, 0x200, 0.0f);
    WriteFloat(data, 0x400, 0.0f);
    WriteFloat(data, 0x410, 1.0f);
    WriteFloat(data, 0x500, 1.0f);
    WriteFloat(data, 0x510, 0.0f);
    const auto first = DrawRead(fixture, data.At(0x100), data.At(0x400));
    const Counts counts;
    const auto bufferCompares = ShaderResources::BufferWordCompares();
    const auto second = DrawRead(fixture, data.At(0x200), data.At(0x500));
    const auto third = DrawRead(fixture, data.At(0x100), data.At(0x400));
    const auto comparedInBuffers = ShaderResources::BufferWordCompares() - bufferCompares;
    std::printf("three draws of one template with their own buffer and pointer words: %s, %s, %s; reused %llu (moved buffers %llu, new data words %llu), made %llu\n", Text(first).c_str(), Text(second).c_str(), Text(third).c_str(), static_cast<unsigned long long>(counts.Since(Count::Reused)), static_cast<unsigned long long>(counts.Since(Count::ReusedMovedBuffers)), static_cast<unsigned long long>(counts.Since(Count::ReusedMovedData)), static_cast<unsigned long long>(counts.Since(Count::Made)));
    ExpectColors(first, {255, 0, 255}, "the first draw");
    ExpectColors(second, {0, 255, 0}, "a draw reusing the template with another buffer, pointer and loaded words did not read through its own words");
    ExpectColors(third, {255, 0, 255}, "a draw reusing the template with the first draw's words");
    if (fixture.off) {
        Expect(counts.Since(Count::Reused) == 0 && counts.Since(Count::Made) == 0, "APS5_NO_BDA_TEMPLATES=1 still made or reused an address-based template");
        return;
    }
    Expect(counts.Since(Count::Reused) == 2, "the two later draws did not both reuse the template");
    Expect(counts.Since(Count::ReusedMovedBuffers) == 1, "the reuse with another buffer did not bind it in place, or the reuse with the template's own buffer moved it");
    Expect(counts.Since(Count::ReusedMovedData) == 1, "the reuse with other loaded words did not bind them, or the reuse with the template's own words moved them");
    Expect(comparedInBuffers == 0, "a reuse compared the draw's loaded words with the template's buffer instead of the words the template kept");
}

void ReuseReadsWritesIntoItsRanges(Fixture& fixture) {
    const auto& data = fixture.data;
    WriteFloat(data, 0x100, 0.0f);
    WriteFloat(data, 0x400, 1.0f);
    WriteFloat(data, 0x410, 0.0f);
    const Counts counts;
    const auto cpuWritten = DrawRead(fixture, data.At(0x100), data.At(0x400));
    WriteFloat(data, 0x600, 0.0f);
    WriteFloat(data, 0x400, 0.0f);
    WriteFloat(data, 0x410, 1.0f);
    std::memset(fixture.target.Bytes(0), 0, ColorBytes);
    Submit(fixture, {{fixture.storeCode, data.At(0x600)}, {fixture.readCode, data.At(0x400), DwordAt(data.At(0x600))}});
    const auto gpuWritten = Brightest(fixture);
    float stored = 0;
    std::memcpy(&stored, data.Bytes(0x600), sizeof(stored));
    std::printf("reuses after the CPU and then a draw wrote the ranges they read: %s, %s (stored %g); reused %llu\n", Text(cpuWritten).c_str(), Text(gpuWritten).c_str(), static_cast<double>(stored), static_cast<unsigned long long>(counts.Since(Count::Reused)));
    ExpectColors(cpuWritten, {0, 255, 0}, "a reuse after the CPU rewrote the buffer, pointer and loaded words did not read the new bytes");
    Expect(stored == 1.0f, "the store through the table did not reach guest memory");
    ExpectColors(gpuWritten, {255, 0, 255}, "a reuse reading a buffer the previous draw stored to did not see the store");
    if (!fixture.off) Expect(counts.Since(Count::Reused) == 2, "the draws after the writes did not reuse the template");
}

void RefusesABufferOutsideTheSpace(Fixture& fixture) {
    const auto& data = fixture.data;
    WriteFloat(fixture.outside, 0x100, 1.0f);
    WriteFloat(data, 0x500, 0.0f);
    WriteFloat(data, 0x510, 0.0f);
    const Counts counts;
    const auto outside = DrawRead(fixture, fixture.outside.At(0x100), data.At(0x500));
    std::printf("a draw whose buffer lies outside the address space: %s; reused %llu, refused for the buffer %llu\n", Text(outside).c_str(), static_cast<unsigned long long>(counts.Since(Count::Reused)), static_cast<unsigned long long>(counts.Since(Count::RefusedBuffer)));
    ExpectColors(outside, {255, 0, 0}, "a draw whose buffer moved outside the address space");
    if (!fixture.off) Expect(counts.Since(Count::Reused) == 0 && counts.Since(Count::RefusedBuffer) == 1, "a buffer outside the address space was not refused");
}

void RefusesAChangedSpace(Fixture& fixture) {
    const auto& data = fixture.data;
    WriteFloat(data, 0x100, 1.0f);
    WriteFloat(data, 0x400, 1.0f);
    WriteFloat(data, 0x410, 1.0f);
    const auto before = DrawRead(fixture, data.At(0x100), data.At(0x400));
    const Counts counts;
    fixture.added.Map(false);
    const auto changed = DrawRead(fixture, data.At(0x100), data.At(0x400));
    const auto after = DrawRead(fixture, data.At(0x100), data.At(0x400));
    std::printf("draws before and after a GPU allocation was added: %s, %s, %s; reused %llu, refused for the space %llu, made %llu\n", Text(before).c_str(), Text(changed).c_str(), Text(after).c_str(), static_cast<unsigned long long>(counts.Since(Count::Reused)), static_cast<unsigned long long>(counts.Since(Count::RefusedSpace)), static_cast<unsigned long long>(counts.Since(Count::Made)));
    ExpectColors(before, {255, 255, 255}, "a draw before the space changed");
    ExpectColors(changed, {255, 255, 255}, "the draw after the space changed");
    ExpectColors(after, {255, 255, 255}, "a draw over the new space");
    if (!fixture.off) Expect(counts.Since(Count::RefusedSpace) == 1 && counts.Since(Count::Made) == 1 && counts.Since(Count::Reused) == 1, "a template over a space that changed was not refused once, rebuilt over the new space and reused");
}

void ReuseRepatchesBufferOffsets(Fixture& fixture, const std::uint32_t* code, const char* path) {
    const auto& data = fixture.data;
    WriteFloat(data, 0x1000, 1.0f);
    WriteFloat(data, 0x1004, 0.0f);
    WriteFloat(data, 0x1008, 1.0f);
    WriteFloat(data, 0x400, 0.0f);
    WriteFloat(data, 0x410, 0.0f);
    const auto first = DrawWith(fixture, code, data.At(0x1000), data.At(0x400));
    const Counts counts;
    const auto second = DrawWith(fixture, code, data.At(0x1004), data.At(0x400));
    const auto third = DrawWith(fixture, code, data.At(0x1008), data.At(0x400));
    const auto fourth = DrawWith(fixture, code, data.At(0x1000), data.At(0x400));
    const auto since = [&](Count count) { return static_cast<unsigned long long>(counts.Since(count)); };
    std::printf("a template whose buffer offset sits in its %s, reused with the buffer 4 and 8 bytes further and back: %s, %s, %s, %s; reused %llu (repatched offsets %llu), refused for the buffer %llu\n", path, Text(first).c_str(), Text(second).c_str(), Text(third).c_str(), Text(fourth).c_str(), since(Count::Reused), since(Count::ReusedRepatched), since(Count::RefusedBuffer));
    ExpectColors(first, {255, 0, 0}, std::string("the first draw with its buffer offset in its ") + path);
    ExpectColors(second, {0, 0, 0}, std::string("a reuse with the buffer 4 bytes further did not read through its own offset (") + path + ")");
    ExpectColors(third, {255, 0, 0}, std::string("a reuse with the buffer 8 bytes further did not read through its own offset (") + path + ")");
    ExpectColors(fourth, {255, 0, 0}, std::string("a reuse with the template's own buffer offset (") + path + ")");
    if (fixture.off) return;
    Expect(since(Count::Reused) == 3 && since(Count::RefusedBuffer) == 0, std::string("a buffer moved within the storage buffer offset alignment was refused instead of repatched (") + path + ")");
}

void ReuseSurvivesASpaceDropBeforeItsVerify(Fixture& fixture) {
    const auto& data = fixture.data;
    WriteFloat(data, 0x100, 1.0f);
    WriteFloat(data, 0x400, 0.0f);
    WriteFloat(data, 0x410, 1.0f);
    static bool dropped = false;
    const auto first = DrawRead(fixture, data.At(0x100), data.At(0x400));
    const Counts counts;
    ShaderResources::SetAddressVerifyHookForTest(+[] {
        if (std::exchange(dropped, true)) return;
        AgcDriver::Graphics::DropAddressSpaceCacheForTest();
    });
    const auto reused = DrawRead(fixture, data.At(0x100), data.At(0x400));
    ShaderResources::SetAddressVerifyHookForTest(nullptr);
    if (!dropped) AgcDriver::Graphics::DropAddressSpaceCacheForTest();
    const auto refused = DrawRead(fixture, data.At(0x100), data.At(0x400));
    const auto again = DrawRead(fixture, data.At(0x100), data.At(0x400));
    const auto since = [&](Count count) { return static_cast<unsigned long long>(counts.Since(count)); };
    std::printf("draws around a drop of the cached space %s: %s, %s, %s, %s; reused %llu, refused for the space %llu, made %llu, verify builds over a newer space %llu\n", fixture.verify ? "between a reuse and its verify build" : "after a reuse", Text(first).c_str(), Text(reused).c_str(), Text(refused).c_str(), Text(again).c_str(), since(Count::Reused), since(Count::RefusedSpace), since(Count::Made), since(Count::VerifiedNewerSpace));
    for (const auto& [colors, what] : {std::pair{first, "the first draw"}, std::pair{reused, "the reuse before the drop"}, std::pair{refused, "the draw after the drop"}, std::pair{again, "the reuse over the new space"}}) ExpectColors(colors, {255, 0, 255}, what);
    if (fixture.off) return;
    Expect(since(Count::Reused) == 2 && since(Count::RefusedSpace) == 1 && since(Count::Made) == 1, "the drop of the cached space did not refuse the next reuse once, rebuild it over the new space and reuse that");
    Expect(since(Count::VerifiedNewerSpace) == (fixture.verify ? 1u : 0u), "the verify build after the drop did not take the newer space");
}

void MirroredSpacesRefreshPerReuse(Fixture& fixture) {
    const auto& data = fixture.data;
    const auto& sparse = fixture.sparse;
    WriteFloat(data, 0x100, 1.0f);
    WriteFloat(sparse, 0x400, 0.0f);
    WriteFloat(sparse, 0x410, 1.0f);
    const Counts counts;
    const auto first = DrawRead(fixture, data.At(0x100), sparse.At(0x400));
    WriteFloat(data, 0x100, 0.0f);
    WriteFloat(sparse, 0x400, 1.0f);
    WriteFloat(sparse, 0x410, 0.0f);
    const auto heaps = DrawRead(fixture, data.At(0x100), sparse.At(0x400));
    const auto images = DrawRead(fixture, AddressOf(ImageWords.data()), AddressOf(ImageConstants.data()));
    ImageWords[0] = 0.0f;
    const auto written = DrawRead(fixture, AddressOf(ImageWords.data()), AddressOf(ImageConstants.data()));
    const auto since = [&](Count count) { return static_cast<unsigned long long>(counts.Since(count)); };
    std::printf("draws over a space of heap, sparse heap, writable image and read-only image mirrors: %s; after CPU writes into the heap mirrors %s; through the image mirrors %s; after a CPU write into the writable image mirror %s; reused %llu (moved buffers %llu, new data words %llu), made %llu, refused %llu; builds over spaces with mirrors %llu (read-only image %llu, writable image %llu, heap %llu, sparse heap %llu, sparse regions %llu, copied ranges %llu)\n", Text(first).c_str(), Text(heaps).c_str(), Text(images).c_str(), Text(written).c_str(), since(Count::Reused), since(Count::ReusedMovedBuffers), since(Count::ReusedMovedData), since(Count::Made), since(Count::RefusedBusy) + since(Count::RefusedSpace) + since(Count::RefusedImports) + since(Count::RefusedTargets) + since(Count::RefusedSnapshot) + since(Count::RefusedProof) + since(Count::RefusedUnmovable) + since(Count::RefusedBuffer), since(Count::KindMirrored), since(Count::KindReadOnlyMirrors), since(Count::KindWritableMirrors), since(Count::KindHeapMirrors), since(Count::KindSparseMirrors), since(Count::KindSparseRegions), since(Count::KindCopied));
    ExpectColors(first, {255, 0, 255}, "the first draw over the mirrored space");
    ExpectColors(heaps, {0, 255, 0}, "a draw after CPU writes into its heap and sparse heap mirrors read stale mirror bytes");
    ExpectColors(images, {255, 0, 255}, "a draw reading through the writable and read-only image mirrors");
    ExpectColors(written, {0, 0, 255}, "a draw after a CPU write into its writable image mirror read stale mirror bytes");
    if (fixture.off) {
        Expect(since(Count::Reused) == 0 && since(Count::Made) == 0, "APS5_NO_BDA_TEMPLATES=1 still made or reused an address-based template over a mirrored space");
        return;
    }
    Expect(since(Count::KindHeapMirrors) != 0 && since(Count::KindSparseMirrors) != 0 && since(Count::KindReadOnlyMirrors) != 0 && since(Count::KindWritableMirrors) != 0, "the space did not carry heap, sparse heap, read-only image and writable image mirrors, so the case does not test them");
    Expect(since(Count::Reused) == 3 && since(Count::Made) == 1, "the draws after the first did not all reuse its template over the mirrored space");
    Expect(since(Count::ReusedMovedBuffers) == 2, "the reuses through the image mirrors did not bind their moved buffer in place");
    Expect(since(Count::ReusedMovedData) == 1, "the reuse after the CPU rewrote the word its program loads did not bind the new word, or a reuse whose loaded word equals the template's moved it");
}

}

int main(int argc, char** argv) {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        const std::string only = argc > 1 ? argv[1] : "";
        const bool mirrors = only == "--mirrors";
        std::optional<ImportBudget> budget;
        if (mirrors) {
            budget.emplace(1);
            MapImagePages();
        }
        Fixture fixture;
        fixture.programs.Map(false);
        FillVertices(fixture.vertices);
        fixture.vertices.Map(false);
        fixture.data.Map(true);
        fixture.target.Map(true);
        fixture.texels.Map(false);
        if (mirrors) {
            fixture.sparse.Map(true);
            MirroredSpacesRefreshPerReuse(fixture);
        }
        if (only.empty() || only == "--words") ReuseBindsTheDrawsWords(fixture);
        if (only.empty() || only == "--writes") ReuseReadsWritesIntoItsRanges(fixture);
        if (only.empty() || only == "--outside") RefusesABufferOutsideTheSpace(fixture);
        if (only.empty() || only == "--space") RefusesAChangedSpace(fixture);
        if (only == "--offsets") {
            ReuseRepatchesBufferOffsets(fixture, fixture.readCode, "push constants");
            ReuseRepatchesBufferOffsets(fixture, fixture.imageCode, "shader data words");
        }
        if (only == "--verify-drop") ReuseSurvivesASpaceDropBeforeItsVerify(fixture);
        std::printf("%s\n", ShaderResources::TakeAddressTemplateText().c_str());
        AgcDriverShutdown_nid_postfix();
        std::puts(fixture.off ? "BDA templates: with APS5_NO_BDA_TEMPLATES=1 every address-based draw builds its own resources" : "BDA templates: a reused address-based draw template binds each draw's words and refuses a moved space or a buffer outside it");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << ShaderResources::TakeAddressTemplateText() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
