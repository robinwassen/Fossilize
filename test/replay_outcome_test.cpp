// Private adapters replace only Vulkan producers. Outcome, parsing, scheduling,
// worker threads and normal CLI entry are the shipping implementation.
#include "diagnostic_test.hpp"
#include "volk.h"
#include "device.hpp"
#include <atomic>
#include <map>
#include <memory>
#include <condition_variable>
static void install_dispatch();
static bool fail_device, non_lazy, marker_mode, physical_adapter;
static bool observed_null_device;
class OutcomeDevice : public Fossilize::VulkanDevice
{
public:
	Fossilize::FeatureFilter legacy_filter;
	Fossilize::FeatureFilter &get_feature_filter()
	{ return non_lazy ? legacy_filter : VulkanDevice::get_feature_filter(); }
	bool init_device(const Options &opts)
	{
		if (fail_device) return false;
		observed_null_device = opts.null_device;
		auto adapted = opts;
		if (physical_adapter) adapted.null_device = true; // CPU-only provenance adapter.
		bool ret = VulkanDevice::init_device(adapted);
		if (ret)
		{
			VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			VkPhysicalDeviceProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
			legacy_filter.init(VK_API_VERSION_1_2, nullptr, 0, &f, &p);
			install_dispatch();
		}
		return ret;
	}
};
#define VulkanDevice OutcomeDevice
#define main fossilize_replay_original_main
#define NO_ROBUST_REPLAYER 1
#include "../cli/fossilize_replay.cpp"
#undef main
#undef VulkanDevice

static DiagnosticChecks checks;
static std::mutex dispatch_lock;
static std::vector<VkResult> results;
static std::atomic<unsigned> calls{0}, destroys{0};
static bool inject_validation, fail_layout;
static ThreadedReplayer *validation_target;
static VkResult next_result()
{
	std::lock_guard<std::mutex> holder(dispatch_lock);
	unsigned call = calls++;
	if (inject_validation) on_validation_error(validation_target);
	return results.empty() ? VK_SUCCESS : results[std::min<size_t>(call, results.size() - 1)];
}
static std::vector<VkPipelineCreateFlags> observed_pipeline_flags;
static VkResult pipeline_result(VkPipeline *p, VkPipelineCreateFlags flags)
{
	if (marker_mode) observed_pipeline_flags.push_back(flags);
	auto result = next_result();
	*p = result == VK_SUCCESS ? (VkPipeline)uintptr_t(0x1234) : VK_NULL_HANDLE;
	return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL graphics(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *, VkPipeline *p) { return pipeline_result(p, i->flags); }
static VKAPI_ATTR VkResult VKAPI_CALL compute(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo *i, const VkAllocationCallbacks *, VkPipeline *p) { return pipeline_result(p, i->flags); }
static VKAPI_ATTR VkResult VKAPI_CALL raytracing(VkDevice, VkDeferredOperationKHR, VkPipelineCache, uint32_t, const VkRayTracingPipelineCreateInfoKHR *i, const VkAllocationCallbacks *, VkPipeline *p) { return pipeline_result(p, i->flags); }
static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice, VkPipeline, const VkAllocationCallbacks *) { destroys++; }
static VKAPI_ATTR VkResult VKAPI_CALL shader(VkDevice, const VkShaderModuleCreateInfo *, const VkAllocationCallbacks *, VkShaderModule *p)
{
	auto r = marker_mode ? VK_SUCCESS : next_result(); *p = r == VK_SUCCESS ? (VkShaderModule)uintptr_t(0x5678) : VK_NULL_HANDLE; return r;
}
static VKAPI_ATTR void VKAPI_CALL destroy_shader(VkDevice, VkShaderModule, const VkAllocationCallbacks *) { destroys++; }
static PFN_vkCreatePipelineLayout original_layout;
static VKAPI_ATTR VkResult VKAPI_CALL layout(VkDevice d, const VkPipelineLayoutCreateInfo *i, const VkAllocationCallbacks *a, VkPipelineLayout *p)
{
	if (!fail_layout) return original_layout(d, i, a, p);
	*p = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void install_dispatch()
{
	vkCreateGraphicsPipelines = graphics; vkCreateComputePipelines = compute;
	vkCreateRayTracingPipelinesKHR = raytracing; vkDestroyPipeline = destroy;
	vkCreateShaderModule = shader; vkDestroyShaderModule = destroy_shader;
	original_layout = vkCreatePipelineLayout; vkCreatePipelineLayout = layout;
}
// Direct initial-object creation without maintenance4 deferral uses the same
// callbacks with a real limited feature filter and controlled Vulkan returns.
#define INITIAL_STUB(Name, Info, Handle) \
static PFN_vkCreate##Name saved_##Name; \
static VKAPI_ATTR VkResult VKAPI_CALL initial_##Name(VkDevice d, const Info *i, const VkAllocationCallbacks *a, Handle *p) \
{ auto result = next_result(); *p = VK_NULL_HANDLE; return result == VK_SUCCESS ? saved_##Name(d,i,a,p) : result; }
INITIAL_STUB(Sampler, VkSamplerCreateInfo, VkSampler)
INITIAL_STUB(DescriptorSetLayout, VkDescriptorSetLayoutCreateInfo, VkDescriptorSetLayout)
INITIAL_STUB(PipelineLayout, VkPipelineLayoutCreateInfo, VkPipelineLayout)
INITIAL_STUB(RenderPass, VkRenderPassCreateInfo, VkRenderPass)
INITIAL_STUB(RenderPass2KHR, VkRenderPassCreateInfo2, VkRenderPass)
#undef INITIAL_STUB
static const uint32_t valid_code[] = {
	0x07230203,0x00010000,0,5,0, 0x00020011,1, 0x0003000e,0,1,
	0x0005000f,5,1,0x6e69616d,0, 0x00060010,1,17,1,1,1,
	0x00020013,2, 0x00030021,3,2, 0x00050036,2,1,0,3,
	0x000200f8,4, 0x000100fd, 0x00010038
};
static VulkanDevice::Options device_options() { VulkanDevice::Options o; o.null_device = true; return o; }
struct Replay : ThreadedReplayer
{
	static Options options(unsigned threads, SharedControlBlock *progress)
	{ Options o; o.num_threads = threads; o.control_block = progress; return o; }
	Replay(unsigned threads = 0, SharedControlBlock *progress = nullptr) : ThreadedReplayer(device_options(), options(threads, progress))
	{
		Global::worker_thread_index = 0;
		non_lazy = false;
		device.reset(new OutcomeDevice);
		checks.require(device->init_device(device_options()), "unit/null-device");
		results.clear(); calls = destroys = 0; inject_validation = fail_layout = false;
		validation_target = this;
	}
};
static uint64_t count(const ThreadedReplayer &r, ReplayFailure f) { return r.outcome.snapshot()[unsigned(f)].count; }
static void expect(const ThreadedReplayer &r, ReplayFailure reason, ResourceTag tag, Hash hash, VkResult result = VK_SUCCESS)
{
	auto snapshot = r.outcome.snapshot(); const auto &e = snapshot[unsigned(reason)];
	checks.require(e.count > 0 && e.tag == tag && e.hash == hash && e.result == result, "reason/exact-evidence/" + std::to_string(unsigned(reason)));
	checks.require(r.outcome.exit_status() == EXIT_FAILURE, "reason/nonzero-independent-of-progress");
}
struct BlobDatabase : DatabaseInterface
{
	std::map<std::pair<ResourceTag, Hash>, std::string> blobs;
	unsigned reject = 0, writes = 0;
	bool writable = false;
	BlobDatabase() : DatabaseInterface(DatabaseMode::ReadOnly) {}
	bool prepare() override { return true; }
	bool read_entry(ResourceTag t, Hash h, size_t *size, void *data, PayloadReadFlags) override
	{
		auto i = blobs.find({t,h});
		if (i == blobs.end() || (!data && reject == 1) || (data && reject == 2)) return false;
		if (data) memcpy(data, i->second.data(), i->second.size());
		*size = i->second.size(); return true;
	}
	bool write_entry(ResourceTag tag, Hash hash, const void *data, size_t size, PayloadWriteFlags) override
	{
		if (!writable) return false;
		writes++; blobs[{tag,hash}] = size ? std::string(static_cast<const char *>(data),size) : std::string(); return true;
	}
	bool has_entry(ResourceTag t, Hash h) override { return blobs.count({t,h}) != 0; }
	bool get_hash_list_for_resource_tag(ResourceTag t, size_t *n, Hash *hashes) override
	{ size_t n2 = 0; for (auto &b : blobs) if (b.first.first == t) { if (hashes) hashes[n2] = b.first.second; n2++; } *n = n2; return true; }
	void flush() override {}
	const char *get_db_path_for_hash(ResourceTag, Hash) override { return "private-memory-fixture"; }
};
static std::string shader_json(Hash h = 0x101, unsigned flags = 0)
{
	// Serialize through the actual recorder, not an assumed JSON dialect.
	StateRecorder recorder;
	VkShaderModuleCreateInfo m = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; m.codeSize = sizeof(valid_code); m.pCode = valid_code; m.flags = flags;
	checks.require(recorder.record_shader_module((VkShaderModule)1, m, h), "serialize/module");
	size_t size = 0; uint8_t *data = nullptr;
	checks.require(recorder.serialize(&data, &size), "serialize/blob");
	std::string text(reinterpret_cast<char *>(data), size); recorder.free_serialized(data); return text;
}
static PipelineWorkItem parse_item(Hash h, unsigned memory = 0)
{ PipelineWorkItem i; i.tag = RESOURCE_SHADER_MODULE; i.hash = h; i.parse_only = true; i.memory_context_index = memory; return i; }
static void state_test()
{
	for (unsigned progress = 0; progress < 2; progress++) for (unsigned reverse = 0; reverse < 2; reverse++)
	{
		SharedControlBlock shared = {}; Replay r(0, progress ? &shared : nullptr);
		checks.require(r.outcome.exit_status() == 0, "state/clean");
		std::mutex lock; std::condition_variable cv; unsigned ready = 0, done = 0; bool go = false;
		std::atomic<unsigned> wrong_observations{0};
		std::vector<std::thread> producers;
		for (unsigned p = 0; p < 16; p++) producers.emplace_back([&, p]() {
			bool failing = (p % 2) == 0;
			bool first = failing == (reverse == 0);
			{ std::unique_lock<std::mutex> hold(lock); ready++; cv.notify_all(); cv.wait(hold, [&]() { return go && (first || done >= 8); }); }
			for (unsigned n = 0; n < 1000; n++) if (failing)
				r.outcome.fail(ReplayFailure(p % unsigned(ReplayFailure::Count)), RESOURCE_COMPUTE_PIPELINE, p + 1, VK_ERROR_DEVICE_LOST);
			else if (r.outcome.exit_status() != (reverse == 0 ? 1 : 0)) wrong_observations++;
			{ std::lock_guard<std::mutex> hold(lock); done++; cv.notify_all(); }
		});
		{ std::unique_lock<std::mutex> hold(lock); cv.wait(hold, [&]() { return ready == 16; }); go = true; cv.notify_all(); }
		for (auto &p : producers) p.join();
		unsigned total = 0; auto snapshot = r.outcome.snapshot();
		for (unsigned reason = 0; reason < snapshot.size(); reason++)
		{
			unsigned expected = 0;
			for (unsigned p = 0; p < 16; p++) if ((p % 2) == 0 && p % snapshot.size() == reason) expected += 1000;
			checks.require(snapshot[reason].count == expected, "state/exact-per-reason-count");
			if (expected) checks.require(snapshot[reason].tag == RESOURCE_COMPUTE_PIPELINE && snapshot[reason].result == VK_ERROR_DEVICE_LOST,
			                            "state/retained-reason-identity-and-result");
			total += unsigned(snapshot[reason].count);
		}
		checks.require(total == 8000 && r.outcome.exit_status() == 1 && wrong_observations == 0, "state/no-lost-concurrent-observations-and-deterministic-orders");
	}
}
static void parse_test()
{
	const std::string good = shader_json();
	std::vector<std::string> payloads = {"{", "{\"version\":999}", shader_json(0x102), "{\"version\":6}", good};
	for (unsigned scenario = 0; scenario < 8; scenario++)
	{
		Replay r; BlobDatabase db; r.global_database = &db;
		if (scenario != 0) db.blobs[{RESOURCE_SHADER_MODULE,0x101}] = scenario < 3 ? good : payloads[scenario - 3];
		if (scenario == 1) db.reject = 1;
		if (scenario == 2) db.reject = 2;
		StateReplayer parser; std::vector<uint8_t> buffer;
		bool ok = r.run_parse_work_item(parser, buffer, parse_item(0x101));
		checks.require(ok == (scenario == 7), "parse/return/" + std::to_string(scenario));
		if (scenario < 3) expect(r, ReplayFailure::Read, RESOURCE_SHADER_MODULE, 0x101);
		else if (scenario < 7) expect(r, ReplayFailure::Parse, RESOURCE_SHADER_MODULE, 0x101);
		else checks.require(r.outcome.exit_status() == 0 && calls == 1, "parse/success-created-module");
	}
	Replay r; BlobDatabase db; r.global_database = &db;
	db.blobs[{RESOURCE_COMPUTE_PIPELINE,0x101}] = good;
	StateReplayer parser; std::vector<uint8_t> buffer; auto item = parse_item(0x101); item.tag = RESOURCE_COMPUTE_PIPELINE;
	checks.require(!r.run_parse_work_item(parser,buffer,item), "parse/mismatched-tag-rejected");
	expect(r,ReplayFailure::Parse,RESOURCE_COMPUTE_PIPELINE,0x101);
	Replay rejected; BlobDatabase unsupported; rejected.global_database = &unsupported;
	unsupported.blobs[{RESOURCE_SHADER_MODULE,0x101}] = shader_json(0x101,1);
	StateReplayer unsupported_parser; buffer.clear();
	checks.require(!rejected.run_parse_work_item(unsupported_parser,buffer,parse_item(0x101)) || !rejected.outcome.succeeded(), "parse/known-support-rejection-retained");
	expect(rejected,ReplayFailure::Unsupported,RESOURCE_SHADER_MODULE,0x101);
	checks.require(!count(rejected,ReplayFailure::Parse) && !count(rejected,ReplayFailure::Create), "parse/known-support-cause-not-generic-parse-or-create");
}
struct Pipeline
{
	VkGraphicsPipelineCreateInfo g = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	VkComputePipelineCreateInfo c = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	VkRayTracingPipelineCreateInfoKHR r = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
	VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	VkPipeline output = VK_NULL_HANDLE, entry = VK_NULL_HANDLE;
	PipelineWorkItem item;
	Pipeline(unsigned kind, Hash h = 0xabc)
	{
		stage.module = (VkShaderModule)uintptr_t(0x5678); stage.pName = "main"; stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		g.stageCount = r.stageCount = 1; g.pStages = r.pStages = &stage; c.stage = stage;
		item.hash = h; item.tag = kind == 0 ? RESOURCE_GRAPHICS_PIPELINE : kind == 1 ? RESOURCE_COMPUTE_PIPELINE : RESOURCE_RAYTRACING_PIPELINE;
		item.output.pipeline = &output; item.hash_map_entry.pipeline = &entry;
		if (kind == 0) item.create_info.graphics_create_info = &g;
		if (kind == 1) item.create_info.compute_create_info = &c;
		if (kind == 2) item.create_info.raytracing_create_info = &r;
	}
	void flags(VkPipelineCreateFlags f) { g.flags = c.flags = r.flags = f; }
	void run(ThreadedReplayer &replay, unsigned kind)
	{ if (kind == 0) replay.run_creation_work_item_graphics(item); if (kind == 1) replay.run_creation_work_item_compute(item); if (kind == 2) replay.run_creation_work_item_raytracing(item); }
};
static void creation_test(bool loops)
{
	for (unsigned kind = 0; kind < 3; kind++) for (unsigned scenario = 0; scenario < (loops ? 6u : 3u); scenario++)
	{
		Replay replay; Pipeline p(kind);
		if (!loops) results = {scenario == 0 ? VK_ERROR_OUT_OF_HOST_MEMORY : scenario == 1 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS};
		else
		{
			p.flags(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
			results = scenario == 0 ? std::vector<VkResult>{VK_PIPELINE_COMPILE_REQUIRED,VK_SUCCESS} :
			          scenario == 1 ? std::vector<VkResult>{VK_PIPELINE_COMPILE_REQUIRED,VK_ERROR_DEVICE_LOST} :
			          scenario == 2 ? std::vector<VkResult>{VK_PIPELINE_COMPILE_REQUIRED} :
			          scenario == 3 ? std::vector<VkResult>{VK_SUCCESS,VK_SUCCESS} :
			          scenario == 4 ? std::vector<VkResult>{VK_ERROR_DEVICE_LOST,VK_SUCCESS} : std::vector<VkResult>{VK_SUCCESS,VK_ERROR_OUT_OF_HOST_MEMORY};
			replay.opts.skip_pipelines_on_compile_required = scenario == 2;
			if (scenario >= 3) replay.loop_count = 2;
		}
		p.run(replay, kind);
		bool success = loops ? (scenario == 0 || scenario == 3) : scenario == 2;
		checks.require(replay.outcome.succeeded() == success && calls == results.size(), "creation/exact-final-outcome-and-calls");
		checks.require(!p.output && !p.entry, "creation/null-output-and-cleanup");
		unsigned successes = 0; for (auto result : results) if (result == VK_SUCCESS) successes++;
		checks.require(destroys == successes, "creation/destroy-each-success-once");
		unsigned successful_iterations = kind == 0 ? replay.graphics_pipeline_count.load() : kind == 1 ? replay.compute_pipeline_count.load() : replay.raytracing_pipeline_count.load();
		unsigned compile_required = kind == 0 ? replay.graphics_pipeline_compile_required_count.load() : kind == 1 ? replay.compute_pipeline_compile_required_count.load() : replay.raytracing_pipeline_compile_required_count.load();
		checks.require(successful_iterations == successes && compile_required == (loops && scenario < 3 ? 1u : 0u), "creation/iteration-and-compile-required-counters");
		if (!success) expect(replay, ReplayFailure::Create, p.item.tag, p.item.hash, loops ? (scenario == 1 || scenario == 4 ? VK_ERROR_DEVICE_LOST : scenario == 2 ? VK_PIPELINE_COMPILE_REQUIRED : VK_ERROR_OUT_OF_HOST_MEMORY) : results.front());
	}
	if (!loops) for (unsigned kind : {0u,2u})
	{
		Replay replay; Pipeline p(kind); p.flags(VK_PIPELINE_CREATE_LIBRARY_BIT_KHR);
		p.run(replay,kind);
		checks.require(replay.outcome.succeeded() && p.entry == (VkPipeline)uintptr_t(0x1234) && p.output == p.entry && destroys == 0,
		               "creation/dependency-success-retains-output-until-owner-cleanup");
		destroy(replay.device->get_device(),p.entry,nullptr);
		checks.require(destroys == 1,"creation/retained-library-cleanup");
	}
	if (!loops) for (unsigned kind = 0; kind < 3; kind++) for (unsigned scenario = 0; scenario < 3; scenario++)
	{
		Replay replay; Pipeline p(kind); PipelineResolver resolver;
		auto *whitelist = new BlobDatabase; whitelist->writable = true;
		auto *cache = new BlobDatabase; cache->writable = true;
		replay.validation_whitelist_db.reset(whitelist); replay.replayer_cache_db.reset(cache);
		results = {scenario == 0 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS}; inject_validation = scenario == 1;
		replay.run_creation_work_item(resolver,p.item); inject_validation = false;
		checks.require(whitelist->writes == (scenario == 2 ? 1u : 0u) && cache->writes == (scenario == 2 ? 1u : 0u),
		               "creation/failed-or-invalid-work-never-marks-reusable-state");
	}
	if (!loops) for (auto result : {VK_ERROR_OUT_OF_HOST_MEMORY,VK_ERROR_DEVICE_LOST,VK_SUCCESS})
	{
		Replay replay; auto *whitelist = new BlobDatabase; whitelist->writable = true; replay.validation_whitelist_db.reset(whitelist);
		results = {result}; auto &d = replay.get_per_thread_data(); d.expected_tag = RESOURCE_SHADER_MODULE; d.expected_hash = 0x101;
		VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.pCode = valid_code; info.codeSize = sizeof(valid_code);
		VkShaderModule module;
		checks.require(replay.enqueue_create_shader_module(0x101, &info, &module), "shader/callback-consumed");
		checks.require(replay.outcome.succeeded() == (result == VK_SUCCESS), "shader/result-not-callback-success");
		checks.require(whitelist->writes == (result == VK_SUCCESS ? 1u : 0u), "shader/creation-failure-never-whitelisted");
		if (result != VK_SUCCESS) { expect(replay, ReplayFailure::Create, RESOURCE_SHADER_MODULE, 0x101, result); checks.require(!module, "shader/null-error-output"); }
	}
}
static void initial_creation_test()
{
	for (unsigned kind = 0; kind < 5; kind++) for (auto result : {VK_ERROR_OUT_OF_HOST_MEMORY,VK_ERROR_DEVICE_LOST,VK_SUCCESS})
	{
		Replay replay; non_lazy = true; results = {result};
		auto tag = kind == 0 ? RESOURCE_SAMPLER : kind == 1 ? RESOURCE_DESCRIPTOR_SET_LAYOUT : kind == 2 ? RESOURCE_PIPELINE_LAYOUT : RESOURCE_RENDER_PASS;
		replay.get_per_thread_data().expected_tag = tag;
		bool accepted = false; bool output_present = false;
		if (kind == 0) { saved_Sampler = vkCreateSampler; vkCreateSampler = initial_Sampler; VkSamplerCreateInfo i = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; VkSampler p; accepted = replay.enqueue_create_sampler(0x777,&i,&p); output_present = p != VK_NULL_HANDLE; }
		if (kind == 1) { saved_DescriptorSetLayout = vkCreateDescriptorSetLayout; vkCreateDescriptorSetLayout = initial_DescriptorSetLayout; VkDescriptorSetLayoutCreateInfo i = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; VkDescriptorSetLayout p; accepted = replay.enqueue_create_descriptor_set_layout(0x777,&i,&p); output_present = p != VK_NULL_HANDLE; }
		if (kind == 2) { saved_PipelineLayout = vkCreatePipelineLayout; vkCreatePipelineLayout = initial_PipelineLayout; VkPipelineLayoutCreateInfo i = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; VkPipelineLayout p; accepted = replay.enqueue_create_pipeline_layout(0x777,&i,&p); output_present = p != VK_NULL_HANDLE; }
		if (kind == 3) { saved_RenderPass = vkCreateRenderPass; vkCreateRenderPass = initial_RenderPass; VkRenderPassCreateInfo i = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; VkRenderPass p; accepted = replay.enqueue_create_render_pass(0x777,&i,&p); output_present = p != VK_NULL_HANDLE; }
		if (kind == 4) { saved_RenderPass2KHR = vkCreateRenderPass2KHR; vkCreateRenderPass2KHR = initial_RenderPass2KHR; VkRenderPassCreateInfo2 i = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2}; VkRenderPass p; accepted = replay.enqueue_create_render_pass2(0x777,&i,&p); output_present = p != VK_NULL_HANDLE; }
		checks.require(accepted == (result == VK_SUCCESS) && output_present == accepted && calls == 1, "initial/direct-creation-status-output-and-dispatch");
		checks.require(replay.outcome.succeeded() == accepted,"initial/retained-aggregate");
		if (!accepted) expect(replay,ReplayFailure::Create,tag,0x777,result);
	}
	non_lazy = false;
}
static void support_test()
{
	for (unsigned kind = 0; kind < 3; kind++) for (unsigned reject = 0; reject < 2; reject++)
	{
		Replay replay; Pipeline p(kind); if (reject) p.flags(0x80000000u);
		PipelineResolver resolver; replay.run_creation_work_item(resolver, p.item);
		checks.require(replay.outcome.succeeded() == !reject && calls == (reject ? 0u : 1u), "support/pipeline-real-filter");
		if (reject) { expect(replay, ReplayFailure::Unsupported, p.item.tag, p.item.hash); checks.require(!count(replay, ReplayFailure::Validation), "support/not-validation"); checks.require(!count(replay,ReplayFailure::Parse) && !count(replay,ReplayFailure::Create),"support/no-spurious-parse-or-create"); }
	}
	for (unsigned reject = 0; reject < 2; reject++)
	{
		Replay replay; VkShaderModuleCreateInfo i = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; i.flags = reject; i.pCode = valid_code; i.codeSize = sizeof(valid_code);
		auto &d = replay.get_per_thread_data(); d.expected_tag = RESOURCE_SHADER_MODULE; d.expected_hash = 0x101;
		VkShaderModule module; replay.enqueue_create_shader_module(0x101, &i, &module);
		checks.require(replay.outcome.succeeded() == !reject, "support/shader-real-filter");
		if (reject) expect(replay, ReplayFailure::Unsupported, RESOURCE_SHADER_MODULE, 0x101);
	}
	Replay replay; replay.get_per_thread_data().expected_tag = RESOURCE_RENDER_PASS;
	VkRenderPassCreateInfo i = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; i.flags = 0x80000000u;
	VkRenderPass pass;
	checks.require(!replay.enqueue_create_render_pass(0x777, &i, &pass) && !pass, "support/main-thread-initial-object");
	expect(replay, ReplayFailure::Unsupported, RESOURCE_RENDER_PASS, 0x777);
}
static void validation_test()
{
#ifdef FOSSILIZE_REPLAYER_SPIRV_VAL
	for (unsigned valid = 0; valid < 2; valid++)
	{
		Replay replay; replay.opts.spirv_validate = true;
		auto &d = replay.get_per_thread_data(); d.expected_tag = RESOURCE_SHADER_MODULE; d.expected_hash = 0x101;
		VkShaderModuleCreateInfo i = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; i.pCode = valid_code; i.codeSize = valid ? sizeof(valid_code) : 20;
		VkShaderModule module; replay.enqueue_create_shader_module(0x101, &i, &module);
		checks.require(replay.outcome.succeeded() == bool(valid) && calls == valid, "validation/real-SPIRV-Tools-rejection-and-control");
		if (!valid) { expect(replay, ReplayFailure::Validation, RESOURCE_SHADER_MODULE, 0x101); checks.require(!module, "validation/null-module"); }
	}
#else
	checks.require(false, "validation/must-compile-SPIRV-Tools");
#endif
	for (unsigned kind = 0; kind < 4; kind++)
	{
		Replay replay; inject_validation = true;
		if (kind < 3) { Pipeline p(kind); PipelineResolver resolver; replay.run_creation_work_item(resolver, p.item); expect(replay, ReplayFailure::Validation, p.item.tag, p.item.hash); }
		else
		{
			auto &d = replay.get_per_thread_data(); d.expected_tag = RESOURCE_SHADER_MODULE; d.expected_hash = 0x101;
			VkShaderModuleCreateInfo i = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; i.pCode = valid_code; i.codeSize = sizeof(valid_code);
			VkShaderModule module; replay.enqueue_create_shader_module(0x101, &i, &module);
			expect(replay, ReplayFailure::Validation, RESOURCE_SHADER_MODULE, 0x101);
		}
		checks.require(!count(replay, ReplayFailure::Create), "validation/Vulkan-success-still-validation-failure");
		checks.require(!count(replay,ReplayFailure::Parse),"validation/no-spurious-parse");
		inject_validation = false;
	}
}
static void setup_test()
{
	for (unsigned kind = 0; kind < 3; kind++) for (unsigned scenario = 0; scenario < 6; scenario++)
	{
		Replay replay; Pipeline p(kind); PipelineResolver resolver;
		if (scenario == 0) p.item.create_info.graphics_create_info = nullptr;
		if (scenario == 1) p.flags(VK_PIPELINE_CREATE_DERIVATIVE_BIT);
		if (scenario == 2 || scenario == 3)
		{
			if (scenario == 2) { p.stage.module = VK_NULL_HANDLE; p.c.stage.module = VK_NULL_HANDLE; }
			VkPipeline missing = VK_NULL_HANDLE;
			VkPipelineLibraryCreateInfoKHR lib = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR}; lib.libraryCount = 1; lib.pLibraries = &missing;
			if (scenario == 3) { p.g.pNext = p.c.pNext = &lib; p.r.pLibraryInfo = &lib; }
			if (kind == 0) replay.enqueue_pipeline(p.item.hash, &p.g, &p.output, 0, 0);
			if (kind == 1) replay.enqueue_pipeline(p.item.hash, &p.c, &p.output, 0, 0);
			if (kind == 2) replay.enqueue_pipeline(p.item.hash, &p.r, &p.output, 0, 0);
			p.item = replay.pipeline_work_queue.front(); replay.pipeline_work_queue.pop();
		}
		if (scenario == 4) p.g.sType = p.c.sType = p.r.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		replay.run_creation_work_item(resolver, p.item);
		checks.require(replay.outcome.succeeded() == (scenario == 5) && calls == (scenario == 5 ? 1u : 0u), "setup/blocked-never-calls-Vulkan");
		if (scenario < 5) { expect(replay, scenario == 0 || scenario == 4 ? ReplayFailure::Setup : ReplayFailure::Dependency, p.item.tag, p.item.hash); checks.require(!count(replay,ReplayFailure::Parse) && !count(replay,ReplayFailure::Create), "setup/no-spurious-parse-or-compiler-failure"); }
	}
	Replay replay; replay.get_per_thread_data().expected_tag = RESOURCE_RENDER_PASS;
	VkRenderPass pass; checks.require(!replay.enqueue_create_render_pass(0x777, nullptr, &pass), "setup/null-initial-object");
	expect(replay, ReplayFailure::Setup, RESOURCE_RENDER_PASS, 0x777);
	fail_device = true;
	ThreadedReplayer setup(device_options(),Replay::options(0,nullptr));
	setup.get_per_thread_data().expected_tag = RESOURCE_APPLICATION_INFO;
	setup.set_application_info(0,nullptr,nullptr);
	expect(setup,ReplayFailure::Setup,RESOURCE_COUNT,0);
	checks.require(!setup.device_ready && !setup.work_drained,"setup/device-failure-has-unknown-item-and-incomplete-work");
	fail_device = false;
}
static void queue_test()
{
	for (unsigned threads : {1u,4u}) for (unsigned progress = 0; progress < 2; progress++) for (unsigned failed = 0; failed < 2; failed++)
	{
		SharedControlBlock shared = {}; Replay replay(threads, progress ? &shared : nullptr);
		BlobDatabase db; StateReplayer global; replay.global_database = &db; replay.global_replayer = &global;
		for (unsigned context = 0; context < NUM_MEMORY_CONTEXTS; context++)
		{
			Hash h = 0x101 + context * 2;
			db.blobs[{RESOURCE_SHADER_MODULE,h}] = shader_json(h);
			db.blobs[{RESOURCE_SHADER_MODULE,h+1}] = failed ? "{\"version\":999}" : shader_json(h+1);
		}
		replay.start_worker_threads();
		std::vector<std::unique_ptr<Pipeline>> items;
		for (unsigned context = 0; context < NUM_MEMORY_CONTEXTS; context++)
		{
			replay.enqueue_work_item(parse_item(0x101 + context * 2, context)); replay.enqueue_work_item(parse_item(0x102 + context * 2, context));
			items.emplace_back(new Pipeline(1, 0xabc + context)); auto &p = *items.back(); p.item.memory_context_index = context;
			if (failed) p.flags(0x80000000u);
			replay.enqueue_work_item(p.item);
		}
		replay.sync_worker_threads(); replay.tear_down_threads();
		for (unsigned context = 0; context < NUM_MEMORY_CONTEXTS; context++) checks.require(replay.queued_count[context] == 3 && replay.completed_count[context] == 3, "queue/exact-drained-context-count");
		checks.require(replay.outcome.succeeded() == !failed, "queue/completion-is-not-success");
		if (failed) checks.require(count(replay, ReplayFailure::Parse) == NUM_MEMORY_CONTEXTS && count(replay, ReplayFailure::Unsupported) == NUM_MEMORY_CONTEXTS, "queue/exact-retained-reasons");
	}
}
static bool fixture(const std::string &mode, const char *path)
{
	if (mode == "emptyhash") return fixture("empty",path);
	if (mode == "compatibility" || mode == "selection" || mode == "hashselection" || mode == "whitelist") return fixture("mixed",path);
	int previous_failures = checks.failures;
	std::string intermediate = std::string(path) + ".source.foz";
	{
		std::unique_ptr<DatabaseInterface> db(create_database(intermediate.c_str(), DatabaseMode::OverWrite));
		if (!db || !db->prepare()) return false;
		StateRecorder recorder; recorder.init_recording_synchronized(db.get());
		VkShaderModuleCreateInfo shader_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		shader_info.pCode = valid_code; shader_info.codeSize = mode == "validation" ? 20 : sizeof(valid_code);
		if (mode == "unsupported-shader") shader_info.flags = 1;
		checks.require(recorder.record_shader_module((VkShaderModule)1, shader_info, 0x101), "fixture/shader");
		VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		checks.require(recorder.record_pipeline_layout((VkPipelineLayout)2, layout_info, 0x102), "fixture/layout");
		for (unsigned n = 0; n < 2; n++)
		{
			VkComputePipelineCreateInfo c = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			c.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; c.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			c.stage.module = (VkShaderModule)1; c.stage.pName = "main"; c.layout = (VkPipelineLayout)2;
			if (mode == "unsupported" && n == 0) c.flags = 0x10000000u;
			checks.require(recorder.record_compute_pipeline((VkPipeline)uintptr_t(10 + n), c, nullptr, 0, 0x201 + n), "fixture/compute");
		}
		if (mode == "library")
		{
			VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; stage.stage = VK_SHADER_STAGE_VERTEX_BIT; stage.module = (VkShaderModule)1; stage.pName = "main";
			VkGraphicsPipelineCreateInfo g = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; g.stageCount = 1; g.pStages = &stage; g.layout = (VkPipelineLayout)2; g.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
			checks.require(recorder.record_graphics_pipeline((VkPipeline)30, g, nullptr, 0, 0x301), "fixture/library-parent");
			VkPipeline parent = (VkPipeline)30;
			VkPipelineLibraryCreateInfoKHR lib = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR}; lib.libraryCount = 1; lib.pLibraries = &parent;
			g.flags = 0; g.pNext = &lib;
			checks.require(recorder.record_graphics_pipeline((VkPipeline)31, g, nullptr, 0, 0x302), "fixture/library-child");
		}
		recorder.tear_down_recording_thread();
	}
	std::map<std::pair<ResourceTag, Hash>, std::vector<uint8_t>> expected;
	{
		std::unique_ptr<DatabaseInterface> source(create_database(intermediate.c_str(), DatabaseMode::ReadOnly));
		std::unique_ptr<DatabaseInterface> dest(create_database(path, DatabaseMode::OverWrite));
		if (!source || !source->prepare() || !dest || !dest->prepare()) return false;
		if (mode != "empty" && mode != "e7") for (unsigned tag = 0; tag < RESOURCE_COUNT; tag++)
		{
			size_t n = 0; if (!source->get_hash_list_for_resource_tag(ResourceTag(tag), &n, nullptr)) return false;
			std::vector<Hash> hashes(n); if (!source->get_hash_list_for_resource_tag(ResourceTag(tag), &n, hashes.data())) return false;
			for (auto hash : hashes)
			{
				if ((mode == "missing" && tag == RESOURCE_SHADER_MODULE) || (mode == "library" && hash == 0x301)) continue;
				size_t size = 0; if (!source->read_entry(ResourceTag(tag), hash, &size, nullptr, 0)) return false;
				auto &bytes = expected[{ResourceTag(tag),hash}]; bytes.resize(size);
				if (!source->read_entry(ResourceTag(tag), hash, &size, bytes.data(), 0)) return false;
				if (mode == "unsupported-shader" && tag == RESOURCE_SHADER_MODULE) { auto text = shader_json(hash,1); bytes.assign(text.begin(),text.end()); }
				if (!dest->write_entry(ResourceTag(tag), hash, bytes.data(), bytes.size(), 0)) return false;
			}
		}
		if (mode == "mixed" || mode == "e7")
		{
			std::string bad = "{\"version\":999}"; auto &bytes = expected[{RESOURCE_COMPUTE_PIPELINE,0x203}]; bytes.assign(bad.begin(),bad.end());
			if (!dest->write_entry(RESOURCE_COMPUTE_PIPELINE,0x203,bytes.data(),bytes.size(),0)) return false;
		}
	}
	std::unique_ptr<DatabaseInterface> db(create_database(path, DatabaseMode::ReadOnly));
	if (!db || !db->prepare()) return false;
	unsigned entries = 0;
	for (unsigned tag = 0; tag < RESOURCE_COUNT; tag++)
	{
		size_t n = 0; if (!db->get_hash_list_for_resource_tag(ResourceTag(tag), &n, nullptr)) return false;
		std::vector<Hash> hashes(n); if (!db->get_hash_list_for_resource_tag(ResourceTag(tag), &n, hashes.data())) return false;
		for (auto hash : hashes)
		{
			size_t size = 0; auto i = expected.find({ResourceTag(tag),hash}); if (i == expected.end() || !db->read_entry(ResourceTag(tag),hash,&size,nullptr,0) || size != i->second.size()) return false;
			std::vector<uint8_t> bytes(size); if (!db->read_entry(ResourceTag(tag),hash,&size,bytes.data(),0) || bytes != i->second) return false;
			entries++;
		}
	}
	checks.require(entries == expected.size(), "fixture/exact-all-tag-hash-payload-readback");
	return checks.failures == previous_failures;
}
static void entry_test(const std::string &scenario, const char *path)
{
	checks.require(fixture("good", path), "entry/known-good-fixture");
	fail_device = scenario == "device";
	fail_layout = scenario == "layout";
	results = scenario == "create" ? std::vector<VkResult>{VK_ERROR_DEVICE_LOST} : std::vector<VkResult>{};
	calls = destroys = 0;
	std::vector<char *> arguments;
	for (const char *arg : {"private-replay", "--null-device", "--num-threads", "2", path}) arguments.push_back(const_cast<char *>(arg));
	int result = fossilize_replay_original_main(int(arguments.size()), arguments.data());
	checks.require(result == (scenario == "good" ? 0 : 1), "entry/actual-main-status");
	fail_device = fail_layout = false;
}
static void normal_matrix()
{
	std::vector<const char *> modes = {"good","mixed","missing","unsupported","unsupported-shader","library"};
#ifdef FOSSILIZE_REPLAYER_SPIRV_VAL
	modes.push_back("validation");
#endif
	for (const char *mode : modes)
	{
		std::string path = std::string(mode) + ".foz"; checks.require(fixture(mode,path.c_str()), "matrix/fixture");
		for (unsigned threads : {1u,4u}) for (unsigned progress = 0; progress < 2; progress++)
		{
			SharedControlBlock shared = {}; ThreadedReplayer::Options opts; opts.num_threads = threads; opts.control_block = progress ? &shared : nullptr;
			opts.spirv_validate = std::string(mode) == "validation";
			Global::worker_thread_index = 0;
			ThreadedReplayer replay(device_options(),opts); results.clear(); inject_validation = fail_layout = false;
			int status = run_normal_process(replay,{path.c_str()},nullptr,0,-1);
			bool good = std::string(mode) == "good";
			unsigned expected_mask = good ? 0u : std::string(mode) == "mixed" ? 2u : std::string(mode) == "unsupported" ? 8u :
			                         std::string(mode) == "unsupported-shader" ? 72u : std::string(mode) == "validation" ? 80u : 65u;
			checks.require(replay.outcome.failure_mask() == expected_mask,"matrix/exact-logical-category-set");
			checks.require(status == (good ? 0 : 1) && replay.work_drained, "matrix/normal-status-and-all-phases-drained");
			for (unsigned context = 0; context < NUM_MEMORY_CONTEXTS; context++) checks.require(replay.queued_count[context] == replay.completed_count[context], "matrix/every-submitted-job-completed");
			checks.require(replay.queued_count[0] == (std::string(mode) == "mixed" ? 5u : 4u) + (std::string(mode) == "library" ? 1u : 0u), "matrix/exact-primary-queue-count");
			if (std::string(mode) == "mixed") expect(replay,ReplayFailure::Parse,RESOURCE_COMPUTE_PIPELINE,0x203);
			if (std::string(mode) == "missing") { expect(replay,ReplayFailure::Read,RESOURCE_SHADER_MODULE,0x101); expect(replay,ReplayFailure::Dependency,RESOURCE_COMPUTE_PIPELINE,0x201); }
			if (std::string(mode) == "unsupported") expect(replay,ReplayFailure::Unsupported,RESOURCE_COMPUTE_PIPELINE,0x201);
			if (std::string(mode) == "unsupported-shader") { expect(replay,ReplayFailure::Unsupported,RESOURCE_SHADER_MODULE,0x101); checks.require(!count(replay,ReplayFailure::Parse) && !count(replay,ReplayFailure::Create),"matrix/known-shader-support-cause-only"); }
			if (std::string(mode) == "validation") { expect(replay,ReplayFailure::Validation,RESOURCE_SHADER_MODULE,0x101); expect(replay,ReplayFailure::Dependency,RESOURCE_COMPUTE_PIPELINE,0x201); checks.require(!count(replay,ReplayFailure::Parse) && !count(replay,ReplayFailure::Create),"matrix/validation-blocked-work-not-parse-or-create"); }
			if (std::string(mode) == "library") { checks.require(count(replay,ReplayFailure::Read)>0 && count(replay,ReplayFailure::Dependency)>0, "matrix/missing-out-of-range-library-retained"); }
		}
	}
}
#include "replay_cache_tests.hpp"

int main(int argc, char **argv)
{
	if (argc == 3 && std::string(argv[1]) == "--whitelist")
	{
		std::unique_ptr<DatabaseInterface> db(create_database(argv[2],DatabaseMode::OverWrite));
		return db && db->prepare() && db->write_entry(RESOURCE_COMPUTE_PIPELINE,0x201,nullptr,0,0) ? 0 : 1;
	}
	if (argc == 4 && std::string(argv[1]) == "--fixture") return fixture(argv[2],argv[3]) ? 0 : 1;
	if (argc == 4 && std::string(argv[1]) == "--entry") { entry_test(argv[2],argv[3]); return checks.failures ? 1 : 0; }
	if (argc != 2) return 2;
	std::string test = argv[1];
	if (test.compare(0,6,"cache-") == 0) cache_test(test.substr(6));
	else if (test == "state") state_test();
	else if (test == "parse-read") parse_test();
	else if (test == "creation") { creation_test(false); initial_creation_test(); }
	else if (test == "compile-required-loop") creation_test(true);
	else if (test == "support") support_test();
	else if (test == "validation") validation_test();
	else if (test == "setup-dependency") setup_test();
	else if (test == "worker-queue") queue_test();
	else if (test == "matrix") normal_matrix();
	else return 2;
	printf("Executed R14/%s; failures=%d\n",test.c_str(),checks.failures);
	return checks.failures ? 1 : 0;
}
