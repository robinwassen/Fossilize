#include "volk.h"
#include "fossilize_feature_filter.hpp"
#include <cstdio>
int main()
{
	Fossilize::FeatureFilter filter;
	VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	if (!filter.init(VK_API_VERSION_1_2, nullptr, 0, &features, &properties))
		return 1;
	VkComputePipelineCreateInfo compute = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	compute.stage.pName = "main";
	compute.stage.module = (VkShaderModule)1;
	if (!filter.compute_pipeline_is_supported(&compute))
		return 2;
	compute.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
	if (filter.compute_pipeline_is_supported(&compute))
		return 3;
	compute.flags = 0;
	if (!filter.compute_pipeline_is_supported(&compute))
		return 4;
	puts("M19 actual input FeatureFilter bool caller supported/rejected/recovery passed");
	return 0;
}
