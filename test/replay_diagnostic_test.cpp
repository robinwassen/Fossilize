// Exercise production replay branches, rather than a duplicate formatter.
#include "diagnostic_test.hpp"
#include <fstream>
#include <cerrno>
#include <vector>
static FILE *diagnostic_open(const char *, const char *);
static size_t diagnostic_write(const void *, size_t, size_t, FILE *);
static int diagnostic_close(FILE *);
#define fopen diagnostic_open
#define fwrite diagnostic_write
#define fclose diagnostic_close
#define main fossilize_replay_original_main
#define NO_ROBUST_REPLAYER 1
#include "../cli/fossilize_replay.cpp"
#undef main
#undef fopen
#undef fwrite
#undef fclose

static std::vector<VkResult> results;
static unsigned calls, destroys, cache_destroys, opens, writes, closes;
static std::string order;
static int io_mode, io_error;
static std::string bytes;
static VkResult next_result(VkPipeline *pipeline)
{
	VkResult result = results.at(calls++);
	*pipeline = result == VK_SUCCESS ? (VkPipeline)0x1234 : VK_NULL_HANDLE;
	errno = ERANGE;
	return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL graphics(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *p) { return next_result(p); }
static VKAPI_ATTR VkResult VKAPI_CALL compute(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *p) { return next_result(p); }
static VKAPI_ATTR VkResult VKAPI_CALL raytracing(VkDevice, VkDeferredOperationKHR, VkPipelineCache, uint32_t, const VkRayTracingPipelineCreateInfoKHR *, const VkAllocationCallbacks *, VkPipeline *p) { return next_result(p); }
static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice, VkPipeline, const VkAllocationCallbacks *) { destroys++; errno = EDOM; }
static FILE *diagnostic_open(const char *path, const char *mode)
{
	if (io_mode < 0) return fopen(path, mode);
	opens++; order += "open ";
	if (io_mode == 1) { errno = io_error; return nullptr; }
	return tmpfile();
}
static size_t diagnostic_write(const void *data, size_t size, size_t count, FILE *file)
{
	if (io_mode < 0) return fwrite(data, size, count, file);
	writes++; order += "write ";
	if (io_mode == 2) { errno = io_error; return count ? count - 1 : 0; }
	bytes.assign(static_cast<const char *>(data), size * count);
	return fwrite(data, size, count, file);
}
static int diagnostic_close(FILE *file)
{
	if (io_mode < 0) return fclose(file);
	closes++; order += "close ";
	int result = fclose(file);
	// Deliberately overwrite a prior fwrite errno, including on successful close.
	errno = io_mode == 3 ? io_error : EDOM;
	return io_mode == 3 ? EOF : result;
}
static VKAPI_ATTR VkResult VKAPI_CALL cache_data(VkDevice, VkPipelineCache, size_t *size, void *data)
{
	static const char content[] = "cache-bytes";
	if (data) memcpy(data, content, sizeof(content));
	*size = sizeof(content);
	return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL validation_data(VkDevice d, VkValidationCacheEXT c, size_t *s, void *p) { return cache_data(d, (VkPipelineCache)c, s, p); }
static VKAPI_ATTR void VKAPI_CALL destroy_cache(VkDevice, VkPipelineCache, const VkAllocationCallbacks *) { cache_destroys++; order += "destroy "; errno = ERANGE; }
static VKAPI_ATTR void VKAPI_CALL destroy_validation(VkDevice d, VkValidationCacheEXT c, const VkAllocationCallbacks *a) { destroy_cache(d, (VkPipelineCache)c, a); }

int main()
{
	DiagnosticChecks checks;
	VulkanDevice::Options device_opts;
	device_opts.null_device = true;
	ThreadedReplayer::Options opts;
	opts.num_threads = 0;
	ThreadedReplayer replay(device_opts, opts);
	replay.device.reset(new VulkanDevice);
	checks.require(replay.device->init_device(device_opts), "null-device-init");
	vkCreateGraphicsPipelines = graphics;
	vkCreateComputePipelines = compute;
	vkCreateRayTracingPipelinesKHR = raytracing;
	vkDestroyPipeline = destroy;
	io_mode = -1;
	const char *kinds[] = { "graphics", "compute", "raytracing" };
	for (unsigned kind = 0; kind < 3; kind++)
	{
		for (unsigned scenario = 0; scenario < 7; scenario++)
		{
			std::string id = std::string(kinds[kind]) + "/" + std::to_string(scenario);
			results = scenario == 0 ? std::vector<VkResult>{VK_ERROR_OUT_OF_HOST_MEMORY} :
			          scenario == 1 ? std::vector<VkResult>{VK_ERROR_DEVICE_LOST} :
			          scenario == 2 ? std::vector<VkResult>{VK_SUCCESS} :
			          scenario == 3 ? std::vector<VkResult>{VK_PIPELINE_COMPILE_REQUIRED} :
			          scenario == 4 ? std::vector<VkResult>{VK_PIPELINE_COMPILE_REQUIRED, VK_SUCCESS} :
			          scenario == 5 ? std::vector<VkResult>{VK_PIPELINE_COMPILE_REQUIRED, VK_ERROR_DEVICE_LOST} :
			                          std::vector<VkResult>{VK_SUCCESS};
			replay.opts.skip_pipelines_on_compile_required = scenario == 3;
			calls = destroys = 0;
			VkPipeline output = VK_NULL_HANDLE, entry = (VkPipeline)0x9876;
			PipelineWorkItem item;
			item.hash = 0xfedcba9876543210ull + kind;
			item.output.pipeline = &output;
			item.hash_map_entry.pipeline = &entry;
			VkGraphicsPipelineCreateInfo g = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
			VkComputePipelineCreateInfo c = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			VkRayTracingPipelineCreateInfoKHR r = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
			if (scenario >= 3) { g.flags = c.flags = r.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT; }
			if (scenario == 6) { g.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR; r.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR; }
			DiagnosticStderr capture;
			if (kind == 0) { item.tag = RESOURCE_GRAPHICS_PIPELINE; item.create_info.graphics_create_info = &g; replay.run_creation_work_item_graphics_iteration(item, VK_NULL_HANDLE, true); }
			if (kind == 1) { item.tag = RESOURCE_COMPUTE_PIPELINE; item.create_info.compute_create_info = &c; replay.run_creation_work_item_compute_iteration(item, VK_NULL_HANDLE, true); }
			if (kind == 2) { item.tag = RESOURCE_RAYTRACING_PIPELINE; item.create_info.raytracing_create_info = &r; replay.run_creation_work_item_raytracing_iteration(item, VK_NULL_HANDLE, true); }
			std::string log = capture.finish();
			// Cleanup after the branch also clobbers unrelated OS error state.
			errno = ENOSPC;
			checks.require(calls == results.size(), id + "/calls");
			bool dependency = scenario == 6 && kind != 1;
			checks.require(entry == (dependency ? (VkPipeline)0x1234 : VK_NULL_HANDLE) && output == (dependency ? (VkPipeline)0x1234 : VK_NULL_HANDLE), id + "/outputs");
			bool success = results.back() == VK_SUCCESS;
			checks.require(destroys == (success && !dependency ? 2u : 1u), id + "/destroy-count");
			if (dependency) destroy(replay.device->get_device(), output, nullptr);
			checks.require(log.find("crash") == std::string::npos, id + "/no-crash-guess");
			char hash[32]; snprintf(hash, sizeof(hash), "0x%016" PRIx64, item.hash);
			if (scenario == 2 || scenario == 6) checks.require(log.empty(), id + "/success-quiet");
			else
			{
				checks.contains(log, std::string("Replay/create ") + kinds[kind] + " pipeline", id + "/phase-kind");
				checks.contains(log, hash, id + "/hash");
				if (!success) checks.contains(log, "VkResult=" + std::to_string(int(results.back())), id + "/raw-result");
				if (scenario >= 3) checks.contains(log, "VkResult=" + std::to_string(int(VK_PIPELINE_COMPILE_REQUIRED)), id + "/compile-required");
				checks.require((log.find("pipeline failed") == std::string::npos) == success, id + "/final-outcome");
			}
		}
	}
	checks.require(replay.graphics_pipeline_count == 3 && replay.compute_pipeline_count == 3 && replay.raytracing_pipeline_count == 3, "success-counters");
	checks.require(replay.graphics_pipeline_compile_required_count == 3 && replay.compute_pipeline_compile_required_count == 3 && replay.raytracing_pipeline_compile_required_count == 3, "compile-required-counters");
	vkGetPipelineCacheData = cache_data;
	vkGetValidationCacheDataEXT = validation_data;
	vkDestroyPipelineCache = destroy_cache;
	vkDestroyValidationCacheEXT = destroy_validation;
	for (unsigned kind = 0; kind < 2; kind++)
	{
		for (int mode = 0; mode <= 3; mode++)
		{
			for (int error : {EACCES, ENOSPC, 0})
			{
				io_mode = mode; io_error = error;
				opens = writes = closes = cache_destroys = 0; order.clear(); bytes.clear();
				replay.opts.on_disk_pipeline_cache_path = "injected-cache";
				replay.opts.on_disk_validation_cache_path = "injected-cache";
				const char *kind_name = kind ? "validation" : "pipeline";
				std::string id = std::string(kind_name) + "/io-" + std::to_string(mode) + "/" + std::to_string(error);
				DiagnosticStderr capture;
				if (kind) { replay.validation_cache = (VkValidationCacheEXT)0x456; replay.flush_validation_cache(); }
				else { replay.disk_pipeline_cache = (VkPipelineCache)0x456; replay.flush_pipeline_cache(); }
				std::string log = capture.finish();
				checks.require(opens == 1 && writes == (mode == 1 ? 0u : 1u) && closes == (mode == 1 ? 0u : 1u) && cache_destroys == 1, id + "/cleanup-counts");
				checks.require(order == (mode == 1 ? "open destroy " : "open write close destroy "), id + "/cleanup-order");
				checks.require(!replay.disk_pipeline_cache && !replay.validation_cache, id + "/cache-cleared");
				if (mode == 0) { checks.require(log.empty(), id + "/quiet"); checks.require(bytes == std::string("cache-bytes", 12), id + "/exact-bytes"); }
				else
				{
					checks.contains(log, std::string("Replay/export ") + kind_name + " cache failed", id + "/phase-kind");
					checks.contains(log, std::string("operation=") + (mode == 1 ? "open" : mode == 2 ? "write" : "close"), id + "/operation");
					checks.contains(log, error ? "errno=" + std::to_string(error) : "errno=unknown", id + "/originating-errno");
					checks.contains(log, "hash=not-applicable", id + "/identity");
				}
			}
		}
	}
	io_mode = -1;
	DiagnosticStderr missing;
	ThreadedReplayer::write_cache_data("missing-parent-diagnostic-test/no-cache", "pipeline", "x", 1);
	checks.contains(missing.finish(), "operation=open, errno=" + std::to_string(ENOENT), "real-missing-parent");
	fprintf(stdout, "Executed VK-PIPELINES:21 OS-CACHE:24+missing-parent; failures=%d\n", checks.failures);
	return checks.failures ? 1 : 0;
}
