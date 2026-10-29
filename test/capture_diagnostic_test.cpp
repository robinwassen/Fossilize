// Fake loader driving the exported layer C ABI. This caller can be built once
// against baseline headers, then run unchanged against another layer library.
#define VK_NO_PROTOTYPES
#include "vulkan/vulkan.h"
#include "vulkan/vk_layer.h"
#include "diagnostic_test.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <vector>
static struct Handle { void *dispatch; } instance_handle, device_handle;
static VkInstance instance = reinterpret_cast<VkInstance>(&instance_handle);
static VkPhysicalDevice gpu = reinterpret_cast<VkPhysicalDevice>(&instance_handle);
static VkDevice device = reinterpret_cast<VkDevice>(&device_handle);
static unsigned calls, destroys;
static bool qa, fail_vulkan;
static std::vector<VkPipeline> destroyed;
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL next_instance(VkInstance, const char *);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL next_device(VkDevice, const char *);
static VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *p) { *p = instance; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *p) { *p = device; return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance, const VkAllocationCallbacks *) {}
static VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice, const VkAllocationCallbacks *) {}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties *p) { memset(p, 0, sizeof(*p)); p->apiVersion = VK_API_VERSION_1_3; strcpy(p->deviceName, "fake-layer-gpu"); }
static VkResult create_pipeline(uint32_t count, VkPipeline *p)
{
	calls++;
	VkResult result = fail_vulkan ? VK_ERROR_DEVICE_LOST : qa && calls == 1 ? VK_PIPELINE_COMPILE_REQUIRED : VK_SUCCESS;
	for (uint32_t i = 0; i < count; i++) p[i] = result == VK_SUCCESS ? (VkPipeline)(uintptr_t(0x1234 + i)) : VK_NULL_HANDLE;
	return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL graphics(VkDevice, VkPipelineCache, uint32_t n, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *p) { return create_pipeline(n, p); }
static VKAPI_ATTR VkResult VKAPI_CALL compute(VkDevice, VkPipelineCache, uint32_t n, const VkComputePipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *p) { return create_pipeline(n, p); }
static VKAPI_ATTR VkResult VKAPI_CALL raytracing(VkDevice, VkDeferredOperationKHR, VkPipelineCache, uint32_t n, const VkRayTracingPipelineCreateInfoKHR *, const VkAllocationCallbacks *, VkPipeline *p) { return create_pipeline(n, p); }
static VKAPI_ATTR void VKAPI_CALL destroy_pipeline(VkDevice, VkPipeline p, const VkAllocationCallbacks *) { destroys++; destroyed.push_back(p); }
static VKAPI_ATTR VkResult VKAPI_CALL binaries(VkDevice, const VkPipelineBinaryCreateInfoKHR *, const VkAllocationCallbacks *, VkPipelineBinaryHandlesInfoKHR *) { calls++; return fail_vulkan ? VK_ERROR_DEVICE_LOST : VK_SUCCESS; }
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL next_instance(VkInstance, const char *name)
{
	if (!strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(create_instance);
	if (!strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(create_device);
	if (!strcmp(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_instance);
	if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return reinterpret_cast<PFN_vkVoidFunction>(properties);
	return nullptr;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL next_device(VkDevice, const char *name)
{
	if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(next_device);
	if (!strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_device);
	if (!strcmp(name, "vkCreateGraphicsPipelines")) return reinterpret_cast<PFN_vkVoidFunction>(graphics);
	if (!strcmp(name, "vkCreateComputePipelines")) return reinterpret_cast<PFN_vkVoidFunction>(compute);
	if (!strcmp(name, "vkCreateRayTracingPipelinesKHR")) return reinterpret_cast<PFN_vkVoidFunction>(raytracing);
	if (!strcmp(name, "vkDestroyPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_pipeline);
	if (!strcmp(name, "vkCreatePipelineBinariesKHR")) return reinterpret_cast<PFN_vkVoidFunction>(binaries);
	return nullptr;
}
static void environment(const char *key, const char *value)
{
#ifdef _WIN32
	_putenv_s(key, value);
#else
	setenv(key, value, 1);
#endif
}
int main(int argc, char **argv)
{
	if (argc != 5) return 2;
	DiagnosticChecks checks;
	const std::string mode = argv[2], kind = argv[3], outcome = argv[4];
	bool rejection = outcome == "reject";
	fail_vulkan = outcome == "vulkan-error";
	qa = mode == "qa";
	environment("FOSSILIZE_DUMP_SYNC", mode == "eager" ? "1" : "0");
	environment("FOSSILIZE_DUMP_SIGSEGV", mode == "paranoid" ? "1" : "0");
	environment("FOSSILIZE_PRECOMPILE_QA", qa ? "1" : "0");
	environment("FOSSILIZE_DUMP_PATH", "capture-fixture");
#ifdef _WIN32
	_putenv_s("FOSSILIZE_APPLICATION_INFO_FILTER_PATH", "");
#else
	unsetenv("FOSSILIZE_APPLICATION_INFO_FILTER_PATH");
#endif
	instance_handle.dispatch = &instance_handle;
	device_handle.dispatch = &device_handle;
#ifdef _WIN32
	HMODULE library = LoadLibraryA(argv[1]);
	auto get_instance = library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "VK_LAYER_fossilize_GetInstanceProcAddr")) : nullptr;
	auto get_device = library ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(library, "VK_LAYER_fossilize_GetDeviceProcAddr")) : nullptr;
#else
	void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	auto get_instance = library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "VK_LAYER_fossilize_GetInstanceProcAddr")) : nullptr;
	auto get_device = library ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(library, "VK_LAYER_fossilize_GetDeviceProcAddr")) : nullptr;
#endif
	checks.require(get_instance && get_device, "layer-export-lookup");
	if (!get_instance || !get_device) return 1;
	VkLayerInstanceLink instance_link = {};
	instance_link.pfnNextGetInstanceProcAddr = next_instance;
	VkLayerInstanceCreateInfo instance_chain = {};
	instance_chain.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
	instance_chain.function = VK_LAYER_LINK_INFO;
	instance_chain.u.pLayerInfo = &instance_link;
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	instance_info.pNext = &instance_chain; instance_info.pApplicationInfo = &app;
	auto create_i = reinterpret_cast<PFN_vkCreateInstance>(get_instance(VK_NULL_HANDLE, "vkCreateInstance"));
	checks.require(create_i && create_i(&instance_info, nullptr, &instance) == VK_SUCCESS, "layer-instance-create");
	VkLayerDeviceLink device_link = {};
	device_link.pfnNextGetInstanceProcAddr = next_instance; device_link.pfnNextGetDeviceProcAddr = next_device;
	VkLayerDeviceCreateInfo device_chain = {};
	device_chain.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO; device_chain.function = VK_LAYER_LINK_INFO;
	device_chain.u.pLayerInfo = &device_link;
	VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; device_info.pNext = &device_chain;
	auto create_d = reinterpret_cast<PFN_vkCreateDevice>(get_instance(instance, "vkCreateDevice"));
	checks.require(create_d && create_d(gpu, &device_info, nullptr, &device) == VK_SUCCESS, "layer-device-create");
	static const uint32_t code[] = {0x07230203, 0x10000, 0, 1, 0};
	VkShaderModuleCreateInfo module = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize = sizeof(code); module.pCode = code;
	VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stage.pNext = &module; stage.pName = rejection ? nullptr : "main"; stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
	VkGraphicsPipelineCreateInfo g = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; g.stageCount = 1; g.pStages = &stage;
	VkComputePipelineCreateInfo c = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; c.stage = stage; c.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	// The compute copier requires a non-null entrypoint name. Use a malformed
	// nested stage tag for its synchronous rejection instead of dereferencing
	// an invalid name; this does not establish an unsupported Vulkan feature.
	VkBaseInStructure malformed = {VK_STRUCTURE_TYPE_MAX_ENUM, reinterpret_cast<const VkBaseInStructure *>(&module)};
	if (rejection) { c.stage.pName = "main"; c.stage.pNext = &malformed; }
	VkPipelineShaderStageCreateInfo ray_stage = stage; ray_stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
	VkRayTracingPipelineCreateInfoKHR r = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR}; r.stageCount = 1; r.pStages = &ray_stage;
	VkPipeline output = VK_NULL_HANDLE;
	VkResult result = VK_ERROR_UNKNOWN;
	DiagnosticStderr capture;
	if (mode == "binary")
	{
		VkPipelineCreateInfoKHR pipeline_info = {VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR};
		pipeline_info.pNext = kind == "graphics" ? static_cast<void *>(&g) : kind == "compute" ? static_cast<void *>(&c) : static_cast<void *>(&r);
		VkPipelineBinaryCreateInfoKHR info = {VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR}; info.pPipelineCreateInfo = &pipeline_info;
		VkPipelineBinaryHandlesInfoKHR handles = {VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
		auto fn = reinterpret_cast<PFN_vkCreatePipelineBinariesKHR>(get_device(device, "vkCreatePipelineBinariesKHR"));
		checks.require(fn != nullptr, "binary-entrypoint"); if (fn) result = fn(device, &info, nullptr, &handles);
	}
	else if (kind == "graphics")
	{
		auto fn = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(get_device(device, "vkCreateGraphicsPipelines"));
		checks.require(fn != nullptr, "graphics-entrypoint"); if (fn) result = fn(device, VK_NULL_HANDLE, 1, &g, nullptr, &output);
	}
	else if (kind == "compute")
	{
		auto fn = reinterpret_cast<PFN_vkCreateComputePipelines>(get_device(device, "vkCreateComputePipelines"));
		checks.require(fn != nullptr, "compute-entrypoint"); if (fn) result = fn(device, VK_NULL_HANDLE, 1, &c, nullptr, &output);
	}
	else
	{
		auto fn = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(get_device(device, "vkCreateRayTracingPipelinesKHR"));
		checks.require(fn != nullptr, "raytracing-entrypoint"); if (fn) result = fn(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &r, nullptr, &output);
	}
	std::string log = capture.finish();
	checks.require(result == (fail_vulkan ? VK_ERROR_DEVICE_LOST : VK_SUCCESS), "application-result-preserved");
	if (mode != "binary") checks.require(output == (fail_vulkan ? VK_NULL_HANDLE : (VkPipeline)0x1234), "application-handle-preserved");
	checks.require(calls == (qa && (!fail_vulkan || kind == "raytracing") ? 2u : 1u), "driver-call-count");
	checks.require(destroys == (qa && !fail_vulkan ? 1u : 0u), "driver-destroy-count");
	if (destroys) checks.require(destroyed[0] == VK_NULL_HANDLE, "qa-destroy-original-null");
	bool records = !fail_vulkan || mode == "paranoid" || mode == "eager";
	if (rejection && records)
	{
		checks.contains(log, "Capture/record " + kind + " pipeline failed", "correct-resource-phase");
		checks.contains(log, "mode=" + (mode == "eager" ? std::string("eager") : mode), "capture-mode");
		checks.contains(log, mode == "binary" ? "handle=not-applicable" : "batch_index=0", "known-identity");
		checks.contains(log, "hash=unknown", "unknown-hash-honest");
		checks.contains(log, "reason=unknown", "unknown-cause-honest");
	}
	else checks.require(log.find("failed") == std::string::npos && log.find("Failed") == std::string::npos, "successful-recording-no-failure");
	checks.require(log.find("unsupported") == std::string::npos, "no-unsupported-speculation");
	checks.require(log.find("crash") == std::string::npos, "no-crash-speculation");
	if (mode != "binary" && output)
		reinterpret_cast<PFN_vkDestroyPipeline>(get_device(device, "vkDestroyPipeline"))(device, output, nullptr);
	reinterpret_cast<PFN_vkDestroyDevice>(get_device(device, "vkDestroyDevice"))(device, nullptr);
	reinterpret_cast<PFN_vkDestroyInstance>(get_instance(instance, "vkDestroyInstance"))(instance, nullptr);
#ifdef _WIN32
	FreeLibrary(library);
#else
	dlclose(library);
#endif
	fprintf(stdout, "Executed CAPTURE-KINDS/%s/%s/%s; failures=%d\n", mode.c_str(), kind.c_str(), outcome.c_str(), checks.failures);
	return checks.failures ? 1 : 0;
}
