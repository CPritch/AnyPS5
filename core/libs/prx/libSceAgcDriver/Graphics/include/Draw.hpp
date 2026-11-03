#include <string>
#include <optional>
#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAW_HPP

#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/Recipe.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include <array>
#include <memory>
#include <vector>

namespace AgcDriver::Graphics {

struct HostImport;

struct DrawTraceContext {
    std::uint64_t token = 0;
    std::uint64_t submission = 0;
    std::uint32_t queue = 0;
    std::size_t offset = 0;
    std::uint64_t target = 0;
};

struct DrawAhead {
    VkDevice device = VK_NULL_HANDLE;
    std::shared_ptr<BufferPool> pool;
    ResourceCache::Key contentKey;
    bool validated = false;
    std::uint32_t fragmentOutputs = 0;
    VkPipelineStageFlags shaderStages = 0;
    bool laidOut = false;
    VertexInputLayout vertexInput;
    std::shared_ptr<const VertexInputLayout> retainedVertexInput;
    DescribedSetLayouts layouts;
    std::uint32_t proxied = 0;
    std::shared_ptr<Pipeline> pipeline;
    std::uint64_t storeHash = 0;
    bool looked = false;
    std::shared_ptr<ShaderResources> found;
    std::uint64_t proofAtLookup = 0;
    std::uint64_t pendingAtLookup = 0;
    bool movedPlanned = false;
    std::optional<ShaderResources::MovedPlan> movedPlan;
    bool aliasesChecked = false;
    std::uint64_t aliasIndexBytes = 0;
    GuestMemory::PrecollectedWrites walks;
    const VertexInputLayout& VertexLayout() const { return retainedVertexInput != nullptr ? *retainedVertexInput : vertexInput; }
};

enum class DrawAheadEvent : std::uint8_t { Offered, Prepared, Thrown, NotStarted, Unfinished, Device, Used, NotLean, PipelineInputs, ShapeMemo, TemplateHinted, TemplateGone, Precollected, PrecollectStale, Verified, Mismatch, MovedPlanned, MovedTaken, Count };
void PrepareDrawAhead(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::uint64_t shape, DrawAhead& ahead);
void CountDrawAhead(DrawAheadEvent event);
std::array<std::uint64_t, static_cast<std::size_t>(DrawAheadEvent::Count)> DrawAheadTotals();
std::string DrawAheadReport();
bool DrawAheadDisabled();
void ThrowInDrawAheadForTest(bool enabled);

// `recipe`, when given, receives the DrawRecipe a recorded, cacheable, reusable, non-indirect draw
// built for its draw-cache entry (design_cpu_final M8; null otherwise, and always under
// APS5_NO_DRAW_RECIPE=1).
void Draw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots = {}, std::shared_ptr<const DrawRecipe>* recipe = nullptr, const DrawTraceContext* trace = nullptr, std::uint64_t shape = 0, DrawAhead* ahead = nullptr);
std::string TakeDrawSplitText();
std::string TakeDrawBindingsText();
std::string TakeReleasedScanText();
struct DrawPacket;
struct DrawPacketDeleter {
    void operator()(DrawPacket* packet) const;
};
using DrawPacketPtr = std::unique_ptr<DrawPacket, DrawPacketDeleter>;
bool NeedsMeshLayerWindows(const Context& context, const State& state, const Pm4::DrawParameters& draw);
DrawPacketPtr ResolveDraw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots = {}, std::shared_ptr<const DrawRecipe>* recipe = nullptr, const DrawTraceContext* trace = nullptr, std::uint64_t shape = 0, DrawAhead* ahead = nullptr);
void EmitDraw(const Context& context, DrawPacket& packet);
void DiscardDrawPacket(const Context& context, DrawPacket& packet);
std::uint64_t DrawRedoSerial();
bool DrawPacketReadsOverlap(const DrawPacket& packet, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
bool DrawPacketTemplateGone(const DrawPacket& packet);
bool RevalidateDrawPacketTemplate(const DrawPacket& packet, std::span<const CompiledShader> shaders);
const void* DrawPacketTemplate(const DrawPacket& packet);
struct ResolverHandoff {
    void* shapeMemo = nullptr;
    std::shared_ptr<const std::vector<std::pair<std::uint64_t, std::uint64_t>>> snapshot;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> queuedStores;
    Recorder::DeferredEffects* abandoned = nullptr;
    std::vector<DrawPacket*>* graveyard = nullptr;
    const std::vector<FoundMultisampleTarget>* multisampleTargets = nullptr;
};
void SetFoundMultisampleTargets(const std::vector<FoundMultisampleTarget>* found);
void* ShapeMemoHandle();
class ResolverScope {
public:
    explicit ResolverScope(const ResolverHandoff& handoff);
    ~ResolverScope();
    ResolverScope(const ResolverScope&) = delete;
    ResolverScope& operator=(const ResolverScope&) = delete;
};
void BuryPackets(std::vector<DrawPacket*>& packets);
void ApplyDrawMemoKeeps(const Context& context, DrawPacket& packet);
void SetRedoHook(void (*hook)());
enum class DrawPacketField : std::uint8_t { Template, Pipeline, Targets, Framebuffer, Inputs, Moved, Depth, Refreshes, Flags, Count };
std::uint32_t CompareDrawPackets(const DrawPacket& first, const DrawPacket& second);
const char* DrawPacketFieldName(DrawPacketField field);
enum class PredictReason : std::uint8_t { Accepted, MultisampledColor, MultisampledDepth, StencilReplay, QuadList, Indirect, NoSlot, NotLooked, NoHint, HintGone, ProofNotCurrent, NoMovedPlan, Count };
PredictReason PredictPureReason(const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const DrawAhead* ahead, bool multisampleFound = false);
bool PredictPureResolve(const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const DrawAhead* ahead);
enum class StaleViewSite : std::uint8_t { InputPending, InputLabel, MovedPending, Count };
void CountStaleView(StaleViewSite site, bool live, std::uint64_t address, std::size_t bytes);

struct PixelFunctionLdsLayouts {
    VkDescriptorSetLayout empty = VK_NULL_HANDLE;
    VkDescriptorSetLayout buffer = VK_NULL_HANDLE;
};

PixelFunctionLdsLayouts PixelFunctionLdsSetLayouts(const Context& context);
void PreparePixelFunctionLds(const Context& context, VkCommandBuffer commands, std::uint32_t subgroupSize);
void BindPixelFunctionLds(const Context& context, VkCommandBuffer commands, VkPipelineLayout layout);
void CheckPixelFunctionLds(const Context& context);
void ReleasePixelFunctionLds(VkDevice device);
bool UsesMeshWindow(std::span<const CompiledShader> shaders);
bool ValidationKey(const Context& context, std::span<const CompiledShader> shaders, const State& state, std::vector<std::uint64_t>& key);
VkDescriptorSetLayout MeshWindowSetLayout(const Context& context);
void PrepareMeshWindow(const Context& context);
void BindMeshWindow(const Context& context, VkCommandBuffer commands, VkPipelineLayout layout);
void ReleaseMeshWindow(VkDevice device);
std::optional<std::string> KnownValidationFailure(const Context& context, std::span<const CompiledShader> shaders, const State& state);
std::uint64_t DrawRenderPassKey(const Context& context, const State& state, std::span<const VkImageView> targetViews);

struct DrawInputCopy {
    std::shared_ptr<Buffer> buffer;
    bool reused = false;
    std::uint32_t derived = 0;
    std::uint64_t generation = 0;
    std::uint64_t registryGeneration = 0;
};
DrawInputCopy CopyDrawInput(const Context& context, Recorder* recorder, std::uint64_t address, std::size_t bytes, std::size_t alignment, Recorder::SnapshotUse use);
DrawInputCopy CopyZeroPaddedDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t validBytes, std::size_t stride = 0);
void KeepDrawInput(Recorder* recorder, std::uint64_t address, const DrawInputCopy& copy, Recorder::SnapshotUse use, std::uint32_t derived);
const HostImport* InPlaceDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t alignment, bool* behindGpuWrite = nullptr, std::vector<std::pair<std::uint64_t, std::size_t>>* deferredReads = nullptr);
struct InPlaceIndices {
    std::optional<std::uint32_t> highest;
    bool reused = false;
};
InPlaceIndices InPlaceIndexRange(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint32_t indexSize, bool skipRestart);

std::array<std::uint32_t, 4> MeshIndexBufferDescriptor(const Pm4::DrawParameters& draw);

struct MeshArguments {
    std::uint32_t groups;
    std::uint32_t instances;
    std::uint32_t layers;
    std::uint32_t indexCount;
    std::uint32_t firstIndex;
};
static_assert(sizeof(MeshArguments) == ShaderRecompiler::MeshArgumentBytes);
struct MeshArgumentRules {
    std::uint32_t indexCount;
    std::uint32_t inputSize;
    std::uint32_t step;
    std::uint32_t primitivesPerGroup;
    std::uint32_t maxGroups;
    std::uint32_t maxInstances;
    std::uint32_t maxTotal;
};
MeshArgumentRules MeshArgumentRulesFor(const Context& context, const ShaderRecompiler::MeshConfiguration& mesh, std::uint32_t indexCount);
struct MeshGrid {
    std::uint32_t groups;
    std::uint32_t instances;
    std::uint32_t layers;
    std::uint32_t layerStride;
};
std::optional<MeshGrid> MeshGridFor(const Context& context, std::uint32_t groups, std::uint32_t instances);
MeshArguments ResolveMeshArguments(const Pm4::DrawArguments& record, const MeshArgumentRules& rules);

// Why DrawWithRecipe did not record from the recipe (the caller then runs Draw): the draw is not
// recordable (no recorder, APS5_SYNC_DRAWS, APS5_DUMP_TARGETS), a resident target is gone from the
// storage cache, the template is gone, the pipeline is gone from the pipeline store, or the
// template's proof failed (ShaderResources::ProveCurrent; the template is removed from the
// resource cache and kept by the batch).
enum class DrawRecipeMiss : std::uint8_t { None, NotRecordable, TargetGone, TemplateGone, ObjectsGone, Proof, Count };
const char* DrawRecipeMissName(DrawRecipeMiss miss);
struct DrawRecipeOutcome {
    bool recorded = false;
    DrawRecipeMiss miss = DrawRecipeMiss::None;
    ShaderResources::ProofReport proof;
    // APS5_PROFILE_DRAW: the proof's and the record's time.
    double proofUs = 0;
    double recordUs = 0;
};
// A draw-cache hit recorded from its recipe, under GuestMemory::GpuMutex after the packet's labels
// (design_cpu_final M8, step 8b): the validation with the recipe's stored outputs, the index and
// vertex work as Draw's, the resident-target proof on the stored images (Refresh while Cached()),
// the template's proof (ProveCurrent) with the alias checks repeated, the pipeline and framebuffer
// from the recipe, then the record as Draw's (pass continued or begun, Kept, MarkGpuWrites). A
// miss records nothing.
DrawRecipeOutcome DrawWithRecipe(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, const DrawRecipe& recipe, const DrawTraceContext* trace = nullptr);
// Whether recorded draws build recipes and draw-cache hits use them (APS5_NO_DRAW_RECIPE unset).
bool DrawRecipes();

// How an indirect draw's records were read, for the [draws] line (APS5_PROFILE_DRAW): Gpu is
// vkCmdDrawIndirect from the host import; every other value names why the CPU read them instead,
// either decided by the driver before the draw (its patched-SGPR fold rules, the shader path, the
// draw index, the vertex ranges, the device features, APS5_NO_GPU_INDIRECT_DRAW) or under the device
// lock in Draw (the memory state of the records). `readMs` is the CPU read's time (its sync);
// `rewritten` counts a GPU-side draw whose records were copied and patched with a constant.
enum class IndirectDrawPath : std::uint8_t { Gpu, NotFolded, FetchUnknown, NonVertexPath, IndxOffset, DrawIndex, VertexRange, FeatureGap, PendingImage, PendingLabelOrCopy, NotImported, Disabled, IndexRange, IndexLimitUnknown, Count };
void CountIndirectDraw(IndirectDrawPath path, double readMs, bool rewritten = false);
std::array<std::uint64_t, static_cast<std::size_t>(IndirectDrawPath::Count)> IndirectDrawCounts();
void CountEmptyIndirectDraw(double readMs);
std::uint64_t EmptyIndirectDrawCount();

struct DrawPassCounts {
    std::uint64_t draws = 0;
    std::uint64_t passesBegun = 0;
    std::uint64_t passesContinued = 0;
    std::uint64_t keyStoreFlushes = 0;
    std::uint64_t storeFlushes = 0;
};
std::array<DrawPassCounts, 2> DrawPassTotals();
std::array<std::uint64_t, PassBreakCount> DrawPassBreaks();
const char* IndirectDrawPathName(IndirectDrawPath path);

// Draw packets that drew nothing, for the [draws] line: Nothing (an empty count, or no color writes
// and no pixel shader), Prechecked (State.hpp's DrawRejection refused it before the decode) and
// Thrown (the decode or the build threw); `us` is the packet's time in the driver.
enum class DrawSkip : std::uint8_t { Nothing, Prechecked, Thrown, Count };
void CountDrawSkip(DrawSkip kind, double us);

// Recorded draws whose written guest buffers were copied (their results reach guest memory by a CPU
// write-back when their batch completes), listed until that write-back ran. It is the draw
// counterpart of VulkanDevice's State::copiedWriters for dispatches, which VulkanDevice.cpp consults
// in two places: DispatchIndirect (an indirect dispatch whose arguments such a writer produced reads
// them on the CPU, behind the write-back, instead of from the host import on the GPU) and the fill
// HLE (a vkCmdFillBuffer over a range a listed write-back will land on must wait for it, or the
// write-back overwrites the fill). Draws are listed here only under APS5_RECORD_COPIED_DRAWS=1,
// which is safe only once BOTH of those checks also test this list, i.e. each extends its
// `std::any_of(state->copiedWriters ...)` with
// `|| std::any_of(Graphics::DrawCopiedWriters()->begin(), Graphics::DrawCopiedWriters()->end(), [&](const auto& writer) { return writer->WritesOverlap(<its range>); })`;
// cleaner still, State::copiedWriters can become this list (one registry for both producers), after
// which the switch can turn into APS5_NO_RECORD_COPIED_DRAWS. Until then such draws are recorded and
// waited for at once. Read and written under GuestMemory::GpuMutex, like the device's list.
std::shared_ptr<std::vector<std::shared_ptr<ShaderResources>>> DrawCopiedWriters();

void RunColorMetadataPass(const Context& context, const ColorMetadataPass& pass);
std::vector<std::pair<std::uint64_t, std::uint64_t>> PipelinedColorMetadataPassWrites(const ColorMetadataPass& pass);
bool DrawPipelineEnabled();
bool CommitTrims();

}

#endif
