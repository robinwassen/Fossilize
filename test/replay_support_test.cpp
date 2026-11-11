#include "diagnostic_test.hpp"
#include "volk.h"
#include "fossilize_feature_filter.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
using namespace Fossilize;
static DiagnosticChecks checks;
struct Fixture
{
	FeatureFilter filter;
	VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	VkPhysicalDeviceProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	VulkanFeatures features = {};
	VulkanProperties properties = {};
	const char *extensions[5] = {VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
								 VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, VK_EXT_MESH_SHADER_EXTENSION_NAME,
								 VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME};
	Fixture(bool ray = true)
	{
		f.pNext = build_pnext_chain(features, VK_API_VERSION_1_3, extensions, 5);
		p.pNext = build_pnext_chain(properties, VK_API_VERSION_1_3, extensions, 5);
		features.ray_tracing_pipeline.rayTracingPipeline = ray;
		features.dynamic_rendering.dynamicRendering = VK_TRUE;
		if (!find_pnext<VkPhysicalDeviceDynamicRenderingFeatures>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, f.pNext))
		{
			features.dynamic_rendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
			features.dynamic_rendering.pNext = f.pNext;
			f.pNext = &features.dynamic_rendering;
		}
		properties.ray_tracing_pipeline.maxRayRecursionDepth = 4;
		properties.ray_tracing_pipeline.maxRayHitAttributeSize = 32;
		properties.multiview.maxMultiviewViewCount = 2;
		p.properties.limits.maxComputeWorkGroupSize[0] = 8;
		p.properties.limits.maxComputeWorkGroupSize[1] = 8;
		p.properties.limits.maxComputeWorkGroupSize[2] = 8;
		p.properties.limits.maxComputeWorkGroupInvocations = 8;
		p.properties.limits.maxPushConstantsSize = 128;
		p.properties.limits.maxBoundDescriptorSets = 4;
		checks.require(filter.init(VK_API_VERSION_1_3, extensions, 5, &f, &p), "M19/non-null-init");
	}
};
static void detail(const SupportDetail &d, SupportKind kind, const char *id, uint64_t known = 0, uint64_t required = 0,
				   uint64_t available = 0, uint64_t raw = 0)
{
	checks.require(d.kind == kind && !strcmp(d.identifier, id), std::string("M19/exact-identifier/") + id);
	checks.require(d.known == known && d.required == required && d.available == available && d.raw == raw,
				   std::string("M19/exact-values/") + id);
}
static VkComputePipelineCreateInfo compute_info()
{
	VkComputePipelineCreateInfo c = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	c.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	c.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	c.stage.pName = "main";
	c.stage.module = (VkShaderModule)1;
	return c;
}
static VkGraphicsPipelineCreateInfo graphics_info()
{
	VkGraphicsPipelineCreateInfo g = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	g.renderPass = (VkRenderPass)1;
	return g;
}
static VkBaseInStructure unknown = {VkStructureType(0x6ffffffe), nullptr};
static void compute_test()
{
	Fixture f;
	SupportDetail d;
	auto c = compute_info();
	checks.require(f.filter.compute_pipeline_is_supported(&c, d) && f.filter.compute_pipeline_is_supported(&c),
				   "M19/compute-supported-bool");
	c.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
	checks.require(!f.filter.compute_pipeline_is_supported(&c, d) && !f.filter.compute_pipeline_is_supported(&c),
				   "M19/compute-feature-old-bool");
	detail(d, SupportKind::Feature, "descriptorBuffer", 3, 1, 0);
	c.flags = 0;
	c.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT;
	checks.require(!f.filter.compute_pipeline_is_supported(&c, d), "M19/compute-subgroup");
	detail(d, SupportKind::Feature, "subgroupSizeControl", 3, 1, 0);
	c.stage.flags = 0;
	VkPipelineTessellationDomainOriginStateCreateInfo ext = {
		VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO};
	// This real pNext extension has no core promotion and is checked by compute.
	VkPipelineRasterizationConservativeStateCreateInfoEXT conservative = {
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT};
	c.pNext = &conservative;
	checks.require(!f.filter.compute_pipeline_is_supported(&c, d), "M19/compute-extension");
	detail(d, SupportKind::Extension, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME, 3, 1, 0);
	(void)ext;
	c.pNext = &unknown;
	checks.require(!f.filter.compute_pipeline_is_supported(&c, d), "M19/compute-unknown");
	detail(d, SupportKind::UnknownSType, "sType", SupportDetail::Raw, 0, 0, uint64_t(unknown.sType));
	c.pNext = nullptr;
	// Real entry-point/execution-mode metadata registered through the production parser.
	uint32_t code[] = {0x07230203, 0x10000, 0, 5, 0, 0x0005000f, 5, 1, 0x6e69616d, 0, 0x00060010, 1, 17, 9, 1, 1};
	VkShaderModuleCreateInfo m = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	m.pCode = code;
	m.codeSize = sizeof(code);
	checks.require(f.filter.register_shader_module_info(c.stage.module, &m), "M19/register-entry-point");
	checks.require(!f.filter.compute_pipeline_is_supported(&c, d), "M19/compute-size-limit");
	detail(d, SupportKind::Property, "maxComputeWorkGroupSize", 3, 9, 8);
	f.filter.unregister_shader_module_info(c.stage.module);
	code[13] = 3;
	code[14] = 3;
	code[15] = 1;
	checks.require(f.filter.register_shader_module_info(c.stage.module, &m), "M19/register-invocation-entry-point");
	checks.require(!f.filter.compute_pipeline_is_supported(&c, d), "M19/compute-invocations-limit");
	detail(d, SupportKind::Property, "maxComputeWorkGroupInvocations", 3, 9, 8);
	f.filter.unregister_shader_module_info(c.stage.module);
	checks.require(f.filter.compute_pipeline_is_supported(&c, d) && d.kind == SupportKind::None && !d.known &&
					   !d.identifier[0],
				   "M19/missing-metadata-no-invented-limit-clears-output");
}
static void graphics_test()
{
	Fixture f;
	SupportDetail d;
	auto g = graphics_info();
	checks.require(f.filter.graphics_pipeline_is_supported(&g, d) && f.filter.graphics_pipeline_is_supported(&g),
				   "M19/graphics-control");
	VkDynamicState state = VkDynamicState(0x6ffffffd);
	VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dynamic.dynamicStateCount = 1;
	dynamic.pDynamicStates = &state;
	g.pDynamicState = &dynamic;
	checks.require(!f.filter.graphics_pipeline_is_supported(&g, d) && !f.filter.graphics_pipeline_is_supported(&g),
				   "M19/graphics-unknown-dynamic-state-bool");
	detail(d, SupportKind::Constraint, "VkDynamicState", SupportDetail::Raw, 0, 0, uint64_t(state));
	state = VK_DYNAMIC_STATE_VIEWPORT;
	checks.require(f.filter.graphics_pipeline_is_supported(&g, d), "M19/graphics-dynamic-state-recovery");
	detail(d, SupportKind::None, "");
	g.pDynamicState = nullptr;
	VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.pName = "main";
	g.stageCount = 1;
	g.pStages = &stage;
	checks.require(!f.filter.graphics_pipeline_is_supported(&g, d) && !f.filter.graphics_pipeline_is_supported(&g),
				   "M19/graphics-forbidden-compute-stage-bool");
	detail(d, SupportKind::Constraint, "graphicsShaderStage", SupportDetail::Raw, 0, 0, uint64_t(stage.stage));
	stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
	checks.require(f.filter.graphics_pipeline_is_supported(&g, d), "M19/graphics-stage-recovery");
	detail(d, SupportKind::None, "");
	g.stageCount = 0;
	g.pStages = nullptr;
	VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	raster.depthClampEnable = VK_TRUE;
	g.pRasterizationState = &raster;
	checks.require(!f.filter.graphics_pipeline_is_supported(&g, d), "M19/graphics-feature");
	detail(d, SupportKind::Feature, "depthClamp", 3, 1, 0);
	raster.depthClampEnable = VK_FALSE;
	VkPipelineRasterizationConservativeStateCreateInfoEXT conservative = {
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT};
	raster.pNext = &conservative;
	checks.require(!f.filter.graphics_pipeline_is_supported(&g, d), "M19/graphics-nested-extension");
	detail(d, SupportKind::Extension, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME, 3, 1, 0);
	raster.pNext = &unknown;
	checks.require(!f.filter.graphics_pipeline_is_supported(&g, d), "M19/graphics-nested-unknown");
	detail(d, SupportKind::UnknownSType, "sType", 4, 0, 0, uint64_t(unknown.sType));
	raster.pNext = nullptr;
	VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rendering.viewMask = 4;
	g.pNext = &rendering;
	checks.require(!f.filter.graphics_pipeline_is_supported(&g, d), "M19/graphics-property-origin");
	detail(d, SupportKind::Property, "maxMultiviewViewCount", 3, 3, 2);
	rendering.viewMask = 0;
	g.pNext = nullptr;
	checks.require(f.filter.graphics_pipeline_is_supported(&g, d) && d.kind == SupportKind::None,
				   "M19/graphics-recovery");
}
static void raytracing_test()
{
	Fixture f;
	SupportDetail d;
	VkRayTracingPipelineCreateInfoKHR r = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
	r.maxPipelineRayRecursionDepth = 4;
	checks.require(f.filter.raytracing_pipeline_is_supported(&r, d) && f.filter.raytracing_pipeline_is_supported(&r),
				   "M19/ray-boundary-equal");
	Fixture disabled(false);
	checks.require(!disabled.filter.raytracing_pipeline_is_supported(&r, d), "M19/ray-feature");
	detail(d, SupportKind::Feature, "rayTracingPipeline", 3, 1, 0);
	r.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
	checks.require(!f.filter.raytracing_pipeline_is_supported(&r, d), "M19/ray-extension");
	detail(d, SupportKind::Extension, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, 3, 1, 0);
	r.flags = 0;
	r.maxPipelineRayRecursionDepth++;
	checks.require(!f.filter.raytracing_pipeline_is_supported(&r, d), "M19/ray-recursion-over");
	detail(d, SupportKind::Property, "maxRayRecursionDepth", 3, 5, 4);
	r.maxPipelineRayRecursionDepth--;
	VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
	stage.pNext = &unknown;
	r.stageCount = 1;
	r.pStages = &stage;
	checks.require(!f.filter.raytracing_pipeline_is_supported(&r, d), "M19/ray-stage-unknown");
	detail(d, SupportKind::UnknownSType, "sType", 4, 0, 0, uint64_t(unknown.sType));
	r.stageCount = 0;
	VkRayTracingShaderGroupCreateInfoKHR group = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
	group.pNext = &unknown;
	r.groupCount = 1;
	r.pGroups = &group;
	checks.require(!f.filter.raytracing_pipeline_is_supported(&r, d), "M19/ray-group-unknown");
	detail(d, SupportKind::UnknownSType, "sType", 4, 0, 0, uint64_t(unknown.sType));
	r.groupCount = 0;
	const char *ext = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
	FeatureFilter library;
	library.init(VK_API_VERSION_1_3, &ext, 1, &f.f, &f.p);
	VkRayTracingPipelineInterfaceCreateInfoKHR interface = {
		VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_INTERFACE_CREATE_INFO_KHR};
	interface.maxPipelineRayHitAttributeSize = 32;
	r.pLibraryInterface = &interface;
	checks.require(library.raytracing_pipeline_is_supported(&r, d), "M19/ray-hit-boundary-equal");
	interface.maxPipelineRayHitAttributeSize++;
	checks.require(!library.raytracing_pipeline_is_supported(&r, d), "M19/ray-hit-over");
	detail(d, SupportKind::Property, "maxRayHitAttributeSize", 3, 33, 32);
	r.pLibraryInterface = nullptr;
	VkPipelineLibraryCreateInfoKHR lib = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
	lib.pNext = &unknown;
	r.pLibraryInfo = &lib;
	checks.require(!library.raytracing_pipeline_is_supported(&r, d), "M19/ray-library-unknown");
	detail(d, SupportKind::UnknownSType, "sType", 4, 0, 0, uint64_t(unknown.sType));
}
static void static_test()
{
	Fixture f;
	SupportDetail d;
	VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	VkDescriptorSetLayoutCreateInfo set = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	uint32_t code[] = {0x07230203, 0x10000, 0, 1, 0, 0x00020011, 10};
	VkShaderModuleCreateInfo module = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	module.pCode = code;
	module.codeSize = 20;
	VkRenderPassCreateInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	VkRenderPassCreateInfo2 pass2 = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
#define CONTROL(Name, Value)                                                                                           \
	checks.require(f.filter.Name##_is_supported(&Value, d) && f.filter.Name##_is_supported(&Value),                    \
				   "M19/static-control/" #Name)
	CONTROL(sampler, sampler);
	CONTROL(descriptor_set_layout, set);
	CONTROL(pipeline_layout, layout);
	CONTROL(shader_module, module);
	CONTROL(render_pass, pass);
	CONTROL(render_pass2, pass2);
#define UNKNOWN(Name, Value)                                                                                           \
	Value.pNext = &unknown;                                                                                            \
	checks.require(!f.filter.Name##_is_supported(&Value, d), "M19/static-unknown/" #Name);                             \
	detail(d, SupportKind::UnknownSType, "sType", 4, 0, 0, uint64_t(unknown.sType));                                   \
	Value.pNext = nullptr
	UNKNOWN(sampler, sampler);
	UNKNOWN(descriptor_set_layout, set);
	UNKNOWN(pipeline_layout, layout);
	UNKNOWN(shader_module, module);
	UNKNOWN(render_pass, pass);
	UNKNOWN(render_pass2, pass2);
#undef UNKNOWN
	sampler.flags = VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT;
	checks.require(!f.filter.sampler_is_supported(&sampler, d), "M19/sampler-feature");
	detail(d, SupportKind::Feature, "nonSeamlessCubeMap", 3, 1, 0);
	sampler.flags = 0;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
	checks.require(!f.filter.sampler_is_supported(&sampler, d), "M19/sampler-extension");
	detail(d, SupportKind::Extension, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, 3, 1, 0);
	set.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
	checks.require(!f.filter.descriptor_set_layout_is_supported(&set, d), "M19/set-feature");
	detail(d, SupportKind::Feature, "descriptorBuffer", 3, 1, 0);
	set.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
	checks.require(!f.filter.descriptor_set_layout_is_supported(&set, d), "M19/set-extension");
	detail(d, SupportKind::Extension, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, 3, 1, 0);
	layout.flags = VkPipelineLayoutCreateFlags(0x40000000u);
	checks.require(!f.filter.pipeline_layout_is_supported(&layout, d) && !f.filter.pipeline_layout_is_supported(&layout),
				   "M19/layout-unknown-flags-bool");
	detail(d, SupportKind::Constraint, "VkPipelineLayoutCreateInfo.flags", 3, uint64_t(layout.flags),
		   uint64_t(VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT | VK_PIPELINE_LAYOUT_CREATE_NO_TASK_SHADER_BIT_KHR));
	layout.flags = 0;
	checks.require(f.filter.pipeline_layout_is_supported(&layout, d), "M19/layout-flags-recovery");
	detail(d, SupportKind::None, "");
	layout.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
	checks.require(!f.filter.pipeline_layout_is_supported(&layout, d), "M19/layout-feature");
	detail(d, SupportKind::Feature, "graphicsPipelineLibrary", 3, 1, 0);
	layout.flags = 0;
	VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 129};
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges = &range;
	checks.require(!f.filter.pipeline_layout_is_supported(&layout, d), "M19/layout-push-limit");
	detail(d, SupportKind::Property, "maxPushConstantsSize", 3, 129, 128);
	layout.pushConstantRangeCount = 0;
	layout.setLayoutCount = 5;
	checks.require(!f.filter.pipeline_layout_is_supported(&layout, d), "M19/layout-set-limit");
	detail(d, SupportKind::Property, "maxBoundDescriptorSets", 3, 5, 4);
	module.codeSize = sizeof(code);
	checks.require(!f.filter.shader_module_is_supported(&module, d), "M19/shader-known-capability");
	detail(d, SupportKind::Capability, "CapabilityFloat64", 7, 1, 0, 10);
	code[6] = 0xf000;
	checks.require(!f.filter.shader_module_is_supported(&module, d), "M19/shader-unknown-capability");
	detail(d, SupportKind::Capability, "SPIR-V.Capability", 4, 0, 0, 0xf000);
	module.codeSize = 3;
	checks.require(!f.filter.shader_module_is_supported(&module, d), "M19/shader-malformed");
	checks.require(d.kind == SupportKind::Malformed, "M19/malformed-separate-from-support");
	for (unsigned k = 0; k < 2; k++)
	{
		if (k)
			pass2.flags = VK_RENDER_PASS_CREATE_TRANSFORM_BIT_QCOM;
		else
			pass.flags = VK_RENDER_PASS_CREATE_TRANSFORM_BIT_QCOM;
		checks.require(
			!(k ? f.filter.render_pass2_is_supported(&pass2, d) : f.filter.render_pass_is_supported(&pass, d)),
			"M19/pass-extension");
		detail(d, SupportKind::Extension, VK_QCOM_RENDER_PASS_TRANSFORM_EXTENSION_NAME, 3, 1, 0);
	}
	pass.flags = 0;
	pass2.flags = 0;
	VkRenderPassMultiviewCreateInfo multi = {VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
	uint32_t mask = 4;
	multi.subpassCount = 1;
	multi.pViewMasks = &mask;
	pass.pNext = &multi;
	checks.require(!f.filter.render_pass_is_supported(&pass, d), "M19/pass-feature");
	detail(d, SupportKind::Feature, "multiview", 3, 1, 0);
	VkSubpassDescription2 sub = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
	sub.viewMask = 4;
	pass2.subpassCount = 1;
	pass2.pSubpasses = &sub;
	checks.require(!f.filter.render_pass2_is_supported(&pass2, d), "M19/pass2-feature-or-property");
#undef CONTROL
}
static void split_depth_stencil_layout_test()
{
	VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	const char *extensions[] = {VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_MAINTENANCE2_EXTENSION_NAME};
	FeatureFilter unavailable, extension_enabled, api_enabled;
	checks.require(unavailable.init(VK_API_VERSION_1_0, extensions, 1, &features, &properties),
				   "M19/split-layout-unavailable-init");
	checks.require(extension_enabled.init(VK_API_VERSION_1_0, extensions, 2, &features, &properties),
				   "M19/split-layout-extension-init");
	checks.require(api_enabled.init(VK_API_VERSION_1_1, extensions, 1, &features, &properties),
				   "M19/split-layout-api-init");
	VkAttachmentDescription attachment = {};
	attachment.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
	VkAttachmentDescription2 attachment2 = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
	attachment2.format = attachment.format;
	VkRenderPassCreateInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	pass.attachmentCount = 1;
	pass.pAttachments = &attachment;
	VkRenderPassCreateInfo2 pass2 = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
	pass2.attachmentCount = 1;
	pass2.pAttachments = &attachment2;
	SupportDetail d;
	for (bool version2 : {false, true})
	{
		auto supported = [&](FeatureFilter &filter) {
			return version2 ? filter.render_pass2_is_supported(&pass2, d) : filter.render_pass_is_supported(&pass, d);
		};
		auto old_supported = [&](FeatureFilter &filter) {
			return version2 ? filter.render_pass2_is_supported(&pass2) : filter.render_pass_is_supported(&pass);
		};
		for (auto split : {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL,
						   VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL})
		{
			for (bool initial : {false, true})
			{
				attachment.initialLayout = attachment2.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
				attachment.finalLayout = attachment2.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
				checks.require(supported(unavailable) && old_supported(unavailable), "M19/split-layout-valid-before");
				detail(d, SupportKind::None, "");
				if (initial)
					attachment.initialLayout = attachment2.initialLayout = split;
				else
					attachment.finalLayout = attachment2.finalLayout = split;
				checks.require(!supported(unavailable) && !old_supported(unavailable), "M19/split-layout-rejected-bool");
				detail(d, SupportKind::Extension, VK_KHR_MAINTENANCE2_EXTENSION_NAME, 3, 1, 0);
				checks.require(supported(extension_enabled) && old_supported(extension_enabled),
							   "M19/split-layout-extension-supported");
				detail(d, SupportKind::None, "");
				checks.require(!supported(unavailable), "M19/split-layout-reused-rejection");
				detail(d, SupportKind::Extension, VK_KHR_MAINTENANCE2_EXTENSION_NAME, 3, 1, 0);
				checks.require(supported(api_enabled) && old_supported(api_enabled), "M19/split-layout-api-supported");
				detail(d, SupportKind::None, "");
				checks.require(!supported(unavailable), "M19/split-layout-reject-before-recovery");
				attachment.initialLayout = attachment2.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
				attachment.finalLayout = attachment2.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
				checks.require(supported(unavailable) && old_supported(unavailable), "M19/split-layout-valid-after");
				detail(d, SupportKind::None, "");
			}
		}
	}
}
// Applicability: sampler has feature/extension/pNext checks, no independent
// property predicate; descriptor layouts add descriptor limits; pipeline layouts
// have feature/push-constant/set limits but no independent extension predicate;
// shader modules have capability/extension/version and malformed-binary checks;
// render passes 1/2 have feature/extension/multiview limits and nested pNext.
static void additional_static_test()
{
	Fixture f;
	SupportDetail d;
	VkDescriptorSetLayoutBinding binding = {};
	binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	binding.descriptorCount = 1;
	VkDescriptorSetLayoutCreateInfo set = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	set.bindingCount = 1;
	set.pBindings = &binding;
	checks.require(!f.filter.descriptor_set_layout_is_supported(&set, d), "M19/set-property");
	detail(d, SupportKind::Property, "maxDescriptorSetSamplers", 3, 1, 0);
	const char extension[] = "SPV_missing_test_extension";
	std::vector<uint32_t> code(5 + 1 + (sizeof(extension) + 3) / 4);
	code[0] = 0x07230203;
	code[1] = 0x10000;
	code[3] = 1;
	code[5] = uint32_t((code.size() - 5) << 16) | 10;
	memcpy(code.data() + 6, extension, sizeof(extension));
	VkShaderModuleCreateInfo module = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	module.pCode = code.data();
	module.codeSize = code.size() * 4;
	checks.require(!f.filter.shader_module_is_supported(&module, d), "M19/shader-extension-identifier");
	detail(d, SupportKind::Extension, extension, 3, 1, 0);
	uint32_t mask = 4;
	VkRenderPassMultiviewCreateInfo multi = {VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
	multi.subpassCount = 1;
	multi.pViewMasks = &mask;
	VkRenderPassCreateInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	pass.pNext = &multi;
	f.features.multiview.multiview = VK_TRUE;
	FeatureFilter views;
	views.init(VK_API_VERSION_1_3, f.extensions, 5, &f.f, &f.p);
	checks.require(!views.render_pass_is_supported(&pass, d), "M19/pass-property");
	detail(d, SupportKind::Property, "maxMultiviewViewCount", 3, 3, 2);
	VkSubpassDescription2 sub = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
	sub.viewMask = 4;
	VkRenderPassCreateInfo2 pass2 = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
	pass2.subpassCount = 1;
	pass2.pSubpasses = &sub;
	checks.require(!views.render_pass2_is_supported(&pass2, d), "M19/pass2-property");
	detail(d, SupportKind::Property, "maxMultiviewViewCount", 3, 3, 2);
	sub.viewMask = 0;
	sub.flags = VK_SUBPASS_DESCRIPTION_ENABLE_LEGACY_DITHERING_BIT_EXT;
	checks.require(!views.render_pass2_is_supported(&pass2, d), "M19/pass2-feature");
	detail(d, SupportKind::Feature, "legacyDithering", 3, 1, 0);
	sub.flags = 0;
	checks.require(views.render_pass2_is_supported(&pass2, d) && d.kind == SupportKind::None,
				   "M19/pass2-valid-after-rejection");
}
struct ReentrantQuery : DeviceQueryInterface
{
	FeatureFilter &filter;
	SupportDetail nested;
	explicit ReentrantQuery(FeatureFilter &f) : filter(f) {}
	bool format_is_supported(VkFormat, VkFormatFeatureFlags) override
	{
		auto c = compute_info();
		c.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
		checks.require(!filter.compute_pipeline_is_supported(&c, nested), "M19/reentrant-explicit-output");
		checks.require(!filter.compute_pipeline_is_supported(&c), "M19/reentrant-old-bool-boundary");
		return false;
	}
	bool descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *) override
	{
		return true;
	}
	void physical_device_feature_query(VkPhysicalDeviceFeatures2 *) override {}
};
static void reentrant_test()
{
	Fixture f;
	ReentrantQuery query(f.filter);
	f.filter.set_device_query_interface(&query);
	auto g = graphics_info();
	VkVertexInputAttributeDescription attribute = {};
	attribute.format = VK_FORMAT_R32_SFLOAT;
	VkPipelineVertexInputStateCreateInfo vertex = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vertex.vertexAttributeDescriptionCount = 1;
	vertex.pVertexAttributeDescriptions = &attribute;
	g.pVertexInputState = &vertex;
	SupportDetail outer;
	checks.require(!f.filter.graphics_pipeline_is_supported(&g, outer), "M19/reentrant-outer-failure");
	detail(query.nested, SupportKind::Feature, "descriptorBuffer", 3, 1, 0);
	detail(outer, SupportKind::Property, "formatFeatures", SupportDetail::Required | SupportDetail::Raw,
		   VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, 0, VK_FORMAT_R32_SFLOAT);
}
static void property_detail_test()
{
	Fixture f;
	VkPhysicalDeviceFeatures2 features = f.f;
	VkPhysicalDeviceProperties2 properties = f.p;
	f.features.descriptor_heap.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
	f.features.descriptor_heap.descriptorHeap = VK_TRUE;
	f.features.descriptor_heap.pNext = features.pNext;
	features.pNext = &f.features.descriptor_heap;
	f.features.fragment_shading_rate.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
	f.features.fragment_shading_rate.attachmentFragmentShadingRate = VK_TRUE;
	f.features.fragment_shading_rate.pNext = features.pNext;
	features.pNext = &f.features.fragment_shading_rate;
	f.properties.descriptor_heap.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
	f.properties.descriptor_heap.bufferDescriptorAlignment = 16;
	f.properties.descriptor_heap.imageDescriptorAlignment = 32;
	f.properties.descriptor_heap.samplerDescriptorAlignment = 8;
	f.properties.descriptor_heap.pNext = properties.pNext;
	properties.pNext = &f.properties.descriptor_heap;
	f.properties.fragment_shading_rate.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR;
	f.properties.fragment_shading_rate.minFragmentShadingRateAttachmentTexelSize = {4, 4};
	f.properties.fragment_shading_rate.maxFragmentShadingRateAttachmentTexelSize = {8, 8};
	f.properties.fragment_shading_rate.maxFragmentShadingRateAttachmentTexelSizeAspectRatio = 2;
	f.properties.fragment_shading_rate.pNext = properties.pNext;
	properties.pNext = &f.properties.fragment_shading_rate;
	const char *extensions[] = {VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME};
	FeatureFilter filter;
	filter.init(VK_API_VERSION_1_3, extensions, 2, &features, &properties);
	SupportDetail d;
	VkAttachmentReference2 reference = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
	reference.attachment = VK_ATTACHMENT_UNUSED;
	reference.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	reference.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	VkFragmentShadingRateAttachmentInfoKHR rate = {VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
	rate.pFragmentShadingRateAttachment = &reference;
	VkSubpassDescription2 sub = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
	sub.pNext = &rate;
	VkRenderPassCreateInfo2 pass = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
	pass.subpassCount = 1;
	pass.pSubpasses = &sub;
	struct Limit
	{
		uint32_t width, height, input, available;
		const char *identifier;
	};
	for (auto limit : {Limit{3, 4, 3, 4, "minFragmentShadingRateAttachmentTexelSize.width"},
					   Limit{9, 4, 9, 8, "maxFragmentShadingRateAttachmentTexelSize.width"},
					   Limit{4, 3, 3, 4, "minFragmentShadingRateAttachmentTexelSize.height"},
					   Limit{4, 9, 9, 8, "maxFragmentShadingRateAttachmentTexelSize.height"}})
	{
		rate.shadingRateAttachmentTexelSize = {limit.width, limit.height};
		checks.require(!filter.render_pass2_is_supported(&pass, d), "M19/texel-size-rejected");
		detail(d, SupportKind::Property, limit.identifier, 3, limit.input, limit.available);
		rate.shadingRateAttachmentTexelSize = {4, 4};
		checks.require(filter.render_pass2_is_supported(&pass, d), "M19/texel-size-restored-control");
	}
	VkDescriptorSetAndBindingMappingEXT mapping = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT};
	mapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT;
	mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
	mapping.sourceData.constantOffset.heapOffset = 3;
	VkShaderDescriptorSetAndBindingMappingInfoEXT mappings = {
		VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT};
	mappings.mappingCount = 1;
	mappings.pMappings = &mapping;
	VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stage.pNext = &mappings;
	stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
	stage.module = (VkShaderModule)1;
	stage.pName = "main";
	auto graphics = graphics_info();
	graphics.stageCount = 1;
	graphics.pStages = &stage;
	checks.require(!filter.graphics_pipeline_is_supported(&graphics, d), "M19/buffer-alignment-rejected");
	detail(d, SupportKind::Property, "bufferDescriptorAlignment", 3, 3, 16);
	mapping.sourceData.constantOffset.heapOffset = 16;
	checks.require(filter.graphics_pipeline_is_supported(&graphics, d), "M19/buffer-alignment-restored-control");
	mapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
	mapping.sourceData.constantOffset.heapOffset = 16;
	checks.require(!filter.graphics_pipeline_is_supported(&graphics, d), "M19/image-alignment-rejected");
	detail(d, SupportKind::Property, "imageDescriptorAlignment", 3, 16, 32);
	mapping.sourceData.constantOffset.heapOffset = 32;
	mapping.sourceData.constantOffset.heapArrayStride = 16;
	checks.require(!filter.graphics_pipeline_is_supported(&graphics, d), "M19/stride-alignment-rejected");
	detail(d, SupportKind::Property, "imageDescriptorAlignment", 3, 16, 32);
	mapping.sourceData.constantOffset.heapArrayStride = 32;
	checks.require(filter.graphics_pipeline_is_supported(&graphics, d), "M19/stride-alignment-restored-control");
	mapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT;
	mapping.sourceData.constantOffset.samplerHeapOffset = 3;
	checks.require(!filter.graphics_pipeline_is_supported(&graphics, d), "M19/sampler-alignment-rejected");
	detail(d, SupportKind::Property, "samplerDescriptorAlignment", 3, 3, 8);
	mapping.sourceData.constantOffset.samplerHeapOffset = 8;
	checks.require(filter.graphics_pipeline_is_supported(&graphics, d), "M19/sampler-alignment-restored-control");
}
static void subgroup_constraint_test()
{
	Fixture f;
	f.features.subgroup_size_control.subgroupSizeControl = VK_TRUE;
	f.features.subgroup_size_control.computeFullSubgroups = VK_TRUE;
	f.properties.subgroup_size_control.minSubgroupSize = 8;
	f.properties.subgroup_size_control.maxSubgroupSize = 32;
	f.properties.subgroup_size_control.requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT;
	f.p.properties.limits.maxComputeWorkGroupSize[0] = 64;
	f.p.properties.limits.maxComputeWorkGroupInvocations = 64;
	FeatureFilter filter;
	filter.init(VK_API_VERSION_1_3, f.extensions, 5, &f.f, &f.p);
	uint32_t code[] = {0x07230203, 0x10000, 0, 5, 0, 0x0005000f, 5, 1, 0x6e69616d, 0, 0x00060010, 1, 17, 16, 1, 1};
	VkShaderModuleCreateInfo module = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	module.pCode = code;
	module.codeSize = sizeof(code);
	auto compute = compute_info();
	compute.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required = {
		VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
	required.requiredSubgroupSize = 32;
	compute.stage.pNext = &required;
	checks.require(filter.register_shader_module_info(compute.stage.module, &module),
				   "M19/register-full-subgroup-input");
	SupportDetail d;
	checks.require(!filter.compute_pipeline_is_supported(&compute, d), "M19/full-subgroup-divisibility-rejected");
	detail(d, SupportKind::Constraint, "workgroupSizeXMultipleOfSubgroupSize", 3, 16, 32);
	filter.unregister_shader_module_info(compute.stage.module);
	code[13] = 32;
	checks.require(filter.register_shader_module_info(compute.stage.module, &module) &&
					   filter.compute_pipeline_is_supported(&compute, d),
				   "M19/full-subgroup-restored-control");
}

static void scope_test()
{
	Fixture f;
	std::mutex mutex;
	std::condition_variable cv;
	unsigned ready = 0;
	bool go = false;
	std::atomic<unsigned> correct{0};
	std::vector<std::thread> workers;
	for (unsigned i = 0; i < 8; i++)
		workers.emplace_back(
			[&, i]()
			{
				{
					std::unique_lock<std::mutex> hold(mutex);
					ready++;
					cv.notify_all();
					cv.wait(hold, [&]() { return go; });
				}
				SupportDetail d;
				auto c = compute_info();
				if (i & 1)
					c.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
				else
					c.pNext = &unknown;
				bool ok = !f.filter.compute_pipeline_is_supported(&c, d);
				if (ok && d.kind == ((i & 1) ? SupportKind::Feature : SupportKind::UnknownSType) &&
					!strcmp(d.identifier, (i & 1) ? "descriptorBuffer" : "sType"))
					correct++;
				c.flags = 0;
				c.pNext = nullptr;
				if (f.filter.compute_pipeline_is_supported(&c, d) && d.kind == SupportKind::None && !d.identifier[0])
					correct++;
			});
	{
		std::unique_lock<std::mutex> hold(mutex);
		cv.wait(hold, [&]() { return ready == 8; });
		go = true;
		cv.notify_all();
	}
	for (auto &w : workers)
		w.join();
	checks.require(correct == 16, "M19/barrier-concurrent-call-isolation-and-reuse");
	SupportDetail retained;
	{
		auto c = compute_info();
		VkBaseInStructure local = unknown;
		c.pNext = &local;
		f.filter.compute_pipeline_is_supported(&c, retained);
	}
	detail(retained, SupportKind::UnknownSType, "sType", 4, 0, 0, uint64_t(unknown.sType));
}
int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	std::string c = argv[1];
	if (c == "compute")
	{
		compute_test();
		subgroup_constraint_test();
	}
	else if (c == "graphics")
	{
		graphics_test();
		property_detail_test();
	}
	else if (c == "raytracing")
		raytracing_test();
	else if (c == "static")
	{
		static_test();
		additional_static_test();
		split_depth_stencil_layout_test();
	}
	else if (c == "call-scope")
	{
		scope_test();
		reentrant_test();
	}
	else
		return 2;
	printf("M19 support %s failures=%d\n", c.c_str(), checks.failures);
	return checks.failures ? 1 : 0;
}
