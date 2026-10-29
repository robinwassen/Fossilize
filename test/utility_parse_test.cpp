// GPU-free fixtures and readback assertions for real utility subprocesses.
// Checks are explicit so Release/NDEBUG exercises the same contract as Debug.
#define VK_NO_PROTOTYPES
#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
using namespace Fossilize;
namespace
{
int failures = 0;
bool check(bool ok, const char *what)
{
	if (!ok) { fprintf(stderr, "FAIL %s\n", what); failures++; }
	return ok;
}
using DB = std::unique_ptr<DatabaseInterface>;
DB open(const std::string &path, DatabaseMode mode)
{
	DB db(create_database(path.c_str(), mode));
	if (!check(db && db->prepare(), "database prepare")) return {};
	return db;
}
std::vector<Hash> hashes(DatabaseInterface &db, ResourceTag tag)
{
	size_t n = 0;
	if (!check(db.get_hash_list_for_resource_tag(tag, &n, nullptr), "hash count")) return {};
	std::vector<Hash> result(n);
	check(db.get_hash_list_for_resource_tag(tag, &n, result.data()), "hash list");
	return result;
}
std::vector<uint8_t> read(DatabaseInterface &db, ResourceTag tag, Hash hash)
{
	size_t n = 0;
	if (!check(db.read_entry(tag, hash, &n, nullptr, 0), "read size")) return {};
	std::vector<uint8_t> result(n);
	check(db.read_entry(tag, hash, &n, result.data(), 0), "read blob");
	return result;
}
void copy(DatabaseInterface &from, DatabaseInterface &to, ResourceTag tag, Hash hash, Hash out_hash = 0)
{
	auto blob = read(from, tag, hash);
	check(to.write_entry(tag, out_hash ? out_hash : hash, blob.data(), blob.size(), 0), "copy record");
}
void reject(DatabaseInterface &db, ResourceTag tag, Hash hash, const std::string &variant)
{
	const char *json = variant == "version" ? "{\"version\":999}" : "{";
	check(db.write_entry(tag, hash, json, strlen(json), 0), "write rejected JSON in valid outer archive");
}
static const uint32_t code[] = {0x07230203, 0x10000, 0, 1, 0};
// Also build an independent expected rehash database, with normal calculated hashes.
void seed(const std::string &path, bool custom)
{
	auto db = open(path, DatabaseMode::OverWrite);
	if (!db) return;
	StateRecorder recorder;
	recorder.set_database_enable_application_feature_links(false);
	recorder.init_recording_synchronized(db.get());
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_0; app.pApplicationName = "utility-parse-test"; app.applicationVersion = 17;
	check(recorder.record_application_info(app), "record app");
	VkPhysicalDeviceFeatures features = {};
	check(recorder.record_physical_device_features(features), "record features");
	VkShaderModuleCreateInfo module = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	module.codeSize = sizeof(code); module.pCode = code;
	check(recorder.record_shader_module((VkShaderModule)1, module, custom ? 0x101 : 0), "record module");
	VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	check(recorder.record_pipeline_layout((VkPipelineLayout)2, layout, custom ? 0x102 : 0), "record layout");
	VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sampler.magFilter = VK_FILTER_LINEAR; sampler.maxLod = 4.0f;
	check(recorder.record_sampler((VkSampler)3, sampler, custom ? 0x103 : 0), "record sampler");
	VkComputePipelineCreateInfo compute = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; compute.stage.module = (VkShaderModule)1;
	compute.stage.pName = "main"; compute.layout = (VkPipelineLayout)2; compute.basePipelineIndex = -1;
	check(recorder.record_compute_pipeline((VkPipeline)4, compute, nullptr, 0, custom ? 0x104 : 0), "record compute");
	recorder.tear_down_recording_thread();
}
struct Verify : StateCreatorInterface
{
	unsigned apps = 0, modules = 0, layouts = 0, samplers = 0, computes = 0;
	Hash module_hash = 0, layout_hash = 0;
	bool default_application = false;
	void set_application_info(Hash, const VkApplicationInfo *app, const VkPhysicalDeviceFeatures2 *features) override
	{
		apps++;
		if (default_application)
		{
			check(!app && !features, "default application context semantics");
			return;
		}
		check(app && app->apiVersion == VK_API_VERSION_1_0 && app->applicationVersion == 17 &&
		      app->pApplicationName && !strcmp(app->pApplicationName, "utility-parse-test"), "application semantics");
	}
	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *info, VkShaderModule *out) override
	{
		modules++; module_hash = hash; *out = (VkShaderModule)hash;
		return check(info->codeSize == sizeof(code) && !memcmp(info->pCode, code, sizeof(code)), "module semantics");
	}
	bool enqueue_create_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo *info, VkPipelineLayout *out) override
	{
		layouts++; layout_hash = hash; *out = (VkPipelineLayout)hash;
		return check(!info->flags && !info->setLayoutCount && !info->pushConstantRangeCount, "layout semantics");
	}
	bool enqueue_create_sampler(Hash hash, const VkSamplerCreateInfo *info, VkSampler *out) override
	{
		samplers++; *out = (VkSampler)hash;
		return check(info->magFilter == VK_FILTER_LINEAR && info->maxLod == 4.0f, "sampler prefix semantics");
	}
	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *info, VkPipeline *out) override
	{
		computes++; *out = (VkPipeline)hash;
		return check(module_hash && layout_hash && (Hash)info->stage.module == module_hash &&
		             (Hash)info->layout == layout_hash && info->stage.stage == VK_SHADER_STAGE_COMPUTE_BIT &&
		             info->stage.pName && !strcmp(info->stage.pName, "main") && !info->flags &&
		             info->basePipelineIndex == -1 && !info->basePipelineHandle, "compute semantics and reference closure");
	}
	bool enqueue_create_descriptor_set_layout(Hash, const VkDescriptorSetLayoutCreateInfo *, VkDescriptorSetLayout *) override { return check(false, "unexpected descriptor"); }
	bool enqueue_create_render_pass(Hash, const VkRenderPassCreateInfo *, VkRenderPass *) override { return check(false, "unexpected render pass"); }
	bool enqueue_create_render_pass2(Hash, const VkRenderPassCreateInfo2 *, VkRenderPass *) override { return check(false, "unexpected render pass2"); }
	bool enqueue_create_graphics_pipeline(Hash, const VkGraphicsPipelineCreateInfo *, VkPipeline *) override { return check(false, "unexpected graphics"); }
	bool enqueue_create_raytracing_pipeline(Hash, const VkRayTracingPipelineCreateInfoKHR *, VkPipeline *) override { return check(false, "unexpected raytracing"); }
};
void make(const std::string &tool, const std::string &kind, const std::string &variant, const std::string &path)
{
	seed(path + ".seed.foz", true);
	auto from = open(path + ".seed.foz", DatabaseMode::ReadOnly);
	auto to = open(path, DatabaseMode::OverWrite);
	if (!from || !to) return;
	auto apps = hashes(*from, RESOURCE_APPLICATION_INFO);
	if (!check(apps.size() == 1, "seed application count")) return;
	if (kind == "empty") return;
	if (kind == "version" || kind == "invalid-json")
	{
		reject(*to, RESOURCE_COMPUTE_PIPELINE, 0x104, kind); return;
	}
	if (kind == "rejected-application")
	{
		if (variant == "after-callback")
		{
			// Valid application callback followed by a gracefully rejected missing
			// descriptor-set reference. Proves that parser false can follow mutation.
			auto blob = read(*from, RESOURCE_APPLICATION_INFO, apps[0]);
			rapidjson::Document doc; doc.Parse(reinterpret_cast<const char *>(blob.data()), blob.size());
			check(!doc.HasParseError(), "application JSON");
			rapidjson::Document layouts; layouts.Parse("{\"0000000000000102\":{\"flags\":0,\"setLayouts\":[\"00000000deadbeef\"]}}");
			doc.AddMember("pipelineLayouts", rapidjson::Value(layouts, doc.GetAllocator()), doc.GetAllocator());
			rapidjson::StringBuffer text; rapidjson::Writer<rapidjson::StringBuffer> writer(text); doc.Accept(writer);
			Verify verifier; StateReplayer replayer;
			check(!replayer.parse(verifier, to.get(), text.GetString(), text.GetSize()) && verifier.apps == 1,
			      "fixture fails normally after accepted application callback");
			check(to->write_entry(RESOURCE_APPLICATION_INFO, 0x102, text.GetString(), text.GetSize(), 0), "write late rejection");
			// Later valid pipelines must remain unprocessed after the rejected metadata.
			copy(*from, *to, RESOURCE_SHADER_MODULE, 0x101);
			copy(*from, *to, RESOURCE_PIPELINE_LAYOUT, 0x102);
			copy(*from, *to, RESOURCE_COMPUTE_PIPELINE, 0x104);
		}
		else reject(*to, RESOURCE_APPLICATION_INFO, 0x102, variant);
		return;
	}
	if (kind == "mixed" && tool == "prune")
	{
		copy(*from, *to, RESOURCE_APPLICATION_INFO, apps[0], 0x101);
		reject(*to, RESOURCE_APPLICATION_INFO, 0x102, variant);
		copy(*from, *to, RESOURCE_SHADER_MODULE, 0x101);
		copy(*from, *to, RESOURCE_PIPELINE_LAYOUT, 0x102);
		copy(*from, *to, RESOURCE_COMPUTE_PIPELINE, 0x104);
		return;
	}
	copy(*from, *to, RESOURCE_APPLICATION_INFO, apps[0]);
	if (kind == "mixed" && tool == "rehash")
	{
		copy(*from, *to, RESOURCE_SAMPLER, 0x103);
		reject(*to, RESOURCE_SAMPLER, 0x104, variant); return;
	}
	if (kind == "empty-dependencies")
	{
		copy(*from, *to, RESOURCE_PIPELINE_LAYOUT, 0x102); return;
	}
	for (auto tag : {RESOURCE_SHADER_MODULE, RESOURCE_PIPELINE_LAYOUT, RESOURCE_COMPUTE_PIPELINE})
	{
		Hash hash = tag == RESOURCE_SHADER_MODULE ? 0x101 : tag == RESOURCE_PIPELINE_LAYOUT ? 0x102 : 0x104;
		if (kind == "dependency-failure" && tag == RESOURCE_PIPELINE_LAYOUT)
			reject(*to, tag, hash, variant);
		else copy(*from, *to, tag, hash);
	}
	if (kind == "mixed") reject(*to, RESOURCE_COMPUTE_PIPELINE, 0x105, variant);
}
void verify(const std::string &tool, const std::string &kind, const std::string &path, const std::string &input)
{
	auto db = open(path, DatabaseMode::ReadOnly);
	if (!db) return;
	std::string expected_path = path + ".expected.foz";
	if (tool == "rehash") seed(expected_path, false);
	auto expected = open(tool == "rehash" ? expected_path : input, DatabaseMode::ReadOnly);
	if (!expected) return;
	Verify verifier; StateReplayer replayer;
	// Preserved recorder startup metadata is not a resource-work record. The
	// planner clarification requires its exact default identity and contents.
	verifier.default_application = tool == "rehash" && (kind == "empty" || kind == "failed");
	for (unsigned t = 0; t < RESOURCE_COUNT; t++)
	{
		auto tag = static_cast<ResourceTag>(t);
		auto actual = hashes(*db, tag);
		size_t count = verifier.default_application && tag == RESOURCE_APPLICATION_INFO ? 1 : 0;
		if (kind == "populated" && (tag == RESOURCE_APPLICATION_INFO || tag == RESOURCE_SHADER_MODULE ||
		                           tag == RESOURCE_PIPELINE_LAYOUT || tag == RESOURCE_COMPUTE_PIPELINE)) count = 1;
		if (kind == "mixed" && (tag == RESOURCE_APPLICATION_INFO || (tool == "rehash" && tag == RESOURCE_SAMPLER))) count = 1;
		// Rehash emits application-to-resource link metadata using its normal policy.
		if (tool == "rehash" && tag == RESOURCE_APPLICATION_BLOB_LINK && (kind == "mixed" || kind == "populated"))
		{
			check(actual.size() == (kind == "mixed" ? 1u : 3u), "output link count"); continue;
		}
		check(actual.size() == count, ("resource count/tag=" + std::to_string(t)).c_str());
		if (count != 1 || actual.size() != 1) continue;
		auto wanted = hashes(*expected, tag);
		if (verifier.default_application && tag == RESOURCE_APPLICATION_INFO)
			wanted = {Hashing::compute_combined_application_feature_hash({})};
		if (kind == "mixed" && tool == "prune") wanted = {0x101};
		check(wanted == actual, "expected preserved or recalculated hash");
		if (kind == "populated" && tool == "rehash" && tag != RESOURCE_APPLICATION_INFO)
			check(actual[0] != (tag == RESOURCE_SHADER_MODULE ? 0x101 : tag == RESOURCE_PIPELINE_LAYOUT ? 0x102 : 0x104), "hash was recalculated");
	}
	// Replay in dependency order; callbacks check actual decoded values and closure.
	for (auto tag : {RESOURCE_APPLICATION_INFO, RESOURCE_SHADER_MODULE, RESOURCE_SAMPLER, RESOURCE_PIPELINE_LAYOUT, RESOURCE_COMPUTE_PIPELINE})
		for (auto hash : hashes(*db, tag))
		{
			auto blob = read(*db, tag, hash);
			check(replayer.parse(verifier, db.get(), blob.data(), blob.size()), "output reparses");
			if (verifier.default_application && tag == RESOURCE_APPLICATION_INFO)
			{
				rapidjson::Document doc; doc.Parse(reinterpret_cast<const char *>(blob.data()), blob.size());
				check(!doc.HasParseError() && doc.IsObject() && doc.MemberCount() == 3 &&
				      doc.HasMember("version") && doc["version"].IsInt() && doc["version"].GetInt() == FOSSILIZE_FORMAT_VERSION &&
				      doc.HasMember("applicationInfo") && doc["applicationInfo"].IsObject() && !doc["applicationInfo"].MemberCount() &&
				      doc.HasMember("physicalDeviceFeatures") && doc["physicalDeviceFeatures"].IsObject() && !doc["physicalDeviceFeatures"].MemberCount(),
				      "exact synthesized default application metadata");
			}
			if (tool == "prune")
				check(blob == read(*expected, tag, hash), "prune retains exact accepted payload");
		}
	if (verifier.default_application) check(verifier.apps == 1, "only synthesized default application context");
	if (kind == "populated") check(verifier.apps == 1 && verifier.modules == 1 && verifier.layouts == 1 && verifier.computes == 1, "decoded populated set");
	if (kind == "mixed") check(verifier.apps == 1 && verifier.samplers == (tool == "rehash" ? 1u : 0u), "decoded promised prefix");
}
}
int main(int argc, char **argv)
{
	if (argc != 7) return 2;
	std::string mode = argv[1];
	if (mode == "make") make(argv[2], argv[3], argv[4], argv[5]);
	else if (mode == "verify") verify(argv[2], argv[3], argv[5], argv[6]);
	else return 2;
	return failures ? 1 : 0;
}
