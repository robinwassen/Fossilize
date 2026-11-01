// Production dispatcher, creation, crash recovery and controller, with private
// link adapters for clocks/syscalls. No production injection API is installed.
#include "diagnostic_test.hpp"
#include "../cli/replay_timeout.hpp"
#include "volk.h"
#include "device.hpp"
#include <atomic>
#include <fstream>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <map>

static DiagnosticChecks checks;
static std::string scenario;
static pid_t master_pid;
static thread_local bool dispatcher_clock;
static thread_local uint64_t dispatcher_now = 100000000000ull;
static uint64_t controller_now = 100000000000ull;
static int ready_pipe[2], release_pipe[2], abort_pipe[2], abort_release[2];
struct Shared { std::atomic<unsigned> ready{0}, teardown{0}, parsed{0}, signal_order{0}, kill_order{0}, abort_requested{0}, child_pid{0}, creates{0}, completed{0}; };
static Shared *barriers;
static ReplayAttemptEvidence *parent_evidence;
static bool heartbeat_reset_parsed;
static bool block_creation, hold_teardown;
static bool direct_dispatcher;
static unsigned reset_steps;
static int reset_pipe[2], reset_ack[2];
static void install_dispatch();
class TimeoutDevice : public Fossilize::VulkanDevice
{
public:
	bool init_device(const Options &o) { bool ok = VulkanDevice::init_device(o); if (ok) install_dispatch(); return ok; }
};
static bool observe_protocol(ReplayAttemptEvidence &e, const char *cmd)
{
	bool ret = replay_parse_timeout(e, cmd);
	if (getpid() == master_pid)
	{
		parent_evidence = &e;
		if (strcmp(cmd,"HEARTBEAT") == 0) heartbeat_reset_parsed = true;
		if (e.triggers[0].present && e.request_count) barriers->parsed.store(1);
	}
	return ret;
}
template <typename Options> static int worker(unsigned, unsigned, const Options &);
template <typename Children> static void observe(const Children &);
#define replay_parse_timeout observe_protocol
#define VulkanDevice TimeoutDevice
#define FOSSILIZE_REPLAY_TEST_WORKER worker
#define FOSSILIZE_REPLAY_TEST_OBSERVE observe
#define main fossilize_replay_original_main
#include "../cli/fossilize_replay.cpp"
#undef main
#undef VulkanDevice
#undef replay_parse_timeout

static std::vector<ReplayWorkerOutcome> retained;
static PFN_vkCreateComputePipelines real_compute;
static PFN_vkGetPipelineCacheData real_cache_data;
static void byte_write(int fd) { char x = 'x'; if (::write(fd, &x, 1) != 1) _exit(88); }
static void byte_read(int fd) { char x; ssize_t n; do { n = ::read(fd, &x, 1); } while (n < 0 && errno == EINTR); if (n != 1) _exit(89); }
static VKAPI_ATTR VkResult VKAPI_CALL compute(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *p)
{
	barriers->creates.fetch_add(1);
	if (block_creation)
	{
		barriers->ready.store(1); byte_write(ready_pipe[1]);
		if (scenario == "cleanup" || scenario == "spontaneous" || scenario == "kill-failure" || scenario.find("timer-") == 0) raise(SIGABRT);
		byte_read(release_pipe[0]);
	}
	auto result = real_compute(d,c,n,i,a,p);
	if (result == VK_SUCCESS) barriers->completed.fetch_add(1);
	return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL cache_data(VkDevice d, VkPipelineCache c, size_t *n, void *p)
{
	if (hold_teardown)
	{
		barriers->teardown.store(1); byte_write(ready_pipe[1]);
		byte_read(release_pipe[0]);
	}
	return real_cache_data(d,c,n,p);
}
static void install_dispatch()
{
	real_compute = vkCreateComputePipelines; vkCreateComputePipelines = compute;
	real_cache_data = vkGetPipelineCacheData; vkGetPipelineCacheData = cache_data;
}
static bool no_progress()
{
	return scenario == "no-progress" || scenario == "escalation" || scenario == "delivery-failure" ||
	       scenario == "ordinary-no-progress" || scenario == "ordinary-delivery-failure" ||
	       scenario == "multithread" || scenario == "recovery" || scenario == "dispatcher-reset" || scenario == "dispatcher-disabled";
}
template <typename Options> static int worker(unsigned, unsigned attempt, const Options &o)
{
	barriers->child_pid.store(unsigned(getpid()));
	if (scenario == "heartbeat" || scenario == "heartbeat-reset" || scenario == "external-kill")
	{
		if (scenario != "external-kill") write_all(STDOUT_FILENO, "BEGIN_HEARTBEAT\n");
		barriers->ready.store(1); byte_write(ready_pipe[1]);
		if (scenario == "external-kill") write_all(STDOUT_FILENO,"HEARTBEAT\n");
		byte_read(release_pipe[0]);
		if (scenario == "external-kill") { ::kill(getpid(),SIGKILL); return 91; }
		if (scenario == "heartbeat-reset") { write_all(STDOUT_FILENO,"HEARTBEAT\n"); byte_read(release_pipe[0]); }
		return 0;
	}
	barriers->child_pid.store(unsigned(getpid()));
	auto opts = o;
	opts.num_threads = scenario == "multithread" || scenario == "recovery" ? 2 : 1;
	block_creation = scenario != "clean" && attempt == 1;
	hold_teardown = attempt == 1 && (scenario == "cleanup" || scenario == "escalation" || scenario == "kill-failure");
	dispatcher_clock = no_progress() && block_creation;
	opts.on_disk_pipeline_cache_path = "cache.bin";
	return run_slave_process(Global::device_options, opts, Global::databases);
}
template <typename Children> static void observe(const Children &children)
{
	checks.require(Global::active_processes == 0,"M16/controller-all-children-reaped");
	for (const auto &p : children)
	{
		checks.require(p.pid < 0 && p.crash_fd < 0 && p.timer_fd < 0 && p.watchdog_timer_fd < 0,"M16/controller-all-ipc-closed");
		retained.push_back(p.outcome);
	}
}

using TimePoint = std::chrono::steady_clock::time_point;
extern "C" TimePoint __real__ZNSt6chrono3_V212steady_clock3nowEv();
extern "C" TimePoint __wrap__ZNSt6chrono3_V212steady_clock3nowEv()
{
	if (getpid() == master_pid) return TimePoint(std::chrono::nanoseconds(controller_now));
	if (dispatcher_clock) return TimePoint(std::chrono::nanoseconds(dispatcher_now));
	return __real__ZNSt6chrono3_V212steady_clock3nowEv();
}
extern "C" int __real_pthread_cond_clockwait(pthread_cond_t *, pthread_mutex_t *, clockid_t, const timespec *);
extern "C" int __wrap_pthread_cond_clockwait(pthread_cond_t *cv, pthread_mutex_t *mutex, clockid_t clock, const timespec *time)
{
	if (!dispatcher_clock) return __real_pthread_cond_clockwait(cv,mutex,clock,time);
	if (scenario == "dispatcher-reset")
	{
		if (reset_steps++ == 0)
		{
			pthread_mutex_unlock(mutex); byte_write(reset_pipe[1]); byte_read(reset_ack[0]); pthread_mutex_lock(mutex);
			dispatcher_now += 1500000000ull;
			return 0;
		}
		dispatcher_now += 2500000000ull;
		return ETIMEDOUT;
	}
	if (barriers->ready.load() == 0) return __real_pthread_cond_clockwait(cv,mutex,clock,time);
	if (scenario == "dispatcher-disabled")
	{
		dispatcher_now += replay_seconds_ns(100);
		block_creation = false; byte_write(release_pipe[1]); dispatcher_clock = false;
		return __real_pthread_cond_clockwait(cv,mutex,clock,time);
	}
	// One delayed wake already exceeds a 2-second deadline. Counted one-second
	// wakeups cannot satisfy the M16/elapsed assertion in the signal adapter.
	dispatcher_now += 2500000000ull;
	return ETIMEDOUT;
}
extern "C" int __real_pthread_kill(pthread_t, int);
extern "C" int __wrap_pthread_kill(pthread_t thread, int signal)
{
	if (getpid() != master_pid && signal == SIGABRT && no_progress())
	{
		// Controller must parse the real child record before allowing delivery.
		if (scenario.find("ordinary-") != 0 && !direct_dispatcher)
		{
			barriers->abort_requested.store(1);
			byte_write(abort_pipe[1]); byte_read(abort_release[0]);
			barriers->signal_order.store(barriers->parsed.load() ? 1 : 2);
		}
		if (scenario == "delivery-failure" || scenario == "ordinary-delivery-failure") return EINVAL;
		if (scenario == "recovery") write_all(crash_fd, "GRAPHICS 0 0\nCOMPUTE 0 0\nRAYTRACE 0 0\n");
	}
	return __real_pthread_kill(thread,signal);
}
struct Timer { itimerspec spec = {}; epoll_event event = {}; bool injected = false; };
static std::map<int,Timer> timers;
static epoll_event old_watchdog = {};
static bool fresh_watchdog, stale_injected;
extern "C" int __real_timerfd_create(int,int);
extern "C" int __wrap_timerfd_create(int clock, int flags)
{
	if (getpid() == master_pid && scenario == "timer-create-failure" && barriers->ready.load()) { errno = EMFILE; return -1; }
	return __real_timerfd_create(clock,flags);
}
extern "C" int __real_timerfd_settime(int, int, const itimerspec *, itimerspec *);
extern "C" int __wrap_timerfd_settime(int fd, int flags, const itimerspec *spec, itimerspec *old)
{
	if (getpid() == master_pid && scenario == "timer-arm-failure" && spec->it_value.tv_sec == 3) { errno = EIO; return -1; }
	if (getpid() == master_pid) { timers[fd].spec = *spec; timers[fd].injected = false; }
	return __real_timerfd_settime(fd,flags,spec,old);
}
extern "C" int __real_epoll_ctl(int,int,int,epoll_event *);
extern "C" int __wrap_epoll_ctl(int epoll, int op, int fd, epoll_event *event)
{
	if (getpid() == master_pid)
	{
		if (scenario == "timer-register-failure" && op == EPOLL_CTL_ADD && event && (event->data.u64 >> 32) != 0 && (event->data.u32 & 0xc0000000u) == 0x80000000u) { errno = EINVAL; return -1; }
		if (op == EPOLL_CTL_ADD && event && (event->data.u32 & 0xc0000000u))
		{
			timers[fd].event = *event;
			if ((event->data.u32 & 0xc0000000u) == 0x40000000u)
			{
				if ((event->data.u64 >> 32) == 1) old_watchdog = *event;
				if ((event->data.u64 >> 32) == 2) fresh_watchdog = true;
			}
		}
		if (op == EPOLL_CTL_DEL) timers.erase(fd);
	}
	return __real_epoll_ctl(epoll,op,fd,event);
}
static int injected_read_fd = -1;
extern "C" ssize_t __real_read(int,void *,size_t);
extern "C" ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
	if (fd == injected_read_fd && size == sizeof(uint64_t)) { *static_cast<uint64_t *>(buffer) = 1; injected_read_fd = -1; return sizeof(uint64_t); }
	return __real_read(fd,buffer,size);
}
static unsigned kill_attempts;
extern "C" int __real_kill(pid_t,int);
extern "C" int __wrap_kill(pid_t pid, int signal)
{
	if (getpid() == master_pid && signal == SIGKILL)
	{
		kill_attempts++;
		bool cleanup = scenario == "cleanup" || scenario == "escalation" || scenario == "delivery-failure" || scenario == "kill-failure";
		bool heartbeat = scenario == "heartbeat" || scenario == "heartbeat-reset";
		if (parent_evidence)
		{
			auto &e = *parent_evidence;
			checks.require(!cleanup || e.triggers[2].present,"M16/cleanup-cause-before-kill");
			checks.require(!heartbeat || e.triggers[1].present,"M16/heartbeat-cause-before-kill");
			checks.require(!e.native_observed,"M16/termination-not-observed-before-syscall");
		}
		barriers->kill_order.store(1);
		if (scenario == "kill-failure" && kill_attempts == 1) { errno = EPERM; return -1; }
	}
	return __real_kill(pid,signal);
}
extern "C" int __real_epoll_wait(int,epoll_event *,int,int);
extern "C" int __wrap_epoll_wait(int epoll, epoll_event *events, int count, int timeout)
{
	static bool abort_released, reset_done, early_reset_event;
	if (barriers->abort_requested.load() && !barriers->parsed.load() && !abort_released)
	{
		// Consume every already-written protocol byte before judging ordering.
		// A broken ordering control must fail an assertion, never a watchdog.
		int pending = __real_epoll_wait(epoll,events,count,0);
		if (pending > 0) return pending;
		checks.require(false,"M16/cause-missing-before-abort-adapter");
		byte_read(abort_pipe[0]); byte_write(abort_release[1]); abort_released = true;
	}
	if (barriers->parsed.load() && !abort_released)
	{
		checks.require(parent_evidence && parent_evidence->triggers[0].elapsed_ns == 2500000000ull &&
		               parent_evidence->triggers[0].threshold_ns == 2000000000ull,"M16/dispatcher-measured-elapsed-before-abort");
		byte_read(abort_pipe[0]); byte_write(abort_release[1]); abort_released = true;
	}
	if (scenario == "recovery" && fresh_watchdog && !stale_injected)
	{
		stale_injected = true; controller_now += 10500000000ull;
		events[0] = old_watchdog; events[0].events = EPOLLIN;
		return 1;
	}
	if (scenario == "external-kill" && barriers->ready.load() && !barriers->kill_order.load())
	{
		// Release a barrier-ready child to send itself an external SIGKILL.
		byte_write(release_pipe[1]);
		barriers->kill_order.store(1);
	}
	for (auto &entry : timers)
	{
		auto &t = entry.second;
		bool cleanup = t.spec.it_value.tv_sec == 3;
		bool watchdog = t.spec.it_value.tv_sec == 10;
		bool want_cleanup = scenario == "cleanup" || scenario == "escalation" || scenario == "delivery-failure" || scenario == "kill-failure";
		bool want_watchdog = scenario == "heartbeat" || scenario == "heartbeat-reset";
		if (t.injected || !t.event.events || (!(cleanup && want_cleanup) && !(watchdog && want_watchdog))) continue;
		if (cleanup && scenario != "delivery-failure" && !barriers->teardown.load()) continue;
		if (watchdog && !barriers->ready.load()) continue;
		if (watchdog && scenario == "heartbeat-reset" && !reset_done)
		{
			// The actual parser rearms only heartbeat. A queued old timer event
			// at the new start must not kill; then expire the rearmed deadline.
			controller_now += 9999999999ull;
			write_all(release_pipe[1], "x"); // Release special child to send heartbeat below.
			reset_done = true;
			break;
		}
		if (watchdog && scenario == "heartbeat-reset" && !heartbeat_reset_parsed) continue;
		if (watchdog && scenario == "heartbeat-reset" && !early_reset_event)
		{
			early_reset_event = true;
			events[0] = t.event; events[0].events = EPOLLIN; injected_read_fd = entry.first;
			return 1;
		}
		if (watchdog && scenario == "heartbeat-reset")
			checks.require(kill_attempts == 0 && !parent_evidence->triggers[1].present,"M16/rearmed-heartbeat-rejects-queued-early-event");
		controller_now += cleanup ? 3500000000ull : 10500000000ull;
		events[0] = t.event; events[0].events = EPOLLIN;
		t.injected = true; injected_read_fd = entry.first;
		return 1;
	}
	return __real_epoll_wait(epoll,events,count,timeout);
}
static bool fixture()
{
	{
		std::unique_ptr<DatabaseInterface> db(create_database("fixture.foz",DatabaseMode::OverWrite));
		if (!db || !db->prepare()) return false;
		StateRecorder recorder; recorder.init_recording_synchronized(db.get());
		static const uint32_t code[] = {0x07230203,0x10000,0,1,0};
		VkShaderModuleCreateInfo shader = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; shader.codeSize = sizeof(code); shader.pCode = code;
		VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		if (!recorder.record_shader_module((VkShaderModule)1,shader,0x101) || !recorder.record_pipeline_layout((VkPipelineLayout)2,layout,0x102)) return false;
		for (unsigned i = 0; i < 2; i++)
		{
			VkComputePipelineCreateInfo c = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			c.layout = (VkPipelineLayout)2; c.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			c.stage.module = (VkShaderModule)1; c.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; c.stage.pName = "main";
			if (!recorder.record_compute_pipeline((VkPipeline)(uintptr_t)(3+i),c,nullptr,0,0x200+i)) return false;
		}
		recorder.tear_down_recording_thread();
	}
	std::unique_ptr<DatabaseInterface> db(create_database("fixture.foz",DatabaseMode::ReadOnly));
	size_t count = 0; if (!db || !db->prepare() || !db->get_hash_list_for_resource_tag(RESOURCE_COMPUTE_PIPELINE,&count,nullptr) || count != 2) return false;
	Hash hashes[2]; if (!db->get_hash_list_for_resource_tag(RESOURCE_COMPUTE_PIPELINE,&count,hashes)) return false;
	size_t size = 0; return hashes[0] == 0x200 && hashes[1] == 0x201 && db->read_entry(RESOURCE_COMPUTE_PIPELINE,hashes[0],&size,nullptr,0) && size;
}
static int protocol_test()
{
	Global::epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	ProcessProgress p; p.pid = fork();
	if (p.pid == 0) { byte_write(ready_pipe[1]); byte_read(release_pipe[0]); _exit(0); }
	byte_read(ready_pipe[0]);
	p.outcome.assign(0,2,0,0,0,0); p.outcome.attempts = 1; p.outcome.timeout.begin(1);
	p.parse_raw("TIMEOUT 1 2000000000");
	checks.require(!p.outcome.timeout.current.triggers[0].present,"M16/actual-parser-truncated-record-unknown");
	p.parse_raw(" 2500000000\nTERMINATE 1 0 0\n");
	checks.require(p.outcome.timeout.current.triggers[0].present,"M16/actual-parser-fragmented-extension");
	p.parse_raw((std::string(4096,'x') + "TIMEOUT 1 2000000000 3000000000\n").c_str());
	checks.require(p.parse_buffer.empty() && !p.discard_record && p.outcome.timeout.current.triggers[0].elapsed_ns == 2500000000ull,"M16/actual-parser-bounded-malformed-record");
	p.begin_heartbeat();
	checks.require(::kill(p.pid,SIGSTOP) == 0,"M16/real-child-stopped");
	int stopped_status; checks.require(waitpid(p.pid,&stopped_status,WUNTRACED) == p.pid && WIFSTOPPED(stopped_status),"M16/kernel-stop-barrier");
	p.stopped = true;
	controller_now += replay_seconds_ns(11);
	checks.require(p.timeout_expired(ReplayTimeoutOrigin::Heartbeat) && !p.outcome.timeout.current.triggers[1].present && kill_attempts == 0,"M16/stopped-worker-is-not-watchdog-killed");
	checks.require(::kill(p.pid,SIGCONT) == 0,"M16/real-child-resumed");
	p.heartbeat(); p.stopped = false;
	checks.require(!p.heartbeat_deadline.expired(controller_now),"M16/resumed-worker-heartbeat-rearms");
	p.begin_cleanup(); auto start = p.cleanup_deadline.start_ns;
	controller_now += replay_seconds_ns(2); p.parse("CRASH"); p.heartbeat();
	checks.require(p.cleanup_deadline.start_ns == start && !p.cleanup_deadline.expired(controller_now),"M16/repeated-crash-and-heartbeat-cannot-extend-cleanup");
	close_and_remove_epoll_entry(p.timer_fd); close_and_remove_epoll_entry(p.watchdog_timer_fd);
	byte_write(release_pipe[1]); int status; checks.require(waitpid(p.pid,&status,0) == p.pid && WIFEXITED(status),"M16/protocol-child-reaped");
	close(Global::epoll_fd); Global::epoll_fd = -1;
	return checks.failures ? 1 : 0;
}
static int dispatcher_reset_test()
{
	DiagnosticStderr capture;
	pid_t child = fork();
	if (child == 0)
	{
		barriers->child_pid.store(unsigned(getpid()));
		if (pipe(reset_pipe) || pipe(reset_ack)) _exit(91);
		direct_dispatcher = dispatcher_clock = true;
		VulkanDevice::Options device; device.null_device = true;
		ThreadedReplayer::Options opts; opts.num_threads = 0; opts.timeout_seconds = 2;
		ThreadedReplayer r(device,opts); install_trivial_crash_handlers(r);
		r.queued_count[0] = 2;
		std::thread producer([&]() {
			byte_read(reset_pipe[0]);
			{ std::lock_guard<std::mutex> lock(r.pipeline_work_queue_mutex); r.completed_count[0] = 1; r.work_done_condition[0].notify_all(); }
			byte_write(reset_ack[1]);
		});
		r.sync_worker_memory_context(0);
		producer.join(); _exit(92);
	}
	int status; checks.require(waitpid(child,&status,0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 1,"M16/dispatcher-reset-bounded-termination");
	auto log = capture.finish();
	checks.contains(log,"threshold_ns=2000000000 elapsed_ns=2500000000","M16/production-completion-resets-only-work-deadline");
	return checks.failures ? 1 : 0;
}
static int ordinary_test()
{
	DiagnosticStderr capture;
	pid_t child = fork();
	if (child == 0)
	{
		barriers->child_pid.store(unsigned(getpid()));
		dispatcher_clock = block_creation = true;
		char a0[] = "replay-timeout-controller-test", a1[] = "--null-device", a2[] = "--timeout-seconds", a3[] = "2",
		     a4[] = "--num-threads", a5[] = "1", a6[] = "fixture.foz";
		char *args[] = {a0,a1,a2,a3,a4,a5,a6};
		_exit(fossilize_replay_original_main(7,args));
	}
	int status; checks.require(waitpid(child,&status,0) == child && WIFEXITED(status) &&
	                          WEXITSTATUS(status) == (scenario == "ordinary-delivery-failure" ? 2 : 1), "M16/ordinary-timeout-bounded-without-controller");
	auto log = capture.finish();
	checks.contains(log,"origin=work-no-progress threshold_ns=2000000000 elapsed_ns=2500000000","M16/ordinary-cause-before-termination");
	if (scenario == "ordinary-delivery-failure") checks.contains(log,"delivery=failed native_error=22","M16/ordinary-raw-delivery-failure");
	return checks.failures ? 1 : 0;
}
static void emergency_watchdog(int)
{
	// Test-only emergency cleanup. A watchdog is a failed fixture, never
	// timeout/defect evidence; terminate and reap the real child before exit.
	if (barriers)
	{
		pid_t child = pid_t(barriers->child_pid.load());
		if (child > 0 && child != getpid())
		{
			__real_kill(child,SIGKILL);
			int status; while (waitpid(child,&status,0) < 0 && errno == EINTR) {}
		}
	}
	static const char message[] = "M16 emergency watchdog: fixture failed\n";
	(void)::write(STDERR_FILENO,message,sizeof(message)-1);
	_exit(98);
}
int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	scenario = argv[1]; master_pid = getpid();
	struct rlimit limit = {}; setrlimit(RLIMIT_CORE,&limit); signal(SIGALRM,emergency_watchdog); alarm(20);
	void *mapping = mmap(nullptr,sizeof(Shared),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
	if (mapping == MAP_FAILED) return 90;
	barriers = new (mapping) Shared;
	if (pipe(ready_pipe) || pipe(release_pipe) || pipe(abort_pipe) || pipe(abort_release)) return 90;
	if (scenario == "protocol") return protocol_test();
	if (scenario == "dispatcher-reset") return dispatcher_reset_test();
	checks.require(fixture(),"M16/known-dependency-complete-input-readback");
	if (scenario.find("ordinary-") == 0) return ordinary_test();
	VulkanDevice::Options device; device.null_device = true;
	ThreadedReplayer::Options opts; opts.num_threads = 1; opts.timeout_seconds = scenario == "dispatcher-disabled" ? 0 : 2; opts.disable_rate_limiter = true;
	int result = run_master_process(device,opts,{"fixture.foz"},nullptr,0,false,-1,-1);
	checks.require(retained.size() == 1,"M16/retained-controller-state");
	if (retained.size() == 1)
	{
		auto &r = retained[0]; auto &e = r.timeout.previous_count ? r.timeout.previous[0] : r.timeout.current;
		bool success = scenario == "clean" || scenario == "recovery" || scenario == "dispatcher-disabled";
		checks.require(result == (success ? 0 : 1),"M16/actual-final-work-status");
		checks.require(r.complete == success,"M16/required-work-effect-distinct-from-attempt");
		checks.require(e.native_observed,"M16/native-effect-independently-observed");
		if (no_progress() && scenario != "dispatcher-disabled")
		{
			checks.require(e.triggers[0].present && e.triggers[0].threshold_ns == 2000000000ull && e.triggers[0].elapsed_ns == 2500000000ull,"M16/no-progress-origin-threshold-actual-elapsed");
			checks.require(barriers->signal_order.load() == 1,"M16/parent-stored-cause-before-actual-abort");
		}
		else checks.require(!e.triggers[0].present,"M16/no-invented-no-progress");
		if (scenario == "heartbeat" || scenario == "heartbeat-reset") checks.require(e.triggers[1].present && e.triggers[1].threshold_ns == 10000000000ull && e.triggers[1].elapsed_ns == 10500000000ull,"M16/heartbeat-measured-origin");
		else checks.require(!e.triggers[1].present,"M16/no-invented-heartbeat");
		if (scenario == "cleanup" || scenario == "escalation" || scenario == "delivery-failure" || scenario == "kill-failure")
			checks.require(e.triggers[2].present && e.triggers[2].threshold_ns == 3000000000ull && e.triggers[2].elapsed_ns == 3500000000ull,"M16/cleanup-measured-origin");
		else checks.require(!e.triggers[2].present,"M16/no-invented-cleanup");
		if (scenario == "delivery-failure") checks.require(e.request_count >= 2 && e.requests[0].delivery == ReplayDelivery::Failed && e.requests[0].native_error == EINVAL && WIFSIGNALED(e.native_status) && WTERMSIG(e.native_status) == SIGKILL,"M16/failed-abort-and-kill-effect-separated");
		if (scenario.find("timer-") == 0) checks.require(r.native_error != 0 && !e.triggers[2].present,"M16/failed-supervision-bounded-without-invented-timeout");
		if (scenario == "kill-failure") checks.require(e.requests[0].delivery == ReplayDelivery::Failed && e.requests[0].native_error == EPERM && e.request_count >= 2,"M16/kill-error-preserved-through-cleanup");
		if (scenario == "multithread" || scenario == "external-kill") checks.require(!r.skipped() && r.unknown_remainder,"M16/unknown-individual-attribution");
		if (scenario == "no-progress" || scenario == "spontaneous") checks.require(r.assigned[1].skipped_hash == 0x200,"M16/independent-known-resource-evidence");
		if (success) checks.require(barriers->completed.load() == 2,"M16/clean-control-actually-created-all-inputs");
		if (scenario == "recovery") checks.require(stale_injected && kill_attempts == 0,"M16/stale-attempt-timer-cannot-kill-replacement");
		if (scenario == "recovery") checks.require(r.attempts == 2 && r.timeout.previous_count == 1 && e.recovery == ReplayRecovery::Retry && r.timeout.current.recovery == ReplayRecovery::Complete,"M16/clean-retry-retains-failed-attempt");
	}
	int status; checks.require(waitpid(-1,&status,WNOHANG) == -1 && errno == ECHILD,"M16/kernel-all-children-reaped");
	printf("Executed M16/controller-%s failures=%d result=%d creates=%u completed=%u\n",scenario.c_str(),checks.failures,result,barriers->creates.load(),barriers->completed.load());
	return checks.failures ? 1 : 0;
}
