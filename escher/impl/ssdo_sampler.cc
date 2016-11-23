// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "escher/impl/ssdo_sampler.h"

#include "escher/impl/command_buffer.h"
#include "escher/impl/glsl_compiler.h"
#include "escher/impl/mesh_impl.h"
#include "escher/impl/model_pipeline.h"
#include "escher/impl/vulkan_utils.h"
#include "escher/renderer/framebuffer.h"
#include "escher/renderer/image.h"
#include "escher/renderer/texture.h"
#include "escher/shape/mesh.h"

namespace escher {
namespace impl {

namespace {

constexpr char g_vertex_src[] = R"GLSL(
  #version 450
  #extension GL_ARB_separate_shader_objects : enable

  layout(location = 0) in vec2 in_position;
  layout(location = 2) in vec2 in_uv;

  layout(location = 0) out vec2 fragment_uv;

  out gl_PerVertex {
    vec4 gl_Position;
  };

  void main() {
    gl_Position = vec4(in_position, 0.f, 1.f);
    fragment_uv = in_uv;
  }
)GLSL";

// Samples occlusion in a neighborhood around each pixel.  Unoccluded samples
// are summed in order to obtain a measure of the amount of light that reaches
// this pixel.  The result is noisy, and should be filtered before used as a
// texture in a subsequent render pass.
constexpr char g_fragment_src[] = R"GLSL(
  #version 450
  #extension GL_ARB_separate_shader_objects : enable

  // Texture coordinates generated by the vertex shader.
  layout(location = 0) in vec2 fragment_uv;

  layout(location = 0) out vec4 outColor;

  // Uniform parameters.
  layout(push_constant) uniform PushConstants {
    // A description of the directional key light:
    //
    //  * theta, phi: The direction from which the light is received. The first
    //    coordinate is theta (the the azimuthal angle, in radians) and the second
    //    coordinate is phi (the polar angle, in radians).
    //  * dispersion: The angular variance in the light, in radians.
    //  * intensity: The amount of light emitted.
    vec4 key_light;

    // The size of the viewing volume in (width, height, depth).
    vec3 viewing_volume;
  } pushed;

  // Depth information about the scene.
  //
  // The shader assumes that the depth information in the r channel.
  layout(set = 0, binding = 0) uniform sampler2D depth_map;

  // A random texture of size kNoiseSize.
  layout(set = 0, binding = 1) uniform sampler2D noise;

  const float kPi = 3.14159265359;

  // Must match SsdoSampler::kNoiseSize (C++).
  const int kNoiseSize = 5;

  // The number of screen-space samples to use in the computation.
  const int kTapCount = 8;

  // These should be relatively primary to each other and to kTapCount;
  // TODO: only kSpirals.x is used... should .y also be used?
  const vec2 kSpirals = vec2(7.0, 5.0);

  // TODO(abarth): Make the shader less sensitive to this parameter.
  const float kSampleRadius = 16.0;  // screen pixels.

  float sampleKeyIllumination(vec2 fragment_uv,
                              float fragment_z,
                              float alpha,
                              vec2 seed) {
    float key_light_dispersion = pushed.key_light.z;
    vec2 key_light0 = pushed.key_light.xy - key_light_dispersion / 2.0;
    float theta = key_light0.x + fract(seed.x + alpha * kSpirals.x) * key_light_dispersion;
    float radius = alpha * kSampleRadius;

    vec2 tap_delta_uv = radius * vec2(cos(theta), sin(theta)) / pushed.viewing_volume.xy;
    float tap_depth_uv = texture(depth_map, fragment_uv + tap_delta_uv).r;
    float tap_z = tap_depth_uv * -pushed.viewing_volume.z;

    // TODO: use clamp here, once we can use GLSL standard library.
    return 1.0 - max(0.0, (tap_z - fragment_z) / radius);
  }

  float sampleFillIllumination(vec2 fragment_uv,
                               float fragment_z,
                               float alpha,
                               vec2 seed) {
    float theta = 2.0 * kPi * (seed.x + alpha * kSpirals.x);
    float radius = alpha * kSampleRadius;

    vec2 tap_delta_uv = radius * vec2(cos(theta), sin(theta)) / pushed.viewing_volume.xy;
    float tap_depth_uv = texture(depth_map, fragment_uv + tap_delta_uv).r;
    float tap_z = tap_depth_uv * -pushed.viewing_volume.z;

    return 1.0 - max(0.0, (tap_z - fragment_z) / radius);
  }

  void main() {
    vec2 seed = texture(noise, fract(gl_FragCoord.xy / float(kNoiseSize))).rg;

    float viewing_volume_depth_range = pushed.viewing_volume.z;
    float fragment_z =
        texture(depth_map, fragment_uv).r *
        -viewing_volume_depth_range;

    float key_light_intensity = pushed.key_light.w;
    float fill_light_intensity = 1.0 - key_light_intensity;

    float L = 0.0;
    for (int i = 0; i < kTapCount; ++i) {
      float alpha = (float(i) + 0.5) / float(kTapCount);
      L += key_light_intensity * sampleKeyIllumination(fragment_uv, fragment_z, alpha, seed);
      L += fill_light_intensity * sampleFillIllumination(fragment_uv, fragment_z, alpha, seed);
    }
    L = clamp(L / float(kTapCount), 0.0, 1.0);

    outColor = vec4(L, 0.0, 0.0, 1.0);
  }
)GLSL";

// TODO: refactor this into a PipelineBuilder class.
std::unique_ptr<ModelPipeline> CreatePipeline(
    vk::Device device,
    vk::RenderPass render_pass,
    const MeshSpec& mesh_spec,
    const MeshSpecImpl& mesh_spec_impl,
    vk::DescriptorSetLayout layout,
    GlslToSpirvCompiler* compiler) {
  ModelPipelineSpec model_pipeline_spec;
  model_pipeline_spec.mesh_spec = mesh_spec;
  model_pipeline_spec.use_depth_prepass = false;

  auto vertex_spirv_future =
      compiler->Compile(vk::ShaderStageFlagBits::eVertex, {{g_vertex_src}},
                        std::string(), "main");
  auto fragment_spirv_future =
      compiler->Compile(vk::ShaderStageFlagBits::eFragment, {{g_fragment_src}},
                        std::string(), "main");

  vk::ShaderModule vertex_module;
  {
    SpirvData spirv = vertex_spirv_future.get();

    vk::ShaderModuleCreateInfo module_info;
    module_info.codeSize = spirv.size() * sizeof(uint32_t);
    module_info.pCode = spirv.data();
    vertex_module =
        ESCHER_CHECKED_VK_RESULT(device.createShaderModule(module_info));
  }

  vk::ShaderModule fragment_module;
  {
    SpirvData spirv = fragment_spirv_future.get();

    vk::ShaderModuleCreateInfo module_info;
    module_info.codeSize = spirv.size() * sizeof(uint32_t);
    module_info.pCode = spirv.data();
    fragment_module =
        ESCHER_CHECKED_VK_RESULT(device.createShaderModule(module_info));
  }

  vk::PipelineShaderStageCreateInfo vertex_stage_info;
  vertex_stage_info.stage = vk::ShaderStageFlagBits::eVertex;
  vertex_stage_info.module = vertex_module;
  vertex_stage_info.pName = "main";

  vk::PipelineShaderStageCreateInfo fragment_stage_info;
  fragment_stage_info.stage = vk::ShaderStageFlagBits::eFragment;
  fragment_stage_info.module = fragment_module;
  fragment_stage_info.pName = "main";

  constexpr uint32_t kNumShaderStages = 2;
  vk::PipelineShaderStageCreateInfo shader_stages[kNumShaderStages] = {
      vertex_stage_info, fragment_stage_info};

  vk::PipelineVertexInputStateCreateInfo vertex_input_info;
  vertex_input_info.vertexBindingDescriptionCount = 1;
  vertex_input_info.pVertexBindingDescriptions = &mesh_spec_impl.binding;
  vertex_input_info.vertexAttributeDescriptionCount =
      mesh_spec_impl.attributes.size();
  vertex_input_info.pVertexAttributeDescriptions =
      mesh_spec_impl.attributes.data();

  vk::PipelineInputAssemblyStateCreateInfo input_assembly_info;
  input_assembly_info.topology = vk::PrimitiveTopology::eTriangleList;
  input_assembly_info.primitiveRestartEnable = false;

  vk::PipelineDepthStencilStateCreateInfo depth_stencil_info;
  depth_stencil_info.depthTestEnable = true;
  depth_stencil_info.depthWriteEnable = model_pipeline_spec.use_depth_prepass;
  depth_stencil_info.stencilTestEnable = false;

  // This is set dynamically during rendering.
  vk::Viewport viewport;
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = 0.f;
  viewport.height = 0.f;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 0.0f;

  // This is set dynamically during rendering.
  vk::Rect2D scissor;
  scissor.offset = vk::Offset2D{0, 0};
  scissor.extent = vk::Extent2D{0, 0};

  vk::PipelineViewportStateCreateInfo viewport_state;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = &viewport;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = &scissor;

  vk::PipelineRasterizationStateCreateInfo rasterizer;
  rasterizer.depthClampEnable = false;
  rasterizer.rasterizerDiscardEnable = false;
  rasterizer.polygonMode = vk::PolygonMode::eFill;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = vk::CullModeFlagBits::eBack;
  rasterizer.frontFace = vk::FrontFace::eClockwise;
  rasterizer.depthBiasEnable = false;

  vk::PipelineMultisampleStateCreateInfo multisampling;
  multisampling.sampleShadingEnable = false;
  multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;

  // TODO: revisit whether this is what we want
  vk::PipelineColorBlendAttachmentState color_blend_attachment;
  color_blend_attachment.colorWriteMask =
      vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
      vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  color_blend_attachment.blendEnable = false;

  // TODO: revisit whether this is what we want
  vk::PipelineColorBlendStateCreateInfo color_blending;
  color_blending.logicOpEnable = false;
  color_blending.logicOp = vk::LogicOp::eCopy;
  color_blending.attachmentCount = 1;
  color_blending.pAttachments = &color_blend_attachment;
  color_blending.blendConstants[0] = 0.0f;
  color_blending.blendConstants[1] = 0.0f;
  color_blending.blendConstants[2] = 0.0f;
  color_blending.blendConstants[3] = 0.0f;

  vk::PipelineDynamicStateCreateInfo dynamic_state;
  const uint32_t kDynamicStateCount = 2;
  vk::DynamicState dynamic_states[] = {vk::DynamicState::eViewport,
                                       vk::DynamicState::eScissor};
  dynamic_state.dynamicStateCount = kDynamicStateCount;
  dynamic_state.pDynamicStates = dynamic_states;

  vk::PushConstantRange push_constants;
  push_constants.stageFlags = vk::ShaderStageFlagBits::eFragment;
  push_constants.offset = 0;
  push_constants.size = sizeof(SsdoSampler::PushConstants);

  vk::PipelineLayoutCreateInfo pipeline_layout_info;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &layout;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constants;

  vk::PipelineLayout pipeline_layout = ESCHER_CHECKED_VK_RESULT(
      device.createPipelineLayout(pipeline_layout_info, nullptr));

  vk::GraphicsPipelineCreateInfo pipeline_info;
  pipeline_info.stageCount = kNumShaderStages;
  pipeline_info.pStages = shader_stages;
  pipeline_info.pVertexInputState = &vertex_input_info;
  pipeline_info.pInputAssemblyState = &input_assembly_info;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &rasterizer;
  pipeline_info.pDepthStencilState = &depth_stencil_info;
  pipeline_info.pMultisampleState = &multisampling;
  pipeline_info.pColorBlendState = &color_blending;
  pipeline_info.pDynamicState = &dynamic_state;
  pipeline_info.layout = pipeline_layout;
  pipeline_info.renderPass = render_pass;
  pipeline_info.subpass = 0;
  pipeline_info.basePipelineHandle = VK_NULL_HANDLE;

  vk::Pipeline pipeline = ESCHER_CHECKED_VK_RESULT(
      device.createGraphicsPipeline(nullptr, pipeline_info));

  device.destroyShaderModule(vertex_module);
  device.destroyShaderModule(fragment_module);

  return std::make_unique<ModelPipeline>(model_pipeline_spec, device, pipeline,
                                         pipeline_layout);
}

vk::RenderPass CreateRenderPass(vk::Device device) {
  constexpr uint32_t kAttachmentCount = 1;
  vk::AttachmentDescription attachments[kAttachmentCount];

  // Only the color attachment is required; there is no depth buffer (although
  // one from a previous pass will be provided to the shader as a texture).
  const uint32_t kColorAttachment = 0;
  auto& color_attachment = attachments[kColorAttachment];
  color_attachment.format = SsdoSampler::kColorFormat;
  color_attachment.samples = vk::SampleCountFlagBits::e1;
  color_attachment.loadOp = vk::AttachmentLoadOp::eClear;
  color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
  color_attachment.initialLayout = vk::ImageLayout::eUndefined;
  color_attachment.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  vk::AttachmentReference color_reference;
  color_reference.attachment = kColorAttachment;
  color_reference.layout = vk::ImageLayout::eColorAttachmentOptimal;

  // A vk::RenderPass needs at least one subpass.
  constexpr uint32_t kSubpassCount = 1;
  vk::SubpassDescription subpasses[kSubpassCount];
  subpasses[0].pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
  subpasses[0].colorAttachmentCount = 1;
  subpasses[0].pColorAttachments = &color_reference;

  // Even though we have a single subpass, we need to declare dependencies to
  // support the layout transitions specified by the attachment references.
  constexpr uint32_t kDependencyCount = 2;
  vk::SubpassDependency dependencies[kDependencyCount];
  auto& input_dependency = dependencies[0];
  auto& output_dependency = dependencies[1];

  // The first dependency transitions from the final layout from the previous
  // render pass, to the initial layout of this one.
  input_dependency.srcSubpass = VK_SUBPASS_EXTERNAL;  // not in vulkan.hpp ?!?
  input_dependency.dstSubpass = 0;
  input_dependency.srcStageMask = vk::PipelineStageFlagBits::eBottomOfPipe;
  input_dependency.dstStageMask =
      vk::PipelineStageFlagBits::eColorAttachmentOutput;
  // TODO: should srcAccessMask also include eMemoryWrite?
  input_dependency.srcAccessMask = vk::AccessFlagBits::eMemoryRead;
  input_dependency.dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead |
                                   vk::AccessFlagBits::eColorAttachmentWrite;
  input_dependency.dependencyFlags = vk::DependencyFlagBits::eByRegion;

  // The second dependency describes the transition from the initial to final
  // layout.
  output_dependency.srcSubpass = 0;
  output_dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
  output_dependency.srcStageMask =
      vk::PipelineStageFlagBits::eColorAttachmentOutput;
  output_dependency.dstStageMask = vk::PipelineStageFlagBits::eBottomOfPipe;
  output_dependency.srcAccessMask = vk::AccessFlagBits::eColorAttachmentRead |
                                    vk::AccessFlagBits::eColorAttachmentWrite;
  output_dependency.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
  output_dependency.dependencyFlags = vk::DependencyFlagBits::eByRegion;

  // Create the render pass, now that we can fully specify it.
  vk::RenderPassCreateInfo info;
  info.attachmentCount = kAttachmentCount;
  info.pAttachments = attachments;
  info.subpassCount = kSubpassCount;
  info.pSubpasses = subpasses;
  info.dependencyCount = kDependencyCount;
  info.pDependencies = dependencies;

  return ESCHER_CHECKED_VK_RESULT(device.createRenderPass(info));
}

}  // namespace

SsdoSampler::SsdoSampler(vk::Device device,
                         MeshPtr full_screen,
                         ImagePtr noise_image,
                         GlslToSpirvCompiler* compiler)
    : device_(device),
      pool_(device, GetDescriptorSetLayoutCreateInfo(), 6),
      full_screen_(full_screen),
      noise_texture_(ftl::MakeRefCounted<Texture>(noise_image,
                                                  device,
                                                  vk::Filter::eNearest)),
      // TODO: VulkanProvider should know the swapchain format and we should use
      // it.
      render_pass_(CreateRenderPass(device)),
      pipeline_(CreatePipeline(device,
                               render_pass_,
                               full_screen->spec,
                               full_screen->spec_impl(),
                               pool_.layout(),
                               compiler)) {
  FTL_DCHECK(noise_image->width() == kNoiseSize &&
             noise_image->height() == kNoiseSize);
}

SsdoSampler::~SsdoSampler() {
  device_.destroyRenderPass(render_pass_);
}

const vk::DescriptorSetLayoutCreateInfo&
SsdoSampler::GetDescriptorSetLayoutCreateInfo() {
  constexpr uint32_t kNumBindings = 2;
  static vk::DescriptorSetLayoutBinding bindings[kNumBindings];
  static vk::DescriptorSetLayoutCreateInfo info;
  static vk::DescriptorSetLayoutCreateInfo* ptr = nullptr;
  if (!ptr) {
    auto& depth_texture_binding = bindings[0];
    auto& noise_texture_binding = bindings[1];

    depth_texture_binding.binding = 0;
    depth_texture_binding.descriptorType =
        vk::DescriptorType::eCombinedImageSampler;
    depth_texture_binding.descriptorCount = 1;
    depth_texture_binding.stageFlags = vk::ShaderStageFlagBits::eFragment;

    // TODO: should probably use a texture array instead of multiple bindings.
    noise_texture_binding.binding = 1;
    noise_texture_binding.descriptorType =
        vk::DescriptorType::eCombinedImageSampler;
    noise_texture_binding.descriptorCount = 1;
    noise_texture_binding.stageFlags = vk::ShaderStageFlagBits::eFragment;

    info.bindingCount = kNumBindings;
    info.pBindings = bindings;
    ptr = &info;
  }
  return *ptr;
}

void SsdoSampler::Draw(CommandBuffer* command_buffer,
                       const FramebufferPtr& framebuffer,
                       const TexturePtr& depth_texture,
                       const PushConstants* push_constants,
                       const std::vector<vk::ClearValue>& clear_values) {
  auto vk_command_buffer = command_buffer->get();
  auto descriptor_set = pool_.Allocate(1, command_buffer)->get(0);

  vk::Viewport viewport;
  viewport.width = framebuffer->width();
  viewport.height = framebuffer->height();
  vk_command_buffer.setViewport(0, 1, &viewport);

  // Common to both image descriptors.
  constexpr uint32_t kUpdatedDescriptorCount = 2;
  vk::WriteDescriptorSet writes[kUpdatedDescriptorCount];
  writes[0].dstSet = writes[1].dstSet = descriptor_set;
  writes[0].dstArrayElement = writes[1].dstArrayElement = 0;
  writes[0].descriptorType = writes[1].descriptorType =
      vk::DescriptorType::eCombinedImageSampler;
  writes[0].descriptorCount = writes[1].descriptorCount = 1;

  // Specific to depth texture.
  vk::DescriptorImageInfo depth_texture_info;
  depth_texture_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  depth_texture_info.imageView = depth_texture->image_view();
  depth_texture_info.sampler = depth_texture->sampler();
  writes[0].dstBinding = 0;
  writes[0].pImageInfo = &depth_texture_info;

  // Specific to noise texture.
  vk::DescriptorImageInfo noise_texture_info;
  noise_texture_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  noise_texture_info.imageView = noise_texture_->image_view();
  noise_texture_info.sampler = noise_texture_->sampler();
  writes[1].dstBinding = 1;
  writes[1].pImageInfo = &noise_texture_info;

  device_.updateDescriptorSets(kUpdatedDescriptorCount, writes, 0, nullptr);

  command_buffer->BeginRenderPass(render_pass_, framebuffer, clear_values);
  {
    auto vk_pipeline_layout = pipeline_->pipeline_layout();

    vk_command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                   pipeline_->pipeline());

    vk_command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                         vk_pipeline_layout, 0, 1,
                                         &descriptor_set, 0, nullptr);

    vk_command_buffer.pushConstants(vk_pipeline_layout,
                                    vk::ShaderStageFlagBits::eFragment, 0,
                                    sizeof(PushConstants), push_constants);

    command_buffer->DrawMesh(full_screen_);
  }
  command_buffer->EndRenderPass();
}

SsdoSampler::PushConstants::PushConstants(const Stage& stage)
    : key_light(vec4(stage.key_light().direction(),
                     stage.key_light().dispersion(),
                     stage.key_light().intensity())),
      viewing_volume(vec3(stage.viewing_volume().width(),
                          stage.viewing_volume().height(),
                          stage.viewing_volume().depth_range())) {}

}  // namespace impl
}  // namespace escher