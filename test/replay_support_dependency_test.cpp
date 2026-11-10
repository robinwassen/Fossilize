#include <array>
#include <cstdint>
#include <mutex>
#include <set>
#include <vector>
struct Edge
{
	uint64_t tag, hash, parent_tag, parent_hash, status, category, omitted;
};
static std::mutex edge_mutex;
static std::vector<Edge> edges;
static void observe_dependency(uint64_t t, uint64_t h, uint64_t pt, uint64_t ph, uint64_t s, uint64_t c,
							   uint64_t omitted)
{
	std::lock_guard<std::mutex> hold(edge_mutex);
	edges.push_back({t, h, pt, ph, s, c, omitted});
}
#define FOSSILIZE_REPLAY_TEST_DEPENDENCY observe_dependency
#define FOSSILIZE_REPLAY_OUTCOME_NO_MAIN
#include "replay_outcome_test.cpp"
#undef FOSSILIZE_REPLAY_TEST_DEPENDENCY
using namespace Fossilize::ReplayEvent;
static std::atomic<unsigned> pipeline_calls{0}, module_calls{0};
static ThreadedReplayer *active_replay;
static VkResult module_result = VK_SUCCESS;
static Hash fail_parent_hash;
static std::set<uint64_t> live_pipelines;
static std::atomic<unsigned> lifetime_errors{0}, cleanup_errors{0};
static std::string graph_mode;
static ResourceTag static_failure_tag = RESOURCE_COUNT;
static PFN_vkCreateSampler graph_real_sampler;
static PFN_vkCreateDescriptorSetLayout graph_real_set;
static PFN_vkCreateRenderPass graph_real_pass;
static VKAPI_ATTR VkResult VKAPI_CALL graph_sampler(VkDevice d, const VkSamplerCreateInfo *i,
													const VkAllocationCallbacks *a, VkSampler *out)
{
	if (static_failure_tag == RESOURCE_SAMPLER)
	{
		*out = VK_NULL_HANDLE;
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	return graph_real_sampler(d, i, a, out);
}
static VKAPI_ATTR VkResult VKAPI_CALL graph_set(VkDevice d, const VkDescriptorSetLayoutCreateInfo *i,
												const VkAllocationCallbacks *a, VkDescriptorSetLayout *out)
{
	if (static_failure_tag == RESOURCE_DESCRIPTOR_SET_LAYOUT)
	{
		*out = VK_NULL_HANDLE;
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	return graph_real_set(d, i, a, out);
}
static VKAPI_ATTR VkResult VKAPI_CALL graph_pass(VkDevice d, const VkRenderPassCreateInfo *i,
												 const VkAllocationCallbacks *a, VkRenderPass *out)
{
	if (static_failure_tag == RESOURCE_RENDER_PASS)
	{
		*out = VK_NULL_HANDLE;
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	return graph_real_pass(d, i, a, out);
}
static VkResult create_pipeline(VkPipeline *out)
{
	pipeline_calls++;
	Hash hash = active_replay->get_per_thread_data().active_hash;
	if (hash >= 0x2000 &&
		(graph_mode.find("parent") != std::string::npos || graph_mode.find("library") != std::string::npos))
	{
		auto tag = active_replay->get_per_thread_data().active_tag;
		uint64_t parent = 0x900 +
						  (tag == RESOURCE_COMPUTE_PIPELINE	   ? 0
						   : tag == RESOURCE_GRAPHICS_PIPELINE ? 1
															   : 2) +
						  0x10000;
		std::lock_guard<std::mutex> hold(edge_mutex);
		if (graph_mode.find("library") != std::string::npos)
		{
			if (!live_pipelines.count(parent))
				lifetime_errors++;
		}
		else if (!active_replay->derivative_parent_succeeded(tag, parent - 0x10000) || live_pipelines.count(parent))
			lifetime_errors++;
	}
	VkResult result = hash == fail_parent_hash ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
	*out = result == VK_SUCCESS ? (VkPipeline)uintptr_t(hash + 0x10000) : VK_NULL_HANDLE;
	if (*out)
	{
		std::lock_guard<std::mutex> hold(edge_mutex);
		if (!live_pipelines.insert((uint64_t)*out).second)
			cleanup_errors++;
	}
	return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL graph_compute(VkDevice, VkPipelineCache, uint32_t,
													const VkComputePipelineCreateInfo *, const VkAllocationCallbacks *,
													VkPipeline *p)
{
	return create_pipeline(p);
}
static VKAPI_ATTR VkResult VKAPI_CALL graph_graphics(VkDevice, VkPipelineCache, uint32_t,
													 const VkGraphicsPipelineCreateInfo *,
													 const VkAllocationCallbacks *, VkPipeline *p)
{
	return create_pipeline(p);
}
static VKAPI_ATTR VkResult VKAPI_CALL graph_raytracing(VkDevice, VkDeferredOperationKHR, VkPipelineCache, uint32_t,
													   const VkRayTracingPipelineCreateInfoKHR *,
													   const VkAllocationCallbacks *, VkPipeline *p)
{
	return create_pipeline(p);
}
static VKAPI_ATTR VkResult VKAPI_CALL graph_module(VkDevice, const VkShaderModuleCreateInfo *,
												   const VkAllocationCallbacks *, VkShaderModule *p)
{
	module_calls++;
	*p = module_result == VK_SUCCESS ? (VkShaderModule)uintptr_t(0x5678) : VK_NULL_HANDLE;
	return module_result;
}
static VKAPI_ATTR void VKAPI_CALL graph_destroy(VkDevice, VkPipeline pipeline, const VkAllocationCallbacks *)
{
	std::lock_guard<std::mutex> hold(edge_mutex);
	if (!live_pipelines.erase((uint64_t)pipeline))
		cleanup_errors++;
}
static void graph_dispatch()
{
	vkCreateComputePipelines = graph_compute;
	vkCreateGraphicsPipelines = graph_graphics;
	vkCreateRayTracingPipelinesKHR = graph_raytracing;
	vkCreateShaderModule = graph_module;
	vkDestroyPipeline = graph_destroy;
	graph_real_sampler = vkCreateSampler;
	vkCreateSampler = graph_sampler;
	graph_real_set = vkCreateDescriptorSetLayout;
	vkCreateDescriptorSetLayout = graph_set;
	graph_real_pass = vkCreateRenderPass;
	vkCreateRenderPass = graph_pass;
	if (non_lazy)
	{
		VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		VkPhysicalDeviceProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		VkPhysicalDeviceDynamicRenderingFeatures dynamic = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
		VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
		f.pNext = &dynamic;
		dynamic.pNext = &ray;
		dynamic.dynamicRendering = ray.rayTracingPipeline = VK_TRUE;
		p.properties.limits.maxComputeWorkGroupSize[0] = 8;
		p.properties.limits.maxComputeWorkGroupSize[1] = 8;
		p.properties.limits.maxComputeWorkGroupSize[2] = 8;
		p.properties.limits.maxComputeWorkGroupInvocations = 8;
		p.properties.limits.maxDescriptorSetSamplers = 64;
		p.properties.limits.maxBoundDescriptorSets = 4;
		active_replay->device->legacy_filter.init(VK_API_VERSION_1_2, nullptr, 0, &f, &p);
	}
}
static void connect_events(ThreadedReplayer &r, State &s, Stream &stream)
{
	s.begin(7, 3);
	char b[MaxFrame];
	frame(b, sizeof(b), "1 HELLO 7 3");
	stream.feed(s, b, strlen(b), [](const char *) {});
	r.event_worker = 7;
	r.event_attempt = 3;
	r.event_writer = [&s, &stream](const char *bytes)
	{
		stream.feed(s, bytes, strlen(bytes), [](const char *) {});
		return true;
	};
}
// Every test writes a real FOZ through StateRecorder, reads all resources back,
// and only then makes the specified missing/malformed-input mutation.
static bool graph(const char *path, const std::string &mode, unsigned n = 4, unsigned kinds = 7)
{
	BlobDatabase source;
	source.writable = true;
	{
		StateRecorder recorder;
		recorder.init_recording_synchronized(&source);
		VkShaderModuleCreateInfo module = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		module.pCode = valid_code;
		module.codeSize = sizeof(valid_code);
		for (unsigned m = 0; m < (mode == "bounded-origins" ? 70u : mode == "missing-multiple-modules" ? 2u : 1u); m++)
			checks.require(recorder.record_shader_module((VkShaderModule)uintptr_t(m + 1), module, 0x101 + m),
						   "M19/record-module");
		VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		checks.require(recorder.record_sampler((VkSampler)3, sampler, 0x110), "M19/record-sampler");
		VkSampler immutable = (VkSampler)3;
		VkDescriptorSetLayoutBinding binding = {};
		binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		binding.descriptorCount = 1;
		binding.pImmutableSamplers = &immutable;
		VkDescriptorSetLayoutCreateInfo set = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		set.bindingCount = 1;
		set.pBindings = &binding;
		checks.require(recorder.record_descriptor_set_layout((VkDescriptorSetLayout)4, set, 0x111), "M19/record-set");
		VkDescriptorSetLayout set_handle = (VkDescriptorSetLayout)4;
		VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		if (mode.find("static") != std::string::npos)
		{
			layout.setLayoutCount = 1;
			layout.pSetLayouts = &set_handle;
		}
		checks.require(recorder.record_pipeline_layout((VkPipelineLayout)2, layout, 0x102), "M19/record-layout");
		VkRenderPassCreateInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		pass.subpassCount = 1;
		pass.pSubpasses = &subpass;
		checks.require(recorder.record_render_pass((VkRenderPass)5, pass, 0x112), "M19/record-pass");
		for (unsigned k = 0; k < 3; k++)
			if (kinds & (1u << k))
			{
				VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
				stage.module = (VkShaderModule)1;
				stage.pName = "main";
				VkPipeline parent = (VkPipeline)uintptr_t(0x500 + k);
				bool parent_mode =
					mode.find("parent") != std::string::npos || mode.find("library") != std::string::npos;
				bool library = mode.find("library") != std::string::npos;
				bool nested = mode.find("nested") != std::string::npos;
				VkPipeline nested_parent = (VkPipeline)uintptr_t(0x600 + k);
				if (nested)
				{
					if (k == 1)
					{
						VkGraphicsPipelineCreateInfo g = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
						stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
						g.stageCount = 1;
						g.pStages = &stage;
						g.layout = (VkPipelineLayout)2;
						g.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
						checks.require(recorder.record_graphics_pipeline(nested_parent, g, nullptr, 0, 0xa00 + k),
									   "M19/record-nested-library");
					}
					else
					{
						VkRayTracingPipelineCreateInfoKHR r = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
						stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
						r.stageCount = 1;
						r.pStages = &stage;
						r.layout = (VkPipelineLayout)2;
						r.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
						checks.require(recorder.record_raytracing_pipeline(nested_parent, r, nullptr, 0, 0xa00 + k),
									   "M19/record-nested-library");
					}
				}
				for (unsigned i = 0; i < n + unsigned(parent_mode); i++)
				{
					bool is_parent = parent_mode && i == 0;
					if (mode == "bounded-origins")
						stage.module = (VkShaderModule)uintptr_t((i % 70) + 1);
					Hash hash = is_parent ? 0x900 + k : 0x2000 + k * 0x10000 + i;
					VkPipeline handle = is_parent ? parent : (VkPipeline)uintptr_t(0x1000 + k * 0x10000 + i);
					VkPipelineLibraryCreateInfoKHR lib = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
					lib.libraryCount = 1;
					lib.pLibraries = is_parent && nested ? &nested_parent : &parent;
					VkPipelineCreateFlags flags = is_parent ? (library ? VK_PIPELINE_CREATE_LIBRARY_BIT_KHR
																	   : VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT)
												  : parent_mode && !library ? VK_PIPELINE_CREATE_DERIVATIVE_BIT
																			: 0;
					if (k == 0)
					{
						VkComputePipelineCreateInfo c = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
						stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
						c.stage = stage;
						c.layout = (VkPipelineLayout)2;
						c.flags = flags;
						if (parent_mode && !is_parent && !library)
							c.basePipelineHandle = parent;
						if (parent_mode && (!is_parent || nested) && library)
							c.pNext = &lib;
						checks.require(recorder.record_compute_pipeline(handle, c, nullptr, 0, hash),
									   "M19/record-compute-consumer");
					}
					else if (k == 1)
					{
						VkGraphicsPipelineCreateInfo g = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
						stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
						g.stageCount = 1;
						g.pStages = &stage;
						g.layout = (VkPipelineLayout)2;

						VkPipelineShaderStageCreateInfo stages[2] = {stage, stage};
						if (mode == "missing-multiple-modules")
						{
							stages[1].module = (VkShaderModule)2;
							stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
							g.stageCount = 2;
							g.pStages = stages;
						}
						g.flags = flags;
						if (mode.find("static-pass") != std::string::npos)
							g.renderPass = (VkRenderPass)5;
						if (parent_mode && !is_parent && !library)
							g.basePipelineHandle = parent;
						if (parent_mode && (!is_parent || nested) && library)
							g.pNext = &lib;
						checks.require(recorder.record_graphics_pipeline(handle, g, nullptr, 0, hash),
									   "M19/record-graphics-consumer");
					}
					else
					{
						VkRayTracingPipelineCreateInfoKHR r = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
						stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
						r.stageCount = 1;
						r.pStages = &stage;
						r.layout = (VkPipelineLayout)2;
						r.flags = flags;
						if (parent_mode && !is_parent && !library)
							r.basePipelineHandle = parent;
						if (parent_mode && (!is_parent || nested) && library)
							r.pLibraryInfo = &lib;
						checks.require(recorder.record_raytracing_pipeline(handle, r, nullptr, 0, hash),
									   "M19/record-ray-consumer");
					}
				}
			}
		recorder.tear_down_recording_thread();
	}
	std::map<std::pair<ResourceTag, Hash>, std::string> expected;
	{
		std::unique_ptr<DatabaseInterface> db(create_database(path, DatabaseMode::OverWrite));
		if (!db || !db->prepare())
			return false;
		for (const auto &entry : source.blobs)
		{
			auto key = entry.first;
			auto bytes = entry.second;
			if (((mode == "missing-module" || mode == "missing-multiple-modules") &&
				 key.first == RESOURCE_SHADER_MODULE) ||
				((mode == "missing-layout" || mode == "missing-static-layout") && key.second == 0x102) ||
				(mode == "missing-static-set" && key.second == 0x111) ||
				(mode == "missing-static-sampler" && key.second == 0x110) ||
				(mode == "missing-static-pass" && key.second == 0x112) ||
				(mode.find("missing-parent") != std::string::npos && key.second >= 0x900 && key.second <= 0x902) ||
				((mode == "missing-library" && key.second >= 0x900 && key.second <= 0x902) ||
				 (mode == "missing-library-nested" && key.second >= 0xa00 && key.second <= 0xa02)))
				continue;
			if ((mode == "invalid-module" && key.first == RESOURCE_SHADER_MODULE) ||
				(mode == "invalid-layout" && key.second == 0x102))
				bytes = "{";
			expected[key] = bytes;
			if (!db->write_entry(key.first, key.second, bytes.data(), bytes.size(), 0))
				return false;
		}
	}
	std::unique_ptr<DatabaseInterface> db(create_database(path, DatabaseMode::ReadOnly));
	if (!db || !db->prepare())
		return false;
	unsigned count = 0;
	for (unsigned t = 0; t < RESOURCE_COUNT; t++)
	{
		size_t n = 0;
		if (!db->get_hash_list_for_resource_tag(ResourceTag(t), &n, nullptr))
			return false;
		std::vector<Hash> hashes(n);
		if (!db->get_hash_list_for_resource_tag(ResourceTag(t), &n, hashes.data()))
			return false;
		for (auto h : hashes)
		{
			size_t size = 0;
			if (!db->read_entry(ResourceTag(t), h, &size, nullptr, 0))
				return false;
			std::string bytes(size, '\0');
			if (!db->read_entry(ResourceTag(t), h, &size, &bytes[0], 0))
				return false;
			if (expected[{ResourceTag(t), h}] != bytes)
				return false;
			count++;
		}
	}
	checks.require(count == expected.size(), "M19/FOZ-exact-tag-hash-bytes-readback");
	return true;
}
static void run_graph(const std::string &mode, unsigned threads, unsigned n, unsigned kinds,
					  VkResult shader_result = VK_SUCCESS, Hash parent_failure = 0, bool fail_lazy_layout = false,
					  bool ordinary = false)
{
	std::string path = mode + "-" + std::to_string(threads) + ".foz";
	checks.require(graph(path.c_str(), mode, n, kinds), "M19/actual-FOZ-fixture");
	ThreadedReplayer::Options options;
	options.num_threads = threads;
	ThreadedReplayer replay(device_options(), options);
	State retained;
	Stream stream;
	connect_events(replay, retained, stream);
	active_replay = &replay;
	graph_mode = mode;
	live_pipelines.clear();
	lifetime_errors = cleanup_errors = 0;
	additional_dispatch = graph_dispatch;
	module_result = shader_result;
	fail_parent_hash = parent_failure;
	non_lazy = ordinary;
	static_failure_tag = mode == "static-sampler-create" ? RESOURCE_SAMPLER
						 : mode == "static-set-create"	 ? RESOURCE_DESCRIPTOR_SET_LAYOUT
						 : mode == "static-pass-create"	 ? RESOURCE_RENDER_PASS
														 : RESOURCE_COUNT;
	fail_layout = fail_lazy_layout;
	pipeline_calls = module_calls = 0;
	edges.clear();
	Global::worker_thread_index = 0;
	int status = run_normal_process(replay, {path.c_str()}, nullptr, 0, -1);
	replay.finish_events(status);
	stream.finish(retained);
	replay.free_pipelines();
	checks.require(lifetime_errors == 0 && cleanup_errors == 0 && live_pipelines.empty(),
				   "M19/library-parent-lifetime-derivative-status-transient-destruction-and-cleanup");
	additional_dispatch = nullptr;
	fail_layout = false;
	non_lazy = false;
	unsigned families = unsigned(bool(kinds & 1)) + unsigned(bool(kinds & 2)) + unsigned(bool(kinds & 4));
	bool blocked = mode.find("missing") != std::string::npos || mode.find("invalid") != std::string::npos ||
				   shader_result != VK_SUCCESS || parent_failure || fail_lazy_layout ||
				   static_failure_tag != RESOURCE_COUNT;
	checks.require(status == (blocked ? 1 : 0) && replay.work_drained, "M19/graph-aggregate-outcome-and-drain");
	checks.require(count(replay, ReplayFailure::Unsupported) == 0, "M19/dependency-never-device-unsupported");
	if (!blocked)
	{
		unsigned parents =
			mode.find("parent") != std::string::npos || mode.find("library") != std::string::npos ? families : 0;
		checks.require(pipeline_calls ==
							   n * families + parents * (mode.find("nested") != std::string::npos ? 2u : 1u) &&
						   edges.empty(),
					   "M19/positive-graph-creates-all-consumers");
		return;
	}
	ResourceTag expected_tag = RESOURCE_SHADER_MODULE;
	Hash expected_hash = 0x101;
	uint64_t category = 0;
	if (mode.find("layout") != std::string::npos)
	{
		expected_tag = RESOURCE_PIPELINE_LAYOUT;
		expected_hash = 0x102;
	}
	if (mode.find("static-set") != std::string::npos)
	{
		expected_tag = RESOURCE_PIPELINE_LAYOUT;
		expected_hash = 0x102;
		category = uint64_t(ReplayFailure::Dependency) + 1;
	}
	if (mode.find("static-sampler") != std::string::npos)
	{
		expected_tag = RESOURCE_PIPELINE_LAYOUT;
		expected_hash = 0x102;
		category = uint64_t(ReplayFailure::Dependency) + 1;
	}
	if (mode.find("static-pass") != std::string::npos)
	{
		expected_tag = RESOURCE_RENDER_PASS;
		expected_hash = 0x112;
	}
	if (static_failure_tag != RESOURCE_COUNT)
	{
		expected_tag = static_failure_tag;
		expected_hash = static_failure_tag == RESOURCE_SAMPLER				   ? 0x110
						: static_failure_tag == RESOURCE_DESCRIPTOR_SET_LAYOUT ? 0x111
																			   : 0x112;
		if (ordinary && static_failure_tag != RESOURCE_RENDER_PASS)
		{
			expected_tag = RESOURCE_PIPELINE_LAYOUT;
			expected_hash = 0x102;
			category = uint64_t(ReplayFailure::Dependency) + 1;
		}
	}
	if (mode.find("parent") != std::string::npos || mode.find("library") != std::string::npos)
	{
		expected_tag = kinds == 1	? RESOURCE_COMPUTE_PIPELINE
					   : kinds == 2 ? RESOURCE_GRAPHICS_PIPELINE
									: RESOURCE_RAYTRACING_PIPELINE;
		expected_hash = 0x900 + (kinds == 1 ? 0 : kinds == 2 ? 1 : 2);
	}
	if (mode == "invalid-module" || mode == "invalid-layout")
		category = uint64_t(ReplayFailure::Parse) + 1;
	if (shader_result != VK_SUCCESS || parent_failure || fail_lazy_layout ||
		(static_failure_tag != RESOURCE_COUNT && !ordinary) || (ordinary && static_failure_tag == RESOURCE_RENDER_PASS))
		category = uint64_t(ReplayFailure::Create) + 1;
	if (mode == "missing-module" || mode == "missing-multiple-modules" ||
		mode.find("missing-parent") != std::string::npos || mode.find("missing-library") != std::string::npos)
		category = uint64_t(ReplayFailure::Read) + 1;
	if (shader_result != VK_SUCCESS)
		checks.require(replay.outcome.snapshot()[unsigned(ReplayFailure::Create)].result == shader_result,
					   "M19/exact-signed-native-OOM-result");
	if (shader_result != VK_SUCCESS)
		checks.require(module_calls == (mode == "bounded-origins" ? 70u : 1u) &&
						   count(replay, ReplayFailure::Create) == (mode == "bounded-origins" ? 70u : 1u),
					   "M19/one-originating-module-create-failure");
	if (mode == "invalid-module")
		checks.require(module_calls == 0 && count(replay, ReplayFailure::Parse) == 1,
					   "M19/one-originating-module-parse-failure");
	if (parent_failure)
		checks.require(pipeline_calls == 1 && count(replay, ReplayFailure::Create) == 1,
					   "M19/one-originating-parent-create-failure");
	else
		checks.require(pipeline_calls == 0, "M19/blocked-consumers-zero-Vulkan-create-calls");
	unsigned extra = mode.find("nested") != std::string::npos							? 1
					 : mode == "missing-static-set"										? 1
					 : mode == "missing-static-sampler"									? 2
					 : ordinary && static_failure_tag == RESOURCE_SAMPLER				? 2
					 : ordinary && static_failure_tag == RESOURCE_DESCRIPTOR_SET_LAYOUT ? 1
																						: 0;
	checks.require(count(replay, ReplayFailure::Dependency) == n * families + extra &&
					   edges.size() == n * families + extra,
				   "M19/every-logical-consumer-blocked-once/" + mode + (ordinary ? "/ordinary" : "/lazy"));
	std::set<std::pair<uint64_t, uint64_t>> consumers;
	for (auto &e : edges)
	{
		if (e.tag != RESOURCE_COMPUTE_PIPELINE && e.tag != RESOURCE_GRAPHICS_PIPELINE &&
			e.tag != RESOURCE_RAYTRACING_PIPELINE)
			continue;

		if (mode.find("nested") != std::string::npos && e.hash == expected_hash)
		{
			checks.require(e.parent_tag == expected_tag && e.parent_hash == expected_hash + 0x100 &&
							   e.category == category,
						   "M19/nested-library-keeps-origin-parent-identity-and-category");
			continue;
		}
		checks.require(consumers.emplace(e.tag, e.hash).second, "M19/distinct-consumer-one-blocked-observation");
		checks.require(e.parent_tag == expected_tag &&
						   e.parent_hash ==
							   (mode == "bounded-origins" ? 0x101 + (e.hash - 0x2000) % 70 : expected_hash),
					   "M19/original-parent-tag-and-hash-before-remapping/" + mode + (ordinary ? "/ordinary" : "/lazy"));
		checks.require(e.omitted == (mode == "missing-multiple-modules" ? 1u : 0u),
					   "M19/first-blocker-and-explicit-secondary-reference-omission");
		checks.require(!category || e.category == (mode.find("nested") != std::string::npos
													   ? uint64_t(ReplayFailure::Dependency) + 1
													   : category),
					   "M19/known-parent-failure-category/" + mode + (ordinary ? "/ordinary" : "/lazy"));
		checks.require(e.status == uint64_t((mode.find("nested") != std::string::npos || category) &&
													(mode.find("nested") != std::string::npos ||
													 category != uint64_t(ReplayFailure::Read) + 1)
												? ReplayDependency::Status::Failed
												: ReplayDependency::Status::Missing),
					   "M19/missing-versus-failed-parent");
	}
	checks.require(consumers.size() == n * families, "M19/N-distinct-blocked-consumers");
	checks.require(retained.categories_known && retained.categories[unsigned(ReplayFailure::Dependency)] ==
													count(replay, ReplayFailure::Dependency),
				   "M19/bounded-detail-independent-attempt-counts");
	if (n * families > MaxEvents)
		checks.require(retained.count == MaxEvents && retained.omitted && retained.failed,
					   "M19/bounded-examples-explicit-omission");
}
static void module_fanout()
{
	for (unsigned threads : {1u, 4u})
	{
		run_graph("invalid-module", threads, 4, 7);
		run_graph("module-create", threads, 4, 7, VK_ERROR_OUT_OF_HOST_MEMORY);
		run_graph("good", threads, 4, 7);
	}
	run_graph("invalid-module", 4, 1030, 1);
}
static void parent_fanout()
{
	for (unsigned threads : {1u, 4u})
		for (unsigned kind : {1u, 2u})
		{
			Hash hash = 0x900 + (kind == 1 ? 0 : 1);
			run_graph("parent", threads, 4, kind, VK_SUCCESS, hash);
			run_graph("parent", threads, 4, kind);
			run_graph("missing-parent", threads, 4, kind);
		}
	for (unsigned kind : {2u, 4u})
	{
		Hash hash = 0x900 + (kind == 2 ? 1 : 2);
		run_graph("library", 4, 4, kind, VK_SUCCESS, hash);
		run_graph("library", 4, 4, kind);
		run_graph("missing-library", 4, 4, kind);
		run_graph("library-nested", 4, 4, kind, VK_SUCCESS, hash + 0x100);
		run_graph("library-nested", 4, 4, kind);
		run_graph("missing-library-nested", 4, 4, kind);
	}
}
static void recursive_read_controls()
{
	struct SelectiveRead : BlobDatabase
	{
		bool fail_payload = false;
		bool read_entry(ResourceTag tag, Hash hash, size_t *size, void *data, PayloadReadFlags flags) override
		{
			if (tag == RESOURCE_SHADER_MODULE && reject && (fail_payload ? data != nullptr : data == nullptr))
				return false;
			unsigned saved = reject;
			reject = 0;
			bool result = BlobDatabase::read_entry(tag, hash, size, data, flags);
			reject = saved;
			return result;
		}
	};
	struct RecursiveReplay : Replay
	{
		bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *info,
										  VkShaderModule *module) override
		{
			auto &thread = get_per_thread_data();
			auto tag = thread.expected_tag;
			auto expected = thread.expected_hash;
			thread.expected_tag = RESOURCE_SHADER_MODULE;
			thread.expected_hash = hash;
			bool result = Replay::enqueue_create_shader_module(hash, info, module);
			thread.expected_tag = tag;
			thread.expected_hash = expected;
			return result;
		}
	};
	for (unsigned scenario = 0; scenario < 3; scenario++)
	{
		checks.require(graph("recursive-read.foz", "good", 4, 1), "M19/record-recursive-input");
		std::unique_ptr<DatabaseInterface> input(create_database("recursive-read.foz", DatabaseMode::ReadOnly));
		checks.require(input && input->prepare(), "M19/open-recursive-input");
		SelectiveRead db;
		for (auto key : {std::make_pair(RESOURCE_SHADER_MODULE, Hash(0x101)),
						 std::make_pair(RESOURCE_COMPUTE_PIPELINE, Hash(0x2000)),
						 std::make_pair(RESOURCE_PIPELINE_LAYOUT, Hash(0x102))})
		{
			size_t size = 0;
			checks.require(input->read_entry(key.first, key.second, &size, nullptr, 0), "M19/recursive-payload-size");
			std::string bytes(size, '\0');
			checks.require(input->read_entry(key.first, key.second, &size, &bytes[0], 0),
						   "M19/recursive-payload-bytes");
			db.blobs[key] = bytes;
		}
		RecursiveReplay replay;
		StateReplayer parser;
		replay.global_database = &db;
		replay.deferred_compute[0].resize(1);
		replay.get_per_thread_data().expected_tag = RESOURCE_PIPELINE_LAYOUT;
		auto &layout = db.blobs[{RESOURCE_PIPELINE_LAYOUT, 0x102}];
		checks.require(parser.parse(replay, &db, layout.data(), layout.size()), "M19/recursive-layout-control");
		db.reject = scenario < 2 ? 1 : 0;
		db.fail_payload = scenario == 1;
		std::string good = db.blobs[{RESOURCE_SHADER_MODULE, 0x101}];
		if (scenario == 2)
			db.blobs[{RESOURCE_SHADER_MODULE, 0x101}] = "{";
		PipelineWorkItem item;
		item.tag = RESOURCE_COMPUTE_PIPELINE;
		item.hash = 0x2000;
		item.index = 0;
		std::vector<uint8_t> buffer;
		edges.clear();
		checks.require(!replay.run_parse_work_item(parser, buffer, item), "M19/recursive-parent-read-or-parse-failure");
		checks.require(edges.size() == 1 && edges[0].parent_tag == RESOURCE_SHADER_MODULE &&
						   edges[0].parent_hash == 0x101 &&
						   edges[0].status == uint64_t(scenario < 2 ? ReplayDependency::Status::ReadFailed
																	: ReplayDependency::Status::Failed) &&
						   edges[0].category == uint64_t(scenario < 2 ? ReplayFailure::Read : ReplayFailure::Parse) + 1,
					   "M19/read-failed-present-parent-versus-malformed-origin");
		checks.require(!count(replay, ReplayFailure::Unsupported) && !count(replay, ReplayFailure::Create) &&
						   count(replay, scenario < 2 ? ReplayFailure::Read : ReplayFailure::Parse) == 1 && !calls,
					   "M19/recursive-origin-and-blocked-no-native-attempt");
		db.reject = 0;
		db.blobs[{RESOURCE_SHADER_MODULE, 0x101}] = good;
		checks.require(replay.run_parse_work_item(parser, buffer, item) && calls == 1,
					   "M19/recursive-valid-control-after-every-failure");
	}
}

static void common_cache()
{
	{
		DiagnosticStderr capture;
		set_thread_log_level(LOG_WARNING);
		run_graph("invalid-layout", 2, 4, 7);
		set_thread_log_level(LOG_NONE);
		auto text = capture.finish();
		checks.contains(text, "Pipeline layout 0000000000000102, but its dependency could not be resolved.",
						"M19/present-malformed-layout-prose-and-source-hash");
		checks.require(text.find("common cache") == std::string::npos &&
						   text.find("does not exist") == std::string::npos,
					   "M19/present-malformed-layout-no-missing-input-advice");
	}
	run_graph("good", 2, 4, 7);
	recursive_read_controls();
	run_graph("missing-multiple-modules", 4, 4, 2);
	for (auto mode : {"missing-module", "missing-layout", "invalid-layout"})
		run_graph(mode, 2, 4, 7);
	run_graph("good", 2, 4, 7);
}
static void static_dependencies()
{
	{
		DiagnosticStderr capture;
		set_thread_log_level(LOG_WARNING);
		run_graph("static-layout-create", 4, 4, 1, VK_SUCCESS, 0, true, true);
		set_thread_log_level(LOG_NONE);
		auto text = capture.finish();
		checks.contains(text, "Pipeline layout 0000000000000102, but its dependency did not produce a usable handle",
						"M19/known-failed-layout-prose-and-source-hash");
		checks.require(text.find("common cache") == std::string::npos &&
						   text.find("does not exist") == std::string::npos,
					   "M19/known-failed-layout-no-missing-input-advice");
	}
	run_graph("good-static", 2, 4, 1, VK_SUCCESS, 0, false, true);
	for (auto mode : {"missing-static-layout", "missing-static-set", "missing-static-sampler"})
		run_graph(mode, 2, 4, 1);
	run_graph("missing-static-pass", 2, 4, 2);
	run_graph("static-layout-create", 4, 4, 7, VK_SUCCESS, 0, true);
	run_graph("good-static", 2, 4, 7);
	run_graph("good-static-pass", 2, 4, 2);
	for (bool ordinary : {false, true})
	{
		for (auto mode : {"static-sampler-create", "static-set-create"})
		{
			run_graph(mode, 4, 4, 1, VK_SUCCESS, 0, false, ordinary);
			run_graph("good-static", 2, 4, 1, VK_SUCCESS, 0, false, ordinary);
		}
		run_graph("static-pass-create", 4, 4, 2, VK_SUCCESS, 0, false, ordinary);
		run_graph("good-static-pass", 2, 4, 2, VK_SUCCESS, 0, false, ordinary);
	}
	run_graph("static-layout-create", 4, 4, 1, VK_SUCCESS, 0, true, true);
	run_graph("good-static", 2, 4, 1, VK_SUCCESS, 0, false, true);
}
static void categories()
{
	for (auto result : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY})
	{
		run_graph("module-create", 2, 4, 7, result);
		run_graph("good", 2, 4, 7);
	}
	run_graph("invalid-module", 2, 4, 7);
	run_graph("good", 2, 4, 7);
	parse_test();
	validation_test();
	support_test();
	{
		Replay r;
		non_lazy = true;
		auto &thread = r.get_per_thread_data();
		thread.expected_tag = RESOURCE_SHADER_MODULE;
		thread.expected_hash = 0x101;
		VkShaderModuleCreateInfo m = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		m.pCode = valid_code;
		m.codeSize = 3;
		VkShaderModule handle;
		r.enqueue_create_shader_module(0x101, &m, &handle);
		checks.require(count(r, ReplayFailure::Parse) == 1 && !count(r, ReplayFailure::Unsupported) &&
						   !count(r, ReplayFailure::Create) && !calls && !handle,
					   "M19/actual-malformed-module-producer-not-unsupported-or-native-create");
		thread.expected_hash = 0x102;
		m.codeSize = sizeof(valid_code);
		r.enqueue_create_shader_module(0x102, &m, &handle);
		checks.require(handle && calls == 1, "M19/actual-module-valid-after-malformed");
		non_lazy = false;
	}
}
static State decode_detail(const Event &event, size_t fragment)
{
	State state;
	Stream stream;
	state.begin(UINT64_MAX, UINT64_MAX);
	char bytes[MaxFrame];
	frame(bytes, sizeof(bytes), "1 HELLO 18446744073709551615 18446744073709551615");
	stream.feed(state, bytes, strlen(bytes), [](const char *) {});
	checks.require(event_frame(bytes, sizeof(bytes), event) > 0 && strlen(bytes) < MaxFrame,
				   "M19/bounded-detail-serialization");
	for (size_t offset = 0, length = strlen(bytes); offset < length; offset += fragment)
		stream.feed(state, bytes + offset, std::min(fragment, length - offset), [](const char *) {});
	frame(bytes, sizeof(bytes), "1 FINAL 1 0");
	stream.feed(state, bytes, strlen(bytes), [](const char *) {});
	stream.finish(state);
	return state;
}
static void detail_codec()
{
	Event event;
	event.phase = Support;
	event.reason = Unsupported;
	event.known = HasTag | HasHash | Worker | Attempt;
	event.tag = event.hash = event.worker = event.attempt = UINT64_MAX;
	event.support.kind = SupportKind::Property;
	event.support.known = 7;
	event.support.required = event.support.available = event.support.raw = UINT64_MAX;
	memset(event.support.identifier, 'x', sizeof(event.support.identifier) - 1);
	for (size_t fragment : {size_t(1), size_t(3), MaxFrame})
	{
		auto state = decode_detail(event, fragment);
		checks.require(state.count == 1 && !state.lost && !state.unsupported && state.final && state.failed,
					   "M19/detail-max-values-fragmentation-and-final");
		if (state.count)
			checks.require(!strcmp(state.events[0].support.identifier, event.support.identifier) &&
							   state.events[0].support.required == UINT64_MAX &&
							   state.events[0].support.raw == UINT64_MAX,
						   "M19/detail-owned-identifier-and-values-roundtrip");
	}
	event.support.kind = SupportKind(999);
	auto unknown = decode_detail(event, 1);
	checks.require(unknown.count == 1 && unknown.unsupported && !unknown.successful(),
				   "M19/unknown-kind-retained-not-success");
	event.phase = Dependency;
	event.reason = Blocked;
	event.support = {};
	event.parent.tag = event.parent.hash = UINT64_MAX;
	event.parent.status = ReplayDependency::Status::Failed;
	event.parent.category = 3;
	event.parent.omitted = UINT64_MAX;
	auto parent = decode_detail(event, 1);
	checks.require(parent.count == 1 && parent.events[0].parent.hash == UINT64_MAX &&
					   parent.events[0].parent.category == 3 && parent.events[0].parent.omitted == UINT64_MAX &&
					   !parent.lost,
				   "M19/dependency-max-identity-roundtrip");
	char valid_frame[MaxFrame];
	event_frame(valid_frame, sizeof(valid_frame), event);
	std::vector<std::string> bad = {"FZ18 3:abc\n", "FZ18 10:1 DETAIL 9\n", std::string(MaxFrame + 100, 'x') + "\n",
									std::string(valid_frame, strlen(valid_frame) - 3) + "\n"};
	for (const auto &bytes : bad)
	{
		State state;
		Stream stream;
		state.begin(UINT64_MAX, UINT64_MAX);
		char hello[MaxFrame];
		frame(hello, sizeof(hello), "1 HELLO 18446744073709551615 18446744073709551615");
		stream.feed(state, hello, strlen(hello), [](const char *) {});
		stream.feed(state, bytes.data(), bytes.size(), [](const char *) {});
		stream.feed(state, valid_frame, strlen(valid_frame), [](const char *) {});
		stream.finish(state);
		checks.require(state.count == 1 && (state.lost || state.unsupported) && !state.successful(),
					   "M19/malformed-detail-bounded-recovery-next-valid-frame");
	}
	State truncated;
	Stream unfinished;
	truncated.begin(1, 1);
	unfinished.feed(truncated, valid_frame, strlen(valid_frame) - 1, [](const char *) {});
	unfinished.finish(truncated);
	checks.require(truncated.lost && !truncated.final && !truncated.successful(),
				   "M19/truncated-detail-EOF-never-completion");
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	set_thread_log_level(LOG_NONE);
	std::string c = argv[1];
	if (c == "module-fanout")
		module_fanout();
	else if (c == "parent-fanout")
		parent_fanout();
	else if (c == "common-cache")
		common_cache();
	else if (c == "static")
		static_dependencies();
	else if (c == "categories")
		categories();
	else if (c == "bounded")
	{
		detail_codec();
		run_graph("bounded-origins", 4, 1100, 1, VK_ERROR_OUT_OF_HOST_MEMORY);
	}
	else
		return 2;
	printf("M19 dependency %s failures=%d\n", c.c_str(), checks.failures);
	return checks.failures ? 1 : 0;
}
