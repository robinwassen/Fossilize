#include "diagnostic_test.hpp"
#include "../cli/device.hpp"
static VkResult diagnostic_initialize();
static uint32_t diagnostic_version();
static void diagnostic_load_instance(VkInstance);
static void diagnostic_load_device(VkDevice);
#define volkInitialize diagnostic_initialize
#define volkGetInstanceVersion diagnostic_version
#define volkLoadInstance diagnostic_load_instance
#define volkLoadDevice diagnostic_load_device
#include "../cli/device.cpp"
#undef volkInitialize
#undef volkGetInstanceVersion
#undef volkLoadInstance
#undef volkLoadDevice
using namespace Fossilize;

static std::string failure_stage;
static VkResult injected_result;
static unsigned destroy_instances, destroy_devices, destroy_callbacks;
static bool no_devices, no_loader;
static VkResult result_for(const char *operation) { return failure_stage == operation ? injected_result : VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL instance_extensions(const char *, uint32_t *count, VkExtensionProperties *data)
{
	if (data) { memset(data, 0, sizeof(*data)); strcpy(data->extensionName, VK_EXT_DEBUG_REPORT_EXTENSION_NAME); }
	*count = 1;
	return result_for(data ? "instance-extension-data" : "instance-extension-count");
}
static VKAPI_ATTR VkResult VKAPI_CALL layers(uint32_t *count, VkLayerProperties *data)
{
	if (data) { memset(data, 0, sizeof(*data)); strcpy(data->layerName, "VK_LAYER_KHRONOS_validation"); }
	*count = 1;
	return result_for(data ? "instance-layer-data" : "instance-layer-count");
}
static VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *p)
{
	auto result = result_for("vkCreateInstance");
	if (result == VK_SUCCESS) *p = (VkInstance)0x1000;
	return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_callback(VkInstance, const VkDebugReportCallbackCreateInfoEXT *, const VkAllocationCallbacks *, VkDebugReportCallbackEXT *p)
{
	auto result = result_for("vkCreateDebugReportCallbackEXT");
	if (result == VK_SUCCESS) *p = (VkDebugReportCallbackEXT)0x2000;
	return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL physical_devices(VkInstance, uint32_t *count, VkPhysicalDevice *data)
{
	if (data) *data = (VkPhysicalDevice)0x3000;
	*count = no_devices ? 0 : 1;
	return result_for(data ? "physical-device-data" : "physical-device-count");
}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties *p)
{
	memset(p, 0, sizeof(*p)); p->apiVersion = VK_API_VERSION_1_1;
	strcpy(p->deviceName, "deterministic-gpu"); p->vendorID = 0x1234; p->deviceID = 0x5678;
}
static VKAPI_ATTR VkResult VKAPI_CALL device_extensions(VkPhysicalDevice, const char *, uint32_t *count, VkExtensionProperties *data)
{
	if (data) { memset(data, 0, sizeof(*data)); strcpy(data->extensionName, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME); }
	*count = 1;
	return result_for(data ? "device-extension-data" : "device-extension-count");
}
static VKAPI_ATTR void VKAPI_CALL physical_features(VkPhysicalDevice, VkPhysicalDeviceFeatures *p) { memset(p, 0, sizeof(*p)); }
static VKAPI_ATTR void VKAPI_CALL queues(VkPhysicalDevice, uint32_t *count, VkQueueFamilyProperties *p)
{
	*count = 1; if (p) { memset(p, 0, sizeof(*p)); p->queueFlags = VK_QUEUE_GRAPHICS_BIT; p->queueCount = 1; }
}
static VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *p)
{
	auto result = result_for("vkCreateDevice");
	if (result == VK_SUCCESS) *p = (VkDevice)0x4000;
	return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL pipeline_key(VkDevice, const VkPipelineCreateInfoKHR *, VkPipelineBinaryKeyKHR *key)
{
	key->keySize = 1; key->key[0] = 0xab;
	return result_for("vkGetPipelineKeyKHR");
}
static VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance, const VkAllocationCallbacks *) { destroy_instances++; errno = ENOSPC; }
static VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice, const VkAllocationCallbacks *) { destroy_devices++; errno = ENOSPC; }
static VKAPI_ATTR void VKAPI_CALL destroy_callback(VkInstance, VkDebugReportCallbackEXT, const VkAllocationCallbacks *) { destroy_callbacks++; errno = ENOSPC; }
static VkResult diagnostic_initialize()
{
	vkEnumerateInstanceExtensionProperties = instance_extensions;
	vkEnumerateInstanceLayerProperties = layers;
	vkCreateInstance = create_instance;
	vkCreateDebugReportCallbackEXT = create_callback;
	vkEnumeratePhysicalDevices = physical_devices;
	vkGetPhysicalDeviceProperties = properties;
	vkEnumerateDeviceExtensionProperties = device_extensions;
	vkGetPhysicalDeviceFeatures = physical_features;
	vkGetPhysicalDeviceQueueFamilyProperties = queues;
	vkCreateDevice = create_device;
	vkGetPipelineKeyKHR = pipeline_key;
	vkDestroyInstance = destroy_instance;
	vkDestroyDevice = destroy_device;
	vkDestroyDebugReportCallbackEXT = destroy_callback;
	return result_for("volkInitialize");
}
static uint32_t diagnostic_version() { return no_loader ? 0 : VK_API_VERSION_1_1; }
static void diagnostic_load_instance(VkInstance) {}
static void diagnostic_load_device(VkDevice) {}
int main()
{
	DiagnosticChecks checks;
#ifdef _WIN32
	_putenv_s("FOSSILIZE_LOG_PIPELINE_BINARY_KEY", "1");
#else
	setenv("FOSSILIZE_LOG_PIPELINE_BINARY_KEY", "1", 1);
#endif
	const char *stages[] = {"volkInitialize", "instance-extension-count", "instance-extension-data", "instance-layer-count", "instance-layer-data", "vkCreateInstance", "vkCreateDebugReportCallbackEXT", "physical-device-count", "physical-device-data", "device-extension-count", "device-extension-data", "vkCreateDevice", "vkGetPipelineKeyKHR", "success", "null-device", "no-devices", "no-loader", "unsupported-api"};
	for (unsigned stage = 0; stage < sizeof(stages) / sizeof(stages[0]); stage++)
	{
		for (auto result : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_DEVICE_LOST})
		{
			failure_stage = stages[stage]; injected_result = result;
			destroy_instances = destroy_devices = destroy_callbacks = 0;
			no_devices = failure_stage == "no-devices"; no_loader = failure_stage == "no-loader";
			VulkanDevice::Options opts;
			opts.enable_validation = true; opts.device_index = 0;
			opts.null_device = failure_stage == "null-device";
			VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
			if (failure_stage == "unsupported-api") opts.application_info = &app;
			bool initialized;
			DiagnosticStderr capture;
			{ VulkanDevice device; initialized = device.init_device(opts); }
			std::string log = capture.finish();
			std::string id = failure_stage + "/" + std::to_string(int(result));
			bool success = stage >= 12 && stage <= 14;
			checks.require(initialized == success, id + "/return");
			checks.require(log.find("crash") == std::string::npos, id + "/no-crash-guess");
			bool instance_created = (stage >= 6 && stage <= 13) || stage == 15;
			bool callback_created = (stage >= 7 && stage <= 13) || stage == 15;
			checks.require(destroy_instances == unsigned(instance_created), id + "/instance-cleanup");
			checks.require(destroy_callbacks == unsigned(callback_created), id + "/callback-cleanup");
			checks.require(destroy_devices == unsigned(stage == 12 || stage == 13), id + "/device-cleanup");
			if (stage <= 12)
			{
				checks.contains(log, "Device/setup " + failure_stage + " failed", id + "/operation-phase");
				checks.contains(log, "VkResult=" + std::to_string(int(result)), id + "/raw-result");
				checks.contains(log, "hash=not-applicable", id + "/no-invented-hash");
				checks.contains(log, stage >= 9 ? "gpu_index=0" : "gpu_index=-1", id + "/gpu-identity");
			}
			else if (success) checks.require(log.find("failed") == std::string::npos, id + "/success-quiet");
			else
			{
				checks.require(log.find("VkResult=") == std::string::npos, id + "/no-synthetic-result");
				checks.contains(log, stage == 15 ? "No physical devices" : stage == 16 ? "Could not find loader" : "unsupported by this Vulkan loader", id + "/limitation");
			}
		}
	}
	fprintf(stdout, "Executed VK-DEVICE:36 including 13 injected stages, success/null and limitations; failures=%d\n", checks.failures);
	return checks.failures ? 1 : 0;
}
