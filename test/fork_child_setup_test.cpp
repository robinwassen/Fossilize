// Linux-only lifecycle regression. Syscall wrapping is confined to this test
// executable; the included production controller and worker remain unmodified.
#include <sys/prctl.h>
static int setup_test_prctl(int option, int value);
#define prctl setup_test_prctl
#define main fossilize_replay_original_main
#include "../cli/fossilize_replay.cpp"
#undef main
#undef prctl
#include <poll.h>
#include <new>
#include <map>
#include <set>
#include <chrono>
#include "diagnostic_test.hpp"

namespace
{
enum EventKind { Birth, Child, Inject, Mask, Output, Input, InputEOF, OutputEOF,
                 Reap, WorkerExit, Fallthrough, ForeignFork, Ready, ForcedCrash,
                 AtExit, Destructor, BarrierRelease, BarrierPassed };
enum Fault { None, Sigmask, Stdout, Stdin, Prctl, ParentRace };
struct Event { int kind; pid_t pid; int generation; int a; int b; };
static int events_fd = -1;
static pid_t controller_pid = -1;
static unsigned generation;
static bool worker, injected, recovery, parent_death;
static Fault fault;
static unsigned fault_generation;
static int pipe_index, crash_read_fd;
static int crash_reads[8];
static FILE *flush_sentinel;
static int fault_barrier[2] = { -1, -1 };
static pid_t worker_pids[8];
static bool barrier_released;

// Each record is a single PIPE_BUF-sized write, including in post-fork setup.
static void event(EventKind kind, int a = 0, int b = 0)
{
	Event record = { int(kind), getpid(), int(generation), a, b };
	ssize_t ret;
	do { ret = write(events_fd, &record, sizeof(record)); } while (ret < 0 && errno == EINTR);
	if (ret != sizeof(record)) _exit(98);
}
static bool selected(Fault site)
{
	return worker && !injected && fault == site && generation == fault_generation;
}
static void injection(Fault site)
{
	if (fault_barrier[0] >= 0)
	{
		// The controller releases only after waitpid collected the real healthy
		// sibling successfully. A WorkerExit notification is not sufficient.
		char byte;
		ssize_t ret;
		do { ret = read(fault_barrier[0], &byte, 1); } while (ret < 0 && errno == EINTR);
		if (ret != 1 || byte != 'R') _exit(95);
		close(fault_barrier[0]); fault_barrier[0] = -1;
		event(BarrierPassed);
	}
	injected = true;
	event(Inject, int(site));
}
static void inherited_atexit() { if (worker && injected) event(AtExit); }
struct InheritedDestructor { ~InheritedDestructor() { if (worker && injected) event(Destructor); } };
static InheritedDestructor inherited_destructor;
static double now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

extern "C"
{
pid_t __real_fork();
int __real_pipe(int *);
int __real_pthread_sigmask(int, const sigset_t *, sigset_t *);
int __real_dup2(int, int);
pid_t __real_getppid();
pid_t __real_waitpid(pid_t, int *, int);
ssize_t __real_read(int, void *, size_t);
int __real_epoll_wait(int, epoll_event *, int, int);
[[noreturn]] void __real_exit(int);

int __wrap_pipe(int *fds)
{
	int ret = __real_pipe(fds);
	if (ret == 0 && getpid() == controller_pid && !worker)
	{
		// start_child_process creates crash then input pipes immediately before fork.
		if ((pipe_index++ & 1) == 0) crash_read_fd = fds[0];
	}
	return ret;
}
pid_t __wrap_fork()
{
	if (worker)
	{
		event(ForeignFork);
		_exit(91); // Bound the original defect without allowing a fork cascade.
	}
	++generation;
	pid_t pid = __real_fork();
	if (pid == 0)
	{
		worker = true;
		if (fault_barrier[1] >= 0) { close(fault_barrier[1]); fault_barrier[1] = -1; }
		if (generation != fault_generation && fault_barrier[0] >= 0)
		{ close(fault_barrier[0]); fault_barrier[0] = -1; }
		event(Child, __real_getppid());
	}
	else if (pid > 0)
	{
		if (generation < 8)
		{
			worker_pids[generation] = pid;
			for (unsigned i = 1; i < generation; i++)
				if (crash_reads[i] == crash_read_fd) crash_reads[i] = -1;
			crash_reads[generation] = crash_read_fd;
		}
		event(Birth, pid, crash_read_fd);
	}
	return pid;
}

pid_t __wrap_getppid()
{
	if (selected(ParentRace)) { injection(ParentRace); return controller_pid + 1; }
	return __real_getppid();
}
int __wrap_pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
	if (worker && how == SIG_SETMASK && set == &Global::old_mask)
	{
		if (selected(Sigmask)) { injection(Sigmask); return EINVAL; }
		int ret = __real_pthread_sigmask(how, set, old);
		if (ret == 0) event(Mask);
		return ret;
	}
	return __real_pthread_sigmask(how, set, old);
}
int __wrap_dup2(int from, int to)
{
	if (worker && (to == STDOUT_FILENO || to == STDIN_FILENO))
	{
		Fault site = to == STDOUT_FILENO ? Stdout : Stdin;
		if (selected(site)) { injection(site); errno = EBADF; return -1; }
		int ret = __real_dup2(from, to);
		if (ret >= 0)
		{
			event(to == STDOUT_FILENO ? Output : Input);
			if (to == STDIN_FILENO)
			{
				// No banned modules are in this fixture. Read actual controller EOF,
				// without setting stdio's EOF flag or replacing the worker protocol.
				char byte;
				ssize_t n;
				do { n = __real_read(STDIN_FILENO, &byte, 1); } while (n < 0 && errno == EINTR);
				if (n != 0) { event(Fallthrough, 1); _exit(92); }
				event(InputEOF);
				if (recovery && generation == 1)
				{
					// Exercise the real shutdown parser and recovery caller, with
					// zero completed or skipped pipelines. This is a controlled crash, not a GPU.
					static const char progress[] = "GRAPHICS 0 0\nCOMPUTE 0 0\nRAYTRACE 0 0\n";
					if (write(STDOUT_FILENO, progress, sizeof(progress) - 1) != sizeof(progress) - 1) _exit(93);
					event(ForcedCrash);
					_exit(2);
				}
				if (parent_death)
				{
					event(Ready);
					for (;;) pause(); // Observer kills controller only after Ready.
				}
			}
		}
		return ret;
	}
	return __real_dup2(from, to);
}
pid_t __wrap_waitpid(pid_t pid, int *status, int options)
{
	pid_t ret = __real_waitpid(pid, status, options);
	if (ret > 0 && getpid() == controller_pid)
	{
		event(Reap, ret, *status);
		unsigned healthy_generation = fault_generation == 1 ? 2 : 1;
		if (fault_barrier[1] >= 0 && !barrier_released &&
		    ret == worker_pids[healthy_generation] && WIFEXITED(*status) && WEXITSTATUS(*status) == 0)
		{
			barrier_released = true;
			event(BarrierRelease, ret, *status);
			const char byte = 'R';
			if (write(fault_barrier[1], &byte, 1) != 1) _exit(95);
			close(fault_barrier[1]); fault_barrier[1] = -1;
		}
	}
	return ret;
}
ssize_t __wrap_read(int fd, void *data, size_t size)
{
	ssize_t ret = __real_read(fd, data, size);
	if (ret == 0 && getpid() == controller_pid)
		for (unsigned i = 1; i <= generation && i < 8; i++)
			if (crash_reads[i] == fd) { event(OutputEOF, int(i), fd); break; }
	return ret;
}
int __wrap_epoll_wait(int fd, epoll_event *events, int count, int timeout)
{
	if (worker) { event(Fallthrough, 2); _exit(90); }
	return __real_epoll_wait(fd, events, count, timeout);
}
[[noreturn]] void __wrap_exit(int status)
{
	if (worker) event(WorkerExit, status);
	__real_exit(status);
}
}

// Typed translation-unit adapter avoids interpreting prctl's optional varargs.
static int setup_test_prctl(int option, int value)
{
	if (worker && option == PR_SET_PDEATHSIG && selected(Prctl))
	{
		injection(Prctl); errno = EPERM; return -1;
	}
	return prctl(option, static_cast<unsigned long>(value), 0UL, 0UL, 0UL);
}

static bool fixture(DiagnosticChecks &checks)
{
	{
		std::unique_ptr<DatabaseInterface> db(create_database("fixture.foz", DatabaseMode::OverWrite));
		if (!db || !db->prepare()) return false;
		StateRecorder recorder;
		recorder.init_recording_synchronized(db.get());
		VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; app.apiVersion = VK_API_VERSION_1_0;
		checks.require(recorder.record_application_info(app), "fixture-application");
		static const uint32_t code[] = { 0x07230203, 0x10000, 0, 1, 0 };
		VkShaderModuleCreateInfo module = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		module.codeSize = sizeof(code); module.pCode = code;
		checks.require(recorder.record_shader_module((VkShaderModule)1, module, 0x101), "fixture-shader");
		VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		checks.require(recorder.record_pipeline_layout((VkPipelineLayout)2, layout, 0x102), "fixture-layout");
		VkComputePipelineCreateInfo pipeline = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		pipeline.layout = (VkPipelineLayout)2;
		pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		pipeline.stage.module = (VkShaderModule)1; pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		pipeline.stage.pName = "main";
		for (unsigned i = 0; i < 2; i++)
		{
			pipeline.flags = i ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0;
			checks.require(recorder.record_compute_pipeline((VkPipeline)(uintptr_t(3 + i)), pipeline, nullptr, 0, 0x104 + i), "fixture-compute");
		}
		recorder.tear_down_recording_thread();
	}
	std::unique_ptr<DatabaseInterface> db(create_database("fixture.foz", DatabaseMode::ReadOnly));
	if (!db || !db->prepare()) return false;
	for (auto tag : { RESOURCE_SHADER_MODULE, RESOURCE_PIPELINE_LAYOUT, RESOURCE_COMPUTE_PIPELINE })
	{
		size_t count = 0;
		checks.require(db->get_hash_list_for_resource_tag(tag, &count, nullptr) && count == (tag == RESOURCE_COMPUTE_PIPELINE ? 2u : 1u), "fixture-count");
		Hash hashes[2] = {};
		checks.require(db->get_hash_list_for_resource_tag(tag, &count, hashes), "fixture-hashes");
		std::sort(hashes, hashes + count);
		checks.require(hashes[0] == (tag == RESOURCE_SHADER_MODULE ? 0x101u : tag == RESOURCE_PIPELINE_LAYOUT ? 0x102u : 0x104u), "fixture-known-hash");
		if (count == 2) checks.require(hashes[1] == 0x105, "fixture-second-known-hash");
	}
	return checks.failures == 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	std::string name = argv[1];
	if (name.find("sigmask") != std::string::npos) fault = Sigmask;
	else if (name.find("stdout-dup2") != std::string::npos) fault = Stdout;
	else if (name.find("stdin-dup2") != std::string::npos) fault = Stdin;
	else if (name == "prctl-failure") fault = Prctl;
	else if (name == "parent-race") fault = ParentRace;
	else if (name != "healthy" && name != "healthy-recovery" && name != "parent-death") return 2;
	recovery = name.find("recovery") != std::string::npos;
	parent_death = name == "parent-death";
	fault_generation = (recovery || name.find("initial-last") != std::string::npos) ? 2 : 1;
	unsigned expected_births = parent_death ? 1 : 2;
	if (fault == Prctl || fault == ParentRace) expected_births = 2;
	DiagnosticChecks checks;
	checks.require(fixture(checks), "fixture-readback-complete");
	if (checks.failures) return 1;
	checks.require(prctl(PR_SET_CHILD_SUBREAPER, 1UL, 0UL, 0UL, 0UL) == 0, "observer-subreaper");
	int event_pipe[2];
	checks.require(__real_pipe(event_pipe) == 0, "event-pipe");
	if (checks.failures) return 1;
	events_fd = event_pipe[1];
	const size_t mapping_size = sizeof(SharedControlBlock) + 4096;
	void *mapping = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	checks.require(mapping != MAP_FAILED, "shared-counters");
	if (checks.failures) return 1;
	auto *control = new (mapping) SharedControlBlock{};
	control->ring_buffer_offset = sizeof(SharedControlBlock); control->ring_buffer_size = 4096;
	control->version_cookie = ControlBlockMagic;
	bool gated_fault = fault != None && !recovery;
	if (gated_fault) checks.require(__real_pipe(fault_barrier) == 0, "fault-barrier-pipe");
	if (checks.failures) return 1;
	fflush(nullptr);
	pid_t controller = __real_fork();
	checks.require(controller >= 0, "controller-fork");
	if (controller < 0) { close(event_pipe[0]); close(event_pipe[1]); munmap(mapping, mapping_size); return 1; }
	if (controller == 0)
	{
		close(event_pipe[0]);
		if (setpgid(0, 0) != 0) _exit(97);
		controller_pid = getpid();
		Global::control_block = control;
		atexit(inherited_atexit);
		// A separate inherited, fully buffered stream distinguishes _exit from
		// exit's stdio flushing. Controller always uses _exit below.
		flush_sentinel = fopen("flush-sentinel", "wb");
		if (!flush_sentinel) _exit(96);
		static char buffer[1024];
		setvbuf(flush_sentinel, buffer, _IOFBF, sizeof(buffer));
		fputs("flush\n", flush_sentinel);
		VulkanDevice::Options device; device.null_device = true;
		ThreadedReplayer::Options opts;
		opts.num_threads = (recovery || parent_death) ? 1 : 2;
		opts.disable_rate_limiter = true;
		int result = run_master_process(device, opts, { "fixture.foz" }, nullptr, 0, true, -1, -1);
		if (getpid() != controller_pid) { event(Fallthrough, 3); _exit(94); }
		for (int fd : fault_barrier) if (fd >= 0) close(fd);
		_exit(result);
	}
	close(event_pipe[1]);
	for (int fd : fault_barrier) if (fd >= 0) close(fd);
	setpgid(controller, controller); // Child also sets it, avoiding parent-death readiness races.
	fcntl(event_pipe[0], F_SETFL, fcntl(event_pipe[0], F_GETFL) | O_NONBLOCK);
	std::vector<Event> records;
	std::map<pid_t, int> observed_status;
	bool no_children = false, eof = false, killed_parent = false;
	double deadline = now() + 12.0;
	while (now() < deadline && !(no_children && eof))
	{
		Event record;
		ssize_t n;
		while ((n = __real_read(event_pipe[0], &record, sizeof(record))) > 0)
		{
			checks.require(n == sizeof(record), "atomic-event-record");
			if (n != sizeof(record)) break;
			records.push_back(record);
			if (record.kind == Ready && parent_death && !killed_parent)
			{
				checks.require(kill(controller, SIGKILL) == 0, "kill-ready-controller");
				killed_parent = true;
			}
		}
		if (n == 0) eof = true;
		int status;
		pid_t pid;
		while ((pid = __real_waitpid(-1, &status, WNOHANG)) > 0)
			checks.require(observed_status.emplace(pid, status).second, "observer-collect-once");
		no_children = pid == -1 && errno == ECHILD;
		if (!(no_children && eof)) { pollfd fd = { event_pipe[0], POLLIN, 0 }; poll(&fd, 1, 10); }
	}
	checks.require(no_children && eof, "bounded-completion-and-event-eof");
	// Always clean the complete process group, even on an assertion failure.
	if (!no_children)
	{
		kill(-controller, SIGKILL);
		for (const auto &record : records)
			if (record.kind == Birth && getpgid(record.a) == controller) kill(record.a, SIGKILL);
	}
	double cleanup_deadline = now() + 3.0;
	while (!no_children && now() < cleanup_deadline)
	{
		int status; pid_t pid = __real_waitpid(-1, &status, WNOHANG);
		if (pid > 0) observed_status.emplace(pid, status);
		else if (pid == -1 && errno == ECHILD) no_children = true;
		else poll(nullptr, 0, 10);
	}
	checks.require(no_children, "observer-echild-after-cleanup");
	close(event_pipe[0]);
	std::map<int, pid_t> births;
	std::map<pid_t, std::vector<Event>> per_pid;
	std::map<pid_t, std::vector<int>> reaps;
	std::set<int> output_eofs;
	unsigned injections = 0, forced = 0, ready = 0, releases = 0, passes = 0;
	size_t healthy_reap_index = records.size(), release_index = records.size(), pass_index = records.size(), inject_index = records.size();
	size_t record_index = 0;
	for (const auto &record : records)
	{
		per_pid[record.pid].push_back(record);
		checks.require(record.kind != Fallthrough && record.kind != ForeignFork, "no-controller-fallthrough-or-worker-fork");
		checks.require(record.kind != AtExit && record.kind != Destructor, "no-fatal-atexit-or-destructor");
		if (record.kind == Birth)
		{
			checks.require(record.pid == controller, "controller-owns-birth");
			checks.require(births.emplace(record.generation, record.a).second, "unique-generation");
		}
		if (record.kind == Reap) { checks.require(record.pid == controller, "original-controller-reaps"); reaps[record.a].push_back(record.b); }
		if (record.kind == OutputEOF) output_eofs.insert(record.a);
		if (record.kind == Inject) { inject_index = record_index; ++injections; checks.require(record.generation == int(fault_generation) && record.a == int(fault), "exact-injection-site"); }
		if (record.kind == ForcedCrash) ++forced;
		if (record.kind == Ready) ++ready;
		if (record.kind == Reap && WIFEXITED(record.b) && WEXITSTATUS(record.b) == 0)
			healthy_reap_index = record_index;
		if (record.kind == BarrierRelease)
		{
			++releases; release_index = record_index;
			checks.require(record.pid == controller && WIFEXITED(record.b) && WEXITSTATUS(record.b) == 0,
			               "barrier-original-controller-successful-reap");
			checks.require(reaps[record.a].size() == 1 && reaps[record.a][0] == record.b,
			               "barrier-reaped-known-sibling");
		}
		if (record.kind == BarrierPassed)
		{
			++passes; pass_index = record_index;
			checks.require(record.generation == int(fault_generation), "barrier-selected-generation");
		}
		++record_index;
	}
	checks.require(births.size() == expected_births, "expected-fork-count");
	checks.require(releases == (gated_fault ? 1u : 0u) && passes == releases, "exact-barrier-release-and-pass");
	if (gated_fault)
	{
		checks.require(healthy_reap_index < release_index && release_index < pass_index && pass_index < inject_index,
		               "successful-controller-reap-before-fault-release");
		for (const auto &record : records)
			if (record.kind == BarrierRelease)
				checks.require(births.count(fault_generation == 1 ? 2 : 1) &&
				               record.a == births[fault_generation == 1 ? 2 : 1], "barrier-exact-healthy-sibling");
	}
	checks.require(injections == (fault == None ? 0u : 1u), "exactly-one-intended-injection");
	checks.require(forced == (recovery ? 1u : 0u), "controlled-recovery-generation");
	checks.require(ready == (parent_death ? 1u : 0u), "parent-death-ready-barrier");
	unsigned normal_workers = 0;
	for (const auto &birth : births)
	{
		pid_t pid = birth.second;
		bool failed = fault != None && birth.first == int(fault_generation);
		bool controlled = recovery && birth.first == 1;
		int status = 0;
		if (parent_death)
		{
			checks.require(observed_status.count(pid) == 1 && reaps[pid].empty(), "orphan-collected-by-subreaper");
			if (observed_status.count(pid)) status = observed_status[pid];
			checks.require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "pdeathsig-worker-sigkill");
		}
		else
		{
			checks.require(reaps[pid].size() == 1 && observed_status.count(pid) == 0, "controller-collection-exactly-once");
			if (reaps[pid].size() == 1) status = reaps[pid][0];
			checks.require(WIFEXITED(status) && WEXITSTATUS(status) == (failed ? EXIT_FAILURE : controlled ? 2 : 0), "exact-worker-exit-status");
			checks.require(output_eofs.count(birth.first) == 1, "worker-output-pipe-eof");
		}
		unsigned masks = 0, outputs = 0, inputs = 0, input_eofs = 0, exits = 0, children = 0;
		for (const auto &record : per_pid[pid])
		{
			masks += record.kind == Mask; outputs += record.kind == Output; inputs += record.kind == Input;
			input_eofs += record.kind == InputEOF; exits += record.kind == WorkerExit; children += record.kind == Child;
			if (record.kind == Child) checks.require(record.a == controller, "worker-parent-identity");
			if (record.kind == WorkerExit) checks.require(record.a == 0, "real-worker-success");
		}
		checks.require(children == 1, "child-start-record");
		bool before_mask = failed && (fault == Prctl || fault == ParentRace || fault == Sigmask);
		checks.require(masks == (before_mask ? 0u : 1u), "setup-mask-order");
		checks.require(outputs == (before_mask || (failed && fault == Stdout) ? 0u : 1u), "setup-output-order");
		checks.require(inputs == (failed ? 0u : 1u) && input_eofs == inputs, "setup-input-order-and-protocol-eof");
		checks.require(exits == (failed || controlled || parent_death ? 0u : 1u), "fatal-does-not-enter-worker");
		if (!failed && !controlled && !parent_death) ++normal_workers;
		checks.require(kill(pid, 0) == -1 && errno == ESRCH, "recorded-worker-no-longer-exists");
	}
	checks.require(observed_status.count(controller) == 1, "controller-collected");
	if (observed_status.count(controller))
	{
		int status = observed_status[controller];
		checks.require(parent_death ? (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) :
		               (WIFEXITED(status) && WEXITSTATUS(status) == (fault == None ? EXIT_SUCCESS : EXIT_FAILURE)), "required-work-parent-status");
	}
	checks.require(observed_status.size() == (parent_death ? 2u : 1u), "no-unexpected-adopted-descendants");
	checks.require(kill(controller, 0) == -1 && errno == ESRCH, "controller-no-longer-exists");
	struct stat sentinel = {};
	checks.require(stat("flush-sentinel", &sentinel) == 0 && sentinel.st_size == off_t(6 * normal_workers), "no-fatal-inherited-stdio-flush");
	checks.require(control->static_total_count_compute.load() == 2, "production-fixture-count");
	checks.require(control->successful_compute.load() == (recovery && fault == None ? 2u : normal_workers) &&
	               control->parsed_compute.load() == (recovery && fault == None ? 2u : normal_workers), "real-null-device-fixture-coverage");
	if (!parent_death)
	{
		checks.require(control->progress_complete.load() == (fault == None ? 1u : 0u), "controller-required-work-complete");
		checks.require(control->clean_process_deaths.load() == (recovery ? 1u : 0u), "production-recovery-progress-retained");
		checks.require(control->dirty_process_deaths.load() == (fault == None ? 0u : 1u), "production-no-progress-failure-retained");
	}
	munmap(mapping, mapping_size);
	fprintf(stdout, "Executed fork-child-%s: births=%zu injections=%u normal-workers=%u failures=%d\n", argv[1], births.size(), injections, normal_workers, checks.failures);
	return checks.failures ? 1 : 0;
}
