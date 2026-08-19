#include "sim/simulation.h"

#include <cstdio>

#include "gpu/resources.h"

static constexpr uint32_t kPassStride = 256;  // min uniform dynamic-offset alignment
static constexpr uint32_t kStepGroups = 22;   // ceil(ceil(256/3)/4)

bool Simulation::Init(const wgpu::Device& device, World& world,
                      const std::vector<MaterialDef>& mats,
                      const std::string& shaderDir) {
  world_ = &world;
  device_ = device;
  shaderDir_ = shaderDir;
  wgpu::Queue queue = device.GetQueue();

  materialBuf_ = CreateBuffer(device, sizeof(MaterialGpu) * 4096,
                              wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst,
                              "materials");
  UploadMaterials(queue, mats);

  // 27 color-phase slices x 2 gravity substeps (54 total)
  {
    std::vector<uint32_t> phases(54 * kPassStride / 4, 0);
    for (uint32_t k = 0; k < 54; k++) {
      uint32_t* p = &phases[k * kPassStride / 4];
      uint32_t c = k % 27;
      p[0] = c % 3;
      p[1] = (c / 3) % 3;
      p[2] = c / 9;
      p[3] = k / 27;  // substep
    }
    queue.WriteBuffer(world_->passUBO, 0, phases.data(), phases.size() * 4);
  }

  // ---- bind group layouts ----
  {
    auto entry = [](uint32_t binding, wgpu::BufferBindingType type,
                    bool dynamic = false) {
      wgpu::BindGroupLayoutEntry e{};
      e.binding = binding;
      e.visibility = wgpu::ShaderStage::Compute;
      e.buffer.type = type;
      e.buffer.hasDynamicOffset = dynamic;
      return e;
    };
    using T = wgpu::BufferBindingType;
    wgpu::BindGroupLayoutEntry entries[] = {
        entry(0, T::Storage),          // voxels
        entry(1, T::Storage),          // dirtyIn
        entry(2, T::Storage),          // dirtyOut
        entry(3, T::ReadOnlyStorage),  // materials
        entry(4, T::Uniform),          // TickParams
        entry(5, T::Uniform, true),    // PassParams (dynamic offset)
        entry(6, T::ReadOnlyStorage),  // brush ops
        entry(7, T::Storage),          // occupancy
        entry(8, T::Storage),          // world hash
        entry(9, T::Storage),          // pick
        entry(10, T::Uniform),         // RenderParams (pick ray)
    };
    wgpu::BindGroupLayoutDescriptor d{};
    d.entryCount = std::size(entries);
    d.entries = entries;
    simBGL_ = device.CreateBindGroupLayout(&d);
  }
  {
    auto entry = [](uint32_t binding, wgpu::BufferBindingType type) {
      wgpu::BindGroupLayoutEntry e{};
      e.binding = binding;
      e.visibility = wgpu::ShaderStage::Fragment;
      e.buffer.type = type;
      return e;
    };
    using T = wgpu::BufferBindingType;
    wgpu::BindGroupLayoutEntry entries[] = {
        entry(0, T::ReadOnlyStorage),  // voxels
        entry(1, T::ReadOnlyStorage),  // occupancy
        entry(2, T::ReadOnlyStorage),  // materials
        entry(3, T::Uniform),          // RenderParams
    };
    wgpu::BindGroupLayoutDescriptor d{};
    d.entryCount = std::size(entries);
    d.entries = entries;
    renderBGL_ = device.CreateBindGroupLayout(&d);
  }
  {
    wgpu::PipelineLayoutDescriptor d{};
    d.bindGroupLayoutCount = 1;
    d.bindGroupLayouts = &simBGL_;
    simPL_ = device.CreatePipelineLayout(&d);
    d.bindGroupLayouts = &renderBGL_;
    renderPL_ = device.CreatePipelineLayout(&d);
  }

  // ---- bind groups ----
  for (int page = 0; page < 2; page++) {
    auto b = [](uint32_t binding, const wgpu::Buffer& buf, uint64_t size = 0) {
      wgpu::BindGroupEntry e{};
      e.binding = binding;
      e.buffer = buf;
      e.size = size ? size : wgpu::kWholeSize;
      return e;
    };
    wgpu::BindGroupEntry entries[] = {
        b(0, world_->voxels),
        b(1, world_->dirty[page]),
        b(2, world_->dirty[1 - page]),
        b(3, materialBuf_),
        b(4, world_->tickUBO),
        b(5, world_->passUBO, 16),  // dynamic-offset window
        b(6, world_->opsBuf),
        b(7, world_->occupancy),
        b(8, world_->hash),
        b(9, world_->pick),
        b(10, world_->renderUBO),
    };
    wgpu::BindGroupDescriptor d{};
    d.layout = simBGL_;
    d.entryCount = std::size(entries);
    d.entries = entries;
    simBG_[page] = device.CreateBindGroup(&d);
  }
  {
    auto b = [](uint32_t binding, const wgpu::Buffer& buf) {
      wgpu::BindGroupEntry e{};
      e.binding = binding;
      e.buffer = buf;
      e.size = wgpu::kWholeSize;
      return e;
    };
    wgpu::BindGroupEntry entries[] = {
        b(0, world_->voxels),
        b(1, world_->occupancy),
        b(2, materialBuf_),
        b(3, world_->renderUBO),
    };
    wgpu::BindGroupDescriptor d{};
    d.layout = renderBGL_;
    d.entryCount = std::size(entries);
    d.entries = entries;
    renderBG_ = device.CreateBindGroup(&d);
  }

  std::string err;
  if (!BuildPipelines(device, &err)) {
    std::fprintf(stderr, "pipeline build failed:\n%s\n", err.c_str());
    return false;
  }
  return true;
}

void Simulation::UploadMaterials(const wgpu::Queue& queue,
                                 const std::vector<MaterialDef>& mats) {
  std::vector<MaterialGpu> table(4096, MaterialGpu{});
  for (size_t i = 0; i < mats.size() && i < 4096; i++) table[i] = mats[i].gpu;
  queue.WriteBuffer(materialBuf_, 0, table.data(), table.size() * sizeof(MaterialGpu));
}

bool Simulation::BuildPipelines(const wgpu::Device& device, std::string* err) {
  auto mod = [&](const char* name) { return LoadShader(device, shaderDir_, name); };
  wgpu::ShaderModule mWorldgen = mod("worldgen.wgsl");
  wgpu::ShaderModule mMutate = mod("sim_mutate.wgsl");
  wgpu::ShaderModule mStep = mod("sim_step.wgsl");
  wgpu::ShaderModule mOcc = mod("sim_occupancy.wgsl");
  wgpu::ShaderModule mPick = mod("sim_pick.wgsl");
  wgpu::ShaderModule mRay = mod("raymarch.wgsl");
  if (!mWorldgen || !mMutate || !mStep || !mOcc || !mPick || !mRay) {
    if (err) *err = "shader file read failure";
    return false;
  }

  worldgen_ = MakeComputePipeline(device, simPL_, mWorldgen, "main", "worldgen");
  mutate_ = MakeComputePipeline(device, simPL_, mMutate, "main", "mutate");
  step_ = MakeComputePipeline(device, simPL_, mStep, "main", "step");
  occupancy_ = MakeComputePipeline(device, simPL_, mOcc, "main", "occupancy");
  pick_ = MakeComputePipeline(device, simPL_, mPick, "main", "pick");

  raymarchModule_ = mRay;
  raymarchFormat_ = wgpu::TextureFormat::Undefined;  // force pipeline rebuild
  return true;
}

bool Simulation::ReloadShaders(const wgpu::Device& device, const wgpu::Instance& instance) {
  device.PushErrorScope(wgpu::ErrorFilter::Validation);
  std::string err;
  bool built = BuildPipelines(device, &err);
  bool hadError = false;
  wgpu::Future f = device.PopErrorScope(
      wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView msg) {
        if (type != wgpu::ErrorType::NoError) {
          hadError = true;
          std::fprintf(stderr, "shader reload error: %.*s\n", (int)msg.length, msg.data);
        }
      });
  instance.WaitAny(f, UINT64_MAX);
  return built && !hadError;
}

void Simulation::EncodeWorldgen(const wgpu::CommandEncoder& enc) {
  page_ = 0;
  enc.ClearBuffer(world_->dirty[0], 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->dirty[1], 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->hash, 0, wgpu::kWholeSize);
  wgpu::ComputePassEncoder pass = enc.BeginComputePass();
  uint32_t off = 0;
  pass.SetBindGroup(0, simBG_[page_], 1, &off);
  pass.SetPipeline(worldgen_);
  pass.DispatchWorkgroups(kWorldN / 4, kWorldN / 4, kWorldN / 4);
  pass.SetPipeline(occupancy_);
  pass.DispatchWorkgroups(kNumChunks, 1, 1);
  pass.End();
}

void Simulation::EncodeTick(const wgpu::CommandEncoder& enc, uint32_t opsCount,
                            bool hashEnable) {
  enc.ClearBuffer(world_->dirty[1 - page_], 0, wgpu::kWholeSize);
  if (hashEnable) enc.ClearBuffer(world_->hash, 0, wgpu::kWholeSize);

  wgpu::ComputePassEncoder pass = enc.BeginComputePass();
  uint32_t off = 0;
  pass.SetBindGroup(0, simBG_[page_], 1, &off);

  if (opsCount > 0) {
    pass.SetPipeline(mutate_);
    pass.DispatchWorkgroups(4 * opsCount, 4, 4);
  }

  pass.SetPipeline(step_);
  for (uint32_t k = 0; k < 54; k++) {  // 27 colors x 2 gravity substeps
    uint32_t offset = k * kPassStride;
    pass.SetBindGroup(0, simBG_[page_], 1, &offset);
    pass.DispatchWorkgroups(kStepGroups, kStepGroups, kStepGroups);
  }

  pass.SetBindGroup(0, simBG_[page_], 1, &off);
  pass.SetPipeline(occupancy_);
  pass.DispatchWorkgroups(kNumChunks, 1, 1);
  pass.SetPipeline(pick_);
  pass.DispatchWorkgroups(1, 1, 1);
  pass.End();
}

wgpu::RenderPassEncoder Simulation::BeginRenderPass(const wgpu::CommandEncoder& enc,
                                                    const wgpu::TextureView& target,
                                                    wgpu::TextureFormat format) {
  if (format != raymarchFormat_) {
    raymarchFormat_ = format;
    wgpu::ColorTargetState ct{};
    ct.format = format;
    wgpu::FragmentState fs{};
    fs.module = raymarchModule_;
    fs.entryPoint = "fs";
    fs.targetCount = 1;
    fs.targets = &ct;
    wgpu::RenderPipelineDescriptor d{};
    d.layout = renderPL_;
    d.vertex.module = raymarchModule_;
    d.vertex.entryPoint = "vs";
    d.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    d.fragment = &fs;
    d.label = "raymarch";
    raymarch_ = device_.CreateRenderPipeline(&d);
  }

  wgpu::RenderPassColorAttachment ca{};
  ca.view = target;
  ca.loadOp = wgpu::LoadOp::Clear;
  ca.storeOp = wgpu::StoreOp::Store;
  ca.clearValue = {0.1, 0.15, 0.25, 1.0};
  wgpu::RenderPassDescriptor d{};
  d.colorAttachmentCount = 1;
  d.colorAttachments = &ca;
  return enc.BeginRenderPass(&d);
}

void Simulation::DrawWorld(const wgpu::RenderPassEncoder& pass) {
  pass.SetPipeline(raymarch_);
  pass.SetBindGroup(0, renderBG_);
  pass.Draw(3);
}

void Simulation::FlipPage() { page_ = 1 - page_; }
