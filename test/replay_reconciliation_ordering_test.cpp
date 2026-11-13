#include "diagnostic_test.hpp"
#include "volk.h"
#include "device.hpp"
#include <fstream>
#include <sys/resource.h>

static void install_ordering_dispatch();
class OrderingDevice : public Fossilize::VulkanDevice
{
public:
	bool init_device(const Options &options)
	{
		bool ok = VulkanDevice::init_device(options);
		if (ok)
			install_ordering_dispatch();
		return ok;
	}
};

namespace Fossilize { class ReplayReconciliation; }
static void observe_result(const Fossilize::ReplayReconciliation &);
template <typename Children> static void observe_attempts(const Children &);
#define FOSSILIZE_REPLAY_TEST_RECONCILIATION observe_result
#define FOSSILIZE_REPLAY_TEST_OBSERVE observe_attempts
#define VulkanDevice OrderingDevice
#define main fossilize_replay_ordering_entry
#include "../cli/fossilize_replay.cpp"
#undef main
#undef VulkanDevice

static DiagnosticChecks checks;
static std::string scenario;
static const Hash selected_first = 0x200;
static const Hash library_hash = 0x300;
static ReplayReconciliation::Counts counts;
static std::array<ReplayTerminal, 21> outcomes;
static std::vector<ReplayWorkerOutcome> attempts;

static void observe_result(const ReplayReconciliation &result)
{
	counts = result.counts();
	if (scenario == "cross-worker")
	{
		auto *consumer = result.item(RESOURCE_GRAPHICS_PIPELINE, 0, selected_first);
		auto *library = result.item(RESOURCE_GRAPHICS_PIPELINE, 21, library_hash);
		checks.require(consumer && consumer->status() == ReplayTerminal::Created &&
		               library && library->status() == ReplayTerminal::CreateFailed,
		               "R21/ordering/dependency-only-success-cannot-replace-selected-library-failure");
		return;
	}
	for (unsigned index = 0; index < outcomes.size(); index++)
	{
		auto *item = result.item(RESOURCE_GRAPHICS_PIPELINE, index, selected_first + index);
		checks.require(item != nullptr, "R21/ordering/stable-selected-identity");
		if (item)
			outcomes[index] = item->status();
	}
	checks.require(!result.item(RESOURCE_GRAPHICS_PIPELINE, 21, library_hash), "R21/ordering/library-outside-ledger");
}

template <typename Children> static void observe_attempts(const Children &children)
{
	for (auto &child : children)
		attempts.push_back(child.outcome);
}

static PFN_vkCreateGraphicsPipelines original_create;
static VKAPI_ATTR VkResult VKAPI_CALL create_graphics(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                     const VkGraphicsPipelineCreateInfo *info,
                                                     const VkAllocationCallbacks *allocator, VkPipeline *pipeline)
{
	auto &active = global_replayer->get_per_thread_data();
	Hash hash = active.current_graphics_pipeline;
	bool crash = (scenario == "crash" && hash == selected_first + 10) ||
	             (scenario == "dependency-crash" && hash == library_hash);
	{
		// Flush before the signal so independent call evidence survives the worker.
		std::ofstream log("calls.txt", std::ios::app);
		log << hash << ' ' << active.selected_creation << ' ' << crash << '\n';
	}
	if (crash)
		raise(SIGSEGV);
	if (scenario == "cross-worker" && hash == library_hash && active.selected_creation)
	{
		*pipeline = VK_NULL_HANDLE;
		return VK_ERROR_DEVICE_LOST;
	}
	return original_create(device, cache, count, info, allocator, pipeline);
}

static void install_ordering_dispatch()
{
	original_create = vkCreateGraphicsPipelines;
	vkCreateGraphicsPipelines = create_graphics;
}

static bool fixture(const char *path)
{
	std::unique_ptr<DatabaseInterface> db(create_database(path, DatabaseMode::OverWrite));
	if (!db || !db->prepare())
		return false;
	StateRecorder recorder;
	recorder.init_recording_synchronized(db.get());

	for (unsigned i = 0; i < 3; i++)
	{
		const uint32_t code[] = {0x07230203, 0x10000, i, 1, 0};
		VkShaderModuleCreateInfo shader = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		shader.codeSize = sizeof(code);
		shader.pCode = code;
		if (!recorder.record_shader_module((VkShaderModule)uintptr_t(1 + i), shader, 0x10 + i))
			return false;
	}
	VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	if (!recorder.record_pipeline_layout((VkPipelineLayout)4, layout, 0x20))
		return false;

	VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
	stage.module = (VkShaderModule)1;
	stage.pName = "main";
	VkGraphicsPipelineCreateInfo graphics = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	graphics.stageCount = 1;
	graphics.pStages = &stage;
	graphics.layout = (VkPipelineLayout)4;

	VkPipeline library = (VkPipeline)uintptr_t(0x70);
	graphics.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
	if (!recorder.record_graphics_pipeline(library, graphics, nullptr, 0, library_hash))
		return false;

	VkPipelineLibraryCreateInfoKHR link = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
	link.libraryCount = 1;
	link.pLibraries = &library;
	graphics.flags = 0;
	for (unsigned index = 0; index < 21; index++)
	{
		graphics.pNext = index == 0 ? &link : nullptr;
		stage.module = (VkShaderModule)uintptr_t(index == 0 ? 1 : index <= 10 ? 2 : 3);
		if (!recorder.record_graphics_pipeline((VkPipeline)uintptr_t(0x80 + index), graphics, nullptr, 0, selected_first + index))
			return false;
	}
	VkComputePipelineCreateInfo compute = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	compute.stage = stage;
	compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	compute.layout = (VkPipelineLayout)4;
	if (!recorder.record_compute_pipeline((VkPipeline)uintptr_t(0xa0), compute, nullptr, 0, selected_first))
		return false;
	recorder.tear_down_recording_thread();
	return true;
}

int main(int argc, char **argv)
{
	if (argc == 3 && std::string(argv[1]) == "--fixture")
		return fixture(argv[2]) ? 0 : 1;
	if (argc != 2)
		return 2;
	scenario = argv[1];
	struct rlimit limit = {};
	checks.require(setrlimit(RLIMIT_CORE, &limit) == 0, "R21/ordering/core-dump-disabled");
	std::remove("calls.txt");
	checks.require(fixture("ordering.foz"), "R21/ordering/recorded-FOZ-fixture");
	if (checks.failures)
		return 1;

	Fossilize::VulkanDevice::Options device;
	device.null_device = true;
	ThreadedReplayer::Options options;
	options.num_threads = scenario == "cross-worker" ? 2u : 1u;
	options.disable_rate_limiter = true;
	options.timeout_seconds = 0;
	options.end_graphics_index = scenario == "cross-worker" ? 22u : 21u;
	options.end_compute_index = options.end_raytracing_index = 0;
	int result = run_master_process(device, options, {"ordering.foz"}, nullptr, 0, true, -1, -1);

	std::ifstream log("calls.txt");
	Hash hash;
	bool selected, failed;
	std::vector<std::tuple<Hash, bool, bool>> actual;
	while (log >> hash >> selected >> failed)
		actual.emplace_back(hash, selected, failed);
	if (scenario == "cross-worker")
	{
		unsigned selected_library = 0, extra_library = 0;
		for (const auto &call : actual)
			if (std::get<0>(call) == library_hash)
				(std::get<1>(call) ? selected_library : extra_library)++;
		checks.require(selected_library == 1 && extra_library == 1 && result == EXIT_FAILURE &&
		               counts[unsigned(ReplayTerminal::Created)] == 21 &&
		               counts[unsigned(ReplayTerminal::CreateFailed)] == 1 &&
		               counts[unsigned(ReplayTerminal::Unknown)] == 0,
		               "R21/ordering/real-parent-executed-in-two-worker-roles");
		printf("Executed R21/ordering-cross-worker failures=%d\n", checks.failures);
		return checks.failures ? 1 : 0;
	}
	std::vector<std::tuple<Hash, bool, bool>> expected;
	std::array<ReplayTerminal, 21> expected_outcomes;
	expected_outcomes.fill(ReplayTerminal::Created);
	if (scenario == "crash")
	{
		for (unsigned index = 1; index < 21; index++)
			expected.emplace_back(selected_first + index, true, index == 10);
		expected_outcomes[0] = ReplayTerminal::Unknown;
		expected_outcomes[10] = ReplayTerminal::CrashSkipped;
	}
	else
	{
		for (unsigned index = 1; index < 21; index++)
			expected.emplace_back(selected_first + index, true, false);
		expected.emplace_back(library_hash, false, scenario == "dependency-crash");
		if (scenario == "clean")
			expected.emplace_back(selected_first, true, false);
		else
		{
			expected_outcomes[0] = ReplayTerminal::Unknown;
			for (unsigned index = 1; index < 21; index++)
				expected.emplace_back(selected_first + index, true, false);
		}
	}
	checks.require(actual == expected, "R21/ordering/actual-null-device-call-identities-and-order");
	checks.require(outcomes == expected_outcomes, "R21/ordering/selected-outcome-oracle");
	ReplayReconciliation::Counts expected_counts = {};
	for (auto status : expected_outcomes)
		expected_counts[unsigned(status)]++;
	checks.require(counts == expected_counts && result == (scenario == "clean" ? EXIT_SUCCESS : EXIT_FAILURE),
	               "R21/ordering/aggregate-counts-and-native-status");
	checks.require(attempts.size() == 1, "R21/ordering/controller-retained-worker");
	if (attempts.size() == 1)
	{
		auto &worker = attempts.front();
		checks.require(worker.original[0].begin == 0 && worker.original[0].end == 21, "R21/ordering/original-range");
		checks.require(worker.assigned[0].confirmed_end == (scenario == "clean" ? 21u : 0u),
		               "R21/ordering/uncreated-deferred-index-prevents-confirmed-prefix");
		if (scenario == "crash")
			checks.require(worker.attempts == 2 && worker.failed_attempts == 1 &&
			               worker.previous_attempt_count == 1 && worker.previous_attempts[0].number == 1 &&
			               worker.previous_attempts[0].ranges[0].begin == 0 &&
			               worker.previous_attempts[0].ranges[0].end == 21 &&
			               worker.active.number == 2 && worker.active.ranges[0].begin == 11 &&
			               worker.active.ranges[0].end == 21 && worker.skipped(),
			               "R21/ordering/actual-retry-only-tail-and-skipped-culprit");
		if (scenario == "dependency-crash")
			checks.require(worker.previous_attempt_count && worker.previous_attempts[0].number == 1 &&
			               worker.active.ranges[0].begin == 1 && !worker.skipped(),
			               "R21/ordering/borrowed-cursor-cannot-name-selected-culprit");
	}
	printf("Executed R21/ordering-%s failures=%d\n", scenario.c_str(), checks.failures);
	return checks.failures ? 1 : 0;
}
