/* Copyright (c) 2019 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "volk.h"
#include "fossilize_feature_filter.hpp"
#include "spirv.hpp"
#include "logging.hpp"
#include <string.h>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <vector>
#include <mutex>

// There is a mismatch in pluralization between extension name and feature structure name which screws up our macros.
#define VK_ARM_TENSOR_EXTENSION_NAME VK_ARM_TENSORS_EXTENSION_NAME
#define VK_EXT_SHADER_64_BIT_INDEXING_EXTENSION_NAME VK_EXT_SHADER_64BIT_INDEXING_EXTENSION_NAME

namespace Fossilize
{
void *build_pnext_chain(VulkanFeatures &features, uint32_t api_version,
                        const char **enabled_extensions, uint32_t extension_count)
{
	features = {};
	void *pNext = nullptr;
	void **ppNext = nullptr;

	std::unordered_set<std::string> enabled_extension_set;
	for (uint32_t i = 0; i < extension_count; i++)
		enabled_extension_set.insert(enabled_extensions[i]);

#define CHAIN(struct_type, member, min_api_version, supports_extension) \
	do { \
		if (api_version >= min_api_version && (supports_extension)) { \
			member.sType = struct_type; \
			if (!pNext) pNext = &member; \
			if (ppNext) *ppNext = &member; \
			ppNext = &member.pNext; \
		} \
	} while (0)
#define SUPPORTS_EXTENSION(required_extension) enabled_extension_set.count(required_extension) != 0

#define F(struct_type, member, minimum_api_version, required_extension) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES, features.member, \
		  VK_API_VERSION_##minimum_api_version, SUPPORTS_EXTENSION(VK_##required_extension##_EXTENSION_NAME))
#define FE(struct_type, member, ext) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES_##ext, features.member, \
		  VK_API_VERSION_1_0, SUPPORTS_EXTENSION(VK_##ext##_##struct_type##_EXTENSION_NAME))
#define FE_ALIAS(struct_type, member, ext, ext_alias) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES_##ext, features.member, \
		  VK_API_VERSION_1_0, \
		  SUPPORTS_EXTENSION(VK_##ext##_##struct_type##_EXTENSION_NAME) || \
			  SUPPORTS_EXTENSION(VK_##ext_alias##_##struct_type##_EXTENSION_NAME))

#include "fossilize_feature_filter_features.inc"

#undef CHAIN
#undef SUPPORTS_EXTENSION
#undef F
#undef FE
#undef FE_ALIAS

	return pNext;
}

static void filter_feature_enablement(
		VkPhysicalDeviceFeatures2 &pdf, VulkanFeatures &features,
		const VkPhysicalDeviceFeatures2 *target_features)
{
	// Workaround RADV quirk on older drivers.
	features.cooperative_matrix.cooperativeMatrixRobustBufferAccess = VK_FALSE;

	// These feature bits conflict according to validation layers.
	if (features.fragment_shading_rate.pipelineFragmentShadingRate == VK_TRUE ||
	    features.fragment_shading_rate.attachmentFragmentShadingRate == VK_TRUE ||
	    features.fragment_shading_rate.primitiveFragmentShadingRate == VK_TRUE)
	{
		features.shading_rate_nv.shadingRateImage = VK_FALSE;
		features.shading_rate_nv.shadingRateCoarseSampleOrder = VK_FALSE;
		features.fragment_density.fragmentDensityMap = VK_FALSE;
	}

	// Only enable robustness if requested since it affects compilation on most implementations.
	if (target_features)
	{
		pdf.features.robustBufferAccess =
				pdf.features.robustBufferAccess && target_features->features.robustBufferAccess;

		const auto *robustness2 = find_pnext<VkPhysicalDeviceRobustness2FeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
				target_features->pNext);

		if (robustness2)
		{
			features.robustness2.robustBufferAccess2 =
					features.robustness2.robustBufferAccess2 && robustness2->robustBufferAccess2;
			features.robustness2.robustImageAccess2 =
					features.robustness2.robustImageAccess2 && robustness2->robustImageAccess2;
			features.robustness2.nullDescriptor =
					features.robustness2.nullDescriptor && robustness2->nullDescriptor;
		}
		else
		{
			reset_features(features.robustness2, VK_FALSE);
		}

		const auto *image_robustness = find_pnext<VkPhysicalDeviceImageRobustnessFeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT,
				target_features->pNext);

		if (image_robustness)
		{
			features.image_robustness.robustImageAccess =
					features.image_robustness.robustImageAccess && image_robustness->robustImageAccess;
		}
		else
		{
			reset_features(features.image_robustness, VK_FALSE);
		}

		const auto *fragment_shading_rate_enums = find_pnext<VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV,
				target_features->pNext);

		if (fragment_shading_rate_enums)
		{
			features.fragment_shading_rate_enums.fragmentShadingRateEnums =
					features.fragment_shading_rate_enums.fragmentShadingRateEnums &&
					fragment_shading_rate_enums->fragmentShadingRateEnums;

			features.fragment_shading_rate_enums.noInvocationFragmentShadingRates =
					features.fragment_shading_rate_enums.noInvocationFragmentShadingRates &&
					fragment_shading_rate_enums->noInvocationFragmentShadingRates;

			features.fragment_shading_rate_enums.supersampleFragmentShadingRates =
					features.fragment_shading_rate_enums.supersampleFragmentShadingRates &&
					fragment_shading_rate_enums->supersampleFragmentShadingRates;
		}
		else
		{
			reset_features(features.fragment_shading_rate_enums, VK_FALSE);
		}

		const auto *fragment_shading_rate = find_pnext<VkPhysicalDeviceFragmentShadingRateFeaturesKHR>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
				target_features->pNext);

		if (fragment_shading_rate)
		{
			features.fragment_shading_rate.pipelineFragmentShadingRate =
					features.fragment_shading_rate.pipelineFragmentShadingRate &&
					fragment_shading_rate->pipelineFragmentShadingRate;
			features.fragment_shading_rate.primitiveFragmentShadingRate =
					features.fragment_shading_rate.primitiveFragmentShadingRate &&
					fragment_shading_rate->primitiveFragmentShadingRate;
			features.fragment_shading_rate.attachmentFragmentShadingRate =
					features.fragment_shading_rate.attachmentFragmentShadingRate &&
					fragment_shading_rate->attachmentFragmentShadingRate;
		}
		else
		{
			reset_features(features.fragment_shading_rate, VK_FALSE);
		}

		const auto *mesh_shader = find_pnext<VkPhysicalDeviceMeshShaderFeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
				target_features->pNext);

		if (mesh_shader)
		{
			features.mesh_shader.taskShader =
					features.mesh_shader.taskShader && mesh_shader->taskShader;
			features.mesh_shader.meshShader =
					features.mesh_shader.meshShader && mesh_shader->meshShader;
			features.mesh_shader.multiviewMeshShader =
					features.mesh_shader.multiviewMeshShader && mesh_shader->multiviewMeshShader;
			features.mesh_shader.meshShaderQueries =
					features.mesh_shader.meshShaderQueries && mesh_shader->meshShaderQueries;
			features.mesh_shader.primitiveFragmentShadingRateMeshShader =
					features.mesh_shader.primitiveFragmentShadingRateMeshShader &&
					features.fragment_shading_rate.primitiveFragmentShadingRate &&
					mesh_shader->primitiveFragmentShadingRateMeshShader;
		}
		else
		{
			reset_features(features.mesh_shader, VK_FALSE);
		}

		const auto *mesh_shader_nv = find_pnext<VkPhysicalDeviceMeshShaderFeaturesNV>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV,
				target_features->pNext);

		if (mesh_shader_nv)
		{
			features.mesh_shader_nv.taskShader =
					features.mesh_shader_nv.taskShader && mesh_shader->taskShader;
			features.mesh_shader_nv.meshShader =
					features.mesh_shader_nv.meshShader && mesh_shader->meshShader;
		}
		else
		{
			reset_features(features.mesh_shader_nv, VK_FALSE);
		}

		const auto *descriptor_buffer = find_pnext<VkPhysicalDeviceDescriptorBufferFeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
				target_features->pNext);

		if (descriptor_buffer)
		{
			features.descriptor_buffer.descriptorBuffer =
					features.descriptor_buffer.descriptorBuffer &&
					descriptor_buffer->descriptorBuffer;
			features.descriptor_buffer.descriptorBufferCaptureReplay =
					features.descriptor_buffer.descriptorBufferCaptureReplay &&
					descriptor_buffer->descriptorBufferCaptureReplay;
			features.descriptor_buffer.descriptorBufferImageLayoutIgnored =
					features.descriptor_buffer.descriptorBufferImageLayoutIgnored &&
					descriptor_buffer->descriptorBufferImageLayoutIgnored;
			features.descriptor_buffer.descriptorBufferPushDescriptors =
					features.descriptor_buffer.descriptorBufferPushDescriptors &&
					descriptor_buffer->descriptorBufferPushDescriptors;
		}
		else
		{
			reset_features(features.descriptor_buffer, VK_FALSE);
		}

		const auto *shader_object = find_pnext<VkPhysicalDeviceShaderObjectFeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
				target_features->pNext);

		if (shader_object)
		{
			features.shader_object.shaderObject =
					features.shader_object.shaderObject &&
					shader_object->shaderObject;
		}
		else
		{
			reset_features(features.shader_object, VK_FALSE);
		}

		const auto *prim_generated = find_pnext<VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT,
				target_features->pNext);

		if (prim_generated)
		{
			features.primitives_generated_query.primitivesGeneratedQuery =
					features.primitives_generated_query.primitivesGeneratedQuery &&
					prim_generated->primitivesGeneratedQuery;

			features.primitives_generated_query.primitivesGeneratedQueryWithNonZeroStreams =
					features.primitives_generated_query.primitivesGeneratedQueryWithNonZeroStreams &&
					prim_generated->primitivesGeneratedQueryWithNonZeroStreams;

			features.primitives_generated_query.primitivesGeneratedQueryWithRasterizerDiscard =
					features.primitives_generated_query.primitivesGeneratedQueryWithRasterizerDiscard &&
					prim_generated->primitivesGeneratedQueryWithRasterizerDiscard;
		}
		else
		{
			reset_features(features.primitives_generated_query, VK_FALSE);
		}

		const auto *image_2d_view_of_3d = find_pnext<VkPhysicalDeviceImage2DViewOf3DFeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT,
				target_features->pNext);

		if (image_2d_view_of_3d)
		{
			features.image_2d_view_of_3d.image2DViewOf3D =
					features.image_2d_view_of_3d.image2DViewOf3D &&
					image_2d_view_of_3d->image2DViewOf3D;

			features.image_2d_view_of_3d.sampler2DViewOf3D =
					features.image_2d_view_of_3d.sampler2DViewOf3D &&
					image_2d_view_of_3d->sampler2DViewOf3D;
		}
		else
		{
			reset_features(features.image_2d_view_of_3d, VK_FALSE);
		}

		const auto *pipeline_robustness = find_pnext<VkPhysicalDevicePipelineRobustnessFeatures>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES,
				target_features->pNext);

		if (pipeline_robustness)
		{
			features.pipeline_robustness.pipelineRobustness =
				features.pipeline_robustness.pipelineRobustness &&
				pipeline_robustness->pipelineRobustness;
		}
		else
		{
			reset_features(features.pipeline_robustness, VK_FALSE);
		}

		const auto *maintenance8 = find_pnext<VkPhysicalDeviceMaintenance8FeaturesKHR>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR,
				target_features->pNext);

		if (maintenance8)
			features.maintenance8.maintenance8 = features.maintenance8.maintenance8 && maintenance8->maintenance8;
		else
			reset_features(features.maintenance8, VK_FALSE);

		const auto *heap = find_pnext<VkPhysicalDeviceDescriptorHeapFeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
				target_features->pNext);

		if (heap)
		{
			features.descriptor_heap.descriptorHeap =
				features.descriptor_heap.descriptorHeap && heap->descriptorHeap;
			features.descriptor_heap.descriptorHeapCaptureReplay =
				features.descriptor_heap.descriptorHeapCaptureReplay && heap->descriptorHeapCaptureReplay;
		}
		else
		{
			reset_features(features.descriptor_heap, VK_FALSE);
		}

		const auto *omm = find_pnext<VkPhysicalDeviceOpacityMicromapFeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT,
				target_features->pNext);

		if (omm)
		{
			features.opacity_micromap_ext.micromap =
				features.opacity_micromap_ext.micromap && omm->micromap;
			features.opacity_micromap_ext.micromapCaptureReplay =
				features.opacity_micromap_ext.micromapCaptureReplay && omm->micromapCaptureReplay;
			features.opacity_micromap_ext.micromapHostCommands =
				features.opacity_micromap_ext.micromapHostCommands && omm->micromapHostCommands;
		}
		else
		{
			reset_features(features.opacity_micromap_ext, VK_FALSE);
		}
	}
	else
	{
		pdf.features.robustBufferAccess = VK_FALSE;
		reset_features(features.robustness2, VK_FALSE);
		reset_features(features.image_robustness, VK_FALSE);
		reset_features(features.fragment_shading_rate_enums, VK_FALSE);
		reset_features(features.fragment_shading_rate, VK_FALSE);
		reset_features(features.mesh_shader, VK_FALSE);
		reset_features(features.mesh_shader_nv, VK_FALSE);
		reset_features(features.descriptor_buffer, VK_FALSE);
		reset_features(features.shader_object, VK_FALSE);
		reset_features(features.primitives_generated_query, VK_FALSE);
		reset_features(features.image_2d_view_of_3d, VK_FALSE);
		reset_features(features.pipeline_robustness, VK_FALSE);
		reset_features(features.maintenance8, VK_FALSE);
		reset_features(features.descriptor_heap, VK_FALSE);
		reset_features(features.opacity_micromap_ext, VK_FALSE);
	}
}

static void remove_extension(const char **active_extensions, size_t *out_extension_count, const char *ext)
{
	size_t count = *out_extension_count;
	for (size_t i = 0; i < count; i++)
	{
		if (strcmp(active_extensions[i], ext) == 0)
		{
			active_extensions[i] = active_extensions[--count];
			break;
		}
	}

	*out_extension_count = count;
}

static void filter_active_extensions(VkPhysicalDeviceFeatures2 &pdf,
                                     const char **active_extensions, size_t *out_extension_count)
{
	// If we end up disabling features deliberately, we should remove the extension enablement as well,
	// and remove the feature pNext.
	// Some implementations will end up changing their PSO keys even though no features are actually enabled.
	auto *pNext = static_cast<VkBaseOutStructure *>(pdf.pNext);

	pdf.pNext = nullptr;
	void **ppNext = &pdf.pNext;

	while (pNext)
	{
		bool accept = true;
		auto *s = pNext;
		pNext = s->pNext;

		switch (s->sType)
		{
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV *>(s);
			if (feature->fragmentShadingRateEnums == VK_FALSE &&
			    feature->noInvocationFragmentShadingRates == VK_FALSE &&
			    feature->supersampleFragmentShadingRates == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count,
				                 VK_NV_FRAGMENT_SHADING_RATE_ENUMS_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceFragmentShadingRateFeaturesKHR *>(s);
			if (feature->attachmentFragmentShadingRate == VK_FALSE &&
			    feature->pipelineFragmentShadingRate == VK_FALSE &&
			    feature->primitiveFragmentShadingRate == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT *>(s);
			if (feature->nullDescriptor == VK_FALSE &&
			    feature->robustBufferAccess2 == VK_FALSE &&
			    feature->robustImageAccess2 == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
				remove_extension(active_extensions, out_extension_count, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceImageRobustnessFeaturesEXT *>(s);
			if (feature->robustImageAccess == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceMeshShaderFeaturesEXT *>(s);
			if (feature->meshShader == VK_FALSE &&
			    feature->taskShader == VK_FALSE &&
			    feature->multiviewMeshShader == VK_FALSE &&
			    feature->primitiveFragmentShadingRateMeshShader == VK_FALSE &&
			    feature->meshShaderQueries == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_MESH_SHADER_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceMeshShaderFeaturesNV *>(s);
			if (feature->meshShader == VK_FALSE &&
			    feature->taskShader == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_NV_MESH_SHADER_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceDescriptorBufferFeaturesEXT *>(s);
			if (feature->descriptorBuffer == VK_FALSE &&
			    feature->descriptorBufferCaptureReplay == VK_FALSE &&
			    feature->descriptorBufferImageLayoutIgnored == VK_FALSE &&
			    feature->descriptorBufferPushDescriptors == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceShaderObjectFeaturesEXT *>(s);
			if (feature->shaderObject == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT *>(s);
			if (feature->primitivesGeneratedQuery == VK_FALSE &&
			    feature->primitivesGeneratedQueryWithNonZeroStreams == VK_FALSE &&
			    feature->primitivesGeneratedQueryWithRasterizerDiscard == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_PRIMITIVES_GENERATED_QUERY_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *>(s);
			if (feature->image2DViewOf3D == VK_FALSE &&
			    feature->sampler2DViewOf3D == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES:
		{
			auto *feature = reinterpret_cast<VkPhysicalDevicePipelineRobustnessFeatures *>(s);
			if (feature->pipelineRobustness == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceMaintenance8FeaturesKHR *>(s);
			if (feature->maintenance8 == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceDescriptorHeapFeaturesEXT *>(s);
			if (feature->descriptorHeap == VK_FALSE && feature->descriptorHeapCaptureReplay == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceOpacityMicromapFeaturesEXT *>(s);
			if (feature->micromap == VK_FALSE && feature->micromapCaptureReplay == VK_FALSE && feature->micromapHostCommands == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		default:
			break;
		}

		if (accept)
		{
			*ppNext = s;
			ppNext = reinterpret_cast<void **>(&s->pNext);
			*ppNext = nullptr;
		}
	}
}

void filter_feature_enablement(VkPhysicalDeviceFeatures2 &pdf,
                               VulkanFeatures &features,
                               const VkPhysicalDeviceFeatures2 *target_features,
							   const char **active_extensions, size_t *out_extension_count)
{
	filter_feature_enablement(pdf, features, target_features);
	filter_active_extensions(pdf, active_extensions, out_extension_count);
}

void *build_pnext_chain(VulkanProperties &props, uint32_t api_version,
                        const char **enabled_extensions, uint32_t extension_count)
{
	props = {};
	void *pNext = nullptr;
	void **ppNext = nullptr;

	std::unordered_set<std::string> enabled_extension_set;
	for (uint32_t i = 0; i < extension_count; i++)
		enabled_extension_set.insert(enabled_extensions[i]);

#define CHAIN(struct_type, member, min_api_version, supports_extension) \
	do { \
		if (api_version >= min_api_version && (supports_extension)) { \
			member.sType = struct_type; \
			if (!pNext) pNext = &member; \
			if (ppNext) *ppNext = &member; \
			ppNext = &member.pNext; \
		} \
	} while (0)
#define SUPPORTS_EXTENSION(required_extension) enabled_extension_set.count(required_extension) != 0

#define P(struct_type, member, minimum_api_version, required_extension) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES, props.member, \
		  VK_API_VERSION_##minimum_api_version, SUPPORTS_EXTENSION(VK_##required_extension##_EXTENSION_NAME))
#define P_CORE(struct_type, member, minimum_api_version) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES, props.member, \
		  VK_API_VERSION_##minimum_api_version, true)
#define PE(struct_type, member, ext) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES_##ext, props.member, \
		  VK_API_VERSION_1_0, SUPPORTS_EXTENSION(VK_##ext##_##struct_type##_EXTENSION_NAME))
#define PE_ALIAS(struct_type, member, ext, ext_alias) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES_##ext, props.member, \
		  VK_API_VERSION_1_0, \
		  SUPPORTS_EXTENSION(VK_##ext##_##struct_type##_EXTENSION_NAME) || \
			  SUPPORTS_EXTENSION(VK_##ext##_##struct_type##_EXTENSION_NAME))

#include "fossilize_feature_filter_properties.inc"

#undef CHAIN
#undef SUPPORTS_EXTENSION
#undef P
#undef P_CORE
#undef PE
#undef PE_ALIAS

	return pNext;
}

// Each public call installs its own receiver. Helpers retain the first specific
// rejection; nested/reentrant calls restore the outer receiver on return.
namespace
{
thread_local SupportDetail *support_receiver = nullptr;
struct SupportScope
{
	SupportDetail *previous;
	explicit SupportScope(SupportDetail *out) : previous(support_receiver)
	{
		support_receiver = out;
		if (out)
			*out = {};
	}
	~SupportScope()
	{
		support_receiver = previous;
	}
};
bool support_reject(SupportKind kind, const char *identifier, uint64_t known = 0, uint64_t required = 0,
					uint64_t available = 0, uint64_t raw = 0)
{
	if (support_receiver && support_receiver->kind == SupportKind::None)
	{
		auto &d = *support_receiver;
		d.kind = kind;
		d.known = known;
		d.required = required;
		d.available = available;
		d.raw = raw;
		std::snprintf(d.identifier, sizeof(d.identifier), "%s", identifier);
		for (auto &c : d.identifier)
			if (c && (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126))
				c = '?';
	}
	return false;
}
} // namespace

struct FeatureFilter::Impl
{
	bool init(uint32_t api_version, const char **device_exts, unsigned count,
	          const VkPhysicalDeviceFeatures2 *enabled_features,
	          const VkPhysicalDeviceProperties2 *properties);

	bool sampler_is_supported(const VkSamplerCreateInfo *info) const;
	bool descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info) const;
	bool pipeline_layout_is_supported(const VkPipelineLayoutCreateInfo *info) const;
	bool shader_module_is_supported(const VkShaderModuleCreateInfo *info) const;
	bool register_shader_module_info(VkShaderModule module, const VkShaderModuleCreateInfo *info);
	void unregister_shader_module_info(VkShaderModule module);
	bool render_pass_is_supported(const VkRenderPassCreateInfo *info) const;
	bool render_pass2_is_supported(const VkRenderPassCreateInfo2 *info) const;
	bool graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info) const;
	bool color_blend_state_is_supported(const VkPipelineColorBlendStateCreateInfo *info,
	                                    const VkDynamicState *dynamic_states, uint32_t num_dynamic_states) const;
	bool compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info) const;
	bool raytracing_pipeline_is_supported(const VkRayTracingPipelineCreateInfoKHR *info) const;

	bool attachment_reference_is_supported(const VkAttachmentReference &ref) const;
	bool attachment_reference2_is_supported(const VkAttachmentReference2 &ref) const;
	bool attachment_description_is_supported(const VkAttachmentDescription &desc,
	                                         VkFormatFeatureFlags format_features) const;
	bool attachment_description2_is_supported(const VkAttachmentDescription2 &desc,
	                                          VkFormatFeatureFlags format_features) const;
	bool subpass_description_flags_is_supported(VkSubpassDescriptionFlags flags) const;
	bool subpass_description_is_supported(const VkSubpassDescription &sub) const;
	bool subpass_description2_is_supported(const VkSubpassDescription2 &sub) const;
	bool subpass_dependency_is_supported(const VkSubpassDependency &dep) const;
	bool subpass_dependency2_is_supported(const VkSubpassDependency2 &dep) const;
	bool dependency_flags_is_supported(VkDependencyFlags deps) const;
	bool store_op_is_supported(VkAttachmentStoreOp store_op) const;
	bool load_op_is_supported(VkAttachmentLoadOp load_op) const;

	bool subgroup_size_control_is_supported(const VkPipelineShaderStageCreateInfo &stage) const;

	bool multiview_mask_is_supported(uint32_t mask) const;
	bool image_layout_is_supported(VkImageLayout layout) const;
	bool polygon_mode_is_supported(VkPolygonMode mode) const;
	bool access_mask_is_supported(VkAccessFlags2 access) const;
	bool pipeline_stage_mask_is_supported(VkPipelineStageFlags2 stages) const;
	bool shader_stage_mask_is_supported(VkShaderStageFlags stages) const;
	bool aspect_mask_is_supported(VkImageAspectFlags aspect) const;
	bool format_is_supported(VkFormat, VkFormatFeatureFlags format_features) const;
	bool attachment_description_flags_are_supported(VkAttachmentDescriptionFlags flags) const;
	bool descriptor_set_and_binding_mapping_is_supported(const VkShaderDescriptorSetAndBindingMappingInfoEXT &info) const;

	bool stage_limits_are_supported(const VkPipelineShaderStageCreateInfo &info) const;

	std::unordered_set<std::string> enabled_extensions;

	DeviceQueryInterface *query = nullptr;

	uint32_t api_version = 0;
	VkPhysicalDeviceProperties2 props2 = {};
	VkPhysicalDeviceFeatures2 features2 = {};
	VulkanFeatures features = {};
	VulkanProperties props = {};
	bool null_device = false;

	struct DeferredEntryPoint
	{
		spv::Id id;
		spv::Id wg_size_literal[3];
		spv::Id wg_size_id[3];
		spv::Id constant_id[3];
		bool has_constant_id[3];
		std::string name;
		spv::ExecutionModel execution_model;
		uint32_t max_vertices;
		uint32_t max_primitives;
	};

	struct ModuleInfo
	{
		std::vector<DeferredEntryPoint> deferred_entry_points;
	};

	mutable std::mutex module_to_info_lock;
	std::unordered_map<VkShaderModule, ModuleInfo> module_to_info;

	void init_features(const void *pNext);
	void init_properties(const void *pNext);
	bool pnext_chain_is_supported(const void *pNext) const;
	bool validate_module_capabilities(const uint32_t *data, size_t size) const;
	bool parse_module_info(const uint32_t *data, size_t size, ModuleInfo &info);
	bool validate_module_capability(spv::Capability cap) const;
	bool validate_spirv_extension(const std::string &ext) const;
};

FeatureFilter::FeatureFilter()
{
	impl = new Impl;
}

FeatureFilter::~FeatureFilter()
{
	delete impl;
}

void FeatureFilter::Impl::init_features(const void *pNext)
{
	while (pNext)
	{
		auto *base = static_cast<const VkBaseInStructure *>(pNext);

#define F(struct_type, member, ...) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES: \
		memcpy(&features.member, base, sizeof(features.member)); \
		features.member.pNext = nullptr; \
		break

#define FE(struct_type, member, ext) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES_##ext: \
		memcpy(&features.member, base, sizeof(features.member)); \
		features.member.pNext = nullptr; \
		break

#define FE_ALIAS(struct_type, member, ext, ext_alias) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES_##ext: \
		memcpy(&features.member, base, sizeof(features.member)); \
		features.member.pNext = nullptr; \
		break

#define FEATURE(struct_name, core_struct, feature) if (core_struct.feature) features.struct_name.feature = VK_TRUE
		switch (base->sType)
		{
#include "fossilize_feature_filter_features.inc"
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
		{
			auto &vk11 = *reinterpret_cast<const VkPhysicalDeviceVulkan11Features *>(base);
			FEATURE(storage_16bit, vk11, storageBuffer16BitAccess);
			FEATURE(storage_16bit, vk11, uniformAndStorageBuffer16BitAccess);
			FEATURE(storage_16bit, vk11, storagePushConstant16);
			FEATURE(storage_16bit, vk11, storageInputOutput16);
			FEATURE(multiview, vk11, multiview);
			FEATURE(multiview, vk11, multiviewGeometryShader);
			FEATURE(multiview, vk11, multiviewTessellationShader);
			FEATURE(variable_pointers, vk11, variablePointersStorageBuffer);
			FEATURE(variable_pointers, vk11, variablePointers);
			// protected memory
			FEATURE(ycbcr_conversion, vk11, samplerYcbcrConversion);
			FEATURE(draw_parameters, vk11, shaderDrawParameters);
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
		{
			auto &vk12 = *reinterpret_cast<const VkPhysicalDeviceVulkan12Features *>(base);
			//samplerMirrorClampToEdge
			//drawIndirectCount
			FEATURE(storage_8bit, vk12, storageBuffer8BitAccess);
			FEATURE(storage_8bit, vk12, uniformAndStorageBuffer8BitAccess);
			FEATURE(storage_8bit, vk12, storagePushConstant8);
			FEATURE(atomic_int64, vk12, shaderBufferInt64Atomics);
			FEATURE(atomic_int64, vk12, shaderSharedInt64Atomics);
			FEATURE(float16_int8, vk12, shaderFloat16);
			FEATURE(float16_int8, vk12, shaderInt8);
			FEATURE(vk12, vk12, descriptorIndexing);
			FEATURE(descriptor_indexing, vk12, shaderInputAttachmentArrayDynamicIndexing);
			FEATURE(descriptor_indexing, vk12, shaderUniformTexelBufferArrayDynamicIndexing);
			FEATURE(descriptor_indexing, vk12, shaderStorageTexelBufferArrayDynamicIndexing);
			FEATURE(descriptor_indexing, vk12, shaderUniformBufferArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, shaderSampledImageArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, shaderStorageBufferArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, shaderStorageImageArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, shaderInputAttachmentArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, shaderUniformTexelBufferArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, shaderStorageTexelBufferArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, descriptorBindingUniformBufferUpdateAfterBind);
			FEATURE(descriptor_indexing, vk12, descriptorBindingSampledImageUpdateAfterBind);
			FEATURE(descriptor_indexing, vk12, descriptorBindingStorageImageUpdateAfterBind);
			FEATURE(descriptor_indexing, vk12, descriptorBindingStorageBufferUpdateAfterBind);
			FEATURE(descriptor_indexing, vk12, descriptorBindingUniformTexelBufferUpdateAfterBind);
			FEATURE(descriptor_indexing, vk12, descriptorBindingStorageTexelBufferUpdateAfterBind);
			FEATURE(descriptor_indexing, vk12, descriptorBindingUpdateUnusedWhilePending);
			FEATURE(descriptor_indexing, vk12, descriptorBindingPartiallyBound);
			FEATURE(descriptor_indexing, vk12, descriptorBindingVariableDescriptorCount);
			FEATURE(descriptor_indexing, vk12, runtimeDescriptorArray);
			FEATURE(vk12, vk12, samplerFilterMinmax);
			FEATURE(scalar_block_layout, vk12, scalarBlockLayout);
			//imagelessFramebuffer;
			//uniformBufferStandardLayout;
			FEATURE(subgroup_extended_types, vk12, shaderSubgroupExtendedTypes);
			FEATURE(separate_ds_layout, vk12, separateDepthStencilLayouts);
			//hostQueryReset;
			//timelineSemaphore;
			FEATURE(buffer_device_address, vk12, bufferDeviceAddress);
			FEATURE(buffer_device_address, vk12, bufferDeviceAddressCaptureReplay);
			FEATURE(buffer_device_address, vk12, bufferDeviceAddressMultiDevice);
			FEATURE(memory_model, vk12, vulkanMemoryModel);
			FEATURE(memory_model, vk12, vulkanMemoryModelDeviceScope);
			FEATURE(memory_model, vk12, vulkanMemoryModelAvailabilityVisibilityChains);
			FEATURE(vk12, vk12, shaderOutputViewportIndex);
			FEATURE(vk12, vk12, shaderOutputLayer);
			//subgroupBroadcastDynamicId;
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
		{
			auto &vk13 = *reinterpret_cast<const VkPhysicalDeviceVulkan13Features *>(base);
			FEATURE(image_robustness, vk13, robustImageAccess);
			FEATURE(inline_uniform_block, vk13, inlineUniformBlock);
			FEATURE(inline_uniform_block, vk13, descriptorBindingInlineUniformBlockUpdateAfterBind);
			//pipelineCreationCacheControl;
			//privateData;
			FEATURE(demote_to_helper, vk13, shaderDemoteToHelperInvocation);
			FEATURE(vk13, vk13, shaderTerminateInvocation);
			FEATURE(subgroup_size_control, vk13, subgroupSizeControl);
			FEATURE(subgroup_size_control, vk13, computeFullSubgroups);
			FEATURE(synchronization2, vk13, synchronization2);
			//textureCompressionASTC_HDR;
			FEATURE(zero_initialize_workgroup_memory, vk13, shaderZeroInitializeWorkgroupMemory);
			FEATURE(dynamic_rendering, vk13, dynamicRendering);
			FEATURE(shader_integer_dot_product, vk13, shaderIntegerDotProduct);
			FEATURE(maintenance4, vk13, maintenance4);
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES:
		{
			auto &vk14 = *reinterpret_cast<const VkPhysicalDeviceVulkan14Features *>(base);
			FEATURE(dynamic_rendering_local_read, vk14, dynamicRenderingLocalRead);
			FEATURE(shader_subgroup_rotate, vk14, shaderSubgroupRotate);
			FEATURE(shader_subgroup_rotate, vk14, shaderSubgroupRotateClustered);
			FEATURE(shader_float_controls2, vk14, shaderFloatControls2);
			FEATURE(expect_assume, vk14, shaderExpectAssume);
			FEATURE(line_rasterization, vk14, rectangularLines);
			FEATURE(line_rasterization, vk14, bresenhamLines);
			FEATURE(line_rasterization, vk14, smoothLines);
			FEATURE(line_rasterization, vk14, stippledRectangularLines);
			FEATURE(line_rasterization, vk14, stippledBresenhamLines);
			FEATURE(line_rasterization, vk14, stippledSmoothLines);
			FEATURE(attribute_divisor, vk14, vertexAttributeInstanceRateDivisor);
			FEATURE(attribute_divisor, vk14, vertexAttributeInstanceRateZeroDivisor);
			FEATURE(maintenance5, vk14, maintenance5);
			FEATURE(pipeline_protected_access, vk14, pipelineProtectedAccess);
			FEATURE(pipeline_robustness, vk14, pipelineRobustness);
			FEATURE(vk14, vk14, pushDescriptor);
			break;
		}

		default:
			break;
		}

#undef F
#undef FE
#undef FE_ALIAS
#undef FEATURE

		pNext = base->pNext;
	}
}

void FeatureFilter::Impl::init_properties(const void *pNext)
{
	while (pNext)
	{
		auto *base = static_cast<const VkBaseInStructure *>(pNext);

#define P(struct_type, member, ...) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES: \
		memcpy(&props.member, base, sizeof(props.member)); \
		props.member.pNext = nullptr; \
		break

#define P_CORE(struct_type, member, ...) P(struct_type, member, __VA_ARGS__)

#define PE(struct_type, member, ext) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES_##ext: \
		memcpy(&props.member, base, sizeof(props.member)); \
		props.member.pNext = nullptr; \
		break

#define PE_ALIAS(struct_type, member, ext, ext_alias) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES_##ext: \
		memcpy(&props.member, base, sizeof(props.member)); \
		props.member.pNext = nullptr; \
		break

#define PROP(struct_name, core_struct, prop) props.struct_name.prop = core_struct.prop
		switch (base->sType)
		{
#include "fossilize_feature_filter_properties.inc"

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
		{
			auto &vk11 = *reinterpret_cast<const VkPhysicalDeviceVulkan11Properties *>(base);
			PROP(subgroup, vk11, subgroupSize);
			props.subgroup.supportedStages = vk11.subgroupSupportedStages;
			props.subgroup.supportedOperations = vk11.subgroupSupportedOperations;
			props.subgroup.quadOperationsInAllStages = vk11.subgroupQuadOperationsInAllStages;
			//pointClippingBehavior;
			PROP(multiview, vk11, maxMultiviewViewCount);
			PROP(multiview, vk11, maxMultiviewInstanceIndex);
			//protectedNoFault;
			//maxPerSetDescriptors;
			//maxMemoryAllocationSize;
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
		{
			auto &vk12 = *reinterpret_cast<const VkPhysicalDeviceVulkan12Properties *>(base);
			//conformanceVersion;
			PROP(float_control, vk12, denormBehaviorIndependence);
			PROP(float_control, vk12, roundingModeIndependence);
			PROP(float_control, vk12, shaderSignedZeroInfNanPreserveFloat16);
			PROP(float_control, vk12, shaderSignedZeroInfNanPreserveFloat32);
			PROP(float_control, vk12, shaderSignedZeroInfNanPreserveFloat64);
			PROP(float_control, vk12, shaderDenormPreserveFloat16);
			PROP(float_control, vk12, shaderDenormPreserveFloat32);
			PROP(float_control, vk12, shaderDenormPreserveFloat64);
			PROP(float_control, vk12, shaderDenormFlushToZeroFloat16);
			PROP(float_control, vk12, shaderDenormFlushToZeroFloat32);
			PROP(float_control, vk12, shaderDenormFlushToZeroFloat64);
			PROP(float_control, vk12, shaderRoundingModeRTEFloat16);
			PROP(float_control, vk12, shaderRoundingModeRTEFloat32);
			PROP(float_control, vk12, shaderRoundingModeRTEFloat64);
			PROP(float_control, vk12, shaderRoundingModeRTZFloat16);
			PROP(float_control, vk12, shaderRoundingModeRTZFloat32);
			PROP(float_control, vk12, shaderRoundingModeRTZFloat64);
			PROP(descriptor_indexing, vk12, maxUpdateAfterBindDescriptorsInAllPools);
			PROP(descriptor_indexing, vk12, shaderUniformBufferArrayNonUniformIndexingNative);
			PROP(descriptor_indexing, vk12, shaderSampledImageArrayNonUniformIndexingNative);
			PROP(descriptor_indexing, vk12, shaderStorageBufferArrayNonUniformIndexingNative);
			PROP(descriptor_indexing, vk12, shaderStorageImageArrayNonUniformIndexingNative);
			PROP(descriptor_indexing, vk12, shaderInputAttachmentArrayNonUniformIndexingNative);
			PROP(descriptor_indexing, vk12, robustBufferAccessUpdateAfterBind);
			PROP(descriptor_indexing, vk12, quadDivergentImplicitLod);
			PROP(descriptor_indexing, vk12, maxPerStageDescriptorUpdateAfterBindSamplers);
			PROP(descriptor_indexing, vk12, maxPerStageDescriptorUpdateAfterBindUniformBuffers);
			PROP(descriptor_indexing, vk12, maxPerStageDescriptorUpdateAfterBindStorageBuffers);
			PROP(descriptor_indexing, vk12, maxPerStageDescriptorUpdateAfterBindSampledImages);
			PROP(descriptor_indexing, vk12, maxPerStageDescriptorUpdateAfterBindStorageImages);
			PROP(descriptor_indexing, vk12, maxPerStageDescriptorUpdateAfterBindInputAttachments);
			PROP(descriptor_indexing, vk12, maxPerStageUpdateAfterBindResources);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindSamplers);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindUniformBuffers);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindUniformBuffersDynamic);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindStorageBuffers);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindStorageBuffersDynamic);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindSampledImages);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindStorageImages);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindInputAttachments);
			PROP(ds_resolve, vk12, supportedDepthResolveModes);
			PROP(ds_resolve, vk12, supportedStencilResolveModes);
			PROP(ds_resolve, vk12, independentResolveNone);
			PROP(ds_resolve, vk12, independentResolve);
			//filterMinmaxSingleComponentFormats;
			//filterMinmaxImageComponentMapping;
			//maxTimelineSemaphoreValueDifference;
			//framebufferIntegerColorSampleCounts;
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES:
		{
			auto &vk13 = *reinterpret_cast<const VkPhysicalDeviceVulkan13Properties *>(base);
			PROP(subgroup_size_control, vk13, minSubgroupSize);
			PROP(subgroup_size_control, vk13, maxSubgroupSize);
			PROP(subgroup_size_control, vk13, maxComputeWorkgroupSubgroups);
			PROP(subgroup_size_control, vk13, requiredSubgroupSizeStages);
			PROP(inline_uniform_block, vk13, maxInlineUniformBlockSize);
			PROP(inline_uniform_block, vk13, maxPerStageDescriptorInlineUniformBlocks);
			PROP(inline_uniform_block, vk13, maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks);
			PROP(inline_uniform_block, vk13, maxDescriptorSetInlineUniformBlocks);
			PROP(inline_uniform_block, vk13, maxDescriptorSetUpdateAfterBindInlineUniformBlocks);
			PROP(vk13, vk13, maxInlineUniformTotalSize);
			// integer dot product hints
			//storageTexelBufferOffsetAlignmentBytes;
			//storageTexelBufferOffsetSingleTexelAlignment;
			//uniformTexelBufferOffsetAlignmentBytes;
			//uniformTexelBufferOffsetSingleTexelAlignment;
			//maxBufferSize;
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES:
		{
			auto &vk14 = *reinterpret_cast<const VkPhysicalDeviceVulkan14Properties *>(base);
			PROP(attribute_divisor, vk14, maxVertexAttribDivisor);
			PROP(push_descriptor, vk14, maxPushDescriptors);
			break;
		}

		default:
			break;
		}

#undef P
#undef PE
#undef PE_ALIAS
#undef PROP

		pNext = base->pNext;
	}
}

bool FeatureFilter::Impl::init(uint32_t api_version_, const char **device_exts, unsigned count,
                               const VkPhysicalDeviceFeatures2 *enabled_features,
                               const VkPhysicalDeviceProperties2 *properties)
{
	for (unsigned i = 0; i < count; i++)
		enabled_extensions.insert(device_exts[i]);

	api_version = api_version_;
	props2 = *properties;
	features2 = *enabled_features;

	init_features(enabled_features->pNext);
	init_properties(properties->pNext);

	return true;
}

bool FeatureFilter::init(uint32_t api_version, const char **device_exts, unsigned count,
                         const VkPhysicalDeviceFeatures2 *enabled_features, const VkPhysicalDeviceProperties2 *props)
{
	return impl->init(api_version, device_exts, count, enabled_features, props);
}

bool FeatureFilter::init_null_device()
{
	impl->null_device = true;
	return true;
}

bool FeatureFilter::Impl::multiview_mask_is_supported(uint32_t mask) const
{
	uint32_t required_views = 0;
	for (uint32_t bits = mask; bits; bits >>= 1)
		required_views++;
	const uint32_t allowed_mask =
			(props.multiview.maxMultiviewViewCount >= 32 ? 0u : (1u << props.multiview.maxMultiviewViewCount)) - 1u;

	return (mask & allowed_mask) == mask || support_reject(SupportKind::Property, "maxMultiviewViewCount", 3,
														   required_views, props.multiview.maxMultiviewViewCount);
}

bool FeatureFilter::Impl::descriptor_set_and_binding_mapping_is_supported(
	const VkShaderDescriptorSetAndBindingMappingInfoEXT &info) const
{
	for (uint32_t i = 0; i < info.mappingCount; i++)
	{
		auto &mapping = info.pMappings[i];
		VkDeviceSize minimum_alignment =
				(std::min)(props.descriptor_heap.bufferDescriptorAlignment,
				           props.descriptor_heap.imageDescriptorAlignment);
		minimum_alignment = (std::min)(minimum_alignment, props.descriptor_heap.samplerDescriptorAlignment);
		constexpr VkSpirvResourceTypeFlagsEXT image_types =
			VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_READ_ONLY_IMAGE_BIT_EXT |
			VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT;
		constexpr VkSpirvResourceTypeFlagsEXT buffer_types = VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT |
			VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT |
			VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT |
			VK_SPIRV_RESOURCE_TYPE_ACCELERATION_STRUCTURE_BIT_EXT;
		bool binds_to_any = mapping.resourceMask == VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
		bool binds_to_image = (mapping.resourceMask & image_types) != 0 && !binds_to_any;
		bool binds_to_buffer = (mapping.resourceMask & buffer_types) != 0 && !binds_to_any;
		bool binds_to_sampler = (mapping.resourceMask & VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT) != 0 && !binds_to_any;
		bool binds_to_combined_sampler =
			(mapping.resourceMask & VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT) != 0;
		VkDeviceSize target_alignment = 1;
		VkDeviceSize combined_alignment = 1;
		if (binds_to_image)
			target_alignment = (std::max)(target_alignment, props.descriptor_heap.imageDescriptorAlignment);
		if (binds_to_buffer)
			target_alignment = (std::max)(target_alignment, props.descriptor_heap.bufferDescriptorAlignment);
		if (binds_to_sampler)
			target_alignment = (std::max)(target_alignment, props.descriptor_heap.samplerDescriptorAlignment);
		if (binds_to_combined_sampler)
			combined_alignment = (std::max)(combined_alignment, props.descriptor_heap.samplerDescriptorAlignment);
		if (binds_to_any)
		{
			target_alignment = (std::max)(target_alignment, minimum_alignment);
			combined_alignment = (std::max)(combined_alignment, props.descriptor_heap.samplerDescriptorAlignment);
		}
		const char *alignment_property = "descriptorHeapAlignment";
		if ((binds_to_buffer || binds_to_any) && target_alignment == props.descriptor_heap.bufferDescriptorAlignment)
			alignment_property = "bufferDescriptorAlignment";
		else if ((binds_to_image || binds_to_any) && target_alignment == props.descriptor_heap.imageDescriptorAlignment)
			alignment_property = "imageDescriptorAlignment";
		else if ((binds_to_sampler || binds_to_any) &&
				 target_alignment == props.descriptor_heap.samplerDescriptorAlignment)
			alignment_property = "samplerDescriptorAlignment";
		target_alignment--;
		combined_alignment--;
		const auto validate_alignments = [&](const auto &m)
		{
			bool mapping_is_pure_embedded_sampler =
				m.pEmbeddedSampler != nullptr && mapping.resourceMask == VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
			if (!mapping_is_pure_embedded_sampler)
			{
				if (m.heapOffset & target_alignment)
					return support_reject(SupportKind::Property, alignment_property, 3, m.heapOffset,
										  target_alignment + 1);
				if (m.heapArrayStride & target_alignment)
					return support_reject(SupportKind::Property, alignment_property, 3, m.heapArrayStride,
										  target_alignment + 1);
			}
			if (binds_to_combined_sampler)
			{
				if (m.samplerHeapOffset & combined_alignment)
					return support_reject(SupportKind::Property, "samplerDescriptorAlignment", 3, m.samplerHeapOffset,
										  combined_alignment + 1);
				if (m.samplerHeapArrayStride & combined_alignment)
					return support_reject(SupportKind::Property, "samplerDescriptorAlignment", 3,
										  m.samplerHeapArrayStride, combined_alignment + 1);
			}
			return true;
		};
		// Validating alignment is tricky, since it depends on the actual types used in the shader.
		// Hope nothing blows up. It's possible we need to look at the shader in question to get exact validity,
		// but that'd be needlessly annoying.
		// If the application is using explicit type mapping, we validate as intended.
		// For ALL mapping, we accept it if the minimum alignment requirement is sound.
		switch (mapping.source)
		{
		case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT:
			if (!validate_alignments(mapping.sourceData.constantOffset))
				return false;
			break;
		case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT:
			if (!validate_alignments(mapping.sourceData.pushIndex))
				return false;
			break;
		case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_ARRAY_EXT:
		{
			auto &m = mapping.sourceData.indirectIndexArray;
			bool mapping_is_pure_embedded_sampler =
				m.pEmbeddedSampler != nullptr && mapping.resourceMask == VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
			// IndexArray does not have array stride since each individual descriptor has its own indirection.
			if (!mapping_is_pure_embedded_sampler)
				if (m.heapOffset & target_alignment)
					return support_reject(SupportKind::Property, alignment_property, 3, m.heapOffset,
										  target_alignment + 1);
			if (binds_to_combined_sampler)
				if (m.samplerHeapOffset & combined_alignment)
					return support_reject(SupportKind::Property, "samplerDescriptorAlignment", 3, m.samplerHeapOffset,
										  combined_alignment + 1);
			break;
		}
		case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_EXT:
			if (!validate_alignments(mapping.sourceData.indirectIndex))
				return false;
			break;
		case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_SHADER_RECORD_INDEX_EXT:
			if (!validate_alignments(mapping.sourceData.shaderRecordIndex))
				return false;
			break;
		default:
			break;
		}
	}
	return true;
}

bool FeatureFilter::Impl::pnext_chain_is_supported(const void *pNext) const
{
	while (pNext)
	{
		auto *base = static_cast<const VkBaseInStructure *>(pNext);
		// These are the currently pNext structs which Fossilize serialize.
		switch (base->sType)
		{
		case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
			if (!enabled_extensions.count(VK_KHR_MAINTENANCE2_EXTENSION_NAME) && api_version < VK_API_VERSION_1_1)
				return support_reject(SupportKind::Extension, VK_KHR_MAINTENANCE2_EXTENSION_NAME, 3, 1, 0);
			break;
		case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR:
		{
			if (!enabled_extensions.count(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) &&
			    !enabled_extensions.count(VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))
			{
				if (!enabled_extensions.count(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))
					return support_reject(SupportKind::Extension, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, 3, 1,
										  0);
				if (!enabled_extensions.count(VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))
					return support_reject(SupportKind::Extension, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, 3, 1,
										  0);
				return false;
			}

			auto *divisor = static_cast<const VkPipelineVertexInputDivisorStateCreateInfoEXT *>(pNext);
			bool use_zero_divisor = false;
			bool use_non_identity_divisor = false;
			uint32_t max_divisor = 0;

			for (uint32_t i = 0; i < divisor->vertexBindingDivisorCount; i++)
			{
				if (divisor->pVertexBindingDivisors[i].divisor != 1)
					use_non_identity_divisor = true;
				if (divisor->pVertexBindingDivisors[i].divisor == 0)
					use_zero_divisor = true;
				max_divisor = (std::max)(max_divisor, divisor->pVertexBindingDivisors[i].divisor);
			}
			if (max_divisor > props.attribute_divisor.maxVertexAttribDivisor)
				return support_reject(SupportKind::Property, "maxVertexAttribDivisor", 3, uint64_t(max_divisor),
									  uint64_t(props.attribute_divisor.maxVertexAttribDivisor));
			if (use_zero_divisor && !features.attribute_divisor.vertexAttributeInstanceRateZeroDivisor)
				return support_reject(SupportKind::Feature, "vertexAttributeInstanceRateZeroDivisor", 3, 1,
									  uint64_t(features.attribute_divisor.vertexAttributeInstanceRateZeroDivisor));
			if (use_non_identity_divisor && !features.attribute_divisor.vertexAttributeInstanceRateDivisor)
				return support_reject(SupportKind::Feature, "vertexAttributeInstanceRateDivisor", 3, 1,
									  uint64_t(features.attribute_divisor.vertexAttributeInstanceRateDivisor));
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
		{
			auto *clip = static_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT *>(pNext);
			if (clip->depthClipEnable && !features.depth_clip.depthClipEnable)
				return support_reject(SupportKind::Feature, "depthClipEnable", 3, 1,
									  uint64_t(features.depth_clip.depthClipEnable));
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT:
			if (features.transform_feedback.geometryStreams == VK_FALSE)
				return support_reject(SupportKind::Feature, "geometryStreams", 3, 1,
									  uint64_t(features.transform_feedback.geometryStreams));
			break;
		case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO:
		{
			if (features.multiview.multiview == VK_FALSE)
				return support_reject(SupportKind::Feature, "multiview", 3, 1, uint64_t(features.multiview.multiview));
			auto *multiview = static_cast<const VkRenderPassMultiviewCreateInfo *>(pNext);
			for (uint32_t i = 0; i < multiview->subpassCount; i++)
			{
				if (!multiview_mask_is_supported(multiview->pViewMasks[i]))
					return false;
			}
			for (uint32_t i = 0; i < multiview->correlationMaskCount; i++)
			{
				if (!multiview_mask_is_supported(multiview->pCorrelationMasks[i]))
					return false;
			}
			break;
		}
		case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT:
		{
			auto *flags = static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo *>(pNext);
			VkDescriptorBindingFlagsEXT flag_union = 0;
			for (uint32_t i = 0; i < flags->bindingCount; i++)
				flag_union |= flags->pBindingFlags[i];
			if ((flag_union & VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT) != 0 &&
			    features.descriptor_indexing.descriptorBindingUpdateUnusedWhilePending == VK_FALSE)
			{
				return support_reject(SupportKind::Feature, "descriptorBindingUpdateUnusedWhilePending", 3, 1,
									  uint64_t(features.descriptor_indexing.descriptorBindingUpdateUnusedWhilePending));
			}
			if ((flag_union & VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT) != 0 &&
			    features.descriptor_indexing.descriptorBindingPartiallyBound == VK_FALSE)
			{
				return support_reject(SupportKind::Feature, "descriptorBindingPartiallyBound", 3, 1,
									  uint64_t(features.descriptor_indexing.descriptorBindingPartiallyBound));
			}
			if ((flag_union & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0 &&
			    features.descriptor_indexing.descriptorBindingVariableDescriptorCount == VK_FALSE)
			{
				return support_reject(SupportKind::Feature, "descriptorBindingVariableDescriptorCount", 3, 1,
									  uint64_t(features.descriptor_indexing.descriptorBindingVariableDescriptorCount));
			}
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT:
			if (features.blend_operation_advanced.advancedBlendCoherentOperations == VK_FALSE)
				return support_reject(SupportKind::Feature, "advancedBlendCoherentOperations", 3, 1,
									  uint64_t(features.blend_operation_advanced.advancedBlendCoherentOperations));
			break;
		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT:
			if (!enabled_extensions.count(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME))
				return support_reject(SupportKind::Extension, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME, 3, 1,
									  0);
			break;
		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_KHR:
		{
			auto *line = static_cast<const VkPipelineRasterizationLineStateCreateInfoKHR *>(pNext);
			if (line->lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR &&
			    features.line_rasterization.rectangularLines == VK_FALSE)
			{
				return support_reject(SupportKind::Feature, "rectangularLines", 3, 1,
									  uint64_t(features.line_rasterization.rectangularLines));
			}
			if (line->lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR &&
			    features.line_rasterization.bresenhamLines == VK_FALSE)
			{
				return support_reject(SupportKind::Feature, "bresenhamLines", 3, 1,
									  uint64_t(features.line_rasterization.bresenhamLines));
			}
			if (line->lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR &&
			    features.line_rasterization.smoothLines == VK_FALSE)
			{
				return support_reject(SupportKind::Feature, "smoothLines", 3, 1,
									  uint64_t(features.line_rasterization.smoothLines));
			}
			if (line->stippledLineEnable == VK_TRUE)
			{
				if (line->lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR &&
				    features.line_rasterization.stippledRectangularLines == VK_FALSE)
				{
					return support_reject(SupportKind::Feature, "stippledRectangularLines", 3, 1,
										  uint64_t(features.line_rasterization.stippledRectangularLines));
				}
				if (line->lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR &&
				    features.line_rasterization.stippledBresenhamLines == VK_FALSE)
				{
					return support_reject(SupportKind::Feature, "stippledBresenhamLines", 3, 1,
										  uint64_t(features.line_rasterization.stippledBresenhamLines));
				}
				if (line->lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR &&
				    features.line_rasterization.stippledSmoothLines == VK_FALSE)
				{
					return support_reject(SupportKind::Feature, "stippledSmoothLines", 3, 1,
										  uint64_t(features.line_rasterization.stippledSmoothLines));
				}
			}
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT:
			// The details are validated explicitly elsewhere since we need to know the shader create info to deduce correctness.
			break;
		case VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT:
		{
			if (features.mutable_descriptor_type.mutableDescriptorType == VK_FALSE)
				return support_reject(SupportKind::Feature, "mutableDescriptorType", 3, 1,
									  uint64_t(features.mutable_descriptor_type.mutableDescriptorType));
			auto *lists = static_cast<const VkMutableDescriptorTypeCreateInfoEXT *>(pNext);
			for (uint32_t i = 0; i < lists->mutableDescriptorTypeListCount; i++)
			{
				for (uint32_t j = 0; j < lists->pMutableDescriptorTypeLists[i].descriptorTypeCount; j++)
				{
					switch (lists->pMutableDescriptorTypeLists[i].pDescriptorTypes[j])
					{
					case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
					case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
					case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
					case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
					case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
					case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
					case VK_DESCRIPTOR_TYPE_SAMPLER:
						break;
					default:
						// Implementation can theoretically support beyond this (and we'd have to query support),
						// but validate against what is required.
						return support_reject(SupportKind::Constraint, "VkDescriptorType", SupportDetail::Raw, 0, 0,
											  uint64_t(lists->pMutableDescriptorTypeLists[i].pDescriptorTypes[j]));
					}
				}
			}
			break;
		}
		case VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT:
		{
			if (!features.separate_ds_layout.separateDepthStencilLayouts)
				return support_reject(SupportKind::Feature, "separateDepthStencilLayouts", 3, 1,
									  uint64_t(features.separate_ds_layout.separateDepthStencilLayouts));
			auto *layout = static_cast<const VkAttachmentDescriptionStencilLayout *>(pNext);
			if (!image_layout_is_supported(layout->stencilInitialLayout))
				return false;
			if (!image_layout_is_supported(layout->stencilFinalLayout))
				return false;
			break;
		}
		case VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT:
		{
			if (!features.separate_ds_layout.separateDepthStencilLayouts)
				return support_reject(SupportKind::Feature, "separateDepthStencilLayouts", 3, 1,
									  uint64_t(features.separate_ds_layout.separateDepthStencilLayouts));
			auto *layout = static_cast<const VkAttachmentReferenceStencilLayout *>(pNext);
			if (!image_layout_is_supported(layout->stencilLayout))
				return false;
			break;
		}
		case VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE:
		{
			if (api_version < VK_API_VERSION_1_2 && !enabled_extensions.count(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME))
				return support_reject(SupportKind::Extension, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, 3, 1, 0);
			auto *resolve = static_cast<const VkSubpassDescriptionDepthStencilResolve *>(pNext);
			if (!resolve->pDepthStencilResolveAttachment)
				break;
			if (!attachment_reference2_is_supported(*resolve->pDepthStencilResolveAttachment))
				return false;
			if (resolve->depthResolveMode &&
			    (props.ds_resolve.supportedDepthResolveModes & resolve->depthResolveMode) == 0)
				return support_reject(SupportKind::Property, "supportedDepthResolveModes", 3,
									  uint64_t(resolve->depthResolveMode),
									  uint64_t(props.ds_resolve.supportedDepthResolveModes));
			if (resolve->stencilResolveMode &&
			    (props.ds_resolve.supportedStencilResolveModes & resolve->stencilResolveMode) == 0)
				return support_reject(SupportKind::Property, "supportedStencilResolveModes", 3,
									  uint64_t(resolve->stencilResolveMode),
									  uint64_t(props.ds_resolve.supportedStencilResolveModes));
			if (resolve->depthResolveMode != resolve->stencilResolveMode)
			{
				bool use_zero = resolve->depthResolveMode == VK_RESOLVE_MODE_NONE ||
				                resolve->stencilResolveMode == VK_RESOLVE_MODE_NONE;
				auto cond = use_zero ?
				            props.ds_resolve.independentResolveNone :
				            props.ds_resolve.independentResolve;
				if (!cond)
					return support_reject(SupportKind::Property,
										  use_zero ? "independentResolveNone" : "independentResolve", 3, 1, cond);
			}
			if (resolve->depthResolveMode == VK_RESOLVE_MODE_EXTERNAL_FORMAT_DOWNSAMPLE_ANDROID ||
			    resolve->stencilResolveMode == VK_RESOLVE_MODE_EXTERNAL_FORMAT_DOWNSAMPLE_ANDROID)
				return support_reject(SupportKind::Constraint, "VkResolveModeFlagBits", SupportDetail::Raw, 0, 0,
									  VK_RESOLVE_MODE_EXTERNAL_FORMAT_DOWNSAMPLE_ANDROID);
			break;
		}
		case VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR:
		{
			if (!features.fragment_shading_rate.attachmentFragmentShadingRate)
				return support_reject(SupportKind::Feature, "attachmentFragmentShadingRate", 3, 1,
									  uint64_t(features.fragment_shading_rate.attachmentFragmentShadingRate));
			auto *attachment = static_cast<const VkFragmentShadingRateAttachmentInfoKHR *>(pNext);
			if (!attachment->pFragmentShadingRateAttachment)
				break;
			if (!attachment_reference2_is_supported(*attachment->pFragmentShadingRateAttachment))
				return false;
			uint32_t width = attachment->shadingRateAttachmentTexelSize.width;
			uint32_t height = attachment->shadingRateAttachmentTexelSize.height;
			if (width == 0 || height == 0)
				return support_reject(SupportKind::Constraint,
									  width == 0 ? "fragmentShadingRateAttachmentTexelSize.width"
												 : "fragmentShadingRateAttachmentTexelSize.height",
									  3, 1, 0);
			if (width < props.fragment_shading_rate.minFragmentShadingRateAttachmentTexelSize.width)
				return support_reject(
					SupportKind::Property, "minFragmentShadingRateAttachmentTexelSize.width", 3, uint64_t(width),
					uint64_t(props.fragment_shading_rate.minFragmentShadingRateAttachmentTexelSize.width));
			if (width > props.fragment_shading_rate.maxFragmentShadingRateAttachmentTexelSize.width)
				return support_reject(
					SupportKind::Property, "maxFragmentShadingRateAttachmentTexelSize.width", 3, uint64_t(width),
					uint64_t(props.fragment_shading_rate.maxFragmentShadingRateAttachmentTexelSize.width));
			if (height < props.fragment_shading_rate.minFragmentShadingRateAttachmentTexelSize.height)
				return support_reject(
					SupportKind::Property, "minFragmentShadingRateAttachmentTexelSize.height", 3, uint64_t(height),
					uint64_t(props.fragment_shading_rate.minFragmentShadingRateAttachmentTexelSize.height));
			if (height > props.fragment_shading_rate.maxFragmentShadingRateAttachmentTexelSize.height)
				return support_reject(
					SupportKind::Property, "maxFragmentShadingRateAttachmentTexelSize.height", 3, uint64_t(height),
					uint64_t(props.fragment_shading_rate.maxFragmentShadingRateAttachmentTexelSize.height));
			uint32_t higher = (std::max)(width, height);
			uint32_t lower = (std::min)(width, height);
			uint32_t aspect = higher / lower;
			if (aspect > props.fragment_shading_rate.maxFragmentShadingRateAttachmentTexelSizeAspectRatio)
				return support_reject(
					SupportKind::Property, "maxFragmentShadingRateAttachmentTexelSizeAspectRatio", 3, uint64_t(aspect),
					uint64_t(props.fragment_shading_rate.maxFragmentShadingRateAttachmentTexelSizeAspectRatio));
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR:
		{
			if (features.dynamic_rendering.dynamicRendering == VK_FALSE)
				return support_reject(SupportKind::Feature, "dynamicRendering", 3, 1,
									  uint64_t(features.dynamic_rendering.dynamicRendering));
			auto *info = static_cast<const VkPipelineRenderingCreateInfoKHR *>(pNext);
			for (uint32_t i = 0; i < info->colorAttachmentCount; i++)
			{
				if (info->pColorAttachmentFormats[i] != VK_FORMAT_UNDEFINED &&
				    !format_is_supported(info->pColorAttachmentFormats[i], VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
				{
					return false;
				}
			}
			if (info->depthAttachmentFormat != VK_FORMAT_UNDEFINED &&
			    !format_is_supported(info->depthAttachmentFormat, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
			{
				return false;
			}
			if (info->stencilAttachmentFormat != VK_FORMAT_UNDEFINED &&
			    !format_is_supported(info->stencilAttachmentFormat, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
			{
				return false;
			}
			if (info->depthAttachmentFormat != VK_FORMAT_UNDEFINED &&
			    info->stencilAttachmentFormat != VK_FORMAT_UNDEFINED &&
			    info->depthAttachmentFormat != info->stencilAttachmentFormat)
			{
				return support_reject(SupportKind::Constraint, "depthStencilAttachmentFormatMatch", 3,
									  info->depthAttachmentFormat, info->stencilAttachmentFormat);
			}
			if (info->viewMask && !multiview_mask_is_supported(info->viewMask))
				return false;
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT:
		{
			if (!features.color_write_enable.colorWriteEnable)
				return support_reject(SupportKind::Feature, "colorWriteEnable", 3, 1,
									  uint64_t(features.color_write_enable.colorWriteEnable));
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
		{
			if (!enabled_extensions.count(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME))
				return support_reject(SupportKind::Extension, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, 3, 1, 0);
			auto *info = static_cast<const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT *>(pNext);
			if (info->provokingVertexMode == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT &&
			    !features.provoking_vertex.provokingVertexLast)
				return support_reject(SupportKind::Feature, "provokingVertexLast", 3, 1,
									  uint64_t(features.provoking_vertex.provokingVertexLast));
			break;
		}
		case VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT:
		{
			if (!enabled_extensions.count(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME))
				return support_reject(SupportKind::Extension, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, 3, 1, 0);
			auto *info = static_cast<const VkSamplerCustomBorderColorCreateInfoEXT *>(pNext);
			if (info->format == VK_FORMAT_UNDEFINED &&
			    !features.custom_border_color.customBorderColorWithoutFormat)
				return support_reject(SupportKind::Feature, "customBorderColorWithoutFormat", 3, 1,
									  uint64_t(features.custom_border_color.customBorderColorWithoutFormat));
			break;
		}
		case VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO:
		{
			if (!enabled_extensions.count(VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME) && !features.vk12.samplerFilterMinmax)
			{
				if (!enabled_extensions.count(VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME))
					return support_reject(SupportKind::Extension, VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME, 3, 1, 0);
				if (!features.vk12.samplerFilterMinmax)
					return support_reject(SupportKind::Feature, "samplerFilterMinmax", 3, 1,
										  uint64_t(features.vk12.samplerFilterMinmax));
				return false;
			}

			auto *info = static_cast<const VkSamplerReductionModeCreateInfo *>(pNext);
			switch (info->reductionMode)
			{
			case VK_SAMPLER_REDUCTION_MODE_MAX:
			case VK_SAMPLER_REDUCTION_MODE_MIN:
			case VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE:
				break;
			default:
				return support_reject(SupportKind::Constraint, "VkSamplerReductionMode", SupportDetail::Raw, 0, 0,
									  uint64_t(info->reductionMode));
			}
			break;
		}
		case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO:
		{
			if (!enabled_extensions.count(VK_KHR_MAINTENANCE_2_EXTENSION_NAME) && api_version < VK_API_VERSION_1_1)
				return support_reject(SupportKind::Extension, VK_KHR_MAINTENANCE_2_EXTENSION_NAME, 3, 1, 0);
			auto *info = static_cast<const VkRenderPassInputAttachmentAspectCreateInfo *>(pNext);
			for (uint32_t i = 0; i < info->aspectReferenceCount; i++)
				if (!aspect_mask_is_supported(info->pAspectReferences[i].aspectMask))
					return false;
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT:
		{
			if (!enabled_extensions.count(VK_EXT_DISCARD_RECTANGLES_EXTENSION_NAME))
				return support_reject(SupportKind::Extension, VK_EXT_DISCARD_RECTANGLES_EXTENSION_NAME, 3, 1, 0);
			break;
		}
		case VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR:
		{
			if (!features.synchronization2.synchronization2)
				return support_reject(SupportKind::Feature, "synchronization2", 3, 1,
									  uint64_t(features.synchronization2.synchronization2));
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR:
		{
			if (!features.fragment_shading_rate.pipelineFragmentShadingRate)
				return support_reject(SupportKind::Feature, "pipelineFragmentShadingRate", 3, 1,
									  uint64_t(features.fragment_shading_rate.pipelineFragmentShadingRate));
			break;
		}
		case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO:
		case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO:
		{
			// Should also validate formats and all sorts of feature bits, but
			// consider that TODO for now.
			// YcbcrConversionCreateInfo is inlined into a VkSamplerCreateInfo when replaying from a Fossilize archive.
			// Normally, it's not a pNext, but we pretend it is to make capture and replay
			// a bit more sane.
			if (!features.ycbcr_conversion.samplerYcbcrConversion)
				return support_reject(SupportKind::Feature, "samplerYcbcrConversion", 3, 1,
									  uint64_t(features.ycbcr_conversion.samplerYcbcrConversion));
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR:
		case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:
		{
			if (features.graphics_pipeline_library.graphicsPipelineLibrary == VK_FALSE)
				return support_reject(SupportKind::Feature, "graphicsPipelineLibrary", 3, 1,
									  uint64_t(features.graphics_pipeline_library.graphicsPipelineLibrary));
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT:
		{
			if (!enabled_extensions.count(VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME))
				return support_reject(SupportKind::Extension, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME, 3, 1, 0);
			auto *info = static_cast<const VkPipelineSampleLocationsStateCreateInfoEXT *>(pNext);
			// If count is 0, this is part of dynamic state, so just ignore it.
			if (!info->sampleLocationsEnable || info->sampleLocationsInfo.sampleLocationsCount == 0)
				break;
			if (info->sampleLocationsInfo.sampleLocationGridSize.width > props.sample_locations.maxSampleLocationGridSize.width ||
			    info->sampleLocationsInfo.sampleLocationGridSize.height > props.sample_locations.maxSampleLocationGridSize.height)
			{
				{
					if (info->sampleLocationsInfo.sampleLocationGridSize.width >
						props.sample_locations.maxSampleLocationGridSize.width)
						return support_reject(SupportKind::Property, "maxSampleLocationGridSize.width", 3,
											  info->sampleLocationsInfo.sampleLocationGridSize.width,
											  props.sample_locations.maxSampleLocationGridSize.width);
					return support_reject(SupportKind::Property, "maxSampleLocationGridSize.height", 3,
										  info->sampleLocationsInfo.sampleLocationGridSize.height,
										  props.sample_locations.maxSampleLocationGridSize.height);
				}
			}

			if ((props.sample_locations.sampleLocationSampleCounts & info->sampleLocationsInfo.sampleLocationsPerPixel) == 0)
				return support_reject(SupportKind::Property, "sampleLocationSampleCounts", 3,
									  uint64_t(info->sampleLocationsInfo.sampleLocationsPerPixel),
									  uint64_t(props.sample_locations.sampleLocationSampleCounts));

			// Sample positions are clamped by implementation.
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT:
		{
			if (!features.depth_clip_control.depthClipControl)
			{
				return support_reject(SupportKind::Feature, "depthClipControl", 3, 1,
									  uint64_t(features.depth_clip_control.depthClipControl));
			}
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT:
		{
			if (enabled_extensions.count(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME) == 0)
				return support_reject(SupportKind::Extension, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME, 3, 1, 0);
			auto *info = static_cast<const VkPipelineRobustnessCreateInfoEXT *>(pNext);
			if ((info->storageBuffers != VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT ||
			     info->images != VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DEVICE_DEFAULT_EXT ||
			     info->uniformBuffers != VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT ||
			     info->vertexInputs != VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT) &&
			    features.pipeline_robustness.pipelineRobustness == VK_FALSE)
			{
				return support_reject(SupportKind::Feature, "pipelineRobustness", 3, 1,
									  uint64_t(features.pipeline_robustness.pipelineRobustness));
			}
			if (!query)
				return support_reject(SupportKind::Constraint, "physicalDeviceFeatureQuery", SupportDetail::Required,
									  1);
			bool need_robust_image =
					info->images == VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_EXT;
			bool need_robust_image2 =
					info->images == VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2_EXT;
			bool need_robust_buffer2 =
					info->storageBuffers == VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT ||
					info->vertexInputs == VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT ||
					info->uniformBuffers == VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT;
			// From spec:
			// Any component of the implementation (the loader, any enabled layers, and drivers) must skip over,
			// without processing (other than reading the sType and pNext members)
			// any extending structures in the chain not defined by core versions
			// or extensions supported by that component.
			// I.e. it's safe for us to query the struct, even if the extension is not supported.
			if (need_robust_image)
			{
				VkPhysicalDeviceFeatures2 pdf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
				VkPhysicalDeviceImageRobustnessFeaturesEXT robust_image =
						{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT };
				pdf2.pNext = &robust_image;
				query->physical_device_feature_query(&pdf2);
				if (robust_image.robustImageAccess == VK_FALSE)
					return support_reject(SupportKind::Feature, "robustImageAccess", 3, 1,
										  robust_image.robustImageAccess);
			}
			if (need_robust_image2)
			{
				VkPhysicalDeviceFeatures2 pdf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
				VkPhysicalDeviceRobustness2FeaturesEXT robust2 =
						{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
				pdf2.pNext = &robust2;
				query->physical_device_feature_query(&pdf2);
				if (robust2.robustImageAccess2 == VK_FALSE)
					return support_reject(SupportKind::Feature, "robustImageAccess2", 3, 1, robust2.robustImageAccess2);
			}
			if (need_robust_buffer2)
			{
				VkPhysicalDeviceFeatures2 pdf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
				VkPhysicalDeviceRobustness2FeaturesEXT robust2 =
						{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
				pdf2.pNext = &robust2;
				query->physical_device_feature_query(&pdf2);
				if (robust2.robustBufferAccess2 == VK_FALSE)
					return support_reject(SupportKind::Feature, "robustBufferAccess2", 3, 1,
										  robust2.robustBufferAccess2);
			}
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR:
			// Actual flags are validated separately.
			if (features.maintenance5.maintenance5 == VK_FALSE)
				return support_reject(SupportKind::Feature, "maintenance5", 3, 1,
									  uint64_t(features.maintenance5.maintenance5));
			break;
		case VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_CONTROL_EXT:
			if (features.subpass_merge_feedback.subpassMergeFeedback == VK_FALSE)
				return support_reject(SupportKind::Feature, "subpassMergeFeedback", 3, 1,
									  uint64_t(features.subpass_merge_feedback.subpassMergeFeedback));
			break;
		case VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT:
			if (features.border_color_swizzle.borderColorSwizzle == VK_FALSE)
				return support_reject(SupportKind::Feature, "borderColorSwizzle", 3, 1,
									  uint64_t(features.border_color_swizzle.borderColorSwizzle));
			break;
		case VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT:
			if (features.multisampled_render_to_single_sampled.multisampledRenderToSingleSampled == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "multisampledRenderToSingleSampled", 3, 1,
					uint64_t(features.multisampled_render_to_single_sampled.multisampledRenderToSingleSampled));
			break;
		case VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT:
		{
			if (enabled_extensions.count(VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME) == 0)
				return support_reject(SupportKind::Extension, VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME, 3, 1, 0);
			auto *info = static_cast<const VkDepthBiasRepresentationInfoEXT *>(pNext);
			if (info->depthBiasRepresentation == VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT &&
			    features.depth_bias_control.leastRepresentableValueForceUnormRepresentation == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "leastRepresentableValueForceUnormRepresentation", 3, 1,
					uint64_t(features.depth_bias_control.leastRepresentableValueForceUnormRepresentation));
			if (info->depthBiasRepresentation == VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT &&
			    features.depth_bias_control.floatRepresentation == VK_FALSE)
				return support_reject(SupportKind::Feature, "floatRepresentation", 3, 1,
									  uint64_t(features.depth_bias_control.floatRepresentation));
			break;
		}
		case VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT:
			if (features.fragment_density.fragmentDensityMap == VK_FALSE)
				return support_reject(SupportKind::Feature, "fragmentDensityMap", 3, 1,
									  uint64_t(features.fragment_density.fragmentDensityMap));
			break;
		case VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT:
			if (enabled_extensions.count(VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME) == 0)
				return support_reject(SupportKind::Extension, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME, 3, 1, 0);
			break;
		case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLAMP_CONTROL_CREATE_INFO_EXT:
			if (features.depth_clamp_control.depthClampControl == VK_FALSE)
				return support_reject(SupportKind::Feature, "depthClampControl", 3, 1,
									  uint64_t(features.depth_clamp_control.depthClampControl));
			break;
		case VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO_KHR:
		case VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO_KHR:
		{
			if (features.dynamic_rendering_local_read.dynamicRenderingLocalRead == VK_FALSE)
				return support_reject(SupportKind::Feature, "dynamicRenderingLocalRead", 3, 1,
									  uint64_t(features.dynamic_rendering_local_read.dynamicRenderingLocalRead));
			break;
		}
		case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_DENSITY_MAP_LAYERED_CREATE_INFO_VALVE:
		{
			auto *info = static_cast<const VkPipelineFragmentDensityMapLayeredCreateInfoVALVE *>(pNext);
			if (features.fragment_density_map_layered_valve.fragmentDensityMapLayered == VK_FALSE)
				return support_reject(SupportKind::Feature, "fragmentDensityMapLayered", 3, 1,
									  uint64_t(features.fragment_density_map_layered_valve.fragmentDensityMapLayered));
			if (info->maxFragmentDensityMapLayers > props.fragment_density_map_layered_valve.maxFragmentDensityMapLayers)
				return support_reject(SupportKind::Property, "maxFragmentDensityMapLayers", 3,
									  uint64_t(info->maxFragmentDensityMapLayers),
									  uint64_t(props.fragment_density_map_layered_valve.maxFragmentDensityMapLayers));
			break;
		}
		case VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CLUSTER_ACCELERATION_STRUCTURE_CREATE_INFO_NV:
		{
			if (features.cluster_acceleration_structure_nv.clusterAccelerationStructure == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "clusterAccelerationStructure", 3, 1,
					uint64_t(features.cluster_acceleration_structure_nv.clusterAccelerationStructure));
			break;
		}
		case VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT:
		{
			auto *info = static_cast<const VkShaderDescriptorSetAndBindingMappingInfoEXT *>(pNext);
			if (features.descriptor_heap.descriptorHeap == VK_FALSE)
				return support_reject(SupportKind::Feature, "descriptorHeap", 3, 1,
									  uint64_t(features.descriptor_heap.descriptorHeap));
			if (!descriptor_set_and_binding_mapping_is_supported(*info))
				return false;
			break;
		}
		case VK_STRUCTURE_TYPE_CUSTOM_RESOLVE_CREATE_INFO_EXT:
		{
			if (features.custom_resolve.customResolve == VK_FALSE)
				return support_reject(SupportKind::Feature, "customResolve", 3, 1,
									  uint64_t(features.custom_resolve.customResolve));
			break;
		}
		default:
			LOGE("Unrecognized pNext sType: %u. Treating as unsupported.\n", unsigned(base->sType));
			return support_reject(SupportKind::UnknownSType, "sType", SupportDetail::Raw, 0, 0, uint64_t(base->sType));
		}
		pNext = base->pNext;
	}
	return true;
}

// The most basic validation, can be extended as required.

bool FeatureFilter::Impl::sampler_is_supported(const VkSamplerCreateInfo *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkSamplerCreateFlags supported_flags =
			VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT |
			VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT |
			VK_SAMPLER_CREATE_SUBSAMPLED_COARSE_RECONSTRUCTION_BIT_EXT |
			VK_SAMPLER_CREATE_IMAGE_PROCESSING_BIT_QCOM;
	if ((info->flags & ~supported_flags) != 0)
		return support_reject(SupportKind::Constraint, "VkSamplerCreateInfo.flags", 3, uint64_t(info->flags),
							  uint64_t(supported_flags));

	if (null_device)
		return true;

	if ((info->flags & VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT) != 0)
		if (!features.non_seamless_cube_map.nonSeamlessCubeMap)
			return support_reject(SupportKind::Feature, "nonSeamlessCubeMap", 3, 1,
								  uint64_t(features.non_seamless_cube_map.nonSeamlessCubeMap));

	if ((info->addressModeU == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
	     info->addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
	     info->addressModeW == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE) &&
	    enabled_extensions.count(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, 3, 1, 0);

	if ((info->minFilter == VK_FILTER_CUBIC_IMG ||
	     info->magFilter == VK_FILTER_CUBIC_IMG) &&
	    enabled_extensions.count(VK_IMG_FILTER_CUBIC_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_IMG_FILTER_CUBIC_EXTENSION_NAME, 3, 1, 0);

	if ((info->flags & (VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT |
	                    VK_SAMPLER_CREATE_SUBSAMPLED_COARSE_RECONSTRUCTION_BIT_EXT)) != 0 &&
	    features.fragment_density.fragmentDensityMap == VK_FALSE)
		return support_reject(SupportKind::Feature, "fragmentDensityMap", 3, 1,
							  uint64_t(features.fragment_density.fragmentDensityMap));

	if ((info->borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT ||
	     info->borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT) &&
	    features.custom_border_color.customBorderColors == VK_FALSE)
		return support_reject(SupportKind::Feature, "customBorderColors", 3, 1,
							  uint64_t(features.custom_border_color.customBorderColors));

	if ((info->flags & VK_SAMPLER_CREATE_IMAGE_PROCESSING_BIT_QCOM) != 0 &&
	    enabled_extensions.count(VK_QCOM_IMAGE_PROCESSING_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_QCOM_IMAGE_PROCESSING_EXTENSION_NAME, 3, 1, 0);

	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info) const
{
	bool must_check_set_layout_before_accept = false;

	// Only allow flags we recognize and validate.
	constexpr VkDescriptorSetLayoutCreateFlags supported_flags =
			VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR |
			VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT |
			VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
			VK_DESCRIPTOR_SET_LAYOUT_CREATE_EMBEDDED_IMMUTABLE_SAMPLERS_BIT_EXT |
			VK_DESCRIPTOR_SET_LAYOUT_CREATE_INDIRECT_BINDABLE_BIT_NV |
			VK_DESCRIPTOR_SET_LAYOUT_CREATE_PER_STAGE_BIT_NV;

	if ((info->flags & ~supported_flags) != 0)
		return support_reject(SupportKind::Constraint, "VkDescriptorSetLayoutCreateInfo.flags", 3,
							  uint64_t(info->flags), uint64_t(supported_flags));

	if (null_device)
		return true;

	if ((info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0)
	{
		if (enabled_extensions.count(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0 && features.vk14.pushDescriptor == 0)
		{
			if (!enabled_extensions.count(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
				return support_reject(SupportKind::Extension, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, 3, 1, 0);
			if (!features.vk14.pushDescriptor)
				return support_reject(SupportKind::Feature, "pushDescriptor", 3, 1,
									  uint64_t(features.vk14.pushDescriptor));
			return false;
		}
	}

	if ((info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) != 0)
	{
		// There doesn't seem to be a specific feature bit for this flag, key it on extension being enabled.
		// For specific descriptor types, we check the individual features.
		if (enabled_extensions.count(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) == 0 && !features.vk12.descriptorIndexing)
		{
			if (!enabled_extensions.count(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
				return support_reject(SupportKind::Extension, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, 3, 1, 0);
			if (!features.vk12.descriptorIndexing)
				return support_reject(SupportKind::Feature, "descriptorIndexing", 3, 1,
									  uint64_t(features.vk12.descriptorIndexing));
			return false;
		}
	}

	if ((info->flags & (VK_DESCRIPTOR_SET_LAYOUT_CREATE_EMBEDDED_IMMUTABLE_SAMPLERS_BIT_EXT |
	                    VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT)) != 0)
	{
		if (!features.descriptor_buffer.descriptorBuffer)
			return support_reject(SupportKind::Feature, "descriptorBuffer", 3, 1,
								  uint64_t(features.descriptor_buffer.descriptorBuffer));
		if (info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR)
		{
			if (!features.descriptor_buffer.descriptorBufferPushDescriptors)
				return support_reject(SupportKind::Feature, "descriptorBufferPushDescriptors", 3, 1,
									  uint64_t(features.descriptor_buffer.descriptorBufferPushDescriptors));
		}
	}
	if ((info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_INDIRECT_BINDABLE_BIT_NV) != 0 &&
	    features.device_generated_commands_compute_nv.deviceGeneratedCompute == VK_FALSE)
		return support_reject(SupportKind::Feature, "deviceGeneratedCompute", 3, 1,
							  uint64_t(features.device_generated_commands_compute_nv.deviceGeneratedCompute));
	if ((info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PER_STAGE_BIT_NV) != 0 &&
	    features.per_stage_descriptor_set_nv.perStageDescriptorSet == VK_FALSE)
		return support_reject(SupportKind::Feature, "perStageDescriptorSet", 3, 1,
							  uint64_t(features.per_stage_descriptor_set_nv.perStageDescriptorSet));
	struct DescriptorCounts
	{
		uint32_t sampled_image;
		uint32_t storage_image;
		uint32_t ssbo;
		uint32_t ubo;
		uint32_t input_attachment;
		uint32_t sampler;
		uint32_t ubo_dynamic;
		uint32_t ssbo_dynamic;
		uint32_t acceleration_structure;
	};
	DescriptorCounts counts = {};
	uint32_t total_count = 0;
	auto *flags = find_pnext<VkDescriptorSetLayoutBindingFlagsCreateInfo>(
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, info->pNext);
	auto *mutable_info = find_pnext<VkMutableDescriptorTypeCreateInfoEXT>(
		VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT, info->pNext);
	bool pool_is_update_after_bind = (info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) != 0;
	for (unsigned i = 0; i < info->bindingCount; i++)
	{
		if (!shader_stage_mask_is_supported(info->pBindings[i].stageFlags))
			return false;
		constexpr VkDescriptorBindingFlags supported_binding_flags =
			VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
			VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
		if (flags && (flags->pBindingFlags[i] & ~supported_binding_flags) != 0)
			return support_reject(SupportKind::Constraint, "descriptorBindingFlags", 3, flags->pBindingFlags[i],
								  supported_binding_flags);
		bool binding_is_update_after_bind =
				flags && i < flags->bindingCount &&
				(flags->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) != 0;
		uint32_t *count = nullptr;
		switch (info->pBindings[i].descriptorType)
		{
		case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT:
			if (features.inline_uniform_block.inlineUniformBlock == VK_FALSE)
				return support_reject(SupportKind::Feature, "inlineUniformBlock", 3, 1,
									  uint64_t(features.inline_uniform_block.inlineUniformBlock));
			// TODO: maxInlineUniformTotalSize, but this shouldn't matter when we use the EXT.
			if (info->pBindings[i].descriptorCount > props.inline_uniform_block.maxInlineUniformBlockSize)
				return support_reject(SupportKind::Property, "maxInlineUniformBlockSize", 3,
									  uint64_t(info->pBindings[i].descriptorCount),
									  uint64_t(props.inline_uniform_block.maxInlineUniformBlockSize));
			if (binding_is_update_after_bind && features.inline_uniform_block.descriptorBindingInlineUniformBlockUpdateAfterBind == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "descriptorBindingInlineUniformBlockUpdateAfterBind", 3, 1,
					uint64_t(features.inline_uniform_block.descriptorBindingInlineUniformBlockUpdateAfterBind));
			break;
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingStorageBufferUpdateAfterBind == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "descriptorBindingStorageBufferUpdateAfterBind", 3, 1,
					uint64_t(features.descriptor_indexing.descriptorBindingStorageBufferUpdateAfterBind));
			count = &counts.ssbo;
			break;
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingUniformBufferUpdateAfterBind == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "descriptorBindingUniformBufferUpdateAfterBind", 3, 1,
					uint64_t(features.descriptor_indexing.descriptorBindingUniformBufferUpdateAfterBind));
			count = &counts.ubo;
			break;
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "descriptorBindingSampledImageUpdateAfterBind", 3, 1,
					uint64_t(features.descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind));
			count = &counts.sampled_image;
			break;
		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingUniformTexelBufferUpdateAfterBind == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "descriptorBindingUniformTexelBufferUpdateAfterBind", 3, 1,
					uint64_t(features.descriptor_indexing.descriptorBindingUniformTexelBufferUpdateAfterBind));
			count = &counts.sampled_image;
			break;
		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingStorageTexelBufferUpdateAfterBind == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "descriptorBindingStorageTexelBufferUpdateAfterBind", 3, 1,
					uint64_t(features.descriptor_indexing.descriptorBindingStorageTexelBufferUpdateAfterBind));
			count = &counts.storage_image;
			break;
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingStorageImageUpdateAfterBind == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "descriptorBindingStorageImageUpdateAfterBind", 3, 1,
					uint64_t(features.descriptor_indexing.descriptorBindingStorageImageUpdateAfterBind));
			count = &counts.storage_image;
			break;
		case VK_DESCRIPTOR_TYPE_SAMPLER:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "descriptorBindingSampledImageUpdateAfterBind", 3, 1,
					uint64_t(features.descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind));
			count = &counts.sampler;
			break;
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
			if (binding_is_update_after_bind)
				return support_reject(SupportKind::Constraint, "descriptorBindingUpdateAfterBind", SupportDetail::Raw,
									  0, 0, info->pBindings[i].descriptorType);
			count = &counts.input_attachment;
			break;
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
			if (binding_is_update_after_bind)
				return support_reject(SupportKind::Constraint, "descriptorBindingUpdateAfterBind", SupportDetail::Raw,
									  0, 0, info->pBindings[i].descriptorType);
			count = &counts.ubo_dynamic;
			break;
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
			if (binding_is_update_after_bind)
				return support_reject(SupportKind::Constraint, "descriptorBindingUpdateAfterBind", SupportDetail::Raw,
									  0, 0, info->pBindings[i].descriptorType);
			count = &counts.ssbo_dynamic;
			break;
		case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
			if (binding_is_update_after_bind &&
			    features.acceleration_structure.descriptorBindingAccelerationStructureUpdateAfterBind == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "descriptorBindingAccelerationStructureUpdateAfterBind", 3, 1,
					uint64_t(features.acceleration_structure.descriptorBindingAccelerationStructureUpdateAfterBind));
			if (features.acceleration_structure.accelerationStructure == VK_FALSE)
				return support_reject(SupportKind::Feature, "accelerationStructure", 3, 1,
									  uint64_t(features.acceleration_structure.accelerationStructure));
			count = &counts.acceleration_structure;
			break;
		case VK_DESCRIPTOR_TYPE_MUTABLE_EXT:
		{
			DescriptorCounts mutable_counts = {};
			if (features.mutable_descriptor_type.mutableDescriptorType == VK_FALSE)
				return support_reject(SupportKind::Feature, "mutableDescriptorType", 3, 1,
									  uint64_t(features.mutable_descriptor_type.mutableDescriptorType));
			if (!mutable_info || i >= mutable_info->mutableDescriptorTypeListCount)
				return support_reject(SupportKind::Malformed, "mutableDescriptorTypeListCount");
			auto &list = mutable_info->pMutableDescriptorTypeLists[i];
			for (uint32_t j = 0; j < list.descriptorTypeCount; j++)
			{
				switch (list.pDescriptorTypes[j])
				{
				case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
					if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingStorageBufferUpdateAfterBind == VK_FALSE)
						return support_reject(
							SupportKind::Feature, "descriptorBindingStorageBufferUpdateAfterBind", 3, 1,
							uint64_t(features.descriptor_indexing.descriptorBindingStorageBufferUpdateAfterBind));
					mutable_counts.ssbo = 1;
					break;
				case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
					if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingUniformBufferUpdateAfterBind == VK_FALSE)
						return support_reject(
							SupportKind::Feature, "descriptorBindingUniformBufferUpdateAfterBind", 3, 1,
							uint64_t(features.descriptor_indexing.descriptorBindingUniformBufferUpdateAfterBind));
					mutable_counts.ubo = 1;
					break;
				case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
					if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind == VK_FALSE)
						return support_reject(
							SupportKind::Feature, "descriptorBindingSampledImageUpdateAfterBind", 3, 1,
							uint64_t(features.descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind));
					mutable_counts.sampled_image = 1;
					break;
				case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
					if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingUniformTexelBufferUpdateAfterBind == VK_FALSE)
						return support_reject(
							SupportKind::Feature, "descriptorBindingUniformTexelBufferUpdateAfterBind", 3, 1,
							uint64_t(features.descriptor_indexing.descriptorBindingUniformTexelBufferUpdateAfterBind));
					mutable_counts.sampled_image = 1;
					break;
				case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
					if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingStorageImageUpdateAfterBind == VK_FALSE)
						return support_reject(
							SupportKind::Feature, "descriptorBindingStorageImageUpdateAfterBind", 3, 1,
							uint64_t(features.descriptor_indexing.descriptorBindingStorageImageUpdateAfterBind));
					mutable_counts.storage_image = 1;
					break;
				case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
					if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingStorageTexelBufferUpdateAfterBind == VK_FALSE)
						return support_reject(
							SupportKind::Feature, "descriptorBindingStorageTexelBufferUpdateAfterBind", 3, 1,
							uint64_t(features.descriptor_indexing.descriptorBindingStorageTexelBufferUpdateAfterBind));
					mutable_counts.storage_image = 1;
					break;
				case VK_DESCRIPTOR_TYPE_SAMPLER:
					// Sampler is a non-standard type for mutable
					mutable_counts.sampled_image = 1;
					must_check_set_layout_before_accept = true;
					break;
				default:
					return support_reject(SupportKind::Constraint, "VkDescriptorType", SupportDetail::Raw, 0, 0,
										  list.pDescriptorTypes[j]);
				}
			}
			counts.sampled_image += info->pBindings[i].descriptorCount * mutable_counts.sampled_image;
			counts.storage_image += info->pBindings[i].descriptorCount * mutable_counts.storage_image;
			counts.ubo += info->pBindings[i].descriptorCount * mutable_counts.ubo;
			counts.ssbo += info->pBindings[i].descriptorCount * mutable_counts.ssbo;
			break;
		}
		case VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM:
		case VK_DESCRIPTOR_TYPE_SAMPLE_WEIGHT_IMAGE_QCOM:
			if (enabled_extensions.count(VK_QCOM_IMAGE_PROCESSING_EXTENSION_NAME) == 0)
				return support_reject(SupportKind::Extension, VK_QCOM_IMAGE_PROCESSING_EXTENSION_NAME, 3, 1, 0);
			// *shrug*
			count = nullptr;
			break;
		case VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV:
			if (features.partitioned_acceleration_structure_nv.partitionedAccelerationStructure == VK_FALSE)
				return support_reject(
					SupportKind::Feature, "partitionedAccelerationStructure", 3, 1,
					uint64_t(features.partitioned_acceleration_structure_nv.partitionedAccelerationStructure));
			// *shrug*
			count = nullptr;
			break;
		case VK_DESCRIPTOR_TYPE_TENSOR_ARM:
			if (features.tensor_arm.tensors == VK_FALSE)
				return support_reject(SupportKind::Feature, "tensors", 3, 1, uint64_t(features.tensor_arm.tensors));
			// *shrug*
			count = nullptr;
			break;
		default:
			return support_reject(SupportKind::Constraint, "VkDescriptorType", SupportDetail::Raw, 0, 0,
								  info->pBindings[i].descriptorType);
		}
		if (count)
			*count += info->pBindings[i].descriptorCount;
		total_count += info->pBindings[i].descriptorCount;
	}
	if (pool_is_update_after_bind)
	{
		if (features.maintenance7.maintenance7)
		{
			if (counts.ubo_dynamic > props.maintenance7.maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic)
				return support_reject(
					SupportKind::Property, "maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic", 3,
					uint64_t(counts.ubo_dynamic),
					uint64_t(props.maintenance7.maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic));
			if (counts.ssbo_dynamic > props.maintenance7.maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic)
				return support_reject(
					SupportKind::Property, "maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic", 3,
					uint64_t(counts.ssbo_dynamic),
					uint64_t(props.maintenance7.maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic));
			if ((counts.ubo_dynamic + counts.ssbo_dynamic) > props.maintenance7.maxDescriptorSetUpdateAfterBindTotalBuffersDynamic)
				return support_reject(SupportKind::Property, "maxDescriptorSetUpdateAfterBindTotalBuffersDynamic", 3,
									  uint64_t(counts.ubo_dynamic + counts.ssbo_dynamic),
									  uint64_t(props.maintenance7.maxDescriptorSetUpdateAfterBindTotalBuffersDynamic));
		}
		else
		{
			if (counts.ubo_dynamic > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic)
				return support_reject(
					SupportKind::Property, "maxDescriptorSetUpdateAfterBindUniformBuffersDynamic", 3,
					uint64_t(counts.ubo_dynamic),
					uint64_t(props.descriptor_indexing.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic));
			if (counts.ssbo_dynamic > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic)
				return support_reject(
					SupportKind::Property, "maxDescriptorSetUpdateAfterBindStorageBuffersDynamic", 3,
					uint64_t(counts.ssbo_dynamic),
					uint64_t(props.descriptor_indexing.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic));
		}
		if (counts.ubo > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindUniformBuffers)
			return support_reject(SupportKind::Property, "maxDescriptorSetUpdateAfterBindUniformBuffers", 3,
								  uint64_t(counts.ubo),
								  uint64_t(props.descriptor_indexing.maxDescriptorSetUpdateAfterBindUniformBuffers));
		if (counts.ssbo > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindStorageBuffers)
			return support_reject(SupportKind::Property, "maxDescriptorSetUpdateAfterBindStorageBuffers", 3,
								  uint64_t(counts.ssbo),
								  uint64_t(props.descriptor_indexing.maxDescriptorSetUpdateAfterBindStorageBuffers));
		if (counts.sampled_image > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindSampledImages)
			return support_reject(SupportKind::Property, "maxDescriptorSetUpdateAfterBindSampledImages", 3,
								  uint64_t(counts.sampled_image),
								  uint64_t(props.descriptor_indexing.maxDescriptorSetUpdateAfterBindSampledImages));
		if (counts.storage_image > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindStorageBuffers)
			return support_reject(SupportKind::Property, "maxDescriptorSetUpdateAfterBindStorageBuffers", 3,
								  uint64_t(counts.storage_image),
								  uint64_t(props.descriptor_indexing.maxDescriptorSetUpdateAfterBindStorageBuffers));
		if (counts.sampler > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindSamplers)
			return support_reject(SupportKind::Property, "maxDescriptorSetUpdateAfterBindSamplers", 3,
								  uint64_t(counts.sampler),
								  uint64_t(props.descriptor_indexing.maxDescriptorSetUpdateAfterBindSamplers));
		if (counts.input_attachment > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindInputAttachments)
			return support_reject(SupportKind::Property, "maxDescriptorSetUpdateAfterBindInputAttachments", 3,
								  uint64_t(counts.input_attachment),
								  uint64_t(props.descriptor_indexing.maxDescriptorSetUpdateAfterBindInputAttachments));
		if (counts.acceleration_structure > props.acceleration_structure.maxDescriptorSetUpdateAfterBindAccelerationStructures)
			return support_reject(
				SupportKind::Property, "maxDescriptorSetUpdateAfterBindAccelerationStructures", 3,
				uint64_t(counts.acceleration_structure),
				uint64_t(props.acceleration_structure.maxDescriptorSetUpdateAfterBindAccelerationStructures));
	}
	else
	{
		if (features.maintenance7.maintenance7)
		{
			if (counts.ubo_dynamic > props.maintenance7.maxDescriptorSetTotalUniformBuffersDynamic)
				return support_reject(SupportKind::Property, "maxDescriptorSetTotalUniformBuffersDynamic", 3,
									  uint64_t(counts.ubo_dynamic),
									  uint64_t(props.maintenance7.maxDescriptorSetTotalUniformBuffersDynamic));
			if (counts.ssbo_dynamic > props.maintenance7.maxDescriptorSetTotalStorageBuffersDynamic)
				return support_reject(SupportKind::Property, "maxDescriptorSetTotalStorageBuffersDynamic", 3,
									  uint64_t(counts.ssbo_dynamic),
									  uint64_t(props.maintenance7.maxDescriptorSetTotalStorageBuffersDynamic));
			if ((counts.ubo_dynamic + counts.ssbo_dynamic) > props.maintenance7.maxDescriptorSetTotalBuffersDynamic)
				return support_reject(SupportKind::Property, "maxDescriptorSetTotalBuffersDynamic", 3,
									  uint64_t(counts.ubo_dynamic + counts.ssbo_dynamic),
									  uint64_t(props.maintenance7.maxDescriptorSetTotalBuffersDynamic));
		}
		else
		{
			if (counts.ubo_dynamic > props2.properties.limits.maxDescriptorSetUniformBuffersDynamic)
				return support_reject(SupportKind::Property, "maxDescriptorSetUniformBuffersDynamic", 3,
									  uint64_t(counts.ubo_dynamic),
									  uint64_t(props2.properties.limits.maxDescriptorSetUniformBuffersDynamic));
			if (counts.ssbo_dynamic > props2.properties.limits.maxDescriptorSetStorageBuffersDynamic)
				return support_reject(SupportKind::Property, "maxDescriptorSetStorageBuffersDynamic", 3,
									  uint64_t(counts.ssbo_dynamic),
									  uint64_t(props2.properties.limits.maxDescriptorSetStorageBuffersDynamic));
		}
		if (counts.ubo > props2.properties.limits.maxDescriptorSetUniformBuffers)
			return support_reject(SupportKind::Property, "maxDescriptorSetUniformBuffers", 3, uint64_t(counts.ubo),
								  uint64_t(props2.properties.limits.maxDescriptorSetUniformBuffers));
		if (counts.ssbo > props2.properties.limits.maxDescriptorSetStorageBuffers)
			return support_reject(SupportKind::Property, "maxDescriptorSetStorageBuffers", 3, uint64_t(counts.ssbo),
								  uint64_t(props2.properties.limits.maxDescriptorSetStorageBuffers));
		if (counts.sampled_image > props2.properties.limits.maxDescriptorSetSampledImages)
			return support_reject(SupportKind::Property, "maxDescriptorSetSampledImages", 3,
								  uint64_t(counts.sampled_image),
								  uint64_t(props2.properties.limits.maxDescriptorSetSampledImages));
		if (counts.storage_image > props2.properties.limits.maxDescriptorSetStorageImages)
			return support_reject(SupportKind::Property, "maxDescriptorSetStorageImages", 3,
								  uint64_t(counts.storage_image),
								  uint64_t(props2.properties.limits.maxDescriptorSetStorageImages));
		if (counts.sampler > props2.properties.limits.maxDescriptorSetSamplers)
			return support_reject(SupportKind::Property, "maxDescriptorSetSamplers", 3, uint64_t(counts.sampler),
								  uint64_t(props2.properties.limits.maxDescriptorSetSamplers));
		if (counts.input_attachment > props2.properties.limits.maxDescriptorSetInputAttachments)
			return support_reject(SupportKind::Property, "maxDescriptorSetInputAttachments", 3,
								  uint64_t(counts.input_attachment),
								  uint64_t(props2.properties.limits.maxDescriptorSetInputAttachments));
		if (counts.acceleration_structure > props.acceleration_structure.maxDescriptorSetAccelerationStructures)
			return support_reject(SupportKind::Property, "maxDescriptorSetAccelerationStructures", 3,
								  uint64_t(counts.acceleration_structure),
								  uint64_t(props.acceleration_structure.maxDescriptorSetAccelerationStructures));
	}
	if ((info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0 &&
	    total_count > props.push_descriptor.maxPushDescriptors)
	{
		return support_reject(SupportKind::Property, "maxPushDescriptors", 3, uint64_t(total_count),
							  uint64_t(props.push_descriptor.maxPushDescriptors));
	}
	if (must_check_set_layout_before_accept)
	{
		if (query && !query->descriptor_set_layout_is_supported(info))
			return false;
	}
	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::pipeline_layout_is_supported(const VkPipelineLayoutCreateInfo *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkPipelineLayoutCreateFlags supported_flags =
			VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT |
			VK_PIPELINE_LAYOUT_CREATE_NO_TASK_SHADER_BIT_KHR;

	if ((info->flags & ~supported_flags) != 0)
		return support_reject(SupportKind::Constraint, "VkDescriptorSetLayoutCreateInfo.flags", 3,
							  uint64_t(info->flags), uint64_t(supported_flags));

	if (null_device)
		return true;

	if ((info->flags & VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT) != 0 &&
	    features.graphics_pipeline_library.graphicsPipelineLibrary == VK_FALSE)
	{
		return support_reject(SupportKind::Feature, "graphicsPipelineLibrary", 3, 1,
							  uint64_t(features.graphics_pipeline_library.graphicsPipelineLibrary));
	}
	if ((info->flags & VK_PIPELINE_LAYOUT_CREATE_NO_TASK_SHADER_BIT_KHR) != 0 &&
	    features.maintenance11.maintenance11 == VK_FALSE)
	{
		return support_reject(SupportKind::Feature, "maintenance11", 3, 1,
							  uint64_t(features.maintenance11.maintenance11));
	}
	unsigned max_push_constant_size = 0;
	for (unsigned i = 0; i < info->pushConstantRangeCount; i++)
	{
		unsigned required_size = info->pPushConstantRanges[i].offset + info->pPushConstantRanges[i].size;
		if (required_size > max_push_constant_size)
			max_push_constant_size = required_size;
	}
	if (max_push_constant_size > props2.properties.limits.maxPushConstantsSize)
		return support_reject(SupportKind::Property, "maxPushConstantsSize", 3, uint64_t(max_push_constant_size),
							  uint64_t(props2.properties.limits.maxPushConstantsSize));
	if (info->setLayoutCount > props2.properties.limits.maxBoundDescriptorSets)
		return support_reject(SupportKind::Property, "maxBoundDescriptorSets", 3, uint64_t(info->setLayoutCount),
							  uint64_t(props2.properties.limits.maxBoundDescriptorSets));
	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::validate_spirv_extension(const std::string &ext) const
{
	static const struct
	{
		const char *spirv_ext;
		const char *vulkan_ext;
		const char *alt_vulkan_ext;
		uint32_t promoted_core_version;
	} ext_mapping[] = {
		// Autogenerated table.
		{"SPV_KHR_variable_pointers", "VK_KHR_variable_pointers", "", VK_API_VERSION_1_1},
		{"SPV_AMD_shader_explicit_vertex_parameter", "VK_AMD_shader_explicit_vertex_parameter", "", 0},
		{"SPV_AMD_gcn_shader", "VK_AMD_gcn_shader", "", 0},
		{"SPV_AMD_gpu_shader_half_float", "VK_AMD_gpu_shader_half_float", "", 0},
		{"SPV_AMD_gpu_shader_int16", "VK_AMD_gpu_shader_int16", "", 0},
		{"SPV_AMD_shader_ballot", "VK_AMD_shader_ballot", "", 0},
		{"SPV_AMD_shader_fragment_mask", "VK_AMD_shader_fragment_mask", "", 0},
		{"SPV_AMD_shader_image_load_store_lod", "VK_AMD_shader_image_load_store_lod", "", 0},
		{"SPV_AMD_shader_trinary_minmax", "VK_AMD_shader_trinary_minmax", "", 0},
		{"SPV_AMD_texture_gather_bias_lod", "VK_AMD_texture_gather_bias_lod", "", 0},
		{"SPV_AMD_shader_early_and_late_fragment_tests", "VK_AMD_shader_early_and_late_fragment_tests", "", 0},
		{"SPV_KHR_shader_draw_parameters", "VK_KHR_shader_draw_parameters", "", VK_API_VERSION_1_1},
		{"SPV_KHR_8bit_storage", "VK_KHR_8bit_storage", "", VK_API_VERSION_1_2},
		{"SPV_KHR_16bit_storage", "VK_KHR_16bit_storage", "", VK_API_VERSION_1_1},
		{"SPV_KHR_shader_clock", "VK_KHR_shader_clock", "", 0},
		{"SPV_KHR_float_controls", "VK_KHR_shader_float_controls", "", VK_API_VERSION_1_2},
		{"SPV_KHR_storage_buffer_storage_class", "VK_KHR_storage_buffer_storage_class", "", VK_API_VERSION_1_1},
		{"SPV_KHR_post_depth_coverage", "VK_EXT_post_depth_coverage", "", 0},
		{"SPV_EXT_shader_stencil_export", "VK_EXT_shader_stencil_export", "", 0},
		{"SPV_KHR_shader_ballot", "VK_EXT_shader_subgroup_ballot", "", 0},
		{"SPV_KHR_subgroup_vote", "VK_EXT_shader_subgroup_vote", "", 0},
		{"SPV_NV_sample_mask_override_coverage", "VK_NV_sample_mask_override_coverage", "", 0},
		{"SPV_NV_geometry_shader_passthrough", "VK_NV_geometry_shader_passthrough", "", 0},
		{"SPV_NV_mesh_shader", "VK_NV_mesh_shader", "", 0},
		{"SPV_NV_viewport_array2", "VK_NV_viewport_array2", "", 0},
		{"SPV_NV_shader_subgroup_partitioned", "VK_NV_shader_subgroup_partitioned",
		 "VK_EXT_shader_subgroup_partitioned", 0},
		{"SPV_EXT_shader_subgroup_partitioned", "VK_EXT_shader_subgroup_partitioned", "", 0},
		{"SPV_NV_shader_invocation_reorder", "VK_NV_ray_tracing_invocation_reorder", "", 0},
		{"SPV_EXT_shader_viewport_index_layer", "VK_EXT_shader_viewport_index_layer", "", VK_API_VERSION_1_2},
		{"SPV_NVX_multiview_per_view_attributes", "VK_NVX_multiview_per_view_attributes", "", 0},
		{"SPV_EXT_descriptor_indexing", "VK_EXT_descriptor_indexing", "", VK_API_VERSION_1_2},
		{"SPV_KHR_vulkan_memory_model", "VK_KHR_vulkan_memory_model", "", VK_API_VERSION_1_2},
		{"SPV_NV_compute_shader_derivatives", "VK_NV_compute_shader_derivatives", "", 0},
		{"SPV_NV_fragment_shader_barycentric", "VK_NV_fragment_shader_barycentric", "", 0},
		{"SPV_NV_shader_image_footprint", "VK_NV_shader_image_footprint", "", 0},
		{"SPV_NV_shading_rate", "VK_NV_shading_rate_image", "", 0},
		{"SPV_NV_ray_tracing", "VK_NV_ray_tracing", "", 0},
		{"SPV_KHR_ray_tracing", "VK_KHR_ray_tracing_pipeline", "", 0},
		{"SPV_KHR_ray_query", "VK_KHR_ray_query", "", 0},
		{"SPV_KHR_ray_cull_mask", "VK_KHR_ray_tracing_maintenance1", "", 0},
		{"SPV_GOOGLE_hlsl_functionality1", "VK_GOOGLE_hlsl_functionality1", "", 0},
		{"SPV_GOOGLE_user_type", "VK_GOOGLE_user_type", "", 0},
		{"SPV_GOOGLE_decorate_string", "VK_GOOGLE_decorate_string", "", 0},
		{"SPV_EXT_fragment_invocation_density", "VK_EXT_fragment_density_map", "", 0},
		{"SPV_KHR_physical_storage_buffer", "VK_KHR_buffer_device_address", "", VK_API_VERSION_1_2},
		{"SPV_EXT_physical_storage_buffer", "VK_EXT_buffer_device_address", "", 0},
		{"SPV_NV_cooperative_matrix", "VK_NV_cooperative_matrix", "", 0},
		{"SPV_NV_shader_sm_builtins", "VK_NV_shader_sm_builtins", "", 0},
		{"SPV_EXT_fragment_shader_interlock", "VK_EXT_fragment_shader_interlock", "", 0},
		{"SPV_EXT_demote_to_helper_invocation", "VK_EXT_shader_demote_to_helper_invocation", "", VK_API_VERSION_1_3},
		{"SPV_KHR_fragment_shading_rate", "VK_KHR_fragment_shading_rate", "", 0},
		{"SPV_KHR_non_semantic_info", "VK_KHR_shader_non_semantic_info", "", VK_API_VERSION_1_3},
		{"SPV_EXT_shader_image_int64", "VK_EXT_shader_image_atomic_int64", "", 0},
		{"SPV_KHR_terminate_invocation", "VK_KHR_shader_terminate_invocation", "", VK_API_VERSION_1_3},
		{"SPV_KHR_multiview", "VK_KHR_multiview", "", VK_API_VERSION_1_1},
		{"SPV_KHR_workgroup_memory_explicit_layout", "VK_KHR_workgroup_memory_explicit_layout", "", 0},
		{"SPV_EXT_shader_atomic_float_add", "VK_EXT_shader_atomic_float", "", 0},
		{"SPV_KHR_fragment_shader_barycentric", "VK_KHR_fragment_shader_barycentric", "", 0},
		{"SPV_KHR_subgroup_uniform_control_flow", "VK_KHR_shader_subgroup_uniform_control_flow", "", 0},
		{"SPV_EXT_shader_atomic_float_min_max", "VK_EXT_shader_atomic_float2", "", 0},
		{"SPV_EXT_shader_atomic_float16_add", "VK_EXT_shader_atomic_float2", "", 0},
		{"SPV_NV_shader_atomic_fp16_vector", "VK_NV_shader_atomic_float16_vector", "", 0},
		{"SPV_EXT_fragment_fully_covered", "VK_EXT_conservative_rasterization", "", 0},
		{"SPV_KHR_integer_dot_product", "VK_KHR_shader_integer_dot_product", "", VK_API_VERSION_1_3},
		{"SPV_INTEL_shader_integer_functions2", "VK_INTEL_shader_integer_functions2", "", 0},
		{"SPV_KHR_device_group", "VK_KHR_device_group", "", VK_API_VERSION_1_1},
		{"SPV_QCOM_image_processing", "VK_QCOM_image_processing", "", 0},
		{"SPV_QCOM_image_processing2", "VK_QCOM_image_processing2", "", 0},
		{"SPV_QCOM_image_processing3", "VK_QCOM_image_processing3", "", 0},
		{"SPV_QCOM_cooperative_matrix_conversion", "VK_QCOM_cooperative_matrix_conversion", "", 0},
		{"SPV_QCOM_multiple_wait_queues", "VK_QCOM_shader_multiple_wait_queues", "", 0},
		{"SPV_EXT_mesh_shader", "VK_EXT_mesh_shader", "", 0},
		{"SPV_KHR_ray_tracing_position_fetch", "VK_KHR_ray_tracing_position_fetch", "", 0},
		{"SPV_EXT_shader_tile_image", "VK_EXT_shader_tile_image", "", 0},
		{"SPV_EXT_opacity_micromap", "VK_EXT_opacity_micromap", "VK_KHR_opacity_micromap", 0},
		{"SPV_KHR_opacity_micromap", "VK_KHR_opacity_micromap", "", 0},
		{"SPV_EXT_split_barrier", "VK_EXT_shader_split_barrier", "", 0},
		{"SPV_KHR_cooperative_matrix", "VK_KHR_cooperative_matrix", "", 0},
		{"SPV_ARM_core_builtins", "VK_ARM_shader_core_builtins", "", 0},
		{"SPV_HUAWEI_cluster_culling_shader", "VK_HUAWEI_cluster_culling_shader", "", 0},
		{"SPV_HUAWEI_subpass_shading", "VK_HUAWEI_subpass_shading", "", 0},
		{"SPV_NV_ray_tracing_motion_blur", "VK_NV_ray_tracing_motion_blur", "", 0},
		{"SPV_KHR_maximal_reconvergence", "VK_KHR_shader_maximal_reconvergence", "", 0},
		{"SPV_KHR_subgroup_rotate", "VK_KHR_shader_subgroup_rotate", "", VK_API_VERSION_1_4},
		{"SPV_KHR_expect_assume", "VK_KHR_shader_expect_assume", "", VK_API_VERSION_1_4},
		{"SPV_KHR_float_controls2", "VK_KHR_shader_float_controls2", "", VK_API_VERSION_1_4},
		{"SPV_KHR_fma", "VK_KHR_shader_fma", "", 0},
		{"SPV_KHR_quad_control", "VK_KHR_shader_quad_control", "", 0},
		{"SPV_KHR_bfloat16", "VK_KHR_shader_bfloat16", "", 0},
		{"SPV_NV_raw_access_chains", "VK_NV_raw_access_chains", "", 0},
		{"SPV_KHR_compute_shader_derivatives", "VK_KHR_compute_shader_derivatives", "", 0},
		{"SPV_EXT_replicated_composites", "VK_EXT_shader_replicated_composites", "", 0},
		{"SPV_KHR_relaxed_extended_instruction", "VK_KHR_shader_relaxed_extended_instruction", "", 0},
		{"SPV_NV_cooperative_matrix2", "VK_NV_cooperative_matrix2", "", 0},
		{"SPV_NV_cooperative_matrix_decode_vector", "VK_NV_cooperative_matrix_decode_vector", "", 0},
		{"SPV_NV_tensor_addressing", "VK_NV_cooperative_matrix2", "", 0},
		{"SPV_NV_linear_swept_spheres", "VK_NV_ray_tracing_linear_swept_spheres", "", 0},
		{"SPV_NV_cluster_acceleration_structure", "VK_NV_cluster_acceleration_structure", "", 0},
		{"SPV_NV_cooperative_vector", "VK_NV_cooperative_vector", "", 0},
		{"SPV_NV_push_constant_bank", "VK_NV_push_constant_bank", "", 0},
		{"SPV_EXT_shader_invocation_reorder", "VK_EXT_ray_tracing_invocation_reorder", "", 0},
		{"SPV_QCOM_tile_shading", "VK_QCOM_tile_shading", "", 0},
		{"SPV_ARM_tensors", "VK_ARM_tensors", "", 0},
		{"SPV_EXT_float8", "VK_EXT_shader_float8", "", 0},
		{"SPV_ARM_graph", "VK_ARM_data_graph", "", 0},
		{"SPV_KHR_untyped_pointers", "VK_KHR_shader_untyped_pointers", "", 0},
		{"SPV_EXT_shader_64bit_indexing", "VK_EXT_shader_64bit_indexing", "", 0},
		{"SPV_EXT_long_vector", "VK_EXT_shader_long_vector", "", 0},
		{"SPV_EXT_descriptor_heap", "VK_EXT_descriptor_heap", "", 0},
		{"SPV_VALVE_mixed_float_dot_product", "VK_VALVE_shader_mixed_float_dot_product", "", 0},
		{"SPV_KHR_abort", "VK_KHR_shader_abort", "", 0},
		{"SPV_KHR_constant_data", "VK_KHR_shader_constant_data", "", 0},
		{"SPV_EXT_ocp_microscaling_types", "VK_EXT_shader_ocp_microscaling_types", "", 0},
		{"SPV_EXT_cooperative_matrix_maintenance1", "VK_EXT_cooperative_matrix_maintenance1", "", 0},
	};
	for (auto &mapping : ext_mapping)
	{
		if (ext == mapping.spirv_ext)
		{
			if (mapping.promoted_core_version && api_version >= mapping.promoted_core_version)
				return true;
			if (enabled_extensions.count(mapping.vulkan_ext) != 0)
				return true;
			if (*mapping.alt_vulkan_ext != '\0' && enabled_extensions.count(mapping.alt_vulkan_ext) != 0)
				return true;
			break;
		}
	}
	return support_reject(SupportKind::Extension, ext.c_str(), 3, 1, 0);
}

static const char *capability_identifier(spv::Capability capability)
{
	switch (capability)
	{
	case spv::CapabilityMatrix:
		return "CapabilityMatrix";
	case spv::CapabilityShader:
		return "CapabilityShader";
	case spv::CapabilityInputAttachment:
		return "CapabilityInputAttachment";
	case spv::CapabilitySampled1D:
		return "CapabilitySampled1D";
	case spv::CapabilityImage1D:
		return "CapabilityImage1D";
	case spv::CapabilitySampledBuffer:
		return "CapabilitySampledBuffer";
	case spv::CapabilityImageBuffer:
		return "CapabilityImageBuffer";
	case spv::CapabilityImageQuery:
		return "CapabilityImageQuery";
	case spv::CapabilityDerivativeControl:
		return "CapabilityDerivativeControl";
	case spv::CapabilityStorageImageExtendedFormats:
		return "CapabilityStorageImageExtendedFormats";
	case spv::CapabilityDeviceGroup:
		return "CapabilityDeviceGroup";
	case spv::CapabilityGeometry:
		return "CapabilityGeometry";
	case spv::CapabilityTessellation:
		return "CapabilityTessellation";
	case spv::CapabilityFloat64:
		return "CapabilityFloat64";
	case spv::CapabilityInt64:
		return "CapabilityInt64";
	case spv::CapabilityInt64Atomics:
		return "CapabilityInt64Atomics";
	case spv::CapabilityAtomicFloat16AddEXT:
		return "CapabilityAtomicFloat16AddEXT";
	case spv::CapabilityAtomicFloat32AddEXT:
		return "CapabilityAtomicFloat32AddEXT";
	case spv::CapabilityAtomicFloat64AddEXT:
		return "CapabilityAtomicFloat64AddEXT";
	case spv::CapabilityAtomicFloat16MinMaxEXT:
		return "CapabilityAtomicFloat16MinMaxEXT";
	case spv::CapabilityAtomicFloat32MinMaxEXT:
		return "CapabilityAtomicFloat32MinMaxEXT";
	case spv::CapabilityAtomicFloat64MinMaxEXT:
		return "CapabilityAtomicFloat64MinMaxEXT";
	case spv::CapabilityInt64ImageEXT:
		return "CapabilityInt64ImageEXT";
	case spv::CapabilityGroups:
		return "CapabilityGroups";
	case spv::CapabilityInt16:
		return "CapabilityInt16";
	case spv::CapabilityTessellationPointSize:
		return "CapabilityTessellationPointSize";
	case spv::CapabilityGeometryPointSize:
		return "CapabilityGeometryPointSize";
	case spv::CapabilityImageGatherExtended:
		return "CapabilityImageGatherExtended";
	case spv::CapabilityStorageImageMultisample:
		return "CapabilityStorageImageMultisample";
	case spv::CapabilityUniformBufferArrayDynamicIndexing:
		return "CapabilityUniformBufferArrayDynamicIndexing";
	case spv::CapabilitySampledImageArrayDynamicIndexing:
		return "CapabilitySampledImageArrayDynamicIndexing";
	case spv::CapabilityStorageBufferArrayDynamicIndexing:
		return "CapabilityStorageBufferArrayDynamicIndexing";
	case spv::CapabilityStorageImageArrayDynamicIndexing:
		return "CapabilityStorageImageArrayDynamicIndexing";
	case spv::CapabilityClipDistance:
		return "CapabilityClipDistance";
	case spv::CapabilityCullDistance:
		return "CapabilityCullDistance";
	case spv::CapabilityImageCubeArray:
		return "CapabilityImageCubeArray";
	case spv::CapabilitySampleRateShading:
		return "CapabilitySampleRateShading";
	case spv::CapabilitySparseResidency:
		return "CapabilitySparseResidency";
	case spv::CapabilityMinLod:
		return "CapabilityMinLod";
	case spv::CapabilitySampledCubeArray:
		return "CapabilitySampledCubeArray";
	case spv::CapabilityImageMSArray:
		return "CapabilityImageMSArray";
	case spv::CapabilityInterpolationFunction:
		return "CapabilityInterpolationFunction";
	case spv::CapabilityStorageImageReadWithoutFormat:
		return "CapabilityStorageImageReadWithoutFormat";
	case spv::CapabilityStorageImageWriteWithoutFormat:
		return "CapabilityStorageImageWriteWithoutFormat";
	case spv::CapabilityMultiViewport:
		return "CapabilityMultiViewport";
	case spv::CapabilityDrawParameters:
		return "CapabilityDrawParameters";
	case spv::CapabilityMultiView:
		return "CapabilityMultiView";
	case spv::CapabilityVariablePointersStorageBuffer:
		return "CapabilityVariablePointersStorageBuffer";
	case spv::CapabilityVariablePointers:
		return "CapabilityVariablePointers";
	case spv::CapabilityShaderClockKHR:
		return "CapabilityShaderClockKHR";
	case spv::CapabilityStencilExportEXT:
		return "CapabilityStencilExportEXT";
	case spv::CapabilitySubgroupBallotKHR:
		return "CapabilitySubgroupBallotKHR";
	case spv::CapabilitySubgroupVoteKHR:
		return "CapabilitySubgroupVoteKHR";
	case spv::CapabilityImageReadWriteLodAMD:
		return "CapabilityImageReadWriteLodAMD";
	case spv::CapabilityImageGatherBiasLodAMD:
		return "CapabilityImageGatherBiasLodAMD";
	case spv::CapabilityFragmentMaskAMD:
		return "CapabilityFragmentMaskAMD";
	case spv::CapabilitySampleMaskOverrideCoverageNV:
		return "CapabilitySampleMaskOverrideCoverageNV";
	case spv::CapabilityGeometryShaderPassthroughNV:
		return "CapabilityGeometryShaderPassthroughNV";
	case spv::CapabilityShaderViewportIndex:
		return "CapabilityShaderViewportIndex";
	case spv::CapabilityShaderLayer:
		return "CapabilityShaderLayer";
	case spv::CapabilityShaderViewportIndexLayerEXT:
		return "CapabilityShaderViewportIndexLayerEXT";
	case spv::CapabilityShaderViewportMaskNV:
		return "CapabilityShaderViewportMaskNV";
	case spv::CapabilityPerViewAttributesNV:
		return "CapabilityPerViewAttributesNV";
	case spv::CapabilityStorageBuffer16BitAccess:
		return "CapabilityStorageBuffer16BitAccess";
	case spv::CapabilityUniformAndStorageBuffer16BitAccess:
		return "CapabilityUniformAndStorageBuffer16BitAccess";
	case spv::CapabilityStoragePushConstant16:
		return "CapabilityStoragePushConstant16";
	case spv::CapabilityStorageInputOutput16:
		return "CapabilityStorageInputOutput16";
	case spv::CapabilityGroupNonUniform:
		return "CapabilityGroupNonUniform";
	case spv::CapabilityGroupNonUniformVote:
		return "CapabilityGroupNonUniformVote";
	case spv::CapabilityGroupNonUniformArithmetic:
		return "CapabilityGroupNonUniformArithmetic";
	case spv::CapabilityGroupNonUniformBallot:
		return "CapabilityGroupNonUniformBallot";
	case spv::CapabilityGroupNonUniformShuffle:
		return "CapabilityGroupNonUniformShuffle";
	case spv::CapabilityGroupNonUniformShuffleRelative:
		return "CapabilityGroupNonUniformShuffleRelative";
	case spv::CapabilityGroupNonUniformClustered:
		return "CapabilityGroupNonUniformClustered";
	case spv::CapabilityGroupNonUniformQuad:
		return "CapabilityGroupNonUniformQuad";
	case spv::CapabilityGroupNonUniformPartitionedEXT:
		return "CapabilityGroupNonUniformPartitionedEXT";
	case spv::CapabilitySampleMaskPostDepthCoverage:
		return "CapabilitySampleMaskPostDepthCoverage";
	case spv::CapabilityShaderNonUniform:
		return "CapabilityShaderNonUniform";
	case spv::CapabilityRuntimeDescriptorArray:
		return "CapabilityRuntimeDescriptorArray";
	case spv::CapabilityInputAttachmentArrayDynamicIndexing:
		return "CapabilityInputAttachmentArrayDynamicIndexing";
	case spv::CapabilityUniformTexelBufferArrayDynamicIndexing:
		return "CapabilityUniformTexelBufferArrayDynamicIndexing";
	case spv::CapabilityStorageTexelBufferArrayDynamicIndexing:
		return "CapabilityStorageTexelBufferArrayDynamicIndexing";
	case spv::CapabilityUniformBufferArrayNonUniformIndexing:
		return "CapabilityUniformBufferArrayNonUniformIndexing";
	case spv::CapabilitySampledImageArrayNonUniformIndexing:
		return "CapabilitySampledImageArrayNonUniformIndexing";
	case spv::CapabilityUniformTexelBufferArrayNonUniformIndexing:
		return "CapabilityUniformTexelBufferArrayNonUniformIndexing";
	case spv::CapabilityStorageBufferArrayNonUniformIndexing:
		return "CapabilityStorageBufferArrayNonUniformIndexing";
	case spv::CapabilityStorageImageArrayNonUniformIndexing:
		return "CapabilityStorageImageArrayNonUniformIndexing";
	case spv::CapabilityInputAttachmentArrayNonUniformIndexing:
		return "CapabilityInputAttachmentArrayNonUniformIndexing";
	case spv::CapabilityStorageTexelBufferArrayNonUniformIndexing:
		return "CapabilityStorageTexelBufferArrayNonUniformIndexing";
	case spv::CapabilityFloat16:
		return "CapabilityFloat16";
	case spv::CapabilityInt8:
		return "CapabilityInt8";
	case spv::CapabilityStorageBuffer8BitAccess:
		return "CapabilityStorageBuffer8BitAccess";
	case spv::CapabilityUniformAndStorageBuffer8BitAccess:
		return "CapabilityUniformAndStorageBuffer8BitAccess";
	case spv::CapabilityStoragePushConstant8:
		return "CapabilityStoragePushConstant8";
	case spv::CapabilityVulkanMemoryModel:
		return "CapabilityVulkanMemoryModel";
	case spv::CapabilityVulkanMemoryModelDeviceScope:
		return "CapabilityVulkanMemoryModelDeviceScope";
	case spv::CapabilityDenormPreserve:
		return "CapabilityDenormPreserve";
	case spv::CapabilityDenormFlushToZero:
		return "CapabilityDenormFlushToZero";
	case spv::CapabilitySignedZeroInfNanPreserve:
		return "CapabilitySignedZeroInfNanPreserve";
	case spv::CapabilityRoundingModeRTE:
		return "CapabilityRoundingModeRTE";
	case spv::CapabilityRoundingModeRTZ:
		return "CapabilityRoundingModeRTZ";
	case spv::CapabilityComputeDerivativeGroupQuadsKHR:
		return "CapabilityComputeDerivativeGroupQuadsKHR";
	case spv::CapabilityComputeDerivativeGroupLinearKHR:
		return "CapabilityComputeDerivativeGroupLinearKHR";
	case spv::CapabilityFragmentBarycentricKHR:
		return "CapabilityFragmentBarycentricKHR";
	case spv::CapabilityImageFootprintNV:
		return "CapabilityImageFootprintNV";
	case spv::CapabilityFragmentDensityEXT:
		return "CapabilityFragmentDensityEXT";
	case spv::CapabilityMeshShadingNV:
		return "CapabilityMeshShadingNV";
	case spv::CapabilityRayTracingNV:
		return "CapabilityRayTracingNV";
	case spv::CapabilityTransformFeedback:
		return "CapabilityTransformFeedback";
	case spv::CapabilityGeometryStreams:
		return "CapabilityGeometryStreams";
	case spv::CapabilityPhysicalStorageBufferAddresses:
		return "CapabilityPhysicalStorageBufferAddresses";
	case spv::CapabilityCooperativeMatrixNV:
		return "CapabilityCooperativeMatrixNV";
	case spv::CapabilityIntegerFunctions2INTEL:
		return "CapabilityIntegerFunctions2INTEL";
	case spv::CapabilityShaderSMBuiltinsNV:
		return "CapabilityShaderSMBuiltinsNV";
	case spv::CapabilityFragmentShaderSampleInterlockEXT:
		return "CapabilityFragmentShaderSampleInterlockEXT";
	case spv::CapabilityFragmentShaderPixelInterlockEXT:
		return "CapabilityFragmentShaderPixelInterlockEXT";
	case spv::CapabilityFragmentShaderShadingRateInterlockEXT:
		return "CapabilityFragmentShaderShadingRateInterlockEXT";
	case spv::CapabilityDemoteToHelperInvocation:
		return "CapabilityDemoteToHelperInvocation";
	case spv::CapabilityFragmentShadingRateKHR:
		return "CapabilityFragmentShadingRateKHR";
	case spv::CapabilityRayQueryKHR:
		return "CapabilityRayQueryKHR";
	case spv::CapabilityRayTracingKHR:
		return "CapabilityRayTracingKHR";
	case spv::CapabilityRayTraversalPrimitiveCullingKHR:
		return "CapabilityRayTraversalPrimitiveCullingKHR";
	case spv::CapabilityWorkgroupMemoryExplicitLayoutKHR:
		return "CapabilityWorkgroupMemoryExplicitLayoutKHR";
	case spv::CapabilityWorkgroupMemoryExplicitLayout8BitAccessKHR:
		return "CapabilityWorkgroupMemoryExplicitLayout8BitAccessKHR";
	case spv::CapabilityWorkgroupMemoryExplicitLayout16BitAccessKHR:
		return "CapabilityWorkgroupMemoryExplicitLayout16BitAccessKHR";
	case spv::CapabilityDotProduct:
		return "CapabilityDotProduct";
	case spv::CapabilityDotProductInputAll:
		return "CapabilityDotProductInputAll";
	case spv::CapabilityDotProductInput4x8Bit:
		return "CapabilityDotProductInput4x8Bit";
	case spv::CapabilityDotProductInput4x8BitPacked:
		return "CapabilityDotProductInput4x8BitPacked";
	case spv::CapabilityMeshShadingEXT:
		return "CapabilityMeshShadingEXT";
	case spv::CapabilityFragmentFullyCoveredEXT:
		return "CapabilityFragmentFullyCoveredEXT";
	case spv::CapabilityRayCullMaskKHR:
		return "CapabilityRayCullMaskKHR";
	case spv::CapabilityRayTracingMotionBlurNV:
		return "CapabilityRayTracingMotionBlurNV";
	case spv::CapabilityRayTracingOpacityMicromapKHR:
		return "CapabilityRayTracingOpacityMicromapKHR";
	case spv::CapabilityTextureSampleWeightedQCOM:
		return "CapabilityTextureSampleWeightedQCOM";
	case spv::CapabilityTextureBoxFilterQCOM:
		return "CapabilityTextureBoxFilterQCOM";
	case spv::CapabilityTextureBlockMatchQCOM:
		return "CapabilityTextureBlockMatchQCOM";
	case spv::CapabilityTextureBlockMatch2QCOM:
		return "CapabilityTextureBlockMatch2QCOM";
	case spv::CapabilityCoreBuiltinsARM:
		return "CapabilityCoreBuiltinsARM";
	case spv::CapabilityShaderInvocationReorderNV:
		return "CapabilityShaderInvocationReorderNV";
	case spv::CapabilityRayTracingPositionFetchKHR:
		return "CapabilityRayTracingPositionFetchKHR";
	case spv::CapabilityRayQueryPositionFetchKHR:
		return "CapabilityRayQueryPositionFetchKHR";
	case spv::CapabilityTileImageColorReadAccessEXT:
		return "CapabilityTileImageColorReadAccessEXT";
	case spv::CapabilityTileImageDepthReadAccessEXT:
		return "CapabilityTileImageDepthReadAccessEXT";
	case spv::CapabilityTileImageStencilReadAccessEXT:
		return "CapabilityTileImageStencilReadAccessEXT";
	case spv::CapabilityCooperativeMatrixKHR:
		return "CapabilityCooperativeMatrixKHR";
	case spv::CapabilityGroupNonUniformRotateKHR:
		return "CapabilityGroupNonUniformRotateKHR";
	case spv::CapabilityExpectAssumeKHR:
		return "CapabilityExpectAssumeKHR";
	case spv::CapabilityFloatControls2:
		return "CapabilityFloatControls2";
	case spv::CapabilityQuadControlKHR:
		return "CapabilityQuadControlKHR";
	case spv::CapabilityRawAccessChainsNV:
		return "CapabilityRawAccessChainsNV";
	case spv::CapabilityReplicatedCompositesEXT:
		return "CapabilityReplicatedCompositesEXT";
	case spv::CapabilityBFloat16TypeKHR:
		return "CapabilityBFloat16TypeKHR";
	case spv::CapabilityBFloat16DotProductKHR:
		return "CapabilityBFloat16DotProductKHR";
	case spv::CapabilityBFloat16CooperativeMatrixKHR:
		return "CapabilityBFloat16CooperativeMatrixKHR";
	case spv::CapabilityTensorAddressingNV:
		return "CapabilityTensorAddressingNV";
	case spv::CapabilityCooperativeMatrixReductionsEXT:
		return "CapabilityCooperativeMatrixReductionsEXT";
	case spv::CapabilityCooperativeMatrixConversionsNV:
		return "CapabilityCooperativeMatrixConversionsNV";
	case spv::CapabilityCooperativeMatrixConversionsEXT:
		return "CapabilityCooperativeMatrixConversionsEXT";
	case spv::CapabilityCooperativeMatrixPerElementOperationsEXT:
		return "CapabilityCooperativeMatrixPerElementOperationsEXT";
	case spv::CapabilityCooperativeMatrixGetCoordinateEXT:
		return "CapabilityCooperativeMatrixGetCoordinateEXT";
	case spv::CapabilityCooperativeMatrixTensorAddressingNV:
		return "CapabilityCooperativeMatrixTensorAddressingNV";
	case spv::CapabilityCooperativeMatrixBlockLoadsNV:
		return "CapabilityCooperativeMatrixBlockLoadsNV";
	case spv::CapabilityRayTracingSpheresGeometryNV:
		return "CapabilityRayTracingSpheresGeometryNV";
	case spv::CapabilityRayTracingLinearSweptSpheresGeometryNV:
		return "CapabilityRayTracingLinearSweptSpheresGeometryNV";
	case spv::CapabilityRayTracingClusterAccelerationStructureNV:
		return "CapabilityRayTracingClusterAccelerationStructureNV";
	case spv::CapabilityCooperativeVectorNV:
		return "CapabilityCooperativeVectorNV";
	case spv::CapabilityCooperativeVectorTrainingNV:
		return "CapabilityCooperativeVectorTrainingNV";
	case spv::CapabilityTileShadingQCOM:
		return "CapabilityTileShadingQCOM";
	case spv::CapabilityFloat8EXT:
		return "CapabilityFloat8EXT";
	case spv::CapabilityFloat8CooperativeMatrixEXT:
		return "CapabilityFloat8CooperativeMatrixEXT";
	case spv::CapabilityTensorsARM:
		return "CapabilityTensorsARM";
	case spv::CapabilityStorageTensorArrayDynamicIndexingARM:
		return "CapabilityStorageTensorArrayDynamicIndexingARM";
	case spv::CapabilityStorageTensorArrayNonUniformIndexingARM:
		return "CapabilityStorageTensorArrayNonUniformIndexingARM";
	case spv::CapabilityUntypedPointersKHR:
		return "CapabilityUntypedPointersKHR";
	case spv::CapabilityDescriptorHeapEXT:
		return "CapabilityDescriptorHeapEXT";
	case spv::CapabilityLongVectorEXT:
		return "CapabilityLongVectorEXT";
	case spv::CapabilityShader64BitIndexingEXT:
		return "CapabilityShader64BitIndexingEXT";
	case spv::CapabilityFMAKHR:
		return "CapabilityFMAKHR";
	case spv::CapabilityShaderInvocationReorderEXT:
		return "CapabilityShaderInvocationReorderEXT";
	case spv::CapabilityPushConstantBanksNV:
		return "CapabilityPushConstantBanksNV";
	case spv::CapabilityCooperativeMatrixConversionQCOM:
		return "CapabilityCooperativeMatrixConversionQCOM";
	case spv::CapabilityDotProductFloat16AccFloat32VALVE:
		return "CapabilityDotProductFloat16AccFloat32VALVE";
	case spv::CapabilityDotProductFloat16AccFloat16VALVE:
		return "CapabilityDotProductFloat16AccFloat16VALVE";
	case spv::CapabilityDotProductBFloat16AccVALVE:
		return "CapabilityDotProductBFloat16AccVALVE";
	case spv::CapabilityDotProductFloat8AccFloat32VALVE:
		return "CapabilityDotProductFloat8AccFloat32VALVE";
	case spv::CapabilityRayTracingOpacityMicromapExecutionModeKHR:
		return "CapabilityRayTracingOpacityMicromapExecutionModeKHR";
	default:
		return "SPIR-V.Capability";
	}
}

bool FeatureFilter::Impl::validate_module_capability(spv::Capability cap) const
{
	// From table 75 in Vulkan spec.

	switch (cap)
	{
	case spv::CapabilityMatrix:
	case spv::CapabilityShader:
	case spv::CapabilityInputAttachment:
	case spv::CapabilitySampled1D:
	case spv::CapabilityImage1D:
	case spv::CapabilitySampledBuffer:
	case spv::CapabilityImageBuffer:
	case spv::CapabilityImageQuery:
	case spv::CapabilityDerivativeControl:
	case spv::CapabilityStorageImageExtendedFormats:
		return true;
	case spv::CapabilityDeviceGroup:
		return (api_version >= VK_API_VERSION_1_1) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGeometry:
		return (features2.features.geometryShader == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTessellation:
		return (features2.features.tessellationShader == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFloat64:
		return (features2.features.shaderFloat64 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityInt64:
		return (features2.features.shaderInt64 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityInt64Atomics:
		return (features.atomic_int64.shaderBufferInt64Atomics == VK_TRUE ||
		       features.atomic_int64.shaderSharedInt64Atomics == VK_TRUE ||
				features.shader_image_atomic_int64.shaderImageInt64Atomics == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityAtomicFloat16AddEXT:
		return (features.shader_atomic_float2.shaderBufferFloat16AtomicAdd == VK_TRUE ||
				features.shader_atomic_float2.shaderSharedFloat16AtomicAdd == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityAtomicFloat32AddEXT:
		return (features.shader_atomic_float.shaderBufferFloat32AtomicAdd == VK_TRUE ||
		       features.shader_atomic_float.shaderSharedFloat32AtomicAdd == VK_TRUE ||
				features.shader_atomic_float.shaderImageFloat32AtomicAdd == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityAtomicFloat64AddEXT:
		return (features.shader_atomic_float.shaderBufferFloat64AtomicAdd == VK_TRUE ||
				features.shader_atomic_float.shaderSharedFloat64AtomicAdd == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityAtomicFloat16MinMaxEXT:
		return (features.shader_atomic_float2.shaderBufferFloat16AtomicMinMax == VK_TRUE ||
				features.shader_atomic_float2.shaderSharedFloat16AtomicMinMax == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityAtomicFloat32MinMaxEXT:
		return (features.shader_atomic_float2.shaderBufferFloat32AtomicMinMax == VK_TRUE ||
				features.shader_atomic_float2.shaderSharedFloat32AtomicMinMax == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityAtomicFloat64MinMaxEXT:
		return (features.shader_atomic_float2.shaderBufferFloat64AtomicMinMax == VK_TRUE ||
				features.shader_atomic_float2.shaderSharedFloat64AtomicMinMax == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityInt64ImageEXT:
		return (features.shader_image_atomic_int64.shaderImageInt64Atomics == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGroups:
		return (enabled_extensions.count(VK_AMD_SHADER_BALLOT_EXTENSION_NAME)) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityInt16:
		return (features2.features.shaderInt16 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTessellationPointSize:
	case spv::CapabilityGeometryPointSize:
		return (features2.features.shaderTessellationAndGeometryPointSize == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityImageGatherExtended:
		return (features2.features.shaderImageGatherExtended == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageImageMultisample:
		return (features2.features.shaderStorageImageMultisample == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityUniformBufferArrayDynamicIndexing:
		return (features2.features.shaderUniformBufferArrayDynamicIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilitySampledImageArrayDynamicIndexing:
		return (features2.features.shaderSampledImageArrayDynamicIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageBufferArrayDynamicIndexing:
		return (features2.features.shaderStorageBufferArrayDynamicIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageImageArrayDynamicIndexing:
		return (features2.features.shaderStorageImageArrayDynamicIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityClipDistance:
		return (features2.features.shaderClipDistance == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCullDistance:
		return (features2.features.shaderCullDistance == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityImageCubeArray:
		return (features2.features.imageCubeArray == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilitySampleRateShading:
		return (features2.features.sampleRateShading == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilitySparseResidency:
		return (features2.features.shaderResourceResidency == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityMinLod:
		return (features2.features.shaderResourceMinLod == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilitySampledCubeArray:
		return (features2.features.imageCubeArray == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityImageMSArray:
		return (features2.features.shaderStorageImageMultisample == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityInterpolationFunction:
		return (features2.features.sampleRateShading == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageImageReadWithoutFormat:
		return (features2.features.shaderStorageImageReadWithoutFormat == VK_TRUE ||
		       api_version >= VK_API_VERSION_1_3 ||
				enabled_extensions.count(VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageImageWriteWithoutFormat:
		return (features2.features.shaderStorageImageWriteWithoutFormat == VK_TRUE ||
		       api_version >= VK_API_VERSION_1_3 ||
				enabled_extensions.count(VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityMultiViewport:
		return (features2.features.multiViewport == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityDrawParameters:
		return (features.draw_parameters.shaderDrawParameters == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityMultiView:
		return (features.multiview.multiview == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityVariablePointersStorageBuffer:
		return (features.variable_pointers.variablePointersStorageBuffer == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityVariablePointers:
		return (features.variable_pointers.variablePointers == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityShaderClockKHR:
		// There aren't two separate capabilities, so we'd have to analyze all opcodes to deduce this.
		// Just gate this on both feature bits being supported to be safe.
		return (features.shader_clock.shaderDeviceClock == VK_TRUE &&
				features.shader_clock.shaderSubgroupClock == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStencilExportEXT:
		return (enabled_extensions.count(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilitySubgroupBallotKHR:
		return (enabled_extensions.count(VK_EXT_SHADER_SUBGROUP_BALLOT_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilitySubgroupVoteKHR:
		return (enabled_extensions.count(VK_EXT_SHADER_SUBGROUP_VOTE_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityImageReadWriteLodAMD:
		return (enabled_extensions.count(VK_AMD_SHADER_IMAGE_LOAD_STORE_LOD_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityImageGatherBiasLodAMD:
		return (enabled_extensions.count(VK_AMD_TEXTURE_GATHER_BIAS_LOD_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFragmentMaskAMD:
		return (enabled_extensions.count(VK_AMD_SHADER_FRAGMENT_MASK_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilitySampleMaskOverrideCoverageNV:
		return (enabled_extensions.count(VK_NV_SAMPLE_MASK_OVERRIDE_COVERAGE_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGeometryShaderPassthroughNV:
		return (enabled_extensions.count(VK_NV_GEOMETRY_SHADER_PASSTHROUGH_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityShaderViewportIndex:
		return (features.vk12.shaderOutputViewportIndex ||
				enabled_extensions.count(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityShaderLayer:
		return (features.vk12.shaderOutputLayer ||
				enabled_extensions.count(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityShaderViewportIndexLayerEXT:
		// NV version is a cloned enum.
		return (enabled_extensions.count(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME) != 0 ||
				enabled_extensions.count(VK_NV_VIEWPORT_ARRAY2_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityShaderViewportMaskNV:
		return (enabled_extensions.count(VK_NV_VIEWPORT_ARRAY2_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityPerViewAttributesNV:
		return (enabled_extensions.count(VK_NVX_MULTIVIEW_PER_VIEW_ATTRIBUTES_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageBuffer16BitAccess:
		return (features.storage_16bit.storageBuffer16BitAccess == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityUniformAndStorageBuffer16BitAccess:
		return (features.storage_16bit.uniformAndStorageBuffer16BitAccess == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStoragePushConstant16:
		return (features.storage_16bit.storagePushConstant16 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageInputOutput16:
		return (features.storage_16bit.storageInputOutput16 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGroupNonUniform:
		return ((props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGroupNonUniformVote:
		return ((props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGroupNonUniformArithmetic:
		return ((props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGroupNonUniformBallot:
		return ((props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGroupNonUniformShuffle:
		return ((props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGroupNonUniformShuffleRelative:
		return ((props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGroupNonUniformClustered:
		return ((props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_CLUSTERED_BIT) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGroupNonUniformQuad:
		return ((props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGroupNonUniformPartitionedEXT:
		return ((props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_PARTITIONED_BIT_NV) != 0 ||
				features.shader_subgroup_partitioned.shaderSubgroupPartitioned == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilitySampleMaskPostDepthCoverage:
		return (enabled_extensions.count(VK_EXT_POST_DEPTH_COVERAGE_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityShaderNonUniform:
		return (enabled_extensions.count(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) != 0 ||
				api_version >= VK_API_VERSION_1_2) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRuntimeDescriptorArray:
		return (features.descriptor_indexing.runtimeDescriptorArray == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityInputAttachmentArrayDynamicIndexing:
		return (features.descriptor_indexing.shaderInputAttachmentArrayDynamicIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityUniformTexelBufferArrayDynamicIndexing:
		return (features.descriptor_indexing.shaderUniformTexelBufferArrayDynamicIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageTexelBufferArrayDynamicIndexing:
		return (features.descriptor_indexing.shaderStorageTexelBufferArrayDynamicIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityUniformBufferArrayNonUniformIndexing:
		return (features.descriptor_indexing.shaderUniformBufferArrayNonUniformIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilitySampledImageArrayNonUniformIndexing:
		return (features.descriptor_indexing.shaderSampledImageArrayNonUniformIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityUniformTexelBufferArrayNonUniformIndexing:
		return (features.descriptor_indexing.shaderUniformTexelBufferArrayNonUniformIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageBufferArrayNonUniformIndexing:
		return (features.descriptor_indexing.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageImageArrayNonUniformIndexing:
		return (features.descriptor_indexing.shaderStorageImageArrayNonUniformIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityInputAttachmentArrayNonUniformIndexing:
		return (features.descriptor_indexing.shaderInputAttachmentArrayNonUniformIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageTexelBufferArrayNonUniformIndexing:
		return (features.descriptor_indexing.shaderStorageTexelBufferArrayNonUniformIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFloat16:
		return (features.float16_int8.shaderFloat16 == VK_TRUE ||
				enabled_extensions.count(VK_AMD_GPU_SHADER_HALF_FLOAT_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityInt8:
		return (features.float16_int8.shaderInt8 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageBuffer8BitAccess:
		return (features.storage_8bit.storageBuffer8BitAccess == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityUniformAndStorageBuffer8BitAccess:
		return (features.storage_8bit.uniformAndStorageBuffer8BitAccess == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStoragePushConstant8:
		return (features.storage_8bit.storagePushConstant8 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityVulkanMemoryModel:
		return (features.memory_model.vulkanMemoryModel == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityVulkanMemoryModelDeviceScope:
		return (features.memory_model.vulkanMemoryModelDeviceScope == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityDenormPreserve:
		// Not sure if we have to inspect every possible type. Assume compiler won't barf if at least one property is set.
		return (props.float_control.shaderDenormPreserveFloat16 == VK_TRUE ||
		       props.float_control.shaderDenormPreserveFloat32 == VK_TRUE ||
				props.float_control.shaderDenormPreserveFloat64 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityDenormFlushToZero:
		// Not sure if we have to inspect every possible type. Assume compiler won't barf if at least one property is set.
		return (props.float_control.shaderDenormFlushToZeroFloat16 == VK_TRUE ||
		       props.float_control.shaderDenormFlushToZeroFloat32 == VK_TRUE ||
				props.float_control.shaderDenormFlushToZeroFloat64 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilitySignedZeroInfNanPreserve:
		// Not sure if we have to inspect every possible type. Assume compiler won't barf if at least one property is set.
		return (props.float_control.shaderSignedZeroInfNanPreserveFloat16 == VK_TRUE ||
		       props.float_control.shaderSignedZeroInfNanPreserveFloat32 == VK_TRUE ||
				props.float_control.shaderSignedZeroInfNanPreserveFloat64 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRoundingModeRTE:
		// Not sure if we have to inspect every possible type. Assume compiler won't barf if at least one property is set.
		return (props.float_control.shaderRoundingModeRTEFloat16 == VK_TRUE ||
		       props.float_control.shaderRoundingModeRTEFloat32 == VK_TRUE ||
				props.float_control.shaderRoundingModeRTEFloat64 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRoundingModeRTZ:
		// Not sure if we have to inspect every possible type. Assume compiler won't barf if at least one property is set.
		return (props.float_control.shaderRoundingModeRTZFloat16 == VK_TRUE ||
		       props.float_control.shaderRoundingModeRTZFloat32 == VK_TRUE ||
				props.float_control.shaderRoundingModeRTZFloat64 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityComputeDerivativeGroupQuadsKHR:
		return (features.compute_shader_derivatives.computeDerivativeGroupQuads == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityComputeDerivativeGroupLinearKHR:
		return (features.compute_shader_derivatives.computeDerivativeGroupLinear == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFragmentBarycentricKHR:
		return (features.barycentric.fragmentShaderBarycentric == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityImageFootprintNV:
		return (features.image_footprint_nv.imageFootprint == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFragmentDensityEXT:
		// Spec mentions ShadingRateImageNV, but that does not appear to exist?
		return (features.shading_rate_nv.shadingRateImage == VK_TRUE ||
				features.fragment_density.fragmentDensityMap == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityMeshShadingNV:
		return (features.mesh_shader_nv.meshShader == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRayTracingNV:
		return (enabled_extensions.count(VK_NV_RAY_TRACING_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTransformFeedback:
		return (features.transform_feedback.transformFeedback == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityGeometryStreams:
		return (features.transform_feedback.geometryStreams == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityPhysicalStorageBufferAddresses:
		// Apparently these are different types?
		return (features.buffer_device_address.bufferDeviceAddress == VK_TRUE ||
				features.buffer_device_address_ext.bufferDeviceAddress == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCooperativeMatrixNV:
		return (features.cooperative_matrix_nv.cooperativeMatrix == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityIntegerFunctions2INTEL:
		return (features.integer_functions2_intel.shaderIntegerFunctions2 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityShaderSMBuiltinsNV:
		return (features.sm_builtins_nv.shaderSMBuiltins == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFragmentShaderSampleInterlockEXT:
		return (features.shader_interlock.fragmentShaderSampleInterlock == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFragmentShaderPixelInterlockEXT:
		return (features.shader_interlock.fragmentShaderPixelInterlock == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFragmentShaderShadingRateInterlockEXT:
		return (features.shader_interlock.fragmentShaderShadingRateInterlock == VK_TRUE ||
				features.shading_rate_nv.shadingRateImage == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityDemoteToHelperInvocation:
		return (features.demote_to_helper.shaderDemoteToHelperInvocation == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFragmentShadingRateKHR:
		return (features.fragment_shading_rate.primitiveFragmentShadingRate == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRayQueryKHR:
		return (features.ray_query.rayQuery == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRayTracingKHR:
		return (features.ray_tracing_pipeline.rayTracingPipeline == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRayTraversalPrimitiveCullingKHR:
		return (features.ray_tracing_pipeline.rayTraversalPrimitiveCulling == VK_TRUE ||
				features.ray_query.rayQuery == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityWorkgroupMemoryExplicitLayoutKHR:
		return (features.workgroup_memory_explicit_layout.workgroupMemoryExplicitLayout == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityWorkgroupMemoryExplicitLayout8BitAccessKHR:
		return (features.workgroup_memory_explicit_layout.workgroupMemoryExplicitLayout8BitAccess == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityWorkgroupMemoryExplicitLayout16BitAccessKHR:
		return (features.workgroup_memory_explicit_layout.workgroupMemoryExplicitLayout16BitAccess == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityDotProduct:
	case spv::CapabilityDotProductInputAll:
	case spv::CapabilityDotProductInput4x8Bit:
	case spv::CapabilityDotProductInput4x8BitPacked:
		return (features.shader_integer_dot_product.shaderIntegerDotProduct == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityMeshShadingEXT:
		return (features.mesh_shader.meshShader == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFragmentFullyCoveredEXT:
		return (enabled_extensions.count(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	// AtomicFloat16VectorNV does not seem to exist.
	case spv::CapabilityRayCullMaskKHR:
		return (features.ray_tracing_maintenance1.rayTracingMaintenance1 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRayTracingMotionBlurNV:
		return (features.ray_tracing_motion_blur_nv.rayTracingMotionBlur == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRayTracingOpacityMicromapKHR:
		return (enabled_extensions.count(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME) != 0 ||
				features.opacity_micromap_khr.micromap == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTextureSampleWeightedQCOM:
		return (features.image_processing_qcom.textureSampleWeighted == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTextureBoxFilterQCOM:
		return (features.image_processing_qcom.textureBoxFilter == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTextureBlockMatchQCOM:
		return (features.image_processing_qcom.textureBlockMatch == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTextureBlockMatch2QCOM:
		return (features.image_processing2_qcom.textureBlockMatch2 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCoreBuiltinsARM:
		return (features.shader_core_builtins_arm.shaderCoreBuiltins == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityShaderInvocationReorderNV:
		return (enabled_extensions.count(VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	// Ignore CapabilityClusterCullingShadingHUAWEI. It does not exist in SPIR-V headers.
	case spv::CapabilityRayTracingPositionFetchKHR:
	case spv::CapabilityRayQueryPositionFetchKHR:
		return (features.ray_tracing_position_fetch.rayTracingPositionFetch == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTileImageColorReadAccessEXT:
		return (features.shader_tile_image.shaderTileImageColorReadAccess == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTileImageDepthReadAccessEXT:
		return (features.shader_tile_image.shaderTileImageDepthReadAccess == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTileImageStencilReadAccessEXT:
		return (features.shader_tile_image.shaderTileImageStencilReadAccess == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCooperativeMatrixKHR:
		return (features.cooperative_matrix.cooperativeMatrix == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	// Ignore ShaderEnqueueAMDX, it's a beta extension.
	case spv::CapabilityGroupNonUniformRotateKHR:
		return (features.shader_subgroup_rotate.shaderSubgroupRotate == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityExpectAssumeKHR:
		return (features.expect_assume.shaderExpectAssume == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFloatControls2:
		return (features.shader_float_controls2.shaderFloatControls2 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityQuadControlKHR:
		return (features.shader_quad_control.shaderQuadControl == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRawAccessChainsNV:
		return (features.raw_access_chains_nv.shaderRawAccessChains == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityReplicatedCompositesEXT:
		return (features.shader_replicated_composites.shaderReplicatedComposites == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityBFloat16TypeKHR:
		return (features.bfloat16.shaderBFloat16Type == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityBFloat16DotProductKHR:
		return (features.bfloat16.shaderBFloat16DotProduct == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityBFloat16CooperativeMatrixKHR:
		return (features.bfloat16.shaderBFloat16CooperativeMatrix == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTensorAddressingNV:
		return (features.cooperative_matrix2_nv.cooperativeMatrixTensorAddressing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCooperativeMatrixReductionsEXT:
		return (features.cooperative_matrix2_nv.cooperativeMatrixReductions == VK_TRUE ||
				features.cooperative_matrix_maintenance1.cooperativeMatrixReductions == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCooperativeMatrixConversionsNV:
		return (features.cooperative_matrix2_nv.cooperativeMatrixConversions == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCooperativeMatrixConversionsEXT:
		return (features.cooperative_matrix_maintenance1.cooperativeMatrixConversions == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCooperativeMatrixPerElementOperationsEXT:
		return (features.cooperative_matrix2_nv.cooperativeMatrixPerElementOperations == VK_TRUE ||
				features.cooperative_matrix_maintenance1.cooperativeMatrixPerElementOperations == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCooperativeMatrixGetCoordinateEXT:
		return (features.cooperative_matrix_maintenance1.cooperativeMatrixGetCoordinate == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCooperativeMatrixTensorAddressingNV:
		return (features.cooperative_matrix2_nv.cooperativeMatrixTensorAddressing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCooperativeMatrixBlockLoadsNV:
		return (features.cooperative_matrix2_nv.cooperativeMatrixBlockLoads == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRayTracingSpheresGeometryNV:
		return (features.ray_tracing_linear_swept_spheres_nv.spheres == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRayTracingLinearSweptSpheresGeometryNV:
		return (features.ray_tracing_linear_swept_spheres_nv.linearSweptSpheres == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRayTracingClusterAccelerationStructureNV:
		return (features.cluster_acceleration_structure_nv.clusterAccelerationStructure == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCooperativeVectorNV:
		return (features.cooperative_vector_nv.cooperativeVector == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCooperativeVectorTrainingNV:
		return (features.cooperative_vector_nv.cooperativeVectorTraining == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTileShadingQCOM:
		return (features.tile_shading_qcom.tileShading == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFloat8EXT:
		return (features.shader_float8.shaderFloat8 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFloat8CooperativeMatrixEXT:
		return (features.shader_float8.shaderFloat8CooperativeMatrix == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityTensorsARM:
		return (features.tensor_arm.shaderTensorAccess == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageTensorArrayDynamicIndexingARM:
		return (features.tensor_arm.shaderStorageTensorArrayDynamicIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityStorageTensorArrayNonUniformIndexingARM:
		return (features.tensor_arm.shaderStorageTensorArrayNonUniformIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityUntypedPointersKHR:
		return (features.shader_untyped_pointers.shaderUntypedPointers == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityDescriptorHeapEXT:
		return (features.descriptor_heap.descriptorHeap == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityLongVectorEXT:
		return (features.long_vector.longVector == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityShader64BitIndexingEXT:
		return (features.shader_64_bit_indexing.shader64BitIndexing == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityFMAKHR:
		return (features.shader_fma.shaderFmaFloat16 == VK_TRUE || features.shader_fma.shaderFmaFloat32 == VK_TRUE ||
				features.shader_fma.shaderFmaFloat64 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityShaderInvocationReorderEXT:
		return (enabled_extensions.count(VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityPushConstantBanksNV:
		return (features.push_constant_bank_nv.pushConstantBank == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityCooperativeMatrixConversionQCOM:
		return (features.cooperative_matrix_conversion.cooperativeMatrixConversion == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityDotProductFloat16AccFloat32VALVE:
		return (features.shader_mixed_dot_product.shaderMixedFloatDotProductFloat16AccFloat32 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityDotProductFloat16AccFloat16VALVE:
		return (features.shader_mixed_dot_product.shaderMixedFloatDotProductFloat16AccFloat16 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityDotProductBFloat16AccVALVE:
		return (features.shader_mixed_dot_product.shaderMixedFloatDotProductBFloat16Acc == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityDotProductFloat8AccFloat32VALVE:
		return (features.shader_mixed_dot_product.shaderMixedFloatDotProductFloat8AccFloat32 == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	case spv::CapabilityRayTracingOpacityMicromapExecutionModeKHR:
		return (features.opacity_micromap_khr.micromap == VK_TRUE) ||
			   support_reject(SupportKind::Capability, capability_identifier(cap),
							  SupportDetail::Required | SupportDetail::Available | SupportDetail::Raw, 1, 0,
							  uint64_t(cap));
	default:
		LOGE("Unrecognized SPIR-V capability %u, treating as unsupported.\n", unsigned(cap));
		return (false) || support_reject(SupportKind::Capability, capability_identifier(cap), SupportDetail::Raw, 0, 0,
										 uint64_t(cap));
	}
}

static std::string extract_string(const uint32_t *words, uint32_t num_words)
{
	std::string ret;
	for (uint32_t i = 0; i < num_words; i++)
	{
		uint32_t w = words[i];
		for (uint32_t j = 0; j < 4; j++, w >>= 8)
		{
			auto c = char(w & 0xff);
			if (c == '\0')
				return ret;
			ret += c;
		}
	}
	return ret;
}

bool FeatureFilter::Impl::parse_module_info(const uint32_t *data, size_t size, ModuleInfo &info)
{
	// We've already validated the module, now parse information which is related to API limits.
	auto num_words = unsigned(size >> 2);
	spv::Id global_workgroup_size_id = 0;

	struct Constant
	{
		spv::Id id;
		uint32_t literal;
		spv::Id constant_id;
		bool has_constant_id;
	};
	std::vector<Constant> constants;

	const auto add_constant = [&](spv::Id id, uint32_t literal)
	{
		for (auto &c : constants)
		{
			if (c.id == id)
			{
				c.literal = literal;
				return;
			}
		}

		constants.push_back({ id, literal, 0, false });
	};

	const auto find_constant = [&](spv::Id id) -> std::vector<Constant>::const_iterator
	{
		return std::find_if(constants.begin(), constants.end(), [id](const Constant &c) { return c.id == id; });
	};

	unsigned offset = 5;
	while (offset < num_words)
	{
		auto op = static_cast<spv::Op>(data[offset] & 0xffff);
		unsigned count = (data[offset] >> 16) & 0xffff;

		if (op == spv::OpFunction)
		{
			// We're now declaring code, so just stop parsing, there cannot be any capability ops after this.
			break;
		}
		else if (op == spv::OpEntryPoint && count >= 4)
		{
			// Must come first.
			auto execution_model = spv::ExecutionModel(data[offset + 1]);
			spv::Id id = data[offset + 2];

			// We only care about compute-like stages here since we need to validate workgroup sizes.
			if (execution_model == spv::ExecutionModelGLCompute ||
			    execution_model == spv::ExecutionModelTaskEXT ||
			    execution_model == spv::ExecutionModelMeshEXT)
			{
				DeferredEntryPoint entry = {};
				entry.name = extract_string(&data[offset + 3], num_words - (offset + 3));
				entry.execution_model = execution_model;
				entry.id = id;
				info.deferred_entry_points.push_back(entry);
			}
		}
		else if (op == spv::OpExecutionMode || op == spv::OpExecutionModeId)
		{
			// Now we can get execution mode.
			spv::Id id = data[offset + 1];
			auto mode = spv::ExecutionMode(data[offset + 2]);

			auto itr = std::find_if(info.deferred_entry_points.begin(), info.deferred_entry_points.end(),
			                        [id](const DeferredEntryPoint &entry)
			                        { return entry.id == id; });

			if (itr != info.deferred_entry_points.end())
			{
				if ((mode == spv::ExecutionModeLocalSize || mode == spv::ExecutionModeLocalSizeId) && count >= 6)
				{
					if (mode == spv::ExecutionModeLocalSizeId)
					{
						for (unsigned i = 0; i < 3; i++)
							itr->wg_size_id[i] = data[offset + 3 + i];
					}
					else
					{
						for (unsigned i = 0; i < 3; i++)
							itr->wg_size_literal[i] = data[offset + 3 + i];
					}
				}
				else if (op == spv::OpExecutionMode && mode == spv::ExecutionModeOutputPrimitivesEXT && count >= 4)
				{
					itr->max_primitives = data[offset + 3];
				}
				else if (op == spv::OpExecutionMode && mode == spv::ExecutionModeOutputVertices && count >= 4)
				{
					itr->max_vertices = data[offset + 3];
				}
			}
		}
		else if (op == spv::OpDecorate && count >= 4)
		{
			// Now we can get decorations.
			spv::Id target_id = data[offset + 1];
			auto dec = spv::Decoration(data[offset + 2]);
			if (dec == spv::DecorationBuiltIn && spv::BuiltIn(data[offset + 3]) == spv::BuiltInWorkgroupSize)
			{
				global_workgroup_size_id = target_id;
			}
			else if (dec == spv::DecorationSpecId)
			{
				Constant c = {};
				c.id = target_id;
				c.constant_id = data[offset + 3];
				c.has_constant_id = true;
				constants.push_back(c);
			}
		}
		else if ((op == spv::OpConstant || op == spv::OpSpecConstant) && count >= 4)
		{
			spv::Id target_id = data[offset + 2];
			if (count == 4)
				add_constant(target_id, data[offset + 3]);
		}
		else if ((op == spv::OpSpecConstantComposite || op == spv::OpConstantComposite) && count >= 6)
		{
			spv::Id target_id = data[offset + 2];
			if (target_id == global_workgroup_size_id)
			{
				// BuiltInWorkgroupSize overrides all execution modes if it exists (deprecated in SPIR-V 1.6).
				for (auto &entry : info.deferred_entry_points)
					for (unsigned i = 0; i < 3; i++)
						entry.wg_size_id[i] = data[offset + 3 + i];
			}
		}

		offset += count;
	}

	for (auto &entry : info.deferred_entry_points)
	{
		for (unsigned i = 0; i < 3; i++)
		{
			if (entry.wg_size_id[i] != 0)
			{
				auto itr = find_constant(entry.wg_size_id[i]);
				if (itr != constants.end())
				{
					entry.wg_size_literal[i] = itr->literal;
					entry.constant_id[i] = itr->constant_id;
					entry.has_constant_id[i] = itr->has_constant_id;
				}
				else
					return false;
			}
		}
	}

	// If we can statically pass an entry point now, go ahead. Remove it from consideration to save on storage.
	auto itr = std::remove_if(info.deferred_entry_points.begin(), info.deferred_entry_points.end(), [&](const DeferredEntryPoint &e)
	{
		// We have no control over workgroup limits.
		if (e.has_constant_id[0] || e.has_constant_id[1] || e.has_constant_id[2])
			return false;

		// It's possible that we have to fail validate based on FULL_SUBGROUPS usage.
		if (props.subgroup_size_control.maxSubgroupSize &&
		    (e.wg_size_literal[0] % props.subgroup_size_control.maxSubgroupSize) != 0)
		{
			return false;
		}

		// Might have to fail validation based on MaxVertices/MaxPrimitives.
		if (e.execution_model == spv::ExecutionModelMeshEXT &&
		    (e.max_vertices > props.mesh_shader.maxMeshOutputVertices ||
		     e.max_primitives > props.mesh_shader.maxMeshOutputPrimitives))
		{
			return false;
		}

		uint32_t wg_limit[3] = {};
		uint32_t wg_invocations = {};
		switch (e.execution_model)
		{
		case spv::ExecutionModelGLCompute:
			memcpy(wg_limit, props2.properties.limits.maxComputeWorkGroupSize, sizeof(wg_limit));
			wg_invocations = props2.properties.limits.maxComputeWorkGroupInvocations;
			break;

		case spv::ExecutionModelMeshEXT:
			memcpy(wg_limit, props.mesh_shader.maxMeshWorkGroupSize, sizeof(wg_limit));
			wg_invocations = props.mesh_shader.maxMeshWorkGroupInvocations;
			break;

		case spv::ExecutionModelTaskEXT:
			memcpy(wg_limit, props.mesh_shader.maxTaskWorkGroupSize, sizeof(wg_limit));
			wg_invocations = props.mesh_shader.maxTaskWorkGroupInvocations;
			break;

		default:
			break;
		}

		// If anyone tries to create this entry point, we have to fail it.
		for (unsigned i = 0; i < 3; i++)
			if (e.wg_size_literal[i] > wg_limit[i])
				return false;

		if (e.wg_size_literal[0] * e.wg_size_literal[1] * e.wg_size_literal[2] > wg_invocations)
			return false;

		return true;
	});

	info.deferred_entry_points.erase(itr, info.deferred_entry_points.end());

	return true;
}

bool FeatureFilter::Impl::register_shader_module_info(VkShaderModule module, const VkShaderModuleCreateInfo *info)
{
	ModuleInfo module_info;
	if (parse_module_info(info->pCode, info->codeSize, module_info))
	{
		if (!module_info.deferred_entry_points.empty())
		{
			std::lock_guard<std::mutex> holder{module_to_info_lock};
			module_to_info[module] = std::move(module_info);
		}
		return true;
	}
	else
		return false;
}

void FeatureFilter::Impl::unregister_shader_module_info(VkShaderModule module)
{
	std::lock_guard<std::mutex> holder{module_to_info_lock};
	auto itr = module_to_info.find(module);
	if (itr != module_to_info.end())
		module_to_info.erase(itr);
}

bool FeatureFilter::Impl::validate_module_capabilities(const uint32_t *data, size_t size) const
{
	// Trivial SPIR-V parser, just need to poke at Capability opcodes.
	if (size & 3)
	{
		LOGE("SPIR-V module size is not aligned to 4 bytes.\n");
		return support_reject(SupportKind::Malformed, "SPIR-V.instruction");
	}
	if (size < 20)
	{
		LOGE("SPIR-V module size is impossibly small.\n");
		return support_reject(SupportKind::Malformed, "SPIR-V.instruction");
	}
	auto num_words = unsigned(size >> 2);
	if (data[0] != spv::MagicNumber)
	{
		LOGE("Invalid magic number of module.\n");
		return support_reject(SupportKind::Malformed, "SPIR-V.instruction");
	}
	unsigned version = data[1];
	if (version > 0x10600)
	{
		LOGE("SPIR-V version above 1.6 not recognized.\n");
		return support_reject(SupportKind::Property, "spirvVersion", 3, version, 0x10600);
	}
	else if (version == 0x10600)
	{
		if (api_version < VK_API_VERSION_1_3)
		{
			LOGE("SPIR-V 1.6 is only supported in Vulkan 1.3 and up.\n");
			return support_reject(SupportKind::Property, "apiVersion", 3, VK_API_VERSION_1_3, api_version);
		}
	}
	else if (version == 0x10500)
	{
		if (api_version < VK_API_VERSION_1_2)
		{
			LOGE("SPIR-V 1.5 is only supported in Vulkan 1.2 and up.\n");
			return support_reject(SupportKind::Property, "apiVersion", 3, VK_API_VERSION_1_2, api_version);
		}
	}
	else if (version >= 0x10400)
	{
		if (api_version < VK_API_VERSION_1_2 && enabled_extensions.count(VK_KHR_SPIRV_1_4_EXTENSION_NAME) == 0)
		{
			LOGE("Need VK_KHR_spirv_1_4 or Vulkan 1.2 for SPIR-V 1.4.\n");
			return support_reject(SupportKind::Extension, VK_KHR_SPIRV_1_4_EXTENSION_NAME, 3, 1, 0);
		}
	}
	else if (version >= 0x10300)
	{
		if (api_version < VK_API_VERSION_1_1)
		{
			LOGE("Need Vulkan 1.1 for SPIR-V 1.3.\n");
			return support_reject(SupportKind::Property, "apiVersion", 3, VK_API_VERSION_1_1, api_version);
		}
	}
	else if (version > 0x10000)
	{
		if (api_version < VK_API_VERSION_1_1)
		{
			LOGE("Need Vulkan 1.1 for SPIR-V 1.1 or 1.2.\n");
			return support_reject(SupportKind::Property, "apiVersion", 3, VK_API_VERSION_1_1, api_version);
		}
	}
	unsigned offset = 5;
	while (offset < num_words)
	{
		auto op = static_cast<spv::Op>(data[offset] & 0xffff);
		unsigned count = (data[offset] >> 16) & 0xffff;
		if (count == 0)
		{
			LOGE("SPIR-V opcodes cannot consume 0 words.\n");
			return support_reject(SupportKind::Malformed, "SPIR-V.instruction");
		}
		if (offset + count > num_words)
		{
			LOGE("Opcode overflows module.\n");
			return support_reject(SupportKind::Malformed, "SPIR-V.instruction");
		}
		if (op == spv::OpCapability)
		{
			if (count != 2)
			{
				LOGE("Instruction length for OpCapability is wrong.\n");
				return support_reject(SupportKind::Malformed, "SPIR-V.instruction");
			}
			if (!validate_module_capability(static_cast<spv::Capability>(data[offset + 1])))
			{
				LOGE("Capability %u is not supported on this device, ignoring shader module.\n", data[offset + 1]);
				return false;
			}
		}
		else if (op == spv::OpExtension)
		{
			if (count < 2)
			{
				LOGE("Instruction length for OpExtension is wrong.\n");
				return support_reject(SupportKind::Malformed, "SPIR-V.instruction");
			}
			auto name = extract_string(&data[offset + 1], num_words - (offset + 1));
			if (!validate_spirv_extension(name))
			{
				LOGE("SPIR-V extension %s is not supported on this device, ignoring shader module.\n",
				     name.c_str());
				return false;
			}
		}
		else if (op == spv::OpFunction)
		{
			// We're now declaring code, so just stop parsing, there cannot be any capability ops after this.
			break;
		}
		offset += count;
	}
	return true;
}

bool FeatureFilter::Impl::shader_module_is_supported(const VkShaderModuleCreateInfo *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkShaderModuleCreateFlags supported_flags = 0;
	if ((info->flags & ~supported_flags) != 0)
		return support_reject(SupportKind::Constraint, "VkShaderModuleCreateInfo.flags", 3, uint64_t(info->flags),
							  uint64_t(supported_flags));

	if (null_device)
		return true;

	if (!validate_module_capabilities(info->pCode, info->codeSize))
		return false;

	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::render_pass_is_supported(const VkRenderPassCreateInfo *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkRenderPassCreateFlags supported_flags =
			VK_RENDER_PASS_CREATE_TRANSFORM_BIT_QCOM |
			VK_RENDER_PASS_CREATE_PER_LAYER_FRAGMENT_DENSITY_BIT_VALVE;
	if ((info->flags & ~supported_flags) != 0)
		return support_reject(SupportKind::Constraint, "VkRenderPassCreateInfo.flags", 3, uint64_t(info->flags),
							  uint64_t(supported_flags));

	if ((info->flags & VK_RENDER_PASS_CREATE_TRANSFORM_BIT_QCOM) != 0 &&
	    enabled_extensions.count(VK_QCOM_RENDER_PASS_TRANSFORM_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_QCOM_RENDER_PASS_TRANSFORM_EXTENSION_NAME, 3, 1, 0);

	if ((info->flags & VK_RENDER_PASS_CREATE_PER_LAYER_FRAGMENT_DENSITY_BIT_VALVE) != 0 &&
	    enabled_extensions.count(VK_VALVE_FRAGMENT_DENSITY_MAP_LAYERED_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_VALVE_FRAGMENT_DENSITY_MAP_LAYERED_EXTENSION_NAME, 3, 1, 0);

	if (null_device)
		return true;

	if (!pnext_chain_is_supported(info->pNext))
		return false;

	for (uint32_t i = 0; i < info->attachmentCount; i++)
	{
		VkFormatFeatureFlags format_features = 0;
		for (uint32_t j = 0; j < info->subpassCount; j++)
		{
			for (uint32_t k = 0; k < info->pSubpasses[j].colorAttachmentCount; k++)
			{
				if (info->pSubpasses[j].pColorAttachments[k].attachment == i)
					format_features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
				if (info->pSubpasses[j].pResolveAttachments &&
				    info->pSubpasses[j].pResolveAttachments[k].attachment == i)
				{
					format_features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
				}
			}
			for (uint32_t k = 0; k < info->pSubpasses[j].inputAttachmentCount; k++)
				if (info->pSubpasses[j].pInputAttachments[k].attachment == i)
					format_features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
			if (info->pSubpasses[j].pDepthStencilAttachment &&
			    info->pSubpasses[j].pDepthStencilAttachment->attachment == i)
			{
				format_features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
			}
		}
		if (!attachment_description_is_supported(info->pAttachments[i], format_features))
			return false;
	}
	for (uint32_t i = 0; i < info->subpassCount; i++)
	{
		if (!subpass_description_is_supported(info->pSubpasses[i]))
			return false;
	}
	for (uint32_t i = 0; i < info->dependencyCount; i++)
	{
		if (!subpass_dependency_is_supported(info->pDependencies[i]))
			return false;
	}
	return true;
}

bool FeatureFilter::Impl::format_is_supported(VkFormat format, VkFormatFeatureFlags format_features) const
{
	if (!query)
		return true;

	return query->format_is_supported(format, format_features) ||
		   support_reject(SupportKind::Property, "formatFeatures", SupportDetail::Required | SupportDetail::Raw,
						  format_features, 0, uint64_t(format));
}

bool FeatureFilter::Impl::aspect_mask_is_supported(VkImageAspectFlags aspect) const
{
	if (aspect == VK_IMAGE_ASPECT_NONE && features.maintenance4.maintenance4 == VK_FALSE)
		return support_reject(SupportKind::Feature, "maintenance4", 3, 1, uint64_t(features.maintenance4.maintenance4));

	constexpr VkImageAspectFlags supported_flags =
			VK_IMAGE_ASPECT_COLOR_BIT |
			VK_IMAGE_ASPECT_DEPTH_BIT |
			VK_IMAGE_ASPECT_STENCIL_BIT |
			VK_IMAGE_ASPECT_METADATA_BIT |
			VK_IMAGE_ASPECT_PLANE_0_BIT |
			VK_IMAGE_ASPECT_PLANE_1_BIT |
			VK_IMAGE_ASPECT_PLANE_2_BIT |
			VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT |
			VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT |
			VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT |
			VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;

	if (aspect & ~supported_flags)
		return support_reject(SupportKind::Constraint, "aspect", 3, uint64_t(aspect), uint64_t(supported_flags));

	if ((aspect & (VK_IMAGE_ASPECT_PLANE_0_BIT |
	               VK_IMAGE_ASPECT_PLANE_1_BIT |
	               VK_IMAGE_ASPECT_PLANE_2_BIT)) != 0 &&
	    features.ycbcr_conversion.samplerYcbcrConversion == VK_FALSE)
		return support_reject(SupportKind::Feature, "samplerYcbcrConversion", 3, 1,
							  uint64_t(features.ycbcr_conversion.samplerYcbcrConversion));

	if ((aspect & (VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT |
	               VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT |
	               VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT |
	               VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT)) != 0 &&
	    enabled_extensions.count(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, 3, 1, 0);

	return true;
}

bool FeatureFilter::Impl::access_mask_is_supported(VkAccessFlags2 access) const
{
	constexpr VkAccessFlags2 sync2_flags =
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
			VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
			VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

	constexpr VkAccessFlags2 supported_flags =
			VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
			VK_ACCESS_INDEX_READ_BIT |
			VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
			VK_ACCESS_UNIFORM_READ_BIT |
			VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
			VK_ACCESS_SHADER_READ_BIT |
			VK_ACCESS_SHADER_WRITE_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_TRANSFER_READ_BIT |
			VK_ACCESS_TRANSFER_WRITE_BIT |
			VK_ACCESS_HOST_READ_BIT |
			VK_ACCESS_HOST_WRITE_BIT |
			VK_ACCESS_MEMORY_READ_BIT |
			VK_ACCESS_MEMORY_WRITE_BIT |
			VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT |
			VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT |
			VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT |
			VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT |
			VK_ACCESS_COLOR_ATTACHMENT_READ_NONCOHERENT_BIT_EXT |
			VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
			VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
			VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT |
			VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR |
			VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR |
			VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR |
			VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR |
			VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR |
			VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT |
			VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR |
			VK_ACCESS_2_MICROMAP_READ_BIT_EXT |
			VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT |
			VK_ACCESS_2_COMMAND_PREPROCESS_READ_BIT_EXT |
			VK_ACCESS_2_COMMAND_PREPROCESS_WRITE_BIT_EXT |
			VK_ACCESS_2_OPTICAL_FLOW_READ_BIT_NV |
			VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV |
			VK_ACCESS_2_SAMPLER_HEAP_READ_BIT_EXT |
			VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT |
			sync2_flags;

	if (access & ~supported_flags)
		return support_reject(SupportKind::Constraint, "access", 3, uint64_t(access), uint64_t(supported_flags));

	if ((access & (VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR)) != 0 &&
	    enabled_extensions.count(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, 3, 1, 0);

	if ((access & (VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR)) != 0 &&
	    enabled_extensions.count(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME, 3, 1, 0);

	if ((access & (VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT |
	               VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT |
	               VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT)) != 0 &&
	    features.transform_feedback.transformFeedback == VK_FALSE)
		return support_reject(SupportKind::Feature, "transformFeedback", 3, 1,
							  uint64_t(features.transform_feedback.transformFeedback));

	if ((access & VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT) != 0 &&
	    features.conditional_rendering.conditionalRendering == VK_FALSE)
		return support_reject(SupportKind::Feature, "conditionalRendering", 3, 1,
							  uint64_t(features.conditional_rendering.conditionalRendering));

	if ((access & VK_ACCESS_COLOR_ATTACHMENT_READ_NONCOHERENT_BIT_EXT) != 0 &&
	    enabled_extensions.count(VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME, 3, 1, 0);

	if ((access & (VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
	               VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR)) != 0 &&
	    features.acceleration_structure.accelerationStructure == VK_FALSE)
		return support_reject(SupportKind::Feature, "accelerationStructure", 3, 1,
							  uint64_t(features.acceleration_structure.accelerationStructure));

	if ((access & VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT) != 0 &&
	    features.fragment_density.fragmentDensityMap == VK_FALSE)
		return support_reject(SupportKind::Feature, "fragmentDensityMap", 3, 1,
							  uint64_t(features.fragment_density.fragmentDensityMap));

	if ((access & VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR) != 0 &&
	    features.fragment_shading_rate.attachmentFragmentShadingRate == VK_FALSE)
		return support_reject(SupportKind::Feature, "attachmentFragmentShadingRate", 3, 1,
							  uint64_t(features.fragment_shading_rate.attachmentFragmentShadingRate));

	if ((access & sync2_flags) != 0 && features.synchronization2.synchronization2 == VK_FALSE)
		return support_reject(SupportKind::Feature, "synchronization2", 3, 1,
							  uint64_t(features.synchronization2.synchronization2));

	if ((access & VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT) != 0 &&
	    features.descriptor_buffer.descriptorBuffer == VK_FALSE)
		return support_reject(SupportKind::Feature, "descriptorBuffer", 3, 1,
							  uint64_t(features.descriptor_buffer.descriptorBuffer));

	if ((access & VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR) != 0 &&
	    features.ray_tracing_maintenance1.rayTracingMaintenance1 == VK_FALSE)
		return support_reject(SupportKind::Feature, "rayTracingMaintenance1", 3, 1,
							  uint64_t(features.ray_tracing_maintenance1.rayTracingMaintenance1));

	if ((access & (VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT | VK_ACCESS_2_MICROMAP_READ_BIT_EXT)) != 0 &&
	    features.opacity_micromap_ext.micromap == VK_FALSE)
		return support_reject(SupportKind::Feature, "micromap", 3, 1, uint64_t(features.opacity_micromap_ext.micromap));

	if ((access & (VK_ACCESS_2_COMMAND_PREPROCESS_READ_BIT_EXT |
	               VK_ACCESS_2_COMMAND_PREPROCESS_WRITE_BIT_EXT)) != 0 &&
	    features.device_generated_commands_nv.deviceGeneratedCommands == VK_FALSE &&
	    features.device_generated_commands.deviceGeneratedCommands == VK_FALSE)
	{
		if (!features.device_generated_commands_nv.deviceGeneratedCommands)
			return support_reject(SupportKind::Feature, "deviceGeneratedCommands", 3, 1,
								  uint64_t(features.device_generated_commands_nv.deviceGeneratedCommands));
		if (!features.device_generated_commands.deviceGeneratedCommands)
			return support_reject(SupportKind::Feature, "deviceGeneratedCommands", 3, 1,
								  uint64_t(features.device_generated_commands.deviceGeneratedCommands));
		return false;
	}

	if ((access & (VK_ACCESS_2_OPTICAL_FLOW_READ_BIT_NV |
	               VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV)) != 0 &&
	    features.optical_flow_nv.opticalFlow == VK_FALSE)
		return support_reject(SupportKind::Feature, "opticalFlow", 3, 1,
							  uint64_t(features.optical_flow_nv.opticalFlow));

	if ((access & (VK_ACCESS_2_SAMPLER_HEAP_READ_BIT_EXT | VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT)) != 0 &&
	    features.descriptor_heap.descriptorHeap == VK_FALSE)
		return support_reject(SupportKind::Feature, "descriptorHeap", 3, 1,
							  uint64_t(features.descriptor_heap.descriptorHeap));

	return true;
}

bool FeatureFilter::Impl::shader_stage_mask_is_supported(VkShaderStageFlags stages) const
{
	if (stages == VK_SHADER_STAGE_ALL || stages == VK_SHADER_STAGE_ALL_GRAPHICS)
		return true;

	constexpr VkShaderStageFlags supported_flags =
			VK_SHADER_STAGE_VERTEX_BIT |
			VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
			VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
			VK_SHADER_STAGE_GEOMETRY_BIT |
			VK_SHADER_STAGE_FRAGMENT_BIT |
			VK_SHADER_STAGE_COMPUTE_BIT |
			VK_SHADER_STAGE_RAYGEN_BIT_KHR |
			VK_SHADER_STAGE_CALLABLE_BIT_KHR |
			VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
			VK_SHADER_STAGE_MISS_BIT_KHR |
			VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
			VK_SHADER_STAGE_INTERSECTION_BIT_KHR |
			VK_SHADER_STAGE_MESH_BIT_EXT |
			VK_SHADER_STAGE_TASK_BIT_EXT;

	if (stages & ~supported_flags)
		return support_reject(SupportKind::Constraint, "stages", 3, uint64_t(stages), uint64_t(supported_flags));

	if ((stages & (VK_SHADER_STAGE_RAYGEN_BIT_KHR |
	               VK_SHADER_STAGE_CALLABLE_BIT_KHR |
	               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
	               VK_SHADER_STAGE_MISS_BIT_KHR |
	               VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
	               VK_SHADER_STAGE_INTERSECTION_BIT_KHR)) != 0 &&
	    features.ray_tracing_pipeline.rayTracingPipeline == VK_FALSE)
	{
		return support_reject(SupportKind::Feature, "rayTracingPipeline", 3, 1,
							  uint64_t(features.ray_tracing_pipeline.rayTracingPipeline));
	}
	if ((stages & VK_SHADER_STAGE_MESH_BIT_EXT) != 0 &&
	    features.mesh_shader.meshShader == VK_FALSE &&
	    features.mesh_shader_nv.meshShader == VK_FALSE)
	{
		if (!features.mesh_shader.meshShader)
			return support_reject(SupportKind::Feature, "meshShader", 3, 1, uint64_t(features.mesh_shader.meshShader));
		if (!features.mesh_shader_nv.meshShader)
			return support_reject(SupportKind::Feature, "meshShader", 3, 1,
								  uint64_t(features.mesh_shader_nv.meshShader));
		return false;
	}

	if ((stages & VK_SHADER_STAGE_TASK_BIT_EXT) != 0 &&
	    features.mesh_shader.taskShader == VK_FALSE &&
	    features.mesh_shader_nv.taskShader == VK_FALSE)
	{
		if (!features.mesh_shader.taskShader)
			return support_reject(SupportKind::Feature, "taskShader", 3, 1, uint64_t(features.mesh_shader.taskShader));
		if (!features.mesh_shader_nv.taskShader)
			return support_reject(SupportKind::Feature, "taskShader", 3, 1,
								  uint64_t(features.mesh_shader_nv.taskShader));
		return false;
	}

	return true;
}

bool FeatureFilter::Impl::pipeline_stage_mask_is_supported(VkPipelineStageFlags2 stages) const
{
	if (stages == VK_PIPELINE_STAGE_NONE && features.synchronization2.synchronization2 == VK_FALSE)
		return support_reject(SupportKind::Feature, "synchronization2", 3, 1,
							  uint64_t(features.synchronization2.synchronization2));

	constexpr VkPipelineStageFlags2 sync2_stages =
			VK_PIPELINE_STAGE_2_COPY_BIT |
			VK_PIPELINE_STAGE_2_CLEAR_BIT |
			VK_PIPELINE_STAGE_2_RESOLVE_BIT |
			VK_PIPELINE_STAGE_2_BLIT_BIT |
			VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
			VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT |
			VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;

	constexpr VkPipelineStageFlags2 supported_flags =
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
			VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
			VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
			VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
			VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
			VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
			VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
			VK_PIPELINE_STAGE_TRANSFER_BIT |
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT |
			VK_PIPELINE_STAGE_HOST_BIT |
			VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT |
			VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR |
			VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR |
			VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT |
			VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT |
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
			VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
			VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT |
			VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
			VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR |
			VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT |
			VK_PIPELINE_STAGE_COMMAND_PREPROCESS_BIT_EXT |
			VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV |
			VK_PIPELINE_STAGE_2_CONVERT_COOPERATIVE_VECTOR_MATRIX_BIT_NV |
			sync2_stages;

	if (stages & ~supported_flags)
		return support_reject(SupportKind::Constraint, "stages", 3, uint64_t(stages), uint64_t(supported_flags));

	if ((stages & VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR) != 0 &&
	    enabled_extensions.count(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, 3, 1, 0);

	if ((stages & VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR) != 0 &&
	    enabled_extensions.count(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME, 3, 1, 0);

	if ((stages & VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT) != 0 &&
	    features.transform_feedback.transformFeedback == VK_FALSE)
		return support_reject(SupportKind::Feature, "transformFeedback", 3, 1,
							  uint64_t(features.transform_feedback.transformFeedback));

	if ((stages & VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT) != 0 &&
	    features.conditional_rendering.conditionalRendering == VK_FALSE)
		return support_reject(SupportKind::Feature, "conditionalRendering", 3, 1,
							  uint64_t(features.conditional_rendering.conditionalRendering));

	if ((stages & VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR) != 0 &&
	    features.acceleration_structure.accelerationStructure == VK_FALSE)
		return support_reject(SupportKind::Feature, "accelerationStructure", 3, 1,
							  uint64_t(features.acceleration_structure.accelerationStructure));

	if ((stages & VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR) != 0 &&
	    features.ray_tracing_pipeline.rayTracingPipeline == VK_FALSE)
		return support_reject(SupportKind::Feature, "rayTracingPipeline", 3, 1,
							  uint64_t(features.ray_tracing_pipeline.rayTracingPipeline));

	if ((stages & VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT) != 0 &&
	    features.fragment_density.fragmentDensityMap == VK_FALSE)
		return support_reject(SupportKind::Feature, "fragmentDensityMap", 3, 1,
							  uint64_t(features.fragment_density.fragmentDensityMap));

	if ((stages & VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR) != 0 &&
	    features.fragment_shading_rate.attachmentFragmentShadingRate == VK_FALSE)
		return support_reject(SupportKind::Feature, "attachmentFragmentShadingRate", 3, 1,
							  uint64_t(features.fragment_shading_rate.attachmentFragmentShadingRate));

	if ((stages & sync2_stages) != 0 && features.synchronization2.synchronization2 == VK_FALSE)
		return support_reject(SupportKind::Feature, "synchronization2", 3, 1,
							  uint64_t(features.synchronization2.synchronization2));

	if ((stages & VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR) != 0 &&
	    features.ray_tracing_maintenance1.rayTracingMaintenance1 == VK_FALSE)
		return support_reject(SupportKind::Feature, "rayTracingMaintenance1", 3, 1,
							  uint64_t(features.ray_tracing_maintenance1.rayTracingMaintenance1));

	if ((stages & VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT) != 0 &&
	    features.opacity_micromap_ext.micromap == VK_FALSE)
		return support_reject(SupportKind::Feature, "micromap", 3, 1, uint64_t(features.opacity_micromap_ext.micromap));

	if ((stages & VK_PIPELINE_STAGE_COMMAND_PREPROCESS_BIT_EXT) != 0 &&
	    features.device_generated_commands_nv.deviceGeneratedCommands == VK_FALSE &&
	    features.device_generated_commands.deviceGeneratedCommands == VK_FALSE)
	{
		if (!features.device_generated_commands_nv.deviceGeneratedCommands)
			return support_reject(SupportKind::Feature, "deviceGeneratedCommands", 3, 1,
								  uint64_t(features.device_generated_commands_nv.deviceGeneratedCommands));
		if (!features.device_generated_commands.deviceGeneratedCommands)
			return support_reject(SupportKind::Feature, "deviceGeneratedCommands", 3, 1,
								  uint64_t(features.device_generated_commands.deviceGeneratedCommands));
		return false;
	}

	if ((stages & VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV) != 0 &&
	    features.optical_flow_nv.opticalFlow == VK_FALSE)
		return support_reject(SupportKind::Feature, "opticalFlow", 3, 1,
							  uint64_t(features.optical_flow_nv.opticalFlow));

	if ((stages & VK_PIPELINE_STAGE_2_CONVERT_COOPERATIVE_VECTOR_MATRIX_BIT_NV) != 0 &&
	    features.cooperative_vector_nv.cooperativeVector == VK_FALSE)
		return support_reject(SupportKind::Feature, "cooperativeVector", 3, 1,
							  uint64_t(features.cooperative_vector_nv.cooperativeVector));

	return true;
}

bool FeatureFilter::Impl::load_op_is_supported(VkAttachmentLoadOp load_op) const
{
	switch (load_op)
	{
	case VK_ATTACHMENT_LOAD_OP_CLEAR:
	case VK_ATTACHMENT_LOAD_OP_LOAD:
	case VK_ATTACHMENT_LOAD_OP_DONT_CARE:
		return true;
	case VK_ATTACHMENT_LOAD_OP_NONE_KHR:
		return (enabled_extensions.count(VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME) != 0 ||
				enabled_extensions.count(VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Extension, VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME, 3, 1, 0);
	default:
		return support_reject(SupportKind::Constraint, "VkAttachmentLoadOp", SupportDetail::Raw, 0, 0,
							  uint64_t(load_op));
	}
}

bool FeatureFilter::Impl::store_op_is_supported(VkAttachmentStoreOp store_op) const
{
	switch (store_op)
	{
	case VK_ATTACHMENT_STORE_OP_DONT_CARE:
	case VK_ATTACHMENT_STORE_OP_STORE:
		return true;
	case VK_ATTACHMENT_STORE_OP_NONE_KHR:
		return (api_version >= VK_API_VERSION_1_3 ||
		       enabled_extensions.count(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) != 0 ||
		       enabled_extensions.count(VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME) != 0 ||
		       enabled_extensions.count(VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME) != 0 ||
				enabled_extensions.count(VK_QCOM_RENDER_PASS_STORE_OPS_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Extension, VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME, 3, 1, 0);
	default:
		return support_reject(SupportKind::Constraint, "VkAttachmentStoreOp", SupportDetail::Raw, 0, 0,
							  uint64_t(store_op));
	}
}

bool FeatureFilter::Impl::polygon_mode_is_supported(VkPolygonMode mode) const
{
	switch (mode)
	{
	case VK_POLYGON_MODE_FILL:
		return true;
	case VK_POLYGON_MODE_POINT:
	case VK_POLYGON_MODE_LINE:
		return (features2.features.fillModeNonSolid == VK_TRUE) ||
			   support_reject(SupportKind::Feature, "fillModeNonSolid", 3, 1,
							  uint64_t(features2.features.fillModeNonSolid));
	case VK_POLYGON_MODE_FILL_RECTANGLE_NV:
		return (enabled_extensions.count(VK_NV_FILL_RECTANGLE_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Extension, VK_NV_FILL_RECTANGLE_EXTENSION_NAME, 3, 1, 0);
	default:
		return support_reject(SupportKind::Constraint, "VkPolygonMode", SupportDetail::Raw, 0, 0, uint64_t(mode));
	}
}

bool FeatureFilter::Impl::image_layout_is_supported(VkImageLayout layout) const
{
	switch (layout)
	{
	case VK_IMAGE_LAYOUT_UNDEFINED:
	case VK_IMAGE_LAYOUT_GENERAL:
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
	case VK_IMAGE_LAYOUT_PREINITIALIZED:
		return true;
	case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
		return (enabled_extensions.count(VK_KHR_SWAPCHAIN_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Extension, VK_KHR_SWAPCHAIN_EXTENSION_NAME, 3, 1, 0);
	case VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR:
		return (enabled_extensions.count(VK_KHR_SHARED_PRESENTABLE_IMAGE_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Extension, VK_KHR_SHARED_PRESENTABLE_IMAGE_EXTENSION_NAME, 3, 1, 0);
	case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
	case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
		return api_version >= VK_API_VERSION_1_1 || enabled_extensions.count(VK_KHR_MAINTENANCE2_EXTENSION_NAME) != 0;
	case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
	case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
	case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
	case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
		return (features.separate_ds_layout.separateDepthStencilLayouts == VK_TRUE) ||
			   support_reject(SupportKind::Feature, "separateDepthStencilLayouts", 3, 1,
							  uint64_t(features.separate_ds_layout.separateDepthStencilLayouts));
	case VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR:
		return (features.fragment_shading_rate.attachmentFragmentShadingRate == VK_TRUE) ||
			   support_reject(SupportKind::Feature, "attachmentFragmentShadingRate", 3, 1,
							  uint64_t(features.fragment_shading_rate.attachmentFragmentShadingRate));
	case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR:
	case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL_KHR:
		return (features.synchronization2.synchronization2 == VK_TRUE) ||
			   support_reject(SupportKind::Feature, "synchronization2", 3, 1,
							  uint64_t(features.synchronization2.synchronization2));
	case VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT:
		return (features.attachment_feedback_loop_layout.attachmentFeedbackLoopLayout == VK_TRUE) ||
			   support_reject(SupportKind::Feature, "attachmentFeedbackLoopLayout", 3, 1,
							  uint64_t(features.attachment_feedback_loop_layout.attachmentFeedbackLoopLayout));
	case VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR:
	case VK_IMAGE_LAYOUT_VIDEO_DECODE_SRC_KHR:
	case VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR:
		return (enabled_extensions.count(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Extension, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, 3, 1, 0);
	case VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR:
	case VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR:
	case VK_IMAGE_LAYOUT_VIDEO_ENCODE_DST_KHR:
		return (enabled_extensions.count(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME) != 0) ||
			   support_reject(SupportKind::Extension, VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME, 3, 1, 0);
	case VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT:
		return (features.fragment_density.fragmentDensityMap == VK_TRUE) ||
			   support_reject(SupportKind::Feature, "fragmentDensityMap", 3, 1,
							  uint64_t(features.fragment_density.fragmentDensityMap));
	case VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR:
		return (features.dynamic_rendering_local_read.dynamicRenderingLocalRead == VK_TRUE) ||
			   support_reject(SupportKind::Feature, "dynamicRenderingLocalRead", 3, 1,
							  uint64_t(features.dynamic_rendering_local_read.dynamicRenderingLocalRead));
	case VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT:
		return (features.zero_initialize_device_memory.zeroInitializeDeviceMemory == VK_TRUE) ||
			   support_reject(SupportKind::Feature, "zeroInitializeDeviceMemory", 3, 1,
							  uint64_t(features.zero_initialize_device_memory.zeroInitializeDeviceMemory));
	default:
		return support_reject(SupportKind::Constraint, "VkImageLayout", SupportDetail::Raw, 0, 0, uint64_t(layout));
	}
}

bool FeatureFilter::Impl::attachment_reference_is_supported(const VkAttachmentReference &ref) const
{
	if (!image_layout_is_supported(ref.layout))
		return false;

	return true;
}

bool FeatureFilter::Impl::attachment_reference2_is_supported(const VkAttachmentReference2 &ref) const
{
	if (!pnext_chain_is_supported(ref.pNext))
		return false;
	if (!image_layout_is_supported(ref.layout))
		return false;
	if (!aspect_mask_is_supported(ref.aspectMask))
		return false;

	return true;
}

bool FeatureFilter::Impl::attachment_description_flags_are_supported(VkAttachmentDescriptionFlags flags) const
{
	constexpr VkAttachmentDescriptionFlags recognized_flags =
			VK_ATTACHMENT_DESCRIPTION_RESOLVE_ENABLE_TRANSFER_FUNCTION_BIT_KHR |
			VK_ATTACHMENT_DESCRIPTION_RESOLVE_SKIP_TRANSFER_FUNCTION_BIT_KHR |
			VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT;

	if (flags & ~recognized_flags)
		return support_reject(SupportKind::Constraint, "flags", 3, uint64_t(flags), uint64_t(recognized_flags));

	if ((flags & (
		     VK_ATTACHMENT_DESCRIPTION_RESOLVE_ENABLE_TRANSFER_FUNCTION_BIT_KHR |
		     VK_ATTACHMENT_DESCRIPTION_RESOLVE_SKIP_TRANSFER_FUNCTION_BIT_KHR)) != 0)
	{
		if (!features.maintenance10.maintenance10)
			return support_reject(SupportKind::Feature, "maintenance10", 3, 1,
								  uint64_t(features.maintenance10.maintenance10));
		if (!props.maintenance10.resolveSrgbFormatSupportsTransferFunctionControl)
			return support_reject(SupportKind::Property, "resolveSrgbFormatSupportsTransferFunctionControl", 3,
								  uint64_t(1),
								  uint64_t(props.maintenance10.resolveSrgbFormatSupportsTransferFunctionControl));
	}
	return true;
}

bool FeatureFilter::Impl::attachment_description_is_supported(const VkAttachmentDescription &desc,
                                                              VkFormatFeatureFlags format_features) const
{

	if (!attachment_description_flags_are_supported(desc.flags))
		return false;
	if (!image_layout_is_supported(desc.initialLayout))
		return false;
	if (!image_layout_is_supported(desc.finalLayout))
		return false;
	if (format_features && !format_is_supported(desc.format, format_features))
		return false;
	if (!load_op_is_supported(desc.loadOp))
		return false;
	if (!load_op_is_supported(desc.stencilLoadOp))
		return false;
	if (!store_op_is_supported(desc.storeOp))
		return false;
	if (!store_op_is_supported(desc.stencilStoreOp))
		return false;

	return true;
}

bool FeatureFilter::Impl::attachment_description2_is_supported(const VkAttachmentDescription2 &desc,
                                                               VkFormatFeatureFlags format_features) const
{
	if (!attachment_description_flags_are_supported(desc.flags))
		return false;
	if (!pnext_chain_is_supported(desc.pNext))
		return false;
	if (!image_layout_is_supported(desc.initialLayout))
		return false;
	if (!image_layout_is_supported(desc.finalLayout))
		return false;
	if (format_features && !format_is_supported(desc.format, format_features))
		return false;
	if (!load_op_is_supported(desc.loadOp))
		return false;
	if (!load_op_is_supported(desc.stencilLoadOp))
		return false;
	if (!store_op_is_supported(desc.storeOp))
		return false;
	if (!store_op_is_supported(desc.stencilStoreOp))
		return false;

	return true;
}

bool FeatureFilter::Impl::subpass_description_flags_is_supported(VkSubpassDescriptionFlags flags) const
{
	constexpr VkSubpassDescriptionFlags supported_flags =
			VK_SUBPASS_DESCRIPTION_PER_VIEW_ATTRIBUTES_BIT_NVX |
			VK_SUBPASS_DESCRIPTION_PER_VIEW_POSITION_X_ONLY_BIT_NVX |
			VK_SUBPASS_DESCRIPTION_ENABLE_LEGACY_DITHERING_BIT_EXT |
			VK_SUBPASS_DESCRIPTION_CUSTOM_RESOLVE_BIT_EXT |
			VK_SUBPASS_DESCRIPTION_FRAGMENT_REGION_BIT_EXT;

	if (flags & ~supported_flags)
		return support_reject(SupportKind::Constraint, "flags", 3, uint64_t(flags), uint64_t(supported_flags));

	if ((flags & (VK_SUBPASS_DESCRIPTION_PER_VIEW_ATTRIBUTES_BIT_NVX |
	              VK_SUBPASS_DESCRIPTION_PER_VIEW_POSITION_X_ONLY_BIT_NVX)) != 0 &&
	    enabled_extensions.count(VK_NVX_MULTIVIEW_PER_VIEW_ATTRIBUTES_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_NVX_MULTIVIEW_PER_VIEW_ATTRIBUTES_EXTENSION_NAME, 3, 1, 0);

	if ((flags & (VK_SUBPASS_DESCRIPTION_FRAGMENT_REGION_BIT_EXT |
	              VK_SUBPASS_DESCRIPTION_CUSTOM_RESOLVE_BIT_EXT)) != 0)
	{
		if (enabled_extensions.count(VK_QCOM_RENDER_PASS_SHADER_RESOLVE_EXTENSION_NAME) == 0 &&
		    features.custom_resolve.customResolve == VK_FALSE)
		{
			{
				if (!enabled_extensions.count(VK_QCOM_RENDER_PASS_SHADER_RESOLVE_EXTENSION_NAME))
					return support_reject(SupportKind::Extension, VK_QCOM_RENDER_PASS_SHADER_RESOLVE_EXTENSION_NAME, 3,
										  1, 0);
				if (!features.custom_resolve.customResolve)
					return support_reject(SupportKind::Feature, "customResolve", 3, 1,
										  uint64_t(features.custom_resolve.customResolve));
				return false;
			}
		}
	}

	if ((flags & VK_SUBPASS_DESCRIPTION_ENABLE_LEGACY_DITHERING_BIT_EXT) != 0 &&
	    features.legacy_dithering.legacyDithering == VK_FALSE)
		return support_reject(SupportKind::Feature, "legacyDithering", 3, 1,
							  uint64_t(features.legacy_dithering.legacyDithering));

	return true;
}

bool FeatureFilter::Impl::subpass_description_is_supported(const VkSubpassDescription &sub) const
{
	if (!subpass_description_flags_is_supported(sub.flags))
		return false;

	for (uint32_t j = 0; j < sub.colorAttachmentCount; j++)
	{
		if (!attachment_reference_is_supported(sub.pColorAttachments[j]))
			return false;
		if (sub.pResolveAttachments && !attachment_reference_is_supported(sub.pResolveAttachments[j]))
			return false;
	}
	for (uint32_t j = 0; j < sub.inputAttachmentCount; j++)
		if (!attachment_reference_is_supported(sub.pInputAttachments[j]))
			return false;
	if (sub.pDepthStencilAttachment && !attachment_reference_is_supported(*sub.pDepthStencilAttachment))
		return false;
	return true;
}

bool FeatureFilter::Impl::subpass_description2_is_supported(const VkSubpassDescription2 &sub) const
{
	if (!subpass_description_flags_is_supported(sub.flags))
		return false;

	if (!pnext_chain_is_supported(sub.pNext))
		return false;

	for (uint32_t j = 0; j < sub.colorAttachmentCount; j++)
	{
		if (!attachment_reference2_is_supported(sub.pColorAttachments[j]))
			return false;
		if (sub.pResolveAttachments && !attachment_reference2_is_supported(sub.pResolveAttachments[j]))
			return false;
	}
	for (uint32_t j = 0; j < sub.inputAttachmentCount; j++)
		if (!attachment_reference2_is_supported(sub.pInputAttachments[j]))
			return false;
	if (sub.pDepthStencilAttachment && !attachment_reference2_is_supported(*sub.pDepthStencilAttachment))
		return false;
	if (sub.viewMask && !multiview_mask_is_supported(sub.viewMask))
		return false;
	return true;
}

bool FeatureFilter::Impl::dependency_flags_is_supported(VkDependencyFlags deps) const
{
	constexpr VkDependencyFlags supported_flags = VK_DEPENDENCY_BY_REGION_BIT |
	                                              VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT |
	                                              VK_DEPENDENCY_VIEW_LOCAL_BIT |
	                                              VK_DEPENDENCY_DEVICE_GROUP_BIT;

	if ((deps & ~supported_flags) != 0)
		return support_reject(SupportKind::Constraint, "deps", 3, uint64_t(deps), uint64_t(supported_flags));

	if ((deps & VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT) != 0)
		if (!features.attachment_feedback_loop_layout.attachmentFeedbackLoopLayout)
			return support_reject(SupportKind::Feature, "attachmentFeedbackLoopLayout", 3, 1,
								  uint64_t(features.attachment_feedback_loop_layout.attachmentFeedbackLoopLayout));

	if ((deps & VK_DEPENDENCY_VIEW_LOCAL_BIT) != 0)
		if (!features.multiview.multiview)
			return support_reject(SupportKind::Feature, "multiview", 3, 1, uint64_t(features.multiview.multiview));

	if ((deps & VK_DEPENDENCY_DEVICE_GROUP_BIT) != 0 && api_version < VK_API_VERSION_1_1)
		return support_reject(SupportKind::Property, "apiVersion", 3, VK_API_VERSION_1_1, api_version);

	return true;
}

bool FeatureFilter::Impl::subpass_dependency_is_supported(const VkSubpassDependency &dep) const
{
	if (!dependency_flags_is_supported(dep.dependencyFlags))
		return false;
	return true;
}

bool FeatureFilter::Impl::subpass_dependency2_is_supported(const VkSubpassDependency2 &dep) const
{
	if (!dependency_flags_is_supported(dep.dependencyFlags))
		return false;
	if (!pnext_chain_is_supported(dep.pNext))
		return false;

	return true;
}

bool FeatureFilter::Impl::stage_limits_are_supported(const VkPipelineShaderStageCreateInfo &info) const
{
	spv::ExecutionModel target_model;
	uint32_t limit_workgroup_size[3];
	uint32_t limit_invocations;

	if (!info.pName)
		return support_reject(SupportKind::Malformed, "stage.pName");

	switch (info.stage)
	{
	case VK_SHADER_STAGE_COMPUTE_BIT:
		target_model = spv::ExecutionModelGLCompute;
		memcpy(limit_workgroup_size, props2.properties.limits.maxComputeWorkGroupSize, sizeof(limit_workgroup_size));
		limit_invocations = props2.properties.limits.maxComputeWorkGroupInvocations;
		break;
	case VK_SHADER_STAGE_TASK_BIT_EXT:
		target_model = spv::ExecutionModelTaskEXT;
		memcpy(limit_workgroup_size, props.mesh_shader.maxTaskWorkGroupSize, sizeof(limit_workgroup_size));
		limit_invocations = props.mesh_shader.maxTaskWorkGroupInvocations;
		break;
	case VK_SHADER_STAGE_MESH_BIT_EXT:
		target_model = spv::ExecutionModelMeshEXT;
		memcpy(limit_workgroup_size, props.mesh_shader.maxMeshWorkGroupSize, sizeof(limit_workgroup_size));
		limit_invocations = props.mesh_shader.maxMeshWorkGroupInvocations;
		break;
	default:
		// Shouldn't happen. Only happens for invalid input.
		return true;
	}
	std::lock_guard<std::mutex> holder{module_to_info_lock};
	// If we cannot find anything, assume that we're statically okay.
	auto itr = module_to_info.find(info.module);
	if (itr == module_to_info.end())
		return true;
	const DeferredEntryPoint *deferred_entry_point = nullptr;
	for (auto &entry : itr->second.deferred_entry_points)
	{
		if (target_model == entry.execution_model && entry.name == info.pName)
		{
			deferred_entry_point = &entry;
			break;
		}
	}
	if (deferred_entry_point)
	{
		auto &entry = *deferred_entry_point;
		uint32_t wg_size[3] = {};
		if (entry.execution_model == spv::ExecutionModelMeshEXT)
		{
			if (entry.max_primitives > props.mesh_shader.maxMeshOutputPrimitives)
				return support_reject(SupportKind::Property, "maxMeshOutputPrimitives", 3,
									  uint64_t(entry.max_primitives),
									  uint64_t(props.mesh_shader.maxMeshOutputPrimitives));
			if (entry.max_vertices > props.mesh_shader.maxMeshOutputVertices)
				return support_reject(SupportKind::Property, "maxMeshOutputVertices", 3, uint64_t(entry.max_vertices),
									  uint64_t(props.mesh_shader.maxMeshOutputVertices));
		}
		for (unsigned i = 0; i < 3; i++)
		{
			wg_size[i] = entry.wg_size_literal[i];
			if (entry.has_constant_id[i] && info.pSpecializationInfo)
			{
				auto *data = static_cast<const uint8_t *>(info.pSpecializationInfo->pData);
				for (unsigned j = 0; j < info.pSpecializationInfo->mapEntryCount; j++)
				{
					auto &map_entry = info.pSpecializationInfo->pMapEntries[j];
					if (map_entry.constantID == entry.constant_id[i] &&
					    map_entry.size == sizeof(uint32_t) &&
					    map_entry.offset + sizeof(uint32_t) <= info.pSpecializationInfo->dataSize)
					{
						memcpy(&wg_size[i], data + map_entry.offset, sizeof(uint32_t));
						break;
					}
				}
			}
			if (wg_size[i] > limit_workgroup_size[i])
				return support_reject(SupportKind::Property,
									  info.stage == VK_SHADER_STAGE_COMPUTE_BIT	   ? "maxComputeWorkGroupSize"
									  : info.stage == VK_SHADER_STAGE_TASK_BIT_EXT ? "maxTaskWorkGroupSize"
																				   : "maxMeshWorkGroupSize",
									  3, wg_size[i], limit_workgroup_size[i]);
		}
		if (wg_size[0] * wg_size[1] * wg_size[2] > limit_invocations)
			return support_reject(SupportKind::Property,
								  info.stage == VK_SHADER_STAGE_COMPUTE_BIT	   ? "maxComputeWorkGroupInvocations"
								  : info.stage == VK_SHADER_STAGE_TASK_BIT_EXT ? "maxTaskWorkGroupInvocations"
																			   : "maxMeshWorkGroupInvocations",
								  3, uint64_t(wg_size[0]) * wg_size[1] * wg_size[2], limit_invocations);
		if (info.flags & VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT)
		{
			auto *required = find_pnext<VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT>(
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT, info.pNext);
			uint32_t target_subgroup_size =
				required ? required->requiredSubgroupSize : props.subgroup_size_control.maxSubgroupSize;
			if (!target_subgroup_size)
				return support_reject(SupportKind::Property, "maxSubgroupSize", SupportDetail::Available, 0,
									  target_subgroup_size);
			if (wg_size[0] % target_subgroup_size != 0)
			{
				// Can only use full subgroups if we're guaranteed maxSubgroupSize is contained.
				// If we're using required size, we can statically validate it.
				return support_reject(SupportKind::Constraint, "workgroupSizeXMultipleOfSubgroupSize", 3, wg_size[0],
									  target_subgroup_size);
			}
		}
	}
	return true;
}

bool FeatureFilter::Impl::subgroup_size_control_is_supported(const VkPipelineShaderStageCreateInfo &stage) const
{
	constexpr VkPipelineShaderStageCreateFlags supported_flags =
			VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT |
			VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;

	if ((stage.flags & ~supported_flags) != 0)
		return support_reject(SupportKind::Constraint, "stage.flags", 3, uint64_t(stage.flags),
							  uint64_t(supported_flags));

	if ((stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT) != 0 &&
	    features.subgroup_size_control.subgroupSizeControl == VK_FALSE)
	{
		return support_reject(SupportKind::Feature, "subgroupSizeControl", 3, 1,
							  uint64_t(features.subgroup_size_control.subgroupSizeControl));
	}
	bool is_compute_like_stage = stage.stage == VK_SHADER_STAGE_MESH_BIT_EXT ||
			stage.stage == VK_SHADER_STAGE_TASK_BIT_EXT ||
			stage.stage == VK_SHADER_STAGE_COMPUTE_BIT;
	if ((stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT) != 0 &&
	    (!is_compute_like_stage ||
	     features.subgroup_size_control.computeFullSubgroups == VK_FALSE ||
	     features.subgroup_size_control.subgroupSizeControl == VK_FALSE))
	{
		{
			if (!features.subgroup_size_control.computeFullSubgroups)
				return support_reject(SupportKind::Feature, "computeFullSubgroups", 3, 1,
									  uint64_t(features.subgroup_size_control.computeFullSubgroups));
			if (!features.subgroup_size_control.subgroupSizeControl)
				return support_reject(SupportKind::Feature, "subgroupSizeControl", 3, 1,
									  uint64_t(features.subgroup_size_control.subgroupSizeControl));
			return support_reject(SupportKind::Constraint, "shaderStage", SupportDetail::Raw, 0, 0,
								  uint64_t(stage.stage));
		}
	}

	auto *required = find_pnext<VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT>(
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
			stage.pNext);

	if (required)
	{
		if (!features.subgroup_size_control.subgroupSizeControl)
			return support_reject(SupportKind::Feature, "subgroupSizeControl", 3, 1, 0);
		if (!(props.subgroup_size_control.requiredSubgroupSizeStages & stage.stage))
			return support_reject(SupportKind::Property, "requiredSubgroupSizeStages", 3, stage.stage,
								  props.subgroup_size_control.requiredSubgroupSizeStages);
		if (required->requiredSubgroupSize > props.subgroup_size_control.maxSubgroupSize)
			return support_reject(SupportKind::Property, "maxSubgroupSize", 3, required->requiredSubgroupSize,
								  props.subgroup_size_control.maxSubgroupSize);
		if (required->requiredSubgroupSize < props.subgroup_size_control.minSubgroupSize)
			return support_reject(SupportKind::Property, "minSubgroupSize", 3, required->requiredSubgroupSize,
								  props.subgroup_size_control.minSubgroupSize);
	}
	return true;
}

bool FeatureFilter::Impl::render_pass2_is_supported(const VkRenderPassCreateInfo2 *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkRenderPassCreateFlags supported_flags =
			VK_RENDER_PASS_CREATE_TRANSFORM_BIT_QCOM |
			VK_RENDER_PASS_CREATE_PER_LAYER_FRAGMENT_DENSITY_BIT_VALVE;

	if ((info->flags & ~supported_flags) != 0)
		return support_reject(SupportKind::Constraint, "VkRenderPassCreateInfo2.flags", 3, uint64_t(info->flags),
							  uint64_t(supported_flags));

	if ((info->flags & VK_RENDER_PASS_CREATE_TRANSFORM_BIT_QCOM) != 0 &&
	    enabled_extensions.count(VK_QCOM_RENDER_PASS_TRANSFORM_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_QCOM_RENDER_PASS_TRANSFORM_EXTENSION_NAME, 3, 1, 0);

	if ((info->flags & VK_RENDER_PASS_CREATE_PER_LAYER_FRAGMENT_DENSITY_BIT_VALVE) != 0 &&
	    enabled_extensions.count(VK_VALVE_FRAGMENT_DENSITY_MAP_LAYERED_EXTENSION_NAME) == 0)
		return support_reject(SupportKind::Extension, VK_VALVE_FRAGMENT_DENSITY_MAP_LAYERED_EXTENSION_NAME, 3, 1, 0);

	if (null_device)
		return true;

	if (!enabled_extensions.count(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME) && api_version < VK_API_VERSION_1_2)
		return support_reject(SupportKind::Extension, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, 3, 1, 0);

	if (!pnext_chain_is_supported(info->pNext))
		return false;

	for (uint32_t i = 0; i < info->attachmentCount; i++)
	{
		VkFormatFeatureFlags format_features = 0;
		for (uint32_t j = 0; j < info->subpassCount; j++)
		{
			for (uint32_t k = 0; k < info->pSubpasses[j].colorAttachmentCount; k++)
			{
				if (info->pSubpasses[j].pColorAttachments[k].attachment == i)
					format_features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
				if (info->pSubpasses[j].pResolveAttachments &&
				    info->pSubpasses[j].pResolveAttachments[k].attachment == i)
				{
					format_features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
				}
			}
			for (uint32_t k = 0; k < info->pSubpasses[j].inputAttachmentCount; k++)
				if (info->pSubpasses[j].pInputAttachments[k].attachment == i)
					format_features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
			if (info->pSubpasses[j].pDepthStencilAttachment &&
			    info->pSubpasses[j].pDepthStencilAttachment->attachment == i)
			{
				format_features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
			}
			auto *ds_resolve = find_pnext<VkSubpassDescriptionDepthStencilResolve>(
					VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE, info->pSubpasses[j].pNext);
			if (ds_resolve &&
			    ds_resolve->pDepthStencilResolveAttachment &&
			    ds_resolve->pDepthStencilResolveAttachment->attachment == i)
			{
				format_features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
			}
			auto *rate_attachment = find_pnext<VkFragmentShadingRateAttachmentInfoKHR>(
					VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR, info->pSubpasses[j].pNext);
			if (rate_attachment &&
			    rate_attachment->pFragmentShadingRateAttachment &&
			    rate_attachment->pFragmentShadingRateAttachment->attachment == i)
			{
				format_features |= VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
			}
		}
		if (!attachment_description2_is_supported(info->pAttachments[i], format_features))
			return false;
	}
	for (uint32_t i = 0; i < info->subpassCount; i++)
	{
		if (!subpass_description2_is_supported(info->pSubpasses[i]))
			return false;
	}
	for (uint32_t i = 0; i < info->dependencyCount; i++)
	{
		if (!subpass_dependency2_is_supported(info->pDependencies[i]))
			return false;
	}
	for (uint32_t i = 0; i < info->correlatedViewMaskCount; i++)
		if (!multiview_mask_is_supported(info->pCorrelatedViewMasks[i]))
			return false;
	return true;
}

static bool blend_op_is_advanced(VkBlendOp blend_op)
{
	switch (blend_op)
	{
	case VK_BLEND_OP_ZERO_EXT:
	case VK_BLEND_OP_SRC_EXT:
	case VK_BLEND_OP_DST_EXT:
	case VK_BLEND_OP_SRC_OVER_EXT:
	case VK_BLEND_OP_DST_OVER_EXT:
	case VK_BLEND_OP_SRC_IN_EXT:
	case VK_BLEND_OP_DST_IN_EXT:
	case VK_BLEND_OP_SRC_OUT_EXT:
	case VK_BLEND_OP_DST_OUT_EXT:
	case VK_BLEND_OP_SRC_ATOP_EXT:
	case VK_BLEND_OP_DST_ATOP_EXT:
	case VK_BLEND_OP_XOR_EXT:
	case VK_BLEND_OP_MULTIPLY_EXT:
	case VK_BLEND_OP_SCREEN_EXT:
	case VK_BLEND_OP_OVERLAY_EXT:
	case VK_BLEND_OP_DARKEN_EXT:
	case VK_BLEND_OP_LIGHTEN_EXT:
	case VK_BLEND_OP_COLORDODGE_EXT:
	case VK_BLEND_OP_COLORBURN_EXT:
	case VK_BLEND_OP_HARDLIGHT_EXT:
	case VK_BLEND_OP_SOFTLIGHT_EXT:
	case VK_BLEND_OP_DIFFERENCE_EXT:
	case VK_BLEND_OP_EXCLUSION_EXT:
	case VK_BLEND_OP_INVERT_EXT:
	case VK_BLEND_OP_INVERT_RGB_EXT:
	case VK_BLEND_OP_LINEARDODGE_EXT:
	case VK_BLEND_OP_LINEARBURN_EXT:
	case VK_BLEND_OP_VIVIDLIGHT_EXT:
	case VK_BLEND_OP_LINEARLIGHT_EXT:
	case VK_BLEND_OP_PINLIGHT_EXT:
	case VK_BLEND_OP_HARDMIX_EXT:
	case VK_BLEND_OP_HSL_HUE_EXT:
	case VK_BLEND_OP_HSL_SATURATION_EXT:
	case VK_BLEND_OP_HSL_COLOR_EXT:
	case VK_BLEND_OP_HSL_LUMINOSITY_EXT:
	case VK_BLEND_OP_PLUS_EXT:
	case VK_BLEND_OP_PLUS_CLAMPED_EXT:
	case VK_BLEND_OP_PLUS_CLAMPED_ALPHA_EXT:
	case VK_BLEND_OP_PLUS_DARKER_EXT:
	case VK_BLEND_OP_MINUS_EXT:
	case VK_BLEND_OP_MINUS_CLAMPED_EXT:
	case VK_BLEND_OP_CONTRAST_EXT:
	case VK_BLEND_OP_INVERT_OVG_EXT:
	case VK_BLEND_OP_RED_EXT:
	case VK_BLEND_OP_GREEN_EXT:
	case VK_BLEND_OP_BLUE_EXT:
		return true;
	default:
		return false;
	}
}

static bool has_dynamic_state(const VkDynamicState *dynamic_states, uint32_t num_dynamic_states, VkDynamicState dyn_state)
{
	for (uint32_t i = 0; i < num_dynamic_states; i++)
		if (dynamic_states[i] == dyn_state)
			return true;
	return false;
}

bool FeatureFilter::Impl::color_blend_state_is_supported(
		const VkPipelineColorBlendStateCreateInfo *info,
		const VkDynamicState *dynamic_states, uint32_t num_dynamic_states) const
{
	constexpr VkPipelineColorBlendStateCreateFlags supported_flags =
			VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT;

	if (info->flags & ~supported_flags)
		return support_reject(SupportKind::Constraint, "VkPipelineColorBlendStateCreateInfo.flags", 3,
							  uint64_t(info->flags), uint64_t(supported_flags));

	if ((info->flags & VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT) != 0 &&
	    features.rasterization_order_attachment_access.rasterizationOrderColorAttachmentAccess == VK_FALSE)
		return support_reject(
			SupportKind::Feature, "rasterizationOrderColorAttachmentAccess", 3, 1,
			uint64_t(features.rasterization_order_attachment_access.rasterizationOrderColorAttachmentAccess));

	if (!has_dynamic_state(dynamic_states, num_dynamic_states, VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT) &&
	    info->logicOpEnable && !features2.features.logicOp)
	{
		return support_reject(SupportKind::Feature, "logicOp", 3, 1, uint64_t(features2.features.logicOp));
	}
	if (!has_dynamic_state(dynamic_states, num_dynamic_states, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT) &&
	    !has_dynamic_state(dynamic_states, num_dynamic_states, VK_DYNAMIC_STATE_COLOR_BLEND_ADVANCED_EXT))
	{
		bool has_advanced_color_blend = false;
		bool has_advanced_alpha_blend = false;
		for (uint32_t i = 0; i < info->attachmentCount; i++)
		{
			auto &att = info->pAttachments[i];
			bool advanced_color = blend_op_is_advanced(att.colorBlendOp);
			bool advanced_alpha = blend_op_is_advanced(att.alphaBlendOp);
			has_advanced_color_blend = has_advanced_color_blend || advanced_color;
			has_advanced_alpha_blend = has_advanced_alpha_blend || advanced_alpha;
			if (advanced_color || advanced_alpha)
			{
				if (att.alphaBlendOp != att.colorBlendOp)
					return support_reject(SupportKind::Constraint, "advancedBlendAlphaColorMatch", 3, att.colorBlendOp,
										  att.alphaBlendOp);
				if (!props.blend_operation_advanced.advancedBlendAllOperations)
					return support_reject(SupportKind::Property, "advancedBlendAllOperations", 3, uint64_t(1),
										  uint64_t(props.blend_operation_advanced.advancedBlendAllOperations));
			}
		}
		if (has_advanced_color_blend || has_advanced_alpha_blend)
		{
			if (info->attachmentCount > props.blend_operation_advanced.advancedBlendMaxColorAttachments)
				return support_reject(SupportKind::Property, "advancedBlendMaxColorAttachments", 3,
									  uint64_t(info->attachmentCount),
									  uint64_t(props.blend_operation_advanced.advancedBlendMaxColorAttachments));
			if (!props.blend_operation_advanced.advancedBlendIndependentBlend)
			{
				for (uint32_t i = 1; i < info->attachmentCount; i++)
				{
					if (info->pAttachments[i].alphaBlendOp != info->pAttachments[0].alphaBlendOp)
						return support_reject(SupportKind::Property, "advancedBlendIndependentBlend", 3, 1,
											  props.blend_operation_advanced.advancedBlendIndependentBlend);
					if (info->pAttachments[i].colorBlendOp != info->pAttachments[0].colorBlendOp)
						return support_reject(SupportKind::Property, "advancedBlendIndependentBlend", 3, 1,
											  props.blend_operation_advanced.advancedBlendIndependentBlend);
				}
			}
		}
	}
	return true;
}

template <typename T>
static VkPipelineCreateFlagBits2KHR get_effective_flags(const T *info)
{
	auto *flags_info = find_pnext<VkPipelineCreateFlags2CreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, info->pNext);
	if (flags_info)
		return flags_info->flags;
	else
		return info->flags;
}

bool FeatureFilter::Impl::graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkPipelineCreateFlags2KHR supported_flags =
			VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT |
			VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT |
			VK_PIPELINE_CREATE_DERIVATIVE_BIT |
			VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT |
			VK_PIPELINE_CREATE_VIEW_INDEX_FROM_DEVICE_INDEX_BIT |
			VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
			VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR |
			VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
			VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
			VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT |
			VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT |
			VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
			VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
			VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
			VK_PIPELINE_CREATE_INDIRECT_BINDABLE_BIT_NV |
			VK_PIPELINE_CREATE_PROTECTED_ACCESS_ONLY_BIT_EXT |
			VK_PIPELINE_CREATE_NO_PROTECTED_ACCESS_BIT_EXT |
			VK_PIPELINE_CREATE_2_ENABLE_LEGACY_DITHERING_BIT_EXT |
			VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT |
			VK_PIPELINE_CREATE_2_DISALLOW_OPACITY_MICROMAP_BIT_ARM |
			VK_PIPELINE_CREATE_2_RAY_TRACING_ALLOW_SPHERES_AND_LINEAR_SWEPT_SPHERES_BIT_NV |
			VK_PIPELINE_CREATE_2_PER_LAYER_FRAGMENT_DENSITY_BIT_VALVE |
			VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |
			VK_PIPELINE_CREATE_2_64_BIT_INDEXING_BIT_EXT |
			VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT |
			VK_PIPELINE_CREATE_2_OPACITY_MICROMAP_DISALLOW_MIXED_SPECIAL_INDEX_BIT_KHR;

	auto flags = get_effective_flags(info);

	if ((flags & ~supported_flags) != 0)
		return support_reject(SupportKind::Constraint, "flags", 3, uint64_t(flags), uint64_t(supported_flags));

	if (null_device)
		return true;

	if ((flags & VK_PIPELINE_CREATE_VIEW_INDEX_FROM_DEVICE_INDEX_BIT) != 0 &&
	    api_version < VK_API_VERSION_1_1)
	{
		return support_reject(SupportKind::Property, "apiVersion", 3, VK_API_VERSION_1_1, api_version);
	}
	if ((flags & VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR) != 0 &&
	    (enabled_extensions.count(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0 ||
	     enabled_extensions.count(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0))
	{
		{
			if (!enabled_extensions.count(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME))
				return support_reject(SupportKind::Extension, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, 3, 1, 0);
			if (!enabled_extensions.count(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
				return support_reject(SupportKind::Extension, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, 3, 1, 0);
			return false;
		}
	}

	if ((flags & VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT) != 0 &&
	    (enabled_extensions.count(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME) == 0 ||
	     enabled_extensions.count(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0))
	{
		{
			if (!enabled_extensions.count(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME))
				return support_reject(SupportKind::Extension, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME, 3, 1, 0);
			if (!enabled_extensions.count(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
				return support_reject(SupportKind::Extension, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, 3, 1, 0);
			return false;
		}
	}

	if ((flags & (VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
	              VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT |
	              VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT)) != 0 &&
	    features.graphics_pipeline_library.graphicsPipelineLibrary == VK_FALSE)
	{
		return support_reject(SupportKind::Feature, "graphicsPipelineLibrary", 3, 1,
							  uint64_t(features.graphics_pipeline_library.graphicsPipelineLibrary));
	}
	if ((flags & VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) != 0)
		if (!features.descriptor_buffer.descriptorBuffer)
			return support_reject(SupportKind::Feature, "descriptorBuffer", 3, 1,
								  uint64_t(features.descriptor_buffer.descriptorBuffer));
	if ((flags & (VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
	              VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)) != 0)
	{
		if (!features.attachment_feedback_loop_layout.attachmentFeedbackLoopLayout)
			return support_reject(SupportKind::Feature, "attachmentFeedbackLoopLayout", 3, 1,
								  uint64_t(features.attachment_feedback_loop_layout.attachmentFeedbackLoopLayout));
	}
	if ((flags & VK_PIPELINE_CREATE_INDIRECT_BINDABLE_BIT_NV) != 0 &&
	    features.device_generated_commands_nv.deviceGeneratedCommands == VK_FALSE)
		return support_reject(SupportKind::Feature, "deviceGeneratedCommands", 3, 1,
							  uint64_t(features.device_generated_commands_nv.deviceGeneratedCommands));
	if ((flags & (VK_PIPELINE_CREATE_PROTECTED_ACCESS_ONLY_BIT_EXT |
	              VK_PIPELINE_CREATE_NO_PROTECTED_ACCESS_BIT_EXT)) != 0 &&
	    features.pipeline_protected_access.pipelineProtectedAccess == VK_FALSE)
		return support_reject(SupportKind::Feature, "pipelineProtectedAccess", 3, 1,
							  uint64_t(features.pipeline_protected_access.pipelineProtectedAccess));
	if ((flags & VK_PIPELINE_CREATE_2_ENABLE_LEGACY_DITHERING_BIT_EXT) != 0 &&
	    features.legacy_dithering.legacyDithering == VK_FALSE)
		return support_reject(SupportKind::Feature, "legacyDithering", 3, 1,
							  uint64_t(features.legacy_dithering.legacyDithering));
	if ((flags & VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT) != 0 &&
	    features.device_generated_commands.deviceGeneratedCommands == VK_FALSE)
		return support_reject(SupportKind::Feature, "deviceGeneratedCommands", 3, 1,
							  uint64_t(features.device_generated_commands.deviceGeneratedCommands));
	if ((flags & VK_PIPELINE_CREATE_2_DISALLOW_OPACITY_MICROMAP_BIT_ARM) != 0 &&
	    features.pipeline_opacity_micromap_arm.pipelineOpacityMicromap == VK_FALSE)
		return support_reject(SupportKind::Feature, "pipelineOpacityMicromap", 3, 1,
							  uint64_t(features.pipeline_opacity_micromap_arm.pipelineOpacityMicromap));
	if ((flags & VK_PIPELINE_CREATE_2_RAY_TRACING_ALLOW_SPHERES_AND_LINEAR_SWEPT_SPHERES_BIT_NV) != 0 &&
	    features.ray_tracing_linear_swept_spheres_nv.spheres == VK_FALSE &&
	    features.ray_tracing_linear_swept_spheres_nv.linearSweptSpheres == VK_FALSE)
	{
		if (!features.ray_tracing_linear_swept_spheres_nv.spheres)
			return support_reject(SupportKind::Feature, "spheres", 3, 1,
								  uint64_t(features.ray_tracing_linear_swept_spheres_nv.spheres));
		if (!features.ray_tracing_linear_swept_spheres_nv.linearSweptSpheres)
			return support_reject(SupportKind::Feature, "linearSweptSpheres", 3, 1,
								  uint64_t(features.ray_tracing_linear_swept_spheres_nv.linearSweptSpheres));
		return false;
	}

	if ((flags & VK_PIPELINE_CREATE_2_PER_LAYER_FRAGMENT_DENSITY_BIT_VALVE) != 0 &&
	    features.fragment_density_map_layered_valve.fragmentDensityMapLayered == VK_FALSE)
		return support_reject(SupportKind::Feature, "fragmentDensityMapLayered", 3, 1,
							  uint64_t(features.fragment_density_map_layered_valve.fragmentDensityMapLayered));

	if ((flags & VK_PIPELINE_CREATE_2_64_BIT_INDEXING_BIT_EXT) != 0 &&
		features.shader_64_bit_indexing.shader64BitIndexing == VK_FALSE)
		return support_reject(SupportKind::Feature, "shader64BitIndexing", 3, 1,
							  uint64_t(features.shader_64_bit_indexing.shader64BitIndexing));

	if ((flags & VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT) != 0 &&
		features.descriptor_heap.descriptorHeap == VK_FALSE)
		return support_reject(SupportKind::Feature, "descriptorHeap", 3, 1,
							  uint64_t(features.descriptor_heap.descriptorHeap));

	if ((flags & VK_PIPELINE_CREATE_2_OPACITY_MICROMAP_DISALLOW_MIXED_SPECIAL_INDEX_BIT_KHR) != 0 &&
		features.opacity_micromap_khr.micromap == VK_FALSE)
		return support_reject(SupportKind::Feature, "micromap", 3, 1, uint64_t(features.opacity_micromap_khr.micromap));

	const VkDynamicState *dynamic_states = nullptr;
	uint32_t num_dynamic_states = 0;

	if (info->pDynamicState)
	{
		dynamic_states = info->pDynamicState->pDynamicStates;
		num_dynamic_states = info->pDynamicState->dynamicStateCount;
	}
	if (info->pColorBlendState && !pnext_chain_is_supported(info->pColorBlendState->pNext))
		return false;
	if (info->pColorBlendState && !color_blend_state_is_supported(
			info->pColorBlendState, dynamic_states, num_dynamic_states))
		return false;
	if (info->pVertexInputState && !pnext_chain_is_supported(info->pVertexInputState->pNext))
		return false;
	if (info->pDepthStencilState && !pnext_chain_is_supported(info->pDepthStencilState->pNext))
		return false;
	if (info->pInputAssemblyState && !pnext_chain_is_supported(info->pInputAssemblyState->pNext))
		return false;
	if (info->pDynamicState && !pnext_chain_is_supported(info->pDynamicState->pNext))
		return false;
	if (info->pMultisampleState && !pnext_chain_is_supported(info->pMultisampleState->pNext))
		return false;
	if (info->pTessellationState && !pnext_chain_is_supported(info->pTessellationState->pNext))
		return false;
	if (info->pViewportState && !pnext_chain_is_supported(info->pViewportState->pNext))
		return false;
	if (info->pRasterizationState && !pnext_chain_is_supported(info->pRasterizationState->pNext))
		return false;
	if (info->pRasterizationState)
	{
		if (!has_dynamic_state(dynamic_states, num_dynamic_states, VK_DYNAMIC_STATE_POLYGON_MODE_EXT) &&
		    !polygon_mode_is_supported(info->pRasterizationState->polygonMode))
			return false;
		if (!has_dynamic_state(dynamic_states, num_dynamic_states, VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT) &&
		    info->pRasterizationState->depthClampEnable && features2.features.depthClamp == VK_FALSE)
			return support_reject(SupportKind::Feature, "depthClamp", 3, 1, uint64_t(features2.features.depthClamp));
	}
	if (info->pDepthStencilState)
	{
		constexpr VkPipelineDepthStencilStateCreateFlags supported_ds_flags =
				VK_PIPELINE_DEPTH_STENCIL_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_DEPTH_ACCESS_BIT_EXT |
				VK_PIPELINE_DEPTH_STENCIL_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_STENCIL_ACCESS_BIT_EXT;
		if (info->pDepthStencilState->flags & ~supported_ds_flags)
			return support_reject(SupportKind::Constraint, "info.pDepthStencilState.flags", 3,
								  uint64_t(info->pDepthStencilState->flags), uint64_t(supported_ds_flags));
		if ((info->pDepthStencilState->flags &
		     VK_PIPELINE_DEPTH_STENCIL_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_DEPTH_ACCESS_BIT_EXT) != 0 &&
		    features.rasterization_order_attachment_access.rasterizationOrderDepthAttachmentAccess == VK_FALSE)
			return support_reject(
				SupportKind::Feature, "rasterizationOrderDepthAttachmentAccess", 3, 1,
				uint64_t(features.rasterization_order_attachment_access.rasterizationOrderDepthAttachmentAccess));
		if ((info->pDepthStencilState->flags &
		     VK_PIPELINE_DEPTH_STENCIL_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_STENCIL_ACCESS_BIT_EXT) != 0 &&
		    features.rasterization_order_attachment_access.rasterizationOrderStencilAttachmentAccess == VK_FALSE)
			return support_reject(
				SupportKind::Feature, "rasterizationOrderStencilAttachmentAccess", 3, 1,
				uint64_t(features.rasterization_order_attachment_access.rasterizationOrderStencilAttachmentAccess));
	}
	if (info->renderPass == VK_NULL_HANDLE && features.dynamic_rendering.dynamicRendering == VK_FALSE)
		return support_reject(SupportKind::Feature, "dynamicRendering", 3, 1,
							  uint64_t(features.dynamic_rendering.dynamicRendering));
	if (info->pVertexInputState)
	{
		for (uint32_t i = 0; i < info->pVertexInputState->vertexAttributeDescriptionCount; i++)
		{
			if (!format_is_supported(info->pVertexInputState->pVertexAttributeDescriptions[i].format,
			                         VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
				return false;
		}
	}
	if (info->pDynamicState)
	{
		auto &dyn = *info->pDynamicState;
		for (uint32_t i = 0; i < dyn.dynamicStateCount; i++)
		{
			switch (dyn.pDynamicStates[i])
			{
			case VK_DYNAMIC_STATE_CULL_MODE_EXT:
			case VK_DYNAMIC_STATE_FRONT_FACE_EXT:
			case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT:
			case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT:
			case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT_EXT:
			case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT:
			case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT:
			case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT:
			case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT:
			case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT:
			case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT:
			case VK_DYNAMIC_STATE_STENCIL_OP_EXT:
				if (!features.extended_dynamic_state.extendedDynamicState && api_version < VK_API_VERSION_1_3)
					return support_reject(SupportKind::Feature, "extendedDynamicState", 3, 1,
										  uint64_t(features.extended_dynamic_state.extendedDynamicState));
				break;
			case VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT:
				if (!features.extended_dynamic_state2.extendedDynamicState2PatchControlPoints)
					return support_reject(
						SupportKind::Feature, "extendedDynamicState2PatchControlPoints", 3, 1,
						uint64_t(features.extended_dynamic_state2.extendedDynamicState2PatchControlPoints));
				break;
			case VK_DYNAMIC_STATE_LOGIC_OP_EXT:
				if (!features.extended_dynamic_state2.extendedDynamicState2LogicOp)
					return support_reject(SupportKind::Feature, "extendedDynamicState2LogicOp", 3, 1,
										  uint64_t(features.extended_dynamic_state2.extendedDynamicState2LogicOp));
				break;
			case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT:
			case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT:
			case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE_EXT:
				if (!features.extended_dynamic_state2.extendedDynamicState2 && api_version < VK_API_VERSION_1_3)
					return support_reject(SupportKind::Feature, "extendedDynamicState2", 3, 1,
										  uint64_t(features.extended_dynamic_state2.extendedDynamicState2));
				break;
			case VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT:
				if (!features.color_write_enable.colorWriteEnable)
					return support_reject(SupportKind::Feature, "colorWriteEnable", 3, 1,
										  uint64_t(features.color_write_enable.colorWriteEnable));
				break;
			case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT:
				if (!features.vertex_input_dynamic_state.vertexInputDynamicState)
					return support_reject(SupportKind::Feature, "vertexInputDynamicState", 3, 1,
										  uint64_t(features.vertex_input_dynamic_state.vertexInputDynamicState));
				break;
			case VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR:
				// Only support dynamic fragment shading rate for now.
				// pNext variant needs to validate against vkGetPhysicalDeviceFragmentShadingRatesKHR on top.
				if (features.fragment_shading_rate.pipelineFragmentShadingRate == VK_FALSE)
					return support_reject(SupportKind::Feature, "pipelineFragmentShadingRate", 3, 1,
										  uint64_t(features.fragment_shading_rate.pipelineFragmentShadingRate));
				break;
			case VK_DYNAMIC_STATE_VIEWPORT_W_SCALING_NV:
				if (!enabled_extensions.count(VK_NV_CLIP_SPACE_W_SCALING_EXTENSION_NAME))
					return support_reject(SupportKind::Extension, VK_NV_CLIP_SPACE_W_SCALING_EXTENSION_NAME, 3, 1, 0);
				break;
			case VK_DYNAMIC_STATE_DISCARD_RECTANGLE_EXT:
				// Technically we have to check v2, but whatever.
			case VK_DYNAMIC_STATE_DISCARD_RECTANGLE_MODE_EXT:
			case VK_DYNAMIC_STATE_DISCARD_RECTANGLE_ENABLE_EXT:
				if (!enabled_extensions.count(VK_EXT_DISCARD_RECTANGLES_EXTENSION_NAME))
					return support_reject(SupportKind::Extension, VK_EXT_DISCARD_RECTANGLES_EXTENSION_NAME, 3, 1, 0);
				break;
			case VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT:
				if (!enabled_extensions.count(VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME))
					return support_reject(SupportKind::Extension, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME, 3, 1, 0);
				break;
			case VK_DYNAMIC_STATE_VIEWPORT_SHADING_RATE_PALETTE_NV:
				if (!features.shading_rate_nv.shadingRateImage)
					return support_reject(SupportKind::Feature, "shadingRateImage", 3, 1,
										  uint64_t(features.shading_rate_nv.shadingRateImage));
				break;
			case VK_DYNAMIC_STATE_VIEWPORT_COARSE_SAMPLE_ORDER_NV:
				if (!features.shading_rate_nv.shadingRateCoarseSampleOrder)
					return support_reject(SupportKind::Feature, "shadingRateCoarseSampleOrder", 3, 1,
										  uint64_t(features.shading_rate_nv.shadingRateCoarseSampleOrder));
				break;
			case VK_DYNAMIC_STATE_EXCLUSIVE_SCISSOR_NV:
				// Technically this is part of v2, but whatever.
			case VK_DYNAMIC_STATE_EXCLUSIVE_SCISSOR_ENABLE_NV:
				if (!enabled_extensions.count(VK_NV_SCISSOR_EXCLUSIVE_EXTENSION_NAME))
					return support_reject(SupportKind::Extension, VK_NV_SCISSOR_EXCLUSIVE_EXTENSION_NAME, 3, 1, 0);
				break;
			case VK_DYNAMIC_STATE_LINE_STIPPLE_EXT:
				if (!enabled_extensions.count(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME))
					return support_reject(SupportKind::Extension, VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME, 3, 1, 0);
				break;
#define DYN_STATE3(state, member)                                                                                      \
	case VK_DYNAMIC_STATE_##state:                                                                                     \
		if (!enabled_extensions.count(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME))                                 \
			return support_reject(SupportKind::Extension, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, 3, 1, 0);    \
		if (features.extended_dynamic_state3.extendedDynamicState3##member == VK_FALSE)                                \
			return support_reject(SupportKind::Feature, "extendedDynamicState3" #member, 3, 1,                         \
								  features.extended_dynamic_state3.extendedDynamicState3##member);                     \
	break
			DYN_STATE3(TESSELLATION_DOMAIN_ORIGIN_EXT, TessellationDomainOrigin);
			DYN_STATE3(DEPTH_CLAMP_ENABLE_EXT, DepthClampEnable);
			DYN_STATE3(POLYGON_MODE_EXT, PolygonMode);
			DYN_STATE3(RASTERIZATION_SAMPLES_EXT, RasterizationSamples);
			DYN_STATE3(SAMPLE_MASK_EXT, SampleMask);
			DYN_STATE3(ALPHA_TO_COVERAGE_ENABLE_EXT, AlphaToCoverageEnable);
			DYN_STATE3(ALPHA_TO_ONE_ENABLE_EXT, AlphaToOneEnable);
			DYN_STATE3(LOGIC_OP_ENABLE_EXT, LogicOpEnable);
			DYN_STATE3(COLOR_BLEND_ENABLE_EXT, ColorBlendEnable);
			DYN_STATE3(COLOR_BLEND_EQUATION_EXT, ColorBlendEquation);
			DYN_STATE3(COLOR_WRITE_MASK_EXT, ColorWriteMask);
			DYN_STATE3(RASTERIZATION_STREAM_EXT, RasterizationStream);
			DYN_STATE3(CONSERVATIVE_RASTERIZATION_MODE_EXT, ConservativeRasterizationMode);
			DYN_STATE3(EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT, ExtraPrimitiveOverestimationSize);
			DYN_STATE3(DEPTH_CLIP_ENABLE_EXT, DepthClipEnable);
			DYN_STATE3(SAMPLE_LOCATIONS_ENABLE_EXT, SampleLocationsEnable);
			DYN_STATE3(COLOR_BLEND_ADVANCED_EXT, ColorBlendAdvanced);
			DYN_STATE3(PROVOKING_VERTEX_MODE_EXT, ProvokingVertexMode);
			DYN_STATE3(LINE_RASTERIZATION_MODE_EXT, LineRasterizationMode);
			DYN_STATE3(LINE_STIPPLE_ENABLE_EXT, LineStippleEnable);
			DYN_STATE3(DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT, DepthClipNegativeOneToOne);
			DYN_STATE3(VIEWPORT_W_SCALING_ENABLE_NV, ViewportWScalingEnable);
			DYN_STATE3(VIEWPORT_SWIZZLE_NV, ViewportSwizzle);
			DYN_STATE3(COVERAGE_TO_COLOR_ENABLE_NV, CoverageToColorEnable);
			DYN_STATE3(COVERAGE_TO_COLOR_LOCATION_NV, CoverageToColorLocation);
			DYN_STATE3(COVERAGE_MODULATION_MODE_NV, CoverageModulationMode);
			DYN_STATE3(COVERAGE_MODULATION_TABLE_ENABLE_NV, CoverageModulationTableEnable);
			DYN_STATE3(COVERAGE_MODULATION_TABLE_NV, CoverageModulationTable);
			DYN_STATE3(SHADING_RATE_IMAGE_ENABLE_NV, ShadingRateImageEnable);
			DYN_STATE3(REPRESENTATIVE_FRAGMENT_TEST_ENABLE_NV, RepresentativeFragmentTestEnable);
			DYN_STATE3(COVERAGE_REDUCTION_MODE_NV, CoverageReductionMode);
#undef DYN_STATE3
			case VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT:
				if (features.attachment_feedback_loop_dynamic_state.attachmentFeedbackLoopDynamicState == VK_FALSE)
					return support_reject(
						SupportKind::Feature, "attachmentFeedbackLoopDynamicState", 3, 1,
						uint64_t(features.attachment_feedback_loop_dynamic_state.attachmentFeedbackLoopDynamicState));
				break;
			case VK_DYNAMIC_STATE_DEPTH_CLAMP_RANGE_EXT:
				if (features.depth_clamp_control.depthClampControl == VK_FALSE)
					return support_reject(SupportKind::Feature, "depthClampControl", 3, 1,
										  uint64_t(features.depth_clamp_control.depthClampControl));
				break;
			case VK_DYNAMIC_STATE_VIEWPORT:
			case VK_DYNAMIC_STATE_SCISSOR:
			case VK_DYNAMIC_STATE_LINE_WIDTH:
			case VK_DYNAMIC_STATE_DEPTH_BIAS:
			case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
			case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
			case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
			case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
			case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
				// Part of core.
				break;
			default:
				// Unrecognized dynamic state, we almost certainly have not enabled the feature.
				return false;
			}
		}
	}
	for (uint32_t i = 0; i < info->stageCount; i++)
	{
		switch (info->pStages[i].stage)
		{
		case VK_SHADER_STAGE_VERTEX_BIT:
		case VK_SHADER_STAGE_FRAGMENT_BIT:
			break;
		case VK_SHADER_STAGE_COMPUTE_BIT:
			return false;
		case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
		case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
			if (!features2.features.tessellationShader)
				return support_reject(SupportKind::Feature, "tessellationShader", 3, 1,
									  uint64_t(features2.features.tessellationShader));
			break;
		case VK_SHADER_STAGE_GEOMETRY_BIT:
			if (!features2.features.geometryShader)
				return support_reject(SupportKind::Feature, "geometryShader", 3, 1,
									  uint64_t(features2.features.geometryShader));
			break;
		case VK_SHADER_STAGE_MESH_BIT_EXT:
			if (!features.mesh_shader.meshShader && !features.mesh_shader_nv.meshShader)
			{
				if (!features.mesh_shader.meshShader)
					return support_reject(SupportKind::Feature, "meshShader", 3, 1,
										  uint64_t(features.mesh_shader.meshShader));
				if (!features.mesh_shader_nv.meshShader)
					return support_reject(SupportKind::Feature, "meshShader", 3, 1,
										  uint64_t(features.mesh_shader_nv.meshShader));
				return false;
			}
			if (!stage_limits_are_supported(info->pStages[i]))
				return false;
			break;

		case VK_SHADER_STAGE_TASK_BIT_EXT:
			if (!features.mesh_shader.taskShader && !features.mesh_shader_nv.taskShader)
			{
				if (!features.mesh_shader.taskShader)
					return support_reject(SupportKind::Feature, "taskShader", 3, 1,
										  uint64_t(features.mesh_shader.taskShader));
				if (!features.mesh_shader_nv.taskShader)
					return support_reject(SupportKind::Feature, "taskShader", 3, 1,
										  uint64_t(features.mesh_shader_nv.taskShader));
				return false;
			}
			if (!stage_limits_are_supported(info->pStages[i]))
				return false;
			break;

		default:
			return support_reject(SupportKind::Constraint, "graphicsShaderStage", SupportDetail::Raw, 0, 0,
								  info->pStages[i].stage);
		}

		if (!subgroup_size_control_is_supported(info->pStages[i]))
			return false;
		if (!pnext_chain_is_supported(info->pStages[i].pNext))
			return false;
	}

	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkPipelineCreateFlags2KHR supported_flags =
			VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT |
			VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT |
			VK_PIPELINE_CREATE_DERIVATIVE_BIT |
			VK_PIPELINE_CREATE_DISPATCH_BASE_BIT |
			VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR |
			VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
			VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
			VK_PIPELINE_CREATE_INDIRECT_BINDABLE_BIT_NV |
			VK_PIPELINE_CREATE_PROTECTED_ACCESS_ONLY_BIT_EXT |
			VK_PIPELINE_CREATE_NO_PROTECTED_ACCESS_BIT_EXT |
			VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT |
			VK_PIPELINE_CREATE_2_DISALLOW_OPACITY_MICROMAP_BIT_ARM |
			VK_PIPELINE_CREATE_2_RAY_TRACING_ALLOW_SPHERES_AND_LINEAR_SWEPT_SPHERES_BIT_NV |
			VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |
			VK_PIPELINE_CREATE_2_64_BIT_INDEXING_BIT_EXT |
			VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT |
			VK_PIPELINE_CREATE_2_OPACITY_MICROMAP_DISALLOW_MIXED_SPECIAL_INDEX_BIT_KHR;

	auto flags = get_effective_flags(info);

	if ((flags & ~supported_flags) != 0)
		return support_reject(SupportKind::Constraint, "flags", 3, uint64_t(flags), uint64_t(supported_flags));

	if (null_device)
		return true;

	if ((flags & VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) != 0)
		if (!features.descriptor_buffer.descriptorBuffer)
			return support_reject(SupportKind::Feature, "descriptorBuffer", 3, 1,
								  uint64_t(features.descriptor_buffer.descriptorBuffer));

	if ((flags & VK_PIPELINE_CREATE_INDIRECT_BINDABLE_BIT_NV) != 0 &&
	    features.device_generated_commands_compute_nv.deviceGeneratedCompute == VK_FALSE)
		return support_reject(SupportKind::Feature, "deviceGeneratedCompute", 3, 1,
							  uint64_t(features.device_generated_commands_compute_nv.deviceGeneratedCompute));

	if ((flags & (VK_PIPELINE_CREATE_PROTECTED_ACCESS_ONLY_BIT_EXT |
	              VK_PIPELINE_CREATE_NO_PROTECTED_ACCESS_BIT_EXT)) != 0 &&
	    features.pipeline_protected_access.pipelineProtectedAccess == VK_FALSE)
		return support_reject(SupportKind::Feature, "pipelineProtectedAccess", 3, 1,
							  uint64_t(features.pipeline_protected_access.pipelineProtectedAccess));

	if ((flags & VK_PIPELINE_CREATE_DISPATCH_BASE_BIT) != 0 &&
	    api_version < VK_API_VERSION_1_1)
	{
		return support_reject(SupportKind::Property, "apiVersion", 3, VK_API_VERSION_1_1, api_version);
	}
	if ((flags & VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT) != 0 &&
	    features.device_generated_commands.deviceGeneratedCommands == VK_FALSE)
		return support_reject(SupportKind::Feature, "deviceGeneratedCommands", 3, 1,
							  uint64_t(features.device_generated_commands.deviceGeneratedCommands));
	if ((flags & VK_PIPELINE_CREATE_2_DISALLOW_OPACITY_MICROMAP_BIT_ARM) != 0 &&
	    features.pipeline_opacity_micromap_arm.pipelineOpacityMicromap == VK_FALSE)
		return support_reject(SupportKind::Feature, "pipelineOpacityMicromap", 3, 1,
							  uint64_t(features.pipeline_opacity_micromap_arm.pipelineOpacityMicromap));
	if ((flags & VK_PIPELINE_CREATE_2_RAY_TRACING_ALLOW_SPHERES_AND_LINEAR_SWEPT_SPHERES_BIT_NV) != 0 &&
	    features.ray_tracing_linear_swept_spheres_nv.spheres == VK_FALSE &&
	    features.ray_tracing_linear_swept_spheres_nv.linearSweptSpheres == VK_FALSE)
	{
		if (!features.ray_tracing_linear_swept_spheres_nv.spheres)
			return support_reject(SupportKind::Feature, "spheres", 3, 1,
								  uint64_t(features.ray_tracing_linear_swept_spheres_nv.spheres));
		if (!features.ray_tracing_linear_swept_spheres_nv.linearSweptSpheres)
			return support_reject(SupportKind::Feature, "linearSweptSpheres", 3, 1,
								  uint64_t(features.ray_tracing_linear_swept_spheres_nv.linearSweptSpheres));
		return false;
	}

	if ((flags & VK_PIPELINE_CREATE_2_64_BIT_INDEXING_BIT_EXT) != 0 &&
	    features.shader_64_bit_indexing.shader64BitIndexing == VK_FALSE)
		return support_reject(SupportKind::Feature, "shader64BitIndexing", 3, 1,
							  uint64_t(features.shader_64_bit_indexing.shader64BitIndexing));

	if ((flags & VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT) != 0 &&
		features.descriptor_heap.descriptorHeap == VK_FALSE)
		return support_reject(SupportKind::Feature, "descriptorHeap", 3, 1,
							  uint64_t(features.descriptor_heap.descriptorHeap));

	if ((flags & VK_PIPELINE_CREATE_2_OPACITY_MICROMAP_DISALLOW_MIXED_SPECIAL_INDEX_BIT_KHR) != 0 &&
	    features.opacity_micromap_khr.micromap == VK_FALSE)
		return support_reject(SupportKind::Feature, "micromap", 3, 1, uint64_t(features.opacity_micromap_khr.micromap));

	if (!subgroup_size_control_is_supported(info->stage))
		return false;

	if (!stage_limits_are_supported(info->stage))
		return false;

	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::raytracing_pipeline_is_supported(const VkRayTracingPipelineCreateInfoKHR *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkPipelineCreateFlags2KHR supported_flags =
			VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT |
			VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT |
			VK_PIPELINE_CREATE_DERIVATIVE_BIT |
			VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_ANY_HIT_SHADERS_BIT_KHR |
			VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_CLOSEST_HIT_SHADERS_BIT_KHR |
			VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_INTERSECTION_SHADERS_BIT_KHR |
			VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_MISS_SHADERS_BIT_KHR |
			VK_PIPELINE_CREATE_RAY_TRACING_SKIP_AABBS_BIT_KHR |
			VK_PIPELINE_CREATE_RAY_TRACING_SKIP_TRIANGLES_BIT_KHR |
			VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
			VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR |
			VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
			VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
			VK_PIPELINE_CREATE_RAY_TRACING_ALLOW_MOTION_BIT_NV |
			VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT |
			VK_PIPELINE_CREATE_PROTECTED_ACCESS_ONLY_BIT_EXT |
			VK_PIPELINE_CREATE_NO_PROTECTED_ACCESS_BIT_EXT |
			VK_PIPELINE_CREATE_2_DISALLOW_OPACITY_MICROMAP_BIT_ARM |
			VK_PIPELINE_CREATE_2_RAY_TRACING_ALLOW_SPHERES_AND_LINEAR_SWEPT_SPHERES_BIT_NV |
			VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |
			VK_PIPELINE_CREATE_2_64_BIT_INDEXING_BIT_EXT |
			VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT |
			VK_PIPELINE_CREATE_2_OPACITY_MICROMAP_DISALLOW_MIXED_SPECIAL_INDEX_BIT_KHR;

	auto flags = get_effective_flags(info);

	if ((flags & ~supported_flags) != 0)
		return support_reject(SupportKind::Constraint, "flags", 3, uint64_t(flags), uint64_t(supported_flags));

	if (null_device)
		return true;

	if ((flags & (VK_PIPELINE_CREATE_RAY_TRACING_SKIP_AABBS_BIT_KHR |
	              VK_PIPELINE_CREATE_RAY_TRACING_SKIP_TRIANGLES_BIT_KHR)) != 0 &&
	    features.ray_tracing_pipeline.rayTraversalPrimitiveCulling == VK_FALSE)
	{
		return support_reject(SupportKind::Feature, "rayTraversalPrimitiveCulling", 3, 1,
							  uint64_t(features.ray_tracing_pipeline.rayTraversalPrimitiveCulling));
	}
	if ((flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0 &&
	    !enabled_extensions.count(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME))
	{
		return support_reject(SupportKind::Extension, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, 3, 1, 0);
	}
	if ((flags & VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) != 0)
		if (!features.descriptor_buffer.descriptorBuffer)
			return support_reject(SupportKind::Feature, "descriptorBuffer", 3, 1,
								  uint64_t(features.descriptor_buffer.descriptorBuffer));
	if ((flags & VK_PIPELINE_CREATE_RAY_TRACING_ALLOW_MOTION_BIT_NV) != 0 &&
	    features.ray_tracing_motion_blur_nv.rayTracingMotionBlur == VK_FALSE)
		return support_reject(SupportKind::Feature, "rayTracingMotionBlur", 3, 1,
							  uint64_t(features.ray_tracing_motion_blur_nv.rayTracingMotionBlur));
	if ((flags & VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_KHR) != 0 &&
	    features.opacity_micromap_ext.micromap == VK_FALSE && features.opacity_micromap_khr.micromap == VK_FALSE)
	{
		if (!features.opacity_micromap_ext.micromap)
			return support_reject(SupportKind::Feature, "micromap", 3, 1,
								  uint64_t(features.opacity_micromap_ext.micromap));
		if (!features.opacity_micromap_khr.micromap)
			return support_reject(SupportKind::Feature, "micromap", 3, 1,
								  uint64_t(features.opacity_micromap_khr.micromap));
		return false;
	}

	if ((flags & VK_PIPELINE_CREATE_2_OPACITY_MICROMAP_DISALLOW_MIXED_SPECIAL_INDEX_BIT_KHR) != 0 &&
		features.opacity_micromap_khr.micromap == VK_FALSE)
		return support_reject(SupportKind::Feature, "micromap", 3, 1, uint64_t(features.opacity_micromap_khr.micromap));

	if ((flags & (VK_PIPELINE_CREATE_PROTECTED_ACCESS_ONLY_BIT_EXT |
	              VK_PIPELINE_CREATE_NO_PROTECTED_ACCESS_BIT_EXT)) != 0 &&
	    features.pipeline_protected_access.pipelineProtectedAccess == VK_FALSE)
		return support_reject(SupportKind::Feature, "pipelineProtectedAccess", 3, 1,
							  uint64_t(features.pipeline_protected_access.pipelineProtectedAccess));

	if ((flags & VK_PIPELINE_CREATE_2_DISALLOW_OPACITY_MICROMAP_BIT_ARM) != 0 &&
	    features.pipeline_opacity_micromap_arm.pipelineOpacityMicromap == VK_FALSE)
		return support_reject(SupportKind::Feature, "pipelineOpacityMicromap", 3, 1,
							  uint64_t(features.pipeline_opacity_micromap_arm.pipelineOpacityMicromap));

	if ((flags & VK_PIPELINE_CREATE_2_RAY_TRACING_ALLOW_SPHERES_AND_LINEAR_SWEPT_SPHERES_BIT_NV) != 0 &&
	    features.ray_tracing_linear_swept_spheres_nv.spheres == VK_FALSE &&
	    features.ray_tracing_linear_swept_spheres_nv.linearSweptSpheres == VK_FALSE)
	{
		if (!features.ray_tracing_linear_swept_spheres_nv.spheres)
			return support_reject(SupportKind::Feature, "spheres", 3, 1,
								  uint64_t(features.ray_tracing_linear_swept_spheres_nv.spheres));
		if (!features.ray_tracing_linear_swept_spheres_nv.linearSweptSpheres)
			return support_reject(SupportKind::Feature, "linearSweptSpheres", 3, 1,
								  uint64_t(features.ray_tracing_linear_swept_spheres_nv.linearSweptSpheres));
		return false;
	}

	if ((flags & VK_PIPELINE_CREATE_2_64_BIT_INDEXING_BIT_EXT) != 0 &&
		features.shader_64_bit_indexing.shader64BitIndexing == VK_FALSE)
		return support_reject(SupportKind::Feature, "shader64BitIndexing", 3, 1,
							  uint64_t(features.shader_64_bit_indexing.shader64BitIndexing));

	if ((flags & VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT) != 0 &&
		features.descriptor_heap.descriptorHeap == VK_FALSE)
		return support_reject(SupportKind::Feature, "descriptorHeap", 3, 1,
							  uint64_t(features.descriptor_heap.descriptorHeap));

	if (features.ray_tracing_pipeline.rayTracingPipeline == VK_FALSE)
		return support_reject(SupportKind::Feature, "rayTracingPipeline", 3, 1,
							  uint64_t(features.ray_tracing_pipeline.rayTracingPipeline));

	if (info->maxPipelineRayRecursionDepth > props.ray_tracing_pipeline.maxRayRecursionDepth)
		return support_reject(SupportKind::Property, "maxRayRecursionDepth", 3,
							  uint64_t(info->maxPipelineRayRecursionDepth),
							  uint64_t(props.ray_tracing_pipeline.maxRayRecursionDepth));

	if (info->pDynamicState)
	{
		for (uint32_t i = 0; i < info->pDynamicState->dynamicStateCount; i++)
			if (info->pDynamicState->pDynamicStates[i] != VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR)
				return support_reject(SupportKind::Constraint, "VkDynamicState", SupportDetail::Raw, 0, 0,
									  info->pDynamicState->pDynamicStates[i]);
		if (!pnext_chain_is_supported(info->pDynamicState->pNext))
			return false;
	}
	if ((info->pLibraryInfo || info->pLibraryInterface) &&
	    !enabled_extensions.count(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME))
		return support_reject(SupportKind::Extension, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, 3, 1, 0);
	for (uint32_t i = 0; i < info->stageCount; i++)
	{
		if (!subgroup_size_control_is_supported(info->pStages[i]))
			return false;
		if (!pnext_chain_is_supported(info->pStages[i].pNext))
			return false;
	}
	if (info->pLibraryInterface)
	{
		if (info->pLibraryInterface->maxPipelineRayHitAttributeSize > props.ray_tracing_pipeline.maxRayHitAttributeSize)
			return support_reject(SupportKind::Property, "maxRayHitAttributeSize", 3,
								  uint64_t(info->pLibraryInterface->maxPipelineRayHitAttributeSize),
								  uint64_t(props.ray_tracing_pipeline.maxRayHitAttributeSize));
		if (!pnext_chain_is_supported(info->pLibraryInterface->pNext))
			return false;
	}
	if (info->pLibraryInfo)
	{
		if (!pnext_chain_is_supported(info->pLibraryInfo->pNext))
			return false;
	}
	for (uint32_t i = 0; i < info->groupCount; i++)
	{
		if (!pnext_chain_is_supported(info->pGroups[i].pNext))
			return false;
	}
	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::sampler_is_supported(const VkSamplerCreateInfo *info) const
{
	SupportScope scope(nullptr);
	return impl->sampler_is_supported(info);
}
bool FeatureFilter::sampler_is_supported(const VkSamplerCreateInfo *info, SupportDetail &detail) const
{
	SupportScope scope(&detail);
	bool supported = impl->sampler_is_supported(info);
	if (!supported && detail.kind == SupportKind::None)
		support_reject(SupportKind::Constraint, "sampler_is_supported");
	return supported;
}

bool FeatureFilter::descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info) const
{
	SupportScope scope(nullptr);
	return impl->descriptor_set_layout_is_supported(info);
}
bool FeatureFilter::descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info,
													   SupportDetail &detail) const
{
	SupportScope scope(&detail);
	bool supported = impl->descriptor_set_layout_is_supported(info);
	if (!supported && detail.kind == SupportKind::None)
		support_reject(SupportKind::Constraint, "descriptor_set_layout_is_supported");
	return supported;
}

bool FeatureFilter::pipeline_layout_is_supported(const VkPipelineLayoutCreateInfo *info) const
{
	SupportScope scope(nullptr);
	return impl->pipeline_layout_is_supported(info);
}
bool FeatureFilter::pipeline_layout_is_supported(const VkPipelineLayoutCreateInfo *info, SupportDetail &detail) const
{
	SupportScope scope(&detail);
	bool supported = impl->pipeline_layout_is_supported(info);
	if (!supported && detail.kind == SupportKind::None)
		support_reject(SupportKind::Constraint, "pipeline_layout_is_supported");
	return supported;
}

bool FeatureFilter::shader_module_is_supported(const VkShaderModuleCreateInfo *info) const
{
	SupportScope scope(nullptr);
	return impl->shader_module_is_supported(info);
}
bool FeatureFilter::shader_module_is_supported(const VkShaderModuleCreateInfo *info, SupportDetail &detail) const
{
	SupportScope scope(&detail);
	bool supported = impl->shader_module_is_supported(info);
	if (!supported && detail.kind == SupportKind::None)
		support_reject(SupportKind::Constraint, "shader_module_is_supported");
	return supported;
}

bool FeatureFilter::register_shader_module_info(VkShaderModule module, const VkShaderModuleCreateInfo *info)
{
	return impl->register_shader_module_info(module, info);
}

void FeatureFilter::unregister_shader_module_info(VkShaderModule module)
{
	impl->unregister_shader_module_info(module);
}

bool FeatureFilter::render_pass_is_supported(const VkRenderPassCreateInfo *info) const
{
	SupportScope scope(nullptr);
	return impl->render_pass_is_supported(info);
}
bool FeatureFilter::render_pass_is_supported(const VkRenderPassCreateInfo *info, SupportDetail &detail) const
{
	SupportScope scope(&detail);
	bool supported = impl->render_pass_is_supported(info);
	if (!supported && detail.kind == SupportKind::None)
		support_reject(SupportKind::Constraint, "render_pass_is_supported");
	return supported;
}

bool FeatureFilter::render_pass2_is_supported(const VkRenderPassCreateInfo2 *info) const
{
	SupportScope scope(nullptr);
	return impl->render_pass2_is_supported(info);
}
bool FeatureFilter::render_pass2_is_supported(const VkRenderPassCreateInfo2 *info, SupportDetail &detail) const
{
	SupportScope scope(&detail);
	bool supported = impl->render_pass2_is_supported(info);
	if (!supported && detail.kind == SupportKind::None)
		support_reject(SupportKind::Constraint, "render_pass2_is_supported");
	return supported;
}

bool FeatureFilter::graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info) const
{
	SupportScope scope(nullptr);
	return impl->graphics_pipeline_is_supported(info);
}
bool FeatureFilter::graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info,
												   SupportDetail &detail) const
{
	SupportScope scope(&detail);
	bool supported = impl->graphics_pipeline_is_supported(info);
	if (!supported && detail.kind == SupportKind::None)
		support_reject(SupportKind::Constraint, "graphics_pipeline_is_supported");
	return supported;
}

bool FeatureFilter::compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info) const
{
	SupportScope scope(nullptr);
	return impl->compute_pipeline_is_supported(info);
}
bool FeatureFilter::compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info, SupportDetail &detail) const
{
	SupportScope scope(&detail);
	bool supported = impl->compute_pipeline_is_supported(info);
	if (!supported && detail.kind == SupportKind::None)
		support_reject(SupportKind::Constraint, "compute_pipeline_is_supported");
	return supported;
}

bool FeatureFilter::raytracing_pipeline_is_supported(const VkRayTracingPipelineCreateInfoKHR *info) const
{
	SupportScope scope(nullptr);
	return impl->raytracing_pipeline_is_supported(info);
}
bool FeatureFilter::raytracing_pipeline_is_supported(const VkRayTracingPipelineCreateInfoKHR *info,
													 SupportDetail &detail) const
{
	SupportScope scope(&detail);
	bool supported = impl->raytracing_pipeline_is_supported(info);
	if (!supported && detail.kind == SupportKind::None)
		support_reject(SupportKind::Constraint, "raytracing_pipeline_is_supported");
	return supported;
}

bool FeatureFilter::supports_scalar_block_layout() const
{
	return impl->null_device || impl->features.scalar_block_layout.scalarBlockLayout;
}

bool FeatureFilter::supports_maintenance4() const
{
	return impl->null_device || impl->features.maintenance4.maintenance4 ||
	       impl->api_version >= VK_MAKE_VERSION(1, 3, 0);
}

bool FeatureFilter::supports_maintenance9() const
{
	return impl->null_device || impl->features.maintenance9.maintenance9;
}

void FeatureFilter::set_device_query_interface(DeviceQueryInterface *iface)
{
	impl->query = iface;
}
}
