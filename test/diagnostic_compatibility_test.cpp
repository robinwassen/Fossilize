// Uses the pre-existing public interfaces. Also compile this file against the
// baseline headers when checking source compatibility with candidate libraries.
#define VK_NO_PROTOTYPES
#include "diagnostic_test.hpp"
#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include "logging.hpp"
#include "fossilize_errors.hpp"
#include "device.hpp"
#include <memory>
using namespace Fossilize;
static void callback(LogLevel, const char *, void *userdata) { ++*static_cast<unsigned *>(userdata); }
int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	DiagnosticChecks checks;
	unsigned messages = 0;
	set_thread_log_callback(callback, &messages);
	set_thread_log_level(LOG_INFO);
	VulkanDevice::Options options;
	options.null_device = true;
	VulkanDevice device;
	checks.require(device.init_device(options), "old-source-null-device");
	{
		std::unique_ptr<DatabaseInterface> db(create_database(argv[1], DatabaseMode::OverWrite));
		checks.require(db && db->prepare(), "fixture-db-prepare");
		if (!db) return 1;
		StateRecorder recorder;
		recorder.init_recording_synchronized(db.get());
		VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_0;
		checks.require(recorder.record_application_info(app), "old-source-application");
		static const uint32_t code[] = {0x07230203, 0x10000, 0, 1, 0};
		VkShaderModuleCreateInfo module = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize = sizeof(code); module.pCode = code;
		checks.require(recorder.record_shader_module((VkShaderModule)1, module, 0x101), "old-source-module");
		VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		checks.require(recorder.record_pipeline_layout((VkPipelineLayout)2, layout, 0x102), "old-source-layout");
		VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		stage.module = (VkShaderModule)1; stage.pName = "main";
		VkGraphicsPipelineCreateInfo g = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		stage.stage = VK_SHADER_STAGE_VERTEX_BIT; g.stageCount = 1; g.pStages = &stage; g.layout = (VkPipelineLayout)2;
		checks.require(recorder.record_graphics_pipeline((VkPipeline)3, g, nullptr, 0, 0x103), "old-source-graphics");
		VkComputePipelineCreateInfo c = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; c.stage = stage; c.layout = (VkPipelineLayout)2;
		checks.require(recorder.record_compute_pipeline((VkPipeline)4, c, nullptr, 0, 0x104), "old-source-compute");
		VkRayTracingPipelineCreateInfoKHR r = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
		stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR; r.stageCount = 1; r.pStages = &stage; r.layout = (VkPipelineLayout)2;
		checks.require(recorder.record_raytracing_pipeline((VkPipeline)5, r, nullptr, 0, 0x105), "old-source-raytracing");
		recorder.tear_down_recording_thread();
	}
	std::unique_ptr<DatabaseInterface> db(create_database(argv[1], DatabaseMode::ReadOnly));
	checks.require(db && db->prepare(), "fixture-readback");
	for (auto tag : {RESOURCE_SHADER_MODULE, RESOURCE_PIPELINE_LAYOUT, RESOURCE_GRAPHICS_PIPELINE, RESOURCE_COMPUTE_PIPELINE, RESOURCE_RAYTRACING_PIPELINE})
	{
		size_t count = 0;
		checks.require(db->get_hash_list_for_resource_tag(tag, &count, nullptr) && count == 1, "fixture-resource-set/" + std::to_string(unsigned(tag)));
		Hash hash = 0;
		checks.require(db->get_hash_list_for_resource_tag(tag, &count, &hash), "fixture-hash-read");
		Hash expected = tag == RESOURCE_SHADER_MODULE ? 0x101 : tag == RESOURCE_PIPELINE_LAYOUT ? 0x102 : tag == RESOURCE_GRAPHICS_PIPELINE ? 0x103 : tag == RESOURCE_COMPUTE_PIPELINE ? 0x104 : 0x105;
		checks.require(hash == expected, "fixture-known-hash/" + std::to_string(unsigned(tag)));
	}
	set_thread_log_callback(nullptr, nullptr);
	checks.require(messages == 0, "old-log-callback-success-quiet");
	fprintf(stdout, "Executed COMPATIBILITY/source-and-fixture; failures=%d\n", checks.failures);
	return checks.failures ? 1 : 0;
}
