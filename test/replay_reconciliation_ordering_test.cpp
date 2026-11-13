#include "diagnostic_test.hpp"
#include "volk.h"
#include "device.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sys/resource.h>
#include <unistd.h>

// Observe large C++ buffers which stay live across the actual controller fork.
// Tracking is enabled only while the narrow-selection controller runs.
namespace
{
struct TrackedAllocation
{
	void *pointer = nullptr;
	size_t size = 0;
};
std::array<TrackedAllocation, 256> large_allocations;
std::atomic<bool> track_allocations{false};
std::mutex allocation_lock;
size_t inherited_large_bytes = 0;
size_t peak_live_large_bytes = 0;
size_t live_large_bytes = 0;

void *allocate(size_t size)
{
	void *pointer = std::malloc(size ? size : 1);
	if (!pointer)
		std::abort();
	if (track_allocations && size >= 256 * 1024)
	{
		std::lock_guard<std::mutex> hold(allocation_lock);
		for (auto &entry : large_allocations)
			if (!entry.pointer)
			{
				entry = {pointer, size};
				live_large_bytes += size;
				peak_live_large_bytes = std::max(peak_live_large_bytes, live_large_bytes);
				return pointer;
			}
		std::abort();
	}
	return pointer;
}

void release(void *pointer) noexcept
{
	if (pointer && track_allocations)
	{
		std::lock_guard<std::mutex> hold(allocation_lock);
		for (auto &entry : large_allocations)
			if (entry.pointer == pointer)
			{
				live_large_bytes -= entry.size;
				entry = {};
				break;
			}
	}
	std::free(pointer);
}

pid_t observe_fork()
{
	if (track_allocations)
		inherited_large_bytes = live_large_bytes;
	pid_t pid = fork();
	if (pid == 0)
		track_allocations = false;
	return pid;
}
}

void *operator new(size_t size) { return allocate(size); }
void *operator new[](size_t size) { return allocate(size); }
void operator delete(void *p) noexcept { release(p); }
void operator delete[](void *p) noexcept { release(p); }
void operator delete(void *p, size_t) noexcept { release(p); }
void operator delete[](void *p, size_t) noexcept { release(p); }

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
#define fork observe_fork
#define VulkanDevice OrderingDevice
#define main fossilize_replay_ordering_entry
#include "../cli/fossilize_replay.cpp"
#undef main
#undef VulkanDevice
#undef fork

static DiagnosticChecks checks;
static std::string scenario;
static const Hash selected_first = 0x200;
static const Hash library_hash = 0x300;
static const Hash foundation_hash = 0x301;
static const unsigned large_universe_size = 105000;
static const unsigned context_padding = 1024;
static ReplayReconciliation::Counts counts;
static std::array<ReplayTerminal, 21> outcomes;
static std::vector<ReplayWorkerOutcome> attempts;
static size_t worker_storage;

static bool nested_parent()
{
	return scenario == "same-worker" || scenario == "same-worker-fail" ||
	       scenario == "same-worker-blocked" || scenario == "same-worker-contexts";
}

static Hash nested_library_hash()
{
	return scenario == "same-worker-contexts" ? 0x20000000 : library_hash;
}

static Hash nested_foundation_hash()
{
	return scenario == "same-worker-contexts" ? 0x20000001 : foundation_hash;
}

static unsigned library_index()
{
	return 21 + (scenario == "same-worker-contexts" ? context_padding : 0);
}

static void observe_result(const ReplayReconciliation &result)
{
	counts = result.counts();
	if (scenario == "narrow-retention")
	{
		auto *selected = result.item(RESOURCE_GRAPHICS_PIPELINE, 1, selected_first + 1);
		checks.require(result.selected_total_known && result.item_capacity() == 1 && selected &&
		               selected->status() == ReplayTerminal::Created,
		               "R21/retention/stable-narrow-selection-survives-report-destruction");
		checks.require(result.storage_bytes() + worker_storage ==
		               sizeof(ReplayReconciliation) + 28 + sizeof(ProcessProgress),
		               "R21/retention/controller-capacity-includes-owned-hash-and-worker-slot");
		return;
	}
	if (nested_parent())
	{
		auto *consumer = result.item(RESOURCE_GRAPHICS_PIPELINE, 0, selected_first);
		auto *library = result.item(RESOURCE_GRAPHICS_PIPELINE, library_index(), nested_library_hash());
		auto *foundation = result.item(RESOURCE_GRAPHICS_PIPELINE, library_index() + 1, nested_foundation_hash());
		bool clean = scenario == "same-worker" || scenario == "same-worker-contexts";
		bool blocked = scenario == "same-worker-blocked";
		checks.require(consumer && consumer->status() == (clean ? ReplayTerminal::Created : ReplayTerminal::Dependency) &&
		               library && library->status() == (clean ? ReplayTerminal::Created : blocked ? ReplayTerminal::Dependency : ReplayTerminal::CreateFailed) &&
		               foundation && foundation->status() == (blocked ? ReplayTerminal::CreateFailed : ReplayTerminal::Created),
		               "R21/ordering/nested-parent-terminal-identities");
		return;
	}
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
	worker_storage = children.capacity() * sizeof(typename Children::value_type);
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
		log << hash << ' ' << (active.selected_index != ~0u) << ' ' << crash << '\n';
		if (nested_parent() && hash == nested_library_hash())
		{
			std::ofstream identity("parent.txt", std::ios::app);
			identity << active.selected_index << ' ' << active.current_graphics_index << ' '
			         << active.selected_crash_cursor << '\n';
		}
	}
	if (crash)
		raise(SIGSEGV);
	if ((scenario == "cross-worker" && hash == library_hash && active.selected_index != ~0u) ||
	    (scenario == "same-worker-fail" && hash == library_hash) ||
	    (scenario == "same-worker-blocked" && hash == foundation_hash))
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
	VkPipeline foundation = (VkPipeline)uintptr_t(0x71);
	VkPipelineLibraryCreateInfoKHR parent_link = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
	if (nested_parent())
	{
		if (!recorder.record_graphics_pipeline(foundation, graphics, nullptr, 0, nested_foundation_hash()))
			return false;
		parent_link.libraryCount = 1;
		parent_link.pLibraries = &foundation;
		graphics.pNext = &parent_link;
	}
	if (!recorder.record_graphics_pipeline(library, graphics, nullptr, 0, nested_library_hash()))
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
	if (scenario == "same-worker-contexts")
	{
		graphics.pNext = nullptr;
		for (unsigned i = 0; i < context_padding; i++)
			if (!recorder.record_graphics_pipeline((VkPipeline)uintptr_t(0x1000 + i), graphics, nullptr, 0,
			                                       0x1000 + i))
				return false;
	}
	VkComputePipelineCreateInfo compute = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	compute.stage = stage;
	compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	compute.layout = (VkPipelineLayout)4;
	if (!recorder.record_compute_pipeline((VkPipeline)uintptr_t(0xa0), compute, nullptr, 0, selected_first))
		return false;
	recorder.tear_down_recording_thread();
	if (scenario == "narrow-retention")
	{
		// The extra records are real archive entries, but hash selection never reads their payloads.
		for (unsigned i = 0; i < large_universe_size; i++)
			if (!db->write_entry(RESOURCE_GRAPHICS_PIPELINE, 0x1000 + i, nullptr, 0, 0))
				return false;
	}
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
	std::remove("parent.txt");
	checks.require(fixture("ordering.foz"), "R21/ordering/recorded-FOZ-fixture");
	if (checks.failures)
		return 1;

	Fossilize::VulkanDevice::Options device;
	device.null_device = true;
	ThreadedReplayer::Options options;
	options.num_threads = scenario == "cross-worker" ? 2u : 1u;
	options.disable_rate_limiter = true;
	options.timeout_seconds = 0;
	options.end_graphics_index = nested_parent() ? library_index() + 2 : scenario == "cross-worker" ? 22u : 21u;
	if (scenario == "narrow-retention")
		options.pipeline_hash = selected_first + 1;
	options.end_compute_index = options.end_raytracing_index = 0;
	track_allocations = scenario == "narrow-retention";
	int result = run_master_process(device, options, {"ordering.foz"}, nullptr, 0, true, -1, -1);
	track_allocations = false;

	std::ifstream log("calls.txt");
	Hash hash;
	bool selected, failed;
	std::vector<std::tuple<Hash, bool, bool>> actual;
	while (log >> hash >> selected >> failed)
		actual.emplace_back(hash, selected, failed);
	if (scenario == "narrow-retention")
	{
		std::unique_ptr<DatabaseInterface> db(create_database("ordering.foz", DatabaseMode::ReadOnly));
		const unsigned starts[] = {0, 0, 0}, ends[] = {~0u, ~0u, ~0u};
		checks.require(db && db->prepare(), "R21/retention/discover-large-archive");
		if (db)
		{
			auto scope = select_replay_scope(*db, starts, ends, selected_first + 1, false);
			checks.require(scope.preparation.discovered.size() >= large_universe_size &&
			               scope.excluded.size() >= large_universe_size && scope.selected.size() == 1,
			               "R21/retention/large-excluded-universe-and-one-selected");
		}
		checks.require(result == EXIT_SUCCESS && actual == decltype(actual){{selected_first + 1, true, false}} &&
		               counts[unsigned(ReplayTerminal::Created)] == 1,
		               "R21/retention/one-real-creation-from-large-universe");
		checks.require(peak_live_large_bytes >= large_universe_size * sizeof(Hash) && inherited_large_bytes == 0,
		               "R21/retention/large-preparation-allocations-released-before-worker-fork");
		printf("Executed R21/retention universe>=%u retained_worker_bytes=%zu large_bytes_peak=%zu large_bytes_at_fork=%zu failures=%d\n",
		       large_universe_size, worker_storage, peak_live_large_bytes, inherited_large_bytes, checks.failures);
		return checks.failures ? 1 : 0;
	}
	if (nested_parent())
	{
		bool clean = scenario == "same-worker" || scenario == "same-worker-contexts";
		bool blocked = scenario == "same-worker-blocked";
		ReplayReconciliation::Counts expected = {};
		expected[unsigned(ReplayTerminal::Created)] = clean ? library_index() + 2 : 21 - unsigned(blocked);
		expected[unsigned(ReplayTerminal::CreateFailed)] = !clean;
		expected[unsigned(ReplayTerminal::Dependency)] = clean ? 0 : 1 + unsigned(blocked);
		checks.require(counts == expected && result == (clean ? EXIT_SUCCESS : EXIT_FAILURE),
		               "R21/ordering/nested-parent-counts-and-native-status");
		checks.require(std::count(actual.begin(), actual.end(), std::make_tuple(nested_library_hash(), true, false)) == !blocked &&
		               std::count_if(actual.begin(), actual.end(), [](const std::tuple<Hash, bool, bool> &call) {
		                   return std::get<0>(call) == nested_library_hash();
		               }) == !blocked, "R21/ordering/nested-parent-real-replacement-call");
		if (!blocked)
		{
			std::ifstream identity("parent.txt");
			unsigned index = ~0u, cursor = ~0u;
			bool selected_cursor = true;
			identity >> index >> cursor >> selected_cursor;
			checks.require(identity && index == library_index() && cursor == 1 && !selected_cursor,
			               "R21/ordering/nested-parent-selected-identity-with-independent-borrowed-cursor");
		}
		checks.require(attempts.size() == 1 && attempts.front().attempts == 1 &&
		               attempts.front().controlled_failure == !clean && attempts.front().failed_attempts == 0,
		               "R21/ordering/nested-parent-controlled-failure-keeps-legacy-counter");
		printf("Executed R21/ordering-%s failures=%d\n", scenario.c_str(), checks.failures);
		return checks.failures ? 1 : 0;
	}
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
