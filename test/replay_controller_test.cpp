// Real production controller, OS processes and current worker protocol. The
// adapters below exist only in this translation unit, never in fossilize-replay.
#include "diagnostic_test.hpp"
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdint>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <errno.h>
#endif
static std::string scenario;
#ifdef _WIN32
static HANDLE hold_ready;
static std::string hold_name;
#else
static int hold_ready[2];
#endif
static DiagnosticChecks checks;
template <typename Options> static int test_worker(unsigned, unsigned, const Options &);
template <typename Children> static void observe(const Children &);
template <typename Children> static void before_harvest(const Children &);
#ifdef _WIN32
template <typename Process> static void child_command(std::string &, const char *, const Process &);
template <typename Handles, typename Children> static DWORD test_wait(const Handles &, const Children &);
#define FOSSILIZE_REPLAY_TEST_CHILD_COMMAND child_command
#define FOSSILIZE_REPLAY_TEST_WAIT test_wait
#else
static pid_t test_waitpid(pid_t, int *, int);
#define FOSSILIZE_REPLAY_TEST_BEFORE_HARVEST before_harvest
#define FOSSILIZE_REPLAY_TEST_WAITPID test_waitpid
#endif
#define FOSSILIZE_REPLAY_TEST_WORKER test_worker
#define FOSSILIZE_REPLAY_TEST_OBSERVE observe
#define main fossilize_replay_original_main
#include "../cli/fossilize_replay.cpp"
#undef main

static unsigned observed = 0;
static std::vector<ReplayWorkerOutcome> retained;
static std::string marker_path(unsigned index, unsigned attempt)
{
	return "worker-" + std::to_string(index) + "-" + std::to_string(attempt) + ".txt";
}
static bool mixed() { return scenario == "mixed-success-first" || scenario == "mixed-loss-first"; }
static void complete_items(std::ofstream &out, unsigned kind, unsigned begin, unsigned end)
{
	for (unsigned i = begin; i < end; i++) out << "complete " << kind << ' ' << i << '\n';
	out.flush();
}

template <typename Options> static int test_worker(unsigned index, unsigned attempt, const Options &opts)
{
	std::ofstream out(marker_path(index, attempt));
#ifdef _WIN32
	out << "pid " << GetCurrentProcessId() << '\n';
#else
	out << "pid " << getpid() << '\n';
#endif
	unsigned begin[] = {opts.start_graphics_index, opts.start_compute_index, opts.start_raytracing_index};
	unsigned end[] = {opts.end_graphics_index, opts.end_compute_index, opts.end_raytracing_index};
	for (unsigned i = 0; i < 3; i++) out << "range " << i << ' ' << begin[i] << ' ' << end[i] << '\n';
	out.flush();
	if (scenario == "held-sibling" && index == 0)
	{
#ifdef _WIN32
		HANDLE event = OpenEventA(SYNCHRONIZE, FALSE, hold_name.c_str());
		if (!event || WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) return 76;
		CloseHandle(event);
#else
		char byte;
		if (read(hold_ready[0], &byte, 1) != 1) return 76;
#endif
	}
	if (scenario == "held-sibling" && index == 1)
	{
#ifdef _WIN32
		HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, hold_name.c_str());
		if (!ready) return 75;
		SetEvent(ready); CloseHandle(ready);
		HANDLE event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
		WaitForSingleObject(event, INFINITE);
#else
		if (write(hold_ready[1], "x", 1) != 1) return 75;
		int fds[2];
		if (pipe(fds) != 0) return 77;
		char byte;
		while (read(fds[0], &byte, 1) < 0 && errno == EINTR) {}
#endif
		return 78;
	}
	if (scenario == "lost" || scenario == "lost-progress" || (mixed() && index == 0) ||
	    (scenario == "held-sibling" && index == 0) || (scenario == "loss-after-retry" && attempt > 1))
		return 2;
	if (scenario == "signal")
	{
#ifdef _WIN32
		TerminateProcess(GetCurrentProcess(), 7);
#else
		raise(SIGABRT);
#endif
		return 79;
	}
	const char *names[] = {"GRAPHICS", "COMPUTE", "RAYTRACE"};
	if (attempt == 1 && (scenario == "recover" || scenario == "loss-after-retry" ||
	                    scenario == "partial" || scenario.find("skip-") == 0))
	{
		unsigned skip_kind = scenario.find("compute") != std::string::npos ? 1 :
		                     scenario.find("raytracing") != std::string::npos ? 2 : 0;
		for (unsigned i = 0; i < 3; i++)
		{
			if (scenario == "partial" && i == 1) continue;
			unsigned cursor = begin[i];
			uint64_t hash = 0;
			if (scenario == "loss-after-retry")
			{
				cursor = begin[i] + 1;
				complete_items(out, i, begin[i], cursor);
			}
			if (scenario.find("skip-") == 0)
			{
				bool final = scenario.find("final") != std::string::npos;
				cursor = final ? end[i] : begin[i];
				if (i == skip_kind) { cursor = final ? end[i] : begin[i] + 1; hash = 0xabc + i; }
			}
#ifdef _WIN32
			char message[128];
			sprintf(message, "%s %u %llx\n", names[i], cursor, static_cast<unsigned long long>(hash));
			write_all(GetStdHandle(STD_OUTPUT_HANDLE), message);
#else
			printf("%s %u %llx\n", names[i], cursor, static_cast<unsigned long long>(hash));
			fflush(stdout);
#endif
		}
		return 2;
	}
	for (unsigned i = 0; i < 3; i++) complete_items(out, i, begin[i], end[i]);
	out << "success\n";
	return 0;
}

#ifndef _WIN32
static std::vector<pid_t> order;
static bool harvest_prepared;
template <typename Children> static void before_harvest(const Children &children)
{
	if (!mixed() || harvest_prepared) return;
	// WNOWAIT provides a barrier on actual kernel terminal state without
	// consuming either status. There are no timing sleeps in this fixture.
	for (const auto &child : children)
	{
		siginfo_t info = {};
		int ret;
		do { ret = waitid(P_PID, child.pid, &info, WEXITED | WNOWAIT); } while (ret < 0 && errno == EINTR);
		checks.require(ret == 0, "mixed/both-workers-terminal");
	}
	unsigned first = scenario == "mixed-success-first" ? 1 : 0;
	order.push_back(children[first].pid);
	order.push_back(children[1 - first].pid);
	harvest_prepared = true;
}
static pid_t test_waitpid(pid_t pid, int *status, int flags)
{
	if (scenario == "status-error") { errno = EIO; return -1; }
	if (!order.empty())
	{
		pid = order.front(); order.erase(order.begin());
		return waitpid(pid, status, 0);
	}
	return waitpid(pid, status, flags);
}
#else
template <typename Process> static void child_command(std::string &command, const char *filename, const Process &p)
{
	command = std::string("\"") + filename + "\" --test-worker " + scenario + " " +
	          std::to_string(p.index) + " " + std::to_string(p.outcome.attempts);
	for (unsigned value : {p.start_graphics_index, p.end_graphics_index, p.start_compute_index,
	                      p.end_compute_index, p.start_raytracing_index, p.end_raytracing_index})
		command += " " + std::to_string(value);
	command += " " + (hold_name.empty() ? "unused" : hold_name);
}
template <typename Handles, typename Children> static DWORD test_wait(const Handles &handles, const Children &children)
{
	if (scenario == "status-error") { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
	if (mixed())
	{
		for (const auto &child : children)
			if (child.process) checks.require(WaitForSingleObject(child.process, 5000) == WAIT_OBJECT_0,
			                                 "mixed/both-workers-terminal");
		unsigned first = scenario == "mixed-success-first" ? 1 : 0;
		for (unsigned n = 0; n < 2; n++)
		{
			HANDLE chosen = children[(first + n) % 2].process;
			if (!chosen) continue;
			for (size_t i = 0; i < handles.size(); i++) if (handles[i] == chosen) return DWORD(i);
		}
	}
	return WaitForMultipleObjects(DWORD(handles.size()), handles.data(), FALSE, INFINITE);
}
#endif

template <typename Children> static void observe(const Children &children)
{
	checks.require(Global::active_processes == 0, "cleanup/no-active-children");
	for (const auto &child : children)
	{
#ifdef _WIN32
		checks.require(child.process == nullptr && child.pipe_event == nullptr && child.timer_handle == nullptr &&
		               child.crash_file_handle == INVALID_HANDLE_VALUE, "cleanup/closed-worker-handles");
#else
		checks.require(child.pid < 0 && child.crash_fd < 0 && child.timer_fd < 0 && child.watchdog_timer_fd < 0,
		               "cleanup/reaped-workers-and-closed-ipc");
#endif
		retained.push_back(child.outcome);
		observed++;
	}
}
static bool fixture(const char *path)
{
	std::unique_ptr<DatabaseInterface> db(create_database(path, DatabaseMode::OverWrite));
	if (!db || !db->prepare()) return false;
	StateRecorder recorder;
	recorder.init_recording_synchronized(db.get());
	static const uint32_t code[] = {0x07230203, 0x10000, 0, 1, 0};
	VkShaderModuleCreateInfo shader = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	shader.codeSize = sizeof(code); shader.pCode = code;
	if (!recorder.record_shader_module((VkShaderModule)1, shader, 0x101)) return false;
	VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	if (!recorder.record_pipeline_layout((VkPipelineLayout)2, layout, 0x102)) return false;
	VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stage.module = (VkShaderModule)1; stage.pName = "main";
	for (unsigned i = 0; i < 8; i++)
	{
		VkGraphicsPipelineCreateInfo g = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		stage.stage = VK_SHADER_STAGE_VERTEX_BIT; g.stageCount = 1; g.pStages = &stage; g.layout = (VkPipelineLayout)2;
		if (!recorder.record_graphics_pipeline((VkPipeline)(uintptr_t)(10 + i), g, nullptr, 0, 0x200 + i)) return false;
		VkComputePipelineCreateInfo c = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; c.stage = stage; c.layout = (VkPipelineLayout)2;
		if (!recorder.record_compute_pipeline((VkPipeline)(uintptr_t)(30 + i), c, nullptr, 0, 0x300 + i)) return false;
		VkRayTracingPipelineCreateInfoKHR r = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
		stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR; r.stageCount = 1; r.pStages = &stage; r.layout = (VkPipelineLayout)2;
		if (!recorder.record_raytracing_pipeline((VkPipeline)(uintptr_t)(50 + i), r, nullptr, 0, 0x400 + i)) return false;
	}
	recorder.tear_down_recording_thread();
	return true;
}
static bool malformed_fixture(const char *path)
{
	{
		std::unique_ptr<DatabaseInterface> db(create_database(path, DatabaseMode::OverWrite));
		if (!db || !db->prepare() || !db->write_entry(RESOURCE_COMPUTE_PIPELINE, 0x605, "{}", 2, 0)) return false;
	}
	std::unique_ptr<DatabaseInterface> db(create_database(path, DatabaseMode::ReadOnly));
	if (!db || !db->prepare()) return false;
	size_t count = 0, size = 0;
	Hash hash = 0;
	if (!db->get_hash_list_for_resource_tag(RESOURCE_COMPUTE_PIPELINE, &count, nullptr) || count != 1 ||
	    !db->get_hash_list_for_resource_tag(RESOURCE_COMPUTE_PIPELINE, &count, &hash) || hash != 0x605 ||
	    !db->read_entry(RESOURCE_COMPUTE_PIPELINE, hash, &size, nullptr, 0) || size != 2) return false;
	char bytes[2];
	return db->read_entry(RESOURCE_COMPUTE_PIPELINE, hash, &size, bytes, 0) && memcmp(bytes, "{}", 2) == 0;
}
static std::string read_marker(unsigned worker, unsigned attempt)
{
	std::ifstream in(marker_path(worker, attempt));
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
static void validate(int result, unsigned workers, bool zero)
{
	bool success = scenario == "clean" || scenario == "zero" || scenario == "recover";
	checks.require(result == (success ? 0 : 1), "controller/direct-return-status");
	checks.require(observed == workers, "controller/retained-workers");
	for (unsigned i = 0; i < retained.size(); i++)
	{
		const auto &r = retained[i];
		unsigned begin = zero ? 2 : 2 + i * 4 / workers;
		unsigned end = zero ? 2 : 2 + (i + 1) * 4 / workers;
		for (unsigned kind = 0; kind < 3; kind++)
		{
			checks.require(r.assigned[kind].begin == begin && r.assigned[kind].end == end,
			               "assignment/immutable-original-range");
			if (success) checks.require(r.assigned[kind].confirmed_end == end, "complete/full-assignment");
		}
		if (success) checks.require(r.complete && !r.unknown_remainder && !r.skipped(), "complete/retained-outcome");
	}
	// Kernel checks complement the controller's own cleared bookkeeping.
	for (unsigned worker = 0; worker < workers; worker++)
		for (unsigned attempt = 1; attempt <= retained[worker].attempts; attempt++)
		{
			std::istringstream marker(read_marker(worker, attempt));
			std::string label; unsigned long identity = 0;
			marker >> label >> identity;
			if (label != "pid") continue; // Cleanup may stop a sibling before its first instruction.
#ifdef _WIN32
			HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, DWORD(identity));
			checks.require(!handle || WaitForSingleObject(handle, 0) == WAIT_OBJECT_0, "cleanup/kernel-child-not-live");
			if (handle) CloseHandle(handle);
#else
			int status = 0;
			checks.require(waitpid(pid_t(identity), &status, WNOHANG) == -1 && errno == ECHILD,
			               "cleanup/kernel-child-reaped");
			checks.require(kill(pid_t(identity), 0) == -1 && errno == ESRCH, "cleanup/kernel-child-not-live");
#endif
		}
	const auto &lost = retained[0];
	if (scenario == "lost" || scenario == "lost-progress" || mixed() || scenario == "partial" ||
	    scenario == "held-sibling" || scenario == "signal" || scenario == "loss-after-retry" || scenario == "status-error")
	{
		checks.require(lost.unknown_remainder && !lost.complete && !lost.skipped(), "loss/unknown-remainder-and-identity");
		checks.require(lost.failed_attempts > 0, "loss/failed-attempt-retained");
	}
	if (mixed()) checks.contains(read_marker(1, 1), "success", "mixed/clean-sibling-actually-completed");
	if (scenario == "recover")
	{
		checks.require(lost.attempts == 2 && lost.failed_attempts == 1, "recover/attempt-history");
		std::string marker = read_marker(0, 2);
		for (unsigned kind = 0; kind < 3; kind++) checks.contains(marker,
			"range " + std::to_string(kind) + " 2 6", "recover/exact-retry-assignment");
		for (unsigned kind = 0; kind < 3; kind++)
			for (unsigned i = 2; i < 6; i++) checks.contains(marker,
				"complete " + std::to_string(kind) + " " + std::to_string(i), "recover/every-required-item");
	}
	if (scenario == "loss-after-retry")
	{
		checks.require(lost.attempts == 2, "partial/second-attempt-lost");
		for (const auto &range : lost.assigned) checks.require(range.confirmed_end == 3, "partial/known-prefix-retained");
		for (unsigned kind = 0; kind < 3; kind++) checks.contains(read_marker(0, 2),
			"range " + std::to_string(kind) + " 3 6", "partial/exact-retry-assignment");
	}
	if (scenario.find("skip-") == 0)
	{
		unsigned kind = scenario.find("compute") != std::string::npos ? 1 : scenario.find("raytracing") != std::string::npos ? 2 : 0;
		bool final = scenario.find("final") != std::string::npos;
		checks.require(lost.skipped() && !lost.complete && lost.assigned[kind].skipped_hash == 0xabc + kind &&
		               lost.assigned[kind].skipped_index == (final ? 5u : 2u), "skip/culprit-retained");
		checks.require(lost.attempts == (final ? 1u : 2u), "skip/retry-or-final-boundary");
		if (!final) checks.contains(read_marker(0, 2), "success", "skip/tail-completed-but-required-culprit-unresolved");
	}
}
static int state_and_protocol()
{
	// Same protocol literals and shared-ring consumer contract as the input.
	struct ProgressMemory { SharedControlBlock control; char ring[4096]; } memory = {};
	memory.control.version_cookie = 0x19bcde1d;
	memory.control.ring_buffer_offset = unsigned(offsetof(ProgressMemory, ring));
	memory.control.ring_buffer_size = sizeof(memory.ring);
	Global::control_block = &memory.control;
#ifdef _WIN32
	Global::shared_mutex = CreateMutexA(nullptr, FALSE, nullptr);
#endif
	ProcessProgress process;
	process.outcome.assign(2, 6, 2, 6, 2, 6);
	process.parse("GRAPHICS 3 abc");
	process.parse("COMPUTE 4 def");
	process.parse("RAYTRACE 5 123");
	checks.require(process.graphics_progress == 3 && process.compute_progress == 4 && process.raytracing_progress == 5,
	               "protocol/preserved-resume-cursors");
	const char *expected[] = {"GRAPHICS 2 abc\n", "COMPUTE 3 def\n", "RAYTRACE 4 123\n"};
	for (const char *value : expected)
	{
		char message[ControlBlockMessageSize] = {};
		checks.require(shared_control_block_read(&memory.control, message, sizeof(message)), "protocol/forwarded-ring-message");
		checks.require(strcmp(message, value) == 0, "protocol/preserved-index-hash-message");
	}
	checks.require(process.outcome.assigned[0].skipped_index == 2 && process.outcome.assigned[0].skipped_hash == 0xabc &&
	               process.outcome.assigned[1].skipped_index == 3 && process.outcome.assigned[1].skipped_hash == 0xdef &&
	               process.outcome.assigned[2].skipped_index == 4 && process.outcome.assigned[2].skipped_hash == 0x123,
	               "protocol/authoritative-culprits-independent-of-forwarding");
	Global::control_block = nullptr;
#ifdef _WIN32
	CloseHandle(Global::shared_mutex); Global::shared_mutex = nullptr;
#endif
	ReplayWorkerOutcome unknown;
	unknown.assign(2, 6, 0, 0, 0, 0);
	unknown.native_error = 5;
	checks.require(unknown.finish(true, true, 6, 0, 0) == WorkerDisposition::Unresolved &&
	               unknown.unknown_remainder && !unknown.complete && unknown.native_error == 5,
	               "state/failed-native-status-never-defaults-to-success");
	ReplayWorkerOutcome retry;
	retry.assign(2, 6, 0, 0, 0, 0);
	checks.require(retry.finish(false, true, 2, -1, -1) == WorkerDisposition::Retry, "state/zero-work-kinds-need-no-marker");
	checks.require(retry.finish(true, true, -1, -1, -1) == WorkerDisposition::Complete && retry.failed_attempts == 1,
	               "state/failed-attempt-does-not-poison-complete-retry");
	printf("Executed R13/state-and-current-protocol; failures=%d\n", checks.failures);
	return checks.failures ? 99 : 0;
}

int main(int argc, char **argv)
{
#ifndef _WIN32
	struct rlimit core_limit = {};
	setrlimit(RLIMIT_CORE, &core_limit);
#endif
#ifdef _WIN32
	if (argc == 12 && std::string(argv[1]) == "--test-worker")
	{
		scenario = argv[2];
		hold_name = argv[11];
		ThreadedReplayer::Options opts;
		opts.start_graphics_index = unsigned(atoi(argv[5])); opts.end_graphics_index = unsigned(atoi(argv[6]));
		opts.start_compute_index = unsigned(atoi(argv[7])); opts.end_compute_index = unsigned(atoi(argv[8]));
		opts.start_raytracing_index = unsigned(atoi(argv[9])); opts.end_raytracing_index = unsigned(atoi(argv[10]));
		return test_worker(unsigned(atoi(argv[3])), unsigned(atoi(argv[4])), opts);
	}
#endif
	if (argc == 3 && std::string(argv[1]) == "--malformed") return malformed_fixture(argv[2]) ? 0 : 1;
	if (argc == 3 && std::string(argv[1]) == "--fixture") return fixture(argv[2]) ? 0 : 1;
	if (argc < 4 || std::string(argv[1]) != "--controller") return 2;
	scenario = argv[2];
	if (scenario == "outcome-state") return state_and_protocol();
	VulkanDevice::Options device_opts;
	device_opts.null_device = true;
	ThreadedReplayer::Options opts;
	opts.num_threads = mixed() || scenario == "held-sibling" ? 2 : 1;
	opts.disable_rate_limiter = true;
	opts.start_graphics_index = opts.start_compute_index = opts.start_raytracing_index = 2;
	opts.end_graphics_index = opts.end_compute_index = opts.end_raytracing_index = scenario == "zero" ? 2 : 6;
	if (scenario == "held-sibling")
	{
#ifdef _WIN32
		hold_name = "FossilizeR13Ready" + std::to_string(GetCurrentProcessId());
		hold_ready = CreateEventA(nullptr, TRUE, FALSE, hold_name.c_str());
		checks.require(hold_ready != nullptr, "cleanup/create-held-sibling-barrier");
#else
		checks.require(pipe(hold_ready) == 0, "cleanup/create-held-sibling-barrier");
#endif
	}
	std::vector<const char *> databases{argv[3]};
	struct ProgressMemory { SharedControlBlock control; char ring[4096]; } memory = {};
	auto &optional_progress = memory.control;
	bool use_progress = scenario == "lost-progress" || scenario == "held-sibling" ||
	                    scenario == "skip-graphics-progress" || scenario == "skip-graphics-final-progress";
	if (use_progress)
	{
		optional_progress.ring_buffer_offset = unsigned(offsetof(ProgressMemory, ring));
		optional_progress.ring_buffer_size = sizeof(memory.ring);
		Global::control_block = &optional_progress;
#ifdef _WIN32
		Global::shared_mutex = CreateMutexA(nullptr, FALSE, nullptr);
#endif
	}
	int result;
#ifdef _WIN32
	result = run_master_process(device_opts, opts, databases, nullptr, 0, true, nullptr, nullptr);
#else
	result = run_master_process(device_opts, opts, databases, nullptr, 0, true, -1, -1);
#endif
	validate(result, opts.num_threads, scenario == "zero");
	if (scenario == "lost-progress" || scenario == "held-sibling")
	{
		checks.require(optional_progress.clean_process_deaths.load() == 0, "progress/loss-is-not-clean-recovery");
		checks.require(optional_progress.dirty_process_deaths.load() == 1, "progress/only-primary-dirty-worker-counted");
		checks.require(optional_progress.progress_complete.load() == 0, "progress/incomplete-on-terminal-loss");
	}
	if (scenario == "skip-graphics-progress" || scenario == "skip-graphics-final-progress")
	{
		checks.require(optional_progress.clean_process_deaths.load() == 1 && optional_progress.dirty_process_deaths.load() == 0,
		               "progress/preserved-clean-crash-counter-independent-of-unresolved-required-work");
	}
	Global::control_block = nullptr;
#ifdef _WIN32
	if (use_progress) { CloseHandle(Global::shared_mutex); Global::shared_mutex = nullptr; }
#endif
	if (scenario == "held-sibling")
	{
#ifdef _WIN32
		CloseHandle(hold_ready);
#else
		close(hold_ready[0]); close(hold_ready[1]);
#endif
		checks.contains(read_marker(1, 1), "pid ", "cleanup/sibling-reached-barrier");
	}
	std::ofstream report("result.txt");
	report << "scenario " << scenario << "\nresult " << result << "\nfailures " << checks.failures << '\n';
	for (unsigned i = 0; i < retained.size(); i++)
	{
		const auto &r = retained[i];
		report << "worker " << i << " attempts " << r.attempts << " failed " << r.failed_attempts
		       << " unknown " << r.unknown_remainder << " complete " << r.complete << " termination " << r.termination << '\n';
		for (unsigned k = 0; k < 3; k++) report << "assigned " << k << ' ' << r.assigned[k].begin << ' ' << r.assigned[k].end
			<< " confirmed " << r.assigned[k].confirmed_end << " culprit " << r.assigned[k].skipped_hash << ' ' << r.assigned[k].skipped_index << '\n';
	}
	printf("Executed R13/%s; failures=%d; controller-result=%d\n", scenario.c_str(), checks.failures, result);
	return checks.failures ? 99 : result;
}
