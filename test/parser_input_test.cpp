// CPU-only parser containment regressions. Every assertion remains active in Release.
#include "fossilize.hpp"
#include "fossilize_hasher.hpp"
#include "layer/utils.hpp"
#include "fossilize_errors.hpp"
#include "diagnostic_test.hpp"
#include <algorithm>
#include <vector>
#include <cstdint>
using namespace Fossilize;

namespace
{
const char *secret = "P17_SHADER_PAYLOAD_SECRET";
const char *valid_shader = "{\"version\":6,\"shaderModules\":{\"1234\":{\"flags\":3,\"codeSize\":8,\"code\":\"AwIjBwEAAAA=\"}}}";
std::string envelope(const std::string &fields) { return "{\"version\":6," + fields + "}"; }
std::string shader(const std::string &fields, const std::string &key = "1234")
{
	return envelope("\"shaderModules\":{\"" + key + "\":{" + fields + "}}");
}
std::string binary(std::string json, const std::string &payload)
{
	json.push_back('\0');
	json += payload;
	return json;
}
const std::string varint_bytes("\x83\x84\x8c\x39\x01", 5);
std::string valid_varint()
{
	return binary(shader("\"flags\":3,\"codeSize\":8,\"varintOffset\":2,\"varintSize\":5"),
	              std::string("\xfe\xff", 2) + varint_bytes);
}
struct Fixture
{
	std::string group, id, data, reason, field;
	bool null_pointer = false;
	size_t null_size = 0;
};
std::vector<Fixture> fixtures()
{
	std::vector<Fixture> cases;
	auto add = [&](const char *group, const std::string &id, const std::string &data,
	               const char *reason, const std::string &field) {
		cases.push_back({group, id, data, reason, field, false, 0});
	};
	cases.push_back({"envelope", "null-zero", "", "malformed-envelope", "buffer", true, 0});
	cases.push_back({"envelope", "null-nonzero", "", "malformed-envelope", "buffer", true, 7});
	add("envelope", "empty", "", "malformed-envelope", "buffer");
	for (auto value : {" ", "{", "{\"version\":", "{\"version\":6,}"})
		add("envelope", "syntax-" + std::to_string(cases.size()), value, "invalid-json", "");
	for (auto value : {"null", "[]", "[{}]", "1", "true", "\"scalar\""})
		add("envelope", "root-" + std::to_string(cases.size()), value, "malformed-envelope", "root");
	add("envelope", "missing-version", "{}", "malformed-envelope", "version");
	add("envelope", "other-field-without-version", "{\"other\":6}", "malformed-envelope", "version");
	for (auto value : {"null", "\"6\"", "true", "{}", "[]", "6.5", "6.0"})
		add("envelope", "version-type-" + std::to_string(cases.size()), std::string("{\"version\":") + value + "}", "malformed-envelope", "version");
	for (auto value : {"-1", "4", "7", "2147483648", "9223372036854775807", "-9223372036854775808", "18446744073709551615", "18446744073709551616"})
		add("envelope", "version-range-" + std::to_string(cases.size()), std::string("{\"version\":") + value + "}", "incompatible-version", "version");
	for (auto field : {"applicationInfo", "physicalDeviceFeatures", "link", "shaderModules", "samplers", "setLayouts", "pipelineLayouts", "renderPasses", "renderPasses2", "computePipelines", "graphicsPipelines", "raytracingPipelines"})
	{
		for (auto value : {"null", "0", "true", "\"x\"", "[]"})
			add("containers", std::string(field) + "-type-" + std::to_string(cases.size()), envelope(std::string("\"") + field + "\":" + value), "malformed-envelope", field);
		if (std::string(field) == "applicationInfo" || std::string(field) == "physicalDeviceFeatures" || std::string(field) == "link") continue;
		for (auto value : {"null", "1", "\"x\"", "[]", "true"})
			add("containers", std::string(field) + "-entry-" + std::to_string(cases.size()), envelope(std::string("\"") + field + "\":{\"1234\":" + value + "}"), "invalid-record", field);
	}
	for (auto value : {"null", "0", "true", "{}", "[]", "\"\"", "\"xyz\"", "\"10000000000000000\"", "\"12\\u000034\""})
		add("containers", "application-" + std::to_string(cases.size()), envelope(std::string("\"application\":") + value), "malformed-envelope", "application");
	const std::string good = "\"flags\":3,\"codeSize\":8,\"code\":\"AwIjBwEAAAA=\"";
	for (auto key : {"", "xyz", "-1", "+1", "10000000000000000", "12\\u000034"})
		add("shader", "hash-" + std::to_string(cases.size()), shader(good, key), "invalid-field", "shaderModules.hash");
	add("shader", "missing-flags", shader("\"codeSize\":8,\"code\":\"AwIjBwEAAAA=\""), "invalid-field", "shaderModules.flags");
	add("shader", "missing-codeSize", shader("\"flags\":0,\"code\":\"AwIjBwEAAAA=\""), "invalid-field", "shaderModules.codeSize");
	for (auto value : {"null", "-1", "4294967296", "1.5", "\"0\"", "[]", "{}", "true"})
		add("shader", "flags-" + std::to_string(cases.size()), shader(std::string("\"flags\":") + value + ",\"codeSize\":8,\"code\":\"AwIjBwEAAAA=\""), "invalid-field", "shaderModules.flags");
	for (auto value : {"null", "-1", "1.5", "\"8\"", "[]", "{}", "true", "18446744073709551616"})
		add("shader", "codeSize-type-" + std::to_string(cases.size()), shader(std::string("\"flags\":0,\"codeSize\":") + value + ",\"code\":\"AwIjBwEAAAA=\""), "invalid-field", "shaderModules.codeSize");
	for (auto value : {"1", "7", "18446744073709551615"})
		add("shader", "codeSize-alignment-" + std::to_string(cases.size()), shader(std::string("\"flags\":0,\"codeSize\":") + value + ",\"code\":\"AwIjBwEAAAA=\""), "invalid-size", "shaderModules.codeSize");
	add("shader", "huge-base64-decoded-size", shader("\"flags\":0,\"codeSize\":18446744073709551612,\"code\":\"AAAA\""), "invalid-base64", "shaderModules.code");
	add("shader", "missing-code", shader("\"flags\":0,\"codeSize\":8"), "invalid-base64", "shaderModules.code");
	for (auto value : {"null", "0", "true", "[]", "{}", "\"\"", "\"AAAA\"", "\"AwIjBwEAAAA\"", "\"AwIjBwEAAAA==\"", "\"AwIjBwEAAA!=\"", "\"=wIjBwEAAAA=\"", "\"AwIjBwEA=AA=\"", "\"AwIjBwEA\\u0000AA=\"", "\"P17_SHADER_PAYLOAD_SECRET\""})
		add("shader", "base64-" + std::to_string(cases.size()), shader(std::string("\"flags\":0,\"codeSize\":8,\"code\":") + value), "invalid-base64", "shaderModules.code");
	for (auto field : {"varintOffset", "varintSize"})
		add("shader", std::string("partial-") + field, shader(std::string("\"flags\":0,\"codeSize\":8,\"") + field + "\":0"), "invalid-field", "shaderModules.varint-pair");
	for (auto value : {"null", "-1", "1.5", "\"1\"", "true", "[]", "{}", "18446744073709551616"})
	{
		add("shader", "offset-type-" + std::to_string(cases.size()), shader(std::string("\"flags\":0,\"codeSize\":8,\"varintOffset\":") + value + ",\"varintSize\":1"), "invalid-field", "shaderModules.varintOffset");
		add("shader", "size-type-" + std::to_string(cases.size()), shader(std::string("\"flags\":0,\"codeSize\":8,\"varintOffset\":0,\"varintSize\":") + value), "invalid-field", "shaderModules.varintSize");
	}
	for (auto pair : {"0,\"varintSize\":5", "18446744073709551615,\"varintSize\":2", "1,\"varintSize\":18446744073709551615", "2,\"varintSize\":0"})
		add("shader", "range-" + std::to_string(cases.size()), binary(shader(std::string("\"flags\":0,\"codeSize\":8,\"varintOffset\":") + pair), "x"), "invalid-range", "shaderModules.varint");
	add("shader", "missing-binary-payload", shader("\"flags\":0,\"codeSize\":8,\"varintOffset\":0,\"varintSize\":0"), "invalid-range", "shaderModules.varint");
	for (auto payload : {std::string(), std::string("\x80", 1), std::string("\x80\x80\x80\x80\x80\x00", 6), std::string("\xff\xff\xff\xff\x7f\x01", 6), std::string("\x01\x02\x03", 3)})
		add("shader", "varint-stream-" + std::to_string(cases.size()), binary(shader("\"flags\":0,\"codeSize\":8,\"varintOffset\":0,\"varintSize\":" + std::to_string(payload.size())), payload), "invalid-varint", "shaderModules.varint");
	add("shader", "huge-varint-decoded-count", binary(shader("\"flags\":0,\"codeSize\":18446744073709551612,\"varintOffset\":0,\"varintSize\":1"), "x"), "invalid-varint", "shaderModules.varint");
	for (auto field : {"application", "hash", "tag"})
	{
		std::string before = std::string(field) == "application" ? "" : "\"application\":\"10\",";
		std::string after = std::string(field) == "hash" ? ",\"tag\":4" : std::string(field) == "application" ? ",\"hash\":\"20\",\"tag\":4" : "";
		if (std::string(field) == "tag") before += "\"hash\":\"20\",";
		std::string absent = before;
		if (!absent.empty()) absent.pop_back();
		if (std::string(field) == "application") absent = "\"hash\":\"20\",\"tag\":4";
		if (std::string(field) == "hash") absent = "\"application\":\"10\",\"tag\":4";
		add("link", std::string("missing-") + field, envelope("\"link\":{" + absent + "}"), "invalid-field", std::string("link.") + field);
		std::vector<std::string> values = {"null", "true", "{}", "[]", "1.5"};
		if (std::string(field) == "tag")
			values.insert(values.end(), {"-1", "11", "2147483648", "4294967296", "18446744073709551615", "\"4\""});
		else values.insert(values.end(), {"1", "\"\"", "\"xyz\"", "\"-1\"", "\"10000000000000000\"", "\"12\\u000034\"", "\"P17_SHADER_PAYLOAD_SECRET\""});
		for (auto &value : values)
			add("link", std::string(field) + "-type-" + std::to_string(cases.size()), envelope("\"link\":{" + before + "\"" + field + "\":" + value + after + "}"), "invalid-field", std::string("link.") + field);
	}
	add("diagnostics", "hostile-hash", shader(good, std::string(3000, 'z') + "\\n\\t"), "invalid-field", "shaderModules.hash");
	add("diagnostics", "shader-secret", shader("\"flags\":0,\"codeSize\":8,\"code\":\"P17_SHADER_PAYLOAD_SECRET\""), "invalid-base64", "shaderModules.code");
	return cases;
}
struct Creator : StateCreatorInterface
{
	unsigned shader_count = 0, other_count = 0, app_count = 0, current_count = 0, link_count = 0;
	Hash shader_hash = 0, current_hash = 0, app_hash = 0, link_hash = 0, link_app = 0, link_resource = 0;
	ResourceTag link_tag = RESOURCE_COUNT;
	VkShaderModuleCreateFlags flags = 0;
	size_t code_size = 0;
	std::vector<uint32_t> words;
	std::vector<std::string> order;
	bool app_null = true, features_null = true;
	uint32_t app_version = 0, engine_version = 0, api_version = 0, robust = 0;
	std::string app_name, engine_name;
	void set_application_info(Hash h, const VkApplicationInfo *app, const VkPhysicalDeviceFeatures2 *features) override
	{
		app_count++; app_hash = h; app_null = !app; features_null = !features; order.push_back("app");
		if (app) { app_version = app->applicationVersion; engine_version = app->engineVersion; api_version = app->apiVersion; app_name = app->pApplicationName ? app->pApplicationName : ""; engine_name = app->pEngineName ? app->pEngineName : ""; }
		if (features) robust = features->features.robustBufferAccess;
	}
	void set_current_application_info(Hash h) override { current_count++; current_hash = h; order.push_back("current"); }
	void notify_application_info_link(Hash h, Hash a, ResourceTag tag, Hash r) override { link_count++; link_hash = h; link_app = a; link_tag = tag; link_resource = r; order.push_back("link"); }
	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *info, VkShaderModule *module) override
	{
		shader_count++; shader_hash = hash; flags = info->flags; code_size = info->codeSize;
		words.assign(info->pCode, info->pCode + info->codeSize / 4);
		*module = (VkShaderModule)(uintptr_t)0x1234; order.push_back("shader"); return true;
	}
#define OTHER(name, type, handle) bool name(Hash, const type *, handle *) override { other_count++; return true; }
	OTHER(enqueue_create_sampler, VkSamplerCreateInfo, VkSampler)
	OTHER(enqueue_create_descriptor_set_layout, VkDescriptorSetLayoutCreateInfo, VkDescriptorSetLayout)
	OTHER(enqueue_create_pipeline_layout, VkPipelineLayoutCreateInfo, VkPipelineLayout)
	OTHER(enqueue_create_render_pass, VkRenderPassCreateInfo, VkRenderPass)
	OTHER(enqueue_create_render_pass2, VkRenderPassCreateInfo2, VkRenderPass)
	OTHER(enqueue_create_compute_pipeline, VkComputePipelineCreateInfo, VkPipeline)
	OTHER(enqueue_create_graphics_pipeline, VkGraphicsPipelineCreateInfo, VkPipeline)
	OTHER(enqueue_create_raytracing_pipeline, VkRayTracingPipelineCreateInfoKHR, VkPipeline)
#undef OTHER
	unsigned callbacks() const { return shader_count + other_count + app_count + current_count + link_count; }
};
struct LogCapture
{
	std::string text;
	LogCapture() { set_thread_log_callback([](LogLevel, const char *msg, void *p) { static_cast<LogCapture *>(p)->text += msg; }, this); }
	~LogCapture() { set_thread_log_callback(nullptr, nullptr); }
};
void check_shader(DiagnosticChecks &c, const Creator &creator, const std::string &id)
{
	c.require(creator.shader_count == 1 && creator.other_count == 0 && creator.shader_hash == 0x1234, id + "/hash-count");
	c.require(creator.flags == 3 && creator.code_size == 8 && creator.words == std::vector<uint32_t>({0x07230203, 1}), id + "/exact-shader");
}
void continuation(DiagnosticChecks &c, const std::string &id)
{
	StateReplayer replay; Creator creator; LogCapture log;
	c.require(replay.parse(creator, nullptr, valid_shader, strlen(valid_shader)), id + "/continued-valid-parse");
	check_shader(c, creator, id + "/continuation");
	c.require(log.text.empty(), id + "/valid-no-error");
}
void run_fixture(DiagnosticChecks &c, const Fixture &f, bool sentinel)
{
	StateReplayer replay; Creator creator;
	std::string text;
	{
		LogCapture log;
		const void *buffer = f.null_pointer ? nullptr : f.data.data();
		size_t size = f.null_pointer ? f.null_size : f.data.size();
		c.require(!replay.parse(creator, nullptr, buffer, size), f.id + "/false-return");
		text = log.text;
	}
	c.contains(text, "reason=" + f.reason, f.id + "/reason");
	if (!f.field.empty()) c.contains(text, "field=" + f.field, f.id + "/field");
	c.require(creator.callbacks() == 0, f.id + "/no-callbacks");
	c.require(text.size() < 512 && text.find(secret) == std::string::npos && text.find(std::string(80, 'z')) == std::string::npos && text.find("driver") == std::string::npos, f.id + "/bounded-private-input-error");
	if (f.group == "shader" && f.field != "shaderModules.hash") c.contains(text, "tag=4 hash=0000000000001234", f.id + "/known-identity");
	if (f.field == "shaderModules.hash") c.require(text.find("hash=") == std::string::npos, f.id + "/unknown-identity");
	continuation(c, f.id);
	if (sentinel) printf("CONTINUED %s/%s\n", f.group.c_str(), f.id.c_str());
}
void valid_controls(DiagnosticChecks &c)
{
	for (auto version : {FOSSILIZE_FORMAT_MIN_COMPAT_VERSION, FOSSILIZE_FORMAT_VERSION})
	{
		StateReplayer replay; Creator creator; LogCapture log;
		std::string data = "{\"version\":" + std::to_string(version) + ",\"unknown\":[null,1],\"shaderModules\":{},\"samplers\":{},\"setLayouts\":{},\"pipelineLayouts\":{},\"renderPasses\":{},\"renderPasses2\":{},\"computePipelines\":{},\"graphicsPipelines\":{},\"raytracingPipelines\":{}}";
		c.require(replay.parse(creator, nullptr, data.data(), data.size()) && creator.callbacks() == 0 && log.text.empty(), "valid/versions-empty-maps");
	}
	for (auto data : {std::string(valid_shader), valid_varint()})
	{
		StateReplayer replay; Creator creator; LogCapture log;
		c.require(replay.parse(creator, nullptr, data.data(), data.size()), "valid/shader-encoding");
		check_shader(c, creator, "valid/shader-encoding");
		c.require(replay.parse(creator, nullptr, data.data(), data.size()) && creator.shader_count == 1, "valid/duplicate-suppression");
		StateReplayer copy; copy.copy_handle_references(replay);
		c.require(copy.parse(creator, nullptr, data.data(), data.size()) && creator.shader_count == 1, "valid/copied-handles");
		copy.forget_handle_references();
		c.require(copy.parse(creator, nullptr, data.data(), data.size()) && creator.shader_count == 2, "valid/forgotten-handles");
		c.require(log.text.empty(), "valid/shader-no-errors");
	}
	for (auto data : {
		shader("\"flags\":3,\"codeSize\":0,\"code\":\"\""),
		binary(shader("\"flags\":3,\"codeSize\":0,\"varintOffset\":0,\"varintSize\":0"), ""),
		shader("\"flags\":3,\"codeSize\":4,\"code\":\"/////w==\""),
		binary(shader("\"flags\":3,\"codeSize\":4,\"varintOffset\":0,\"varintSize\":5"), std::string("\xff\xff\xff\xff\x0f", 5))})
	{
		StateReplayer replay; Creator creator; LogCapture log;
		c.require(replay.parse(creator, nullptr, data.data(), data.size()), "valid/zero-and-max-word-parse");
		c.require(creator.shader_count == 1 && creator.shader_hash == 0x1234 && creator.flags == 3 &&
		          ((creator.code_size == 0 && creator.words.empty()) ||
		           (creator.code_size == 4 && creator.words == std::vector<uint32_t>({0xffffffffu}))), "valid/zero-and-max-word-bytes");
		c.require(log.text.empty(), "valid/zero-and-max-word-no-error");
	}

	{
		StateReplayer replay; Creator creator; LogCapture log;
		std::string data = envelope("\"applicationInfo\":{},\"physicalDeviceFeatures\":{},\"application\":\"fEdCbA9876543210\",\"link\":{\"application\":\"10\",\"hash\":\"20\",\"tag\":4},\"shaderModules\":{\"1234\":{\"flags\":3,\"codeSize\":8,\"code\":\"AwIjBwEAAAA=\"}}");
		c.require(replay.parse(creator, nullptr, data.data(), data.size()), "valid/app-link-parse");
		c.require(creator.app_count == 1 && creator.app_null && creator.features_null && creator.app_hash == Hashing::compute_combined_application_feature_hash(Hashing::compute_application_feature_hash(nullptr, nullptr)), "valid/empty-app-null-semantics");
		c.require(creator.current_count == 1 && creator.current_hash == 0xfedcba9876543210ull, "valid/exact-application-reference");
		c.require(creator.link_count == 1 && creator.link_app == 0x10 && creator.link_resource == 0x20 && creator.link_tag == RESOURCE_SHADER_MODULE && creator.link_hash == Hashing::compute_hash_application_info_link(0x10, RESOURCE_SHADER_MODULE, 0x20), "valid/exact-link");
		c.require(creator.order == std::vector<std::string>({"app", "current", "link", "shader"}), "valid/callback-order");
		check_shader(c, creator, "valid/app-link"); c.require(log.text.empty(), "valid/app-link-no-errors");
	}
	{
		StateRecorder recorder;
		VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
		app.apiVersion = VK_API_VERSION_1_0; app.applicationVersion = 123; app.engineVersion = 456; app.pApplicationName = "Parser control"; app.pEngineName = "Engine";
		VkPhysicalDeviceFeatures features = {}; features.robustBufferAccess = 1;
		uint32_t words[] = { 0x07230203, 1 };
		VkShaderModuleCreateInfo module = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO }; module.flags = 3; module.codeSize = sizeof(words); module.pCode = words;
		c.require(recorder.record_application_info(app) && recorder.record_physical_device_features(features) && recorder.record_shader_module((VkShaderModule)(uintptr_t)0x1234, module, 0x1234), "valid/recorder-input");
		uint8_t *buffer = nullptr; size_t size = 0;
		c.require(recorder.serialize(&buffer, &size) && buffer && size, "valid/recorder-serialize");
		if (buffer)
		{
			StateReplayer replay; Creator creator; LogCapture log;
			c.require(replay.parse(creator, nullptr, buffer, size), "valid/recorder-roundtrip");
			check_shader(c, creator, "valid/recorder-roundtrip");
			c.require(!creator.app_null && !creator.features_null && creator.app_version == 123 && creator.engine_version == 456 && creator.api_version == VK_API_VERSION_1_0 && creator.robust == 1 && creator.app_name == app.pApplicationName && creator.engine_name == app.pEngineName, "valid/recorder-exact-metadata");
			c.require(log.text.empty(), "valid/recorder-no-errors");
			StateRecorder::free_serialized(buffer);
		}
	}
}
void mutations(DiagnosticChecks &c)
{
	uint32_t state = 0x170008; uint64_t digest = 1469598103934665603ull;
	unsigned accepted = 0, rejected = 0;
	std::vector<std::string> seeds = {"{\"version\":6}", valid_shader, valid_varint(), envelope("\"link\":{\"application\":\"10\",\"hash\":\"20\",\"tag\":4}")};
	auto table = fixtures();
	for (unsigned i = 0; i < 384; i++)
	{
		state ^= state << 13; state ^= state >> 17; state ^= state << 5;
		std::string data;
		if (i % 3 == 0) data = seeds[i % seeds.size()]; // Accepted controls in the actual corpus.
		else if (i % 3 == 1) data = table[2 + state % (table.size() - 2)].data; // Structured field/type/range mutations.
		else
		{
			data = seeds[i % seeds.size()];
			data[state % data.size()] = char(1 + ((state >> 8) % 126));
			if (state & 1) data.resize((state >> 16) % (data.size() + 1));
		}
		c.require(data.size() <= 4096, "mutation/bounded-input");
		for (unsigned char byte : data) { digest ^= byte; digest *= 1099511628211ull; }
		StateReplayer replay; Creator creator;
		{
			LogCapture log;
			bool ok = replay.parse(creator, nullptr, data.data(), data.size());
			if (ok) { accepted++; c.require(log.text.empty(), "mutation/accepted-no-failure"); }
			else { rejected++; c.contains(log.text, "reason=", "mutation/rejection-reason-" + std::to_string(i)); }
			c.require(log.text.find(secret) == std::string::npos && log.text.size() < 512, "mutation/private-bounded-log");
		}
		continuation(c, "mutation/" + std::to_string(i));
		printf("MUTATION_CONTINUED %u\n", i);
	}
	c.require(accepted > 0 && rejected > 0, "mutation/accepted-and-rejected");
	printf("MUTATION_SENTINEL seed=00170008 count=384 accepted=%u rejected=%u digest=%016llx\n", accepted, rejected, static_cast<unsigned long long>(digest));
}
}
int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	set_thread_log_level(LOG_ERROR);
	std::string mode = argv[1]; DiagnosticChecks c; auto table = fixtures();
	if (mode == "--inventory")
	{
		for (auto &f : table) printf("CASE %s/%s\n", f.group.c_str(), f.id.c_str());
		return 0;
	}
	if (mode == "--probe-envelope" || mode == "--probe-nested")
	{
		std::string data = mode == "--probe-envelope" ? "{}" : shader("\"codeSize\":8,\"code\":\"AwIjBwEAAAA=\"");
		StateReplayer replay; Creator creator; LogCapture log;
		bool ok = replay.parse(creator, nullptr, data.data(), data.size());
		c.require(!ok && creator.callbacks() == 0, "probe/reject");
		c.contains(log.text, mode == "--probe-envelope" ? "reason=malformed-envelope" : "reason=invalid-field", "probe/reason");
		continuation(c, "probe");
		printf("PROBE_SENTINEL %s failures=%d\n", mode.c_str(), c.failures);
		return c.failures ? 1 : 0;
	}
	if (mode == "valid") valid_controls(c);
	else if (mode == "mutations") mutations(c);
	else
	{
		unsigned count = 0;
		for (auto &f : table) if (mode == "sentinel" || mode == f.group) { run_fixture(c, f, mode == "sentinel"); count++; }
		c.require(count > 0, "nonempty-table");
		if (mode == "diagnostics")
		{
			StateReplayer replay; Creator creator; LogCapture log;
			std::string invalid = "{\"version\":6,}";
			c.require(!replay.parse(creator, nullptr, invalid.data(), invalid.size()), "diagnostics/syntax-false");
			c.contains(log.text, "reason=invalid-json error=4 offset=13", "diagnostics/exact-parse-code-offset");
		}
		printf("PARSER_%s_SENTINEL cases=%u failures=%d\n", mode.c_str(), count, c.failures);
	}
	printf("PARSER_COMPLETE mode=%s failures=%d\n", mode.c_str(), c.failures);
	return c.failures ? 1 : 0;
}
