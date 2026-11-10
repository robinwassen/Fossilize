// Real production controller, OS processes and current worker protocol. The
// adapters below exist only in this translation unit, never in fossilize-replay.
#include "diagnostic_test.hpp"
#include "volk.h"
#include "device.hpp"
#include "../cli/logging.hpp"
#undef LOGI
#undef LOGW
#undef LOGE
#define LOGI(...) discard_prose(__VA_ARGS__)
#define LOGW(...) discard_prose(__VA_ARGS__)
#define LOGE(...) discard_prose(__VA_ARGS__)
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
#include <unistd.h>
#endif
static void discard_prose(const char *, ...) {}
static std::string scenario;
static void install_event_dispatch();
class EventDevice : public Fossilize::VulkanDevice
{
public:
  Fossilize::FeatureFilter support_filter;
  Fossilize::FeatureFilter &get_feature_filter()
  {
	  return scenario == "event-support" ? support_filter : VulkanDevice::get_feature_filter();
  }
  bool init_device(const Options &o)
  {
	  if (scenario == "event-empty")
		  return false;
	  bool ret = VulkanDevice::init_device(o);
	  if (ret)
	  {
		  if (scenario == "event-support")
		  {
			  VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			  VkPhysicalDeviceProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
			  VkPhysicalDeviceDynamicRenderingFeatures dynamic = {
				  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
			  VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray = {
				  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
			  VkPhysicalDeviceMaintenance4Features maintenance = {
				  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
			  f.pNext = &dynamic;
			  dynamic.pNext = &ray;
			  ray.pNext = &maintenance;
			  dynamic.dynamicRendering = ray.rayTracingPipeline = maintenance.maintenance4 = VK_TRUE;
			  support_filter.init(VK_API_VERSION_1_3, nullptr, 0, &f, &p);
		  }
		  install_event_dispatch();
	  }
	  return ret;
  }
};
#define VulkanDevice EventDevice

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
static int event_pipe(int *);
static pid_t event_fork();
static ssize_t event_native_write(int, const void *, size_t);
#define pipe event_pipe
#define fork event_fork
#define write event_native_write
#define FOSSILIZE_REPLAY_TEST_BEFORE_HARVEST before_harvest
#define FOSSILIZE_REPLAY_TEST_WAITPID test_waitpid
#endif
#define FOSSILIZE_REPLAY_TEST_WORKER test_worker
#define FOSSILIZE_REPLAY_TEST_OBSERVE observe
#define FOSSILIZE_REPLAY_TEST_PROSE_OFF
#define main fossilize_replay_original_main
#include "../cli/fossilize_replay.cpp"
#undef main
#undef VulkanDevice
#ifndef _WIN32
#undef pipe
#undef fork
#undef write
#endif


#ifndef _WIN32
static int startup_input_read=-1,startup_input_write=-1,startup_ready=-1;
static pid_t startup_child=-1;
static bool startup_close(){return scenario=="event-startup-close"||scenario=="event-startup-hash-close";}
static int event_pipe(int *fds){int ret=::pipe(fds);if(ret==0&&startup_close()){startup_input_read=fds[0];startup_input_write=fds[1];}return ret;}
static void startup_barrier()
{
 char byte;checks.require(::read(startup_ready,&byte,1)==1,"M18/startup-reader-close-barrier");close(startup_ready);startup_ready=-1;
 siginfo_t info={};checks.require(waitid(P_PID,startup_child,&info,WEXITED|WNOWAIT)==0,"M18/startup-native-terminal-before-input-write");
}
static pid_t event_fork()
{
 if(!startup_close())return ::fork();
 int ready[2];if(::pipe(ready)!=0)return -1;
 pid_t pid=::fork();
 if(pid==0)
 {
  close(ready[0]);
  if(scenario=="event-startup-hash-close")
  {
   char line[Fossilize::ReplayEvent::MaxFrame];size_t n=0;
   while(n<sizeof(line)){if(::read(startup_input_read,line+n,1)!=1)_exit(94);if(line[n++]=='\n')break;}
   uint64_t w,a;bool recognized=false;if(!Fossilize::ReplayEvent::offer(line,n,w,a,recognized))_exit(95);
  }
  close(startup_input_read);
  if(::write(ready[1],"x",1)!=1)_exit(96);
  close(ready[1]);_exit(2);
 }
 close(ready[1]);startup_child=pid;startup_ready=ready[0];
 if(scenario=="event-startup-close")startup_barrier();
 return pid;
}
static ssize_t event_native_write(int fd,const void *bytes,size_t n)
{
 ssize_t ret=::write(fd,bytes,n);
 if(scenario=="event-startup-hash-close"&&fd==startup_input_write&&startup_ready>=0&&ret>0)startup_barrier();
 return ret;
}
static void startup_signal_controls()
{
 sigset_t before,after,blocked,pending;pthread_sigmask(SIG_SETMASK,nullptr,&before);
 struct sigaction prior={},later={};sigaction(SIGPIPE,nullptr,&prior);
 int fds[2];checks.require(::pipe(fds)==0,"M18/closed-reader-pipe");close(fds[0]);
 checks.require(send_faulty_modules_and_close(fds[1],0,1)==EPIPE,"M18/startup-raw-EPIPE");
 pthread_sigmask(SIG_SETMASK,nullptr,&after);sigaction(SIGPIPE,nullptr,&later);
 checks.require(sigismember(&before,SIGPIPE)==sigismember(&after,SIGPIPE)&&prior.sa_handler==later.sa_handler,"M18/startup-preserves-signal-mask-and-disposition");
 sigemptyset(&blocked);sigaddset(&blocked,SIGPIPE);pthread_sigmask(SIG_BLOCK,&blocked,nullptr);raise(SIGPIPE);
 checks.require(::pipe(fds)==0,"M18/pending-signal-pipe");close(fds[0]);
 checks.require(send_faulty_modules_and_close(fds[1],0,1)==EPIPE,"M18/pending-signal-EPIPE");sigpending(&pending);
 checks.require(sigismember(&pending,SIGPIPE)==1,"M18/pre-existing-pending-signal-preserved");
 timespec zero={};sigtimedwait(&blocked,nullptr,&zero);pthread_sigmask(SIG_SETMASK,&before,nullptr);
}
#endif
static PFN_vkCreateComputePipelines event_real_compute;
static VKAPI_ATTR VkResult VKAPI_CALL event_compute(VkDevice d,VkPipelineCache c,uint32_t n,const VkComputePipelineCreateInfo *i,const VkAllocationCallbacks *a,VkPipeline *p)
{if(scenario=="event-creation"){*p=VK_NULL_HANDLE;return VK_ERROR_DEVICE_LOST;}return event_real_compute(d,c,n,i,a,p);}
static void install_event_dispatch(){event_real_compute=vkCreateComputePipelines;vkCreateComputePipelines=event_compute;}
static bool event_case(){return scenario.find("event-")==0;}
static bool event_real()
{
	return event_case() &&
		   (scenario == "event-clean" || scenario == "event-empty" || scenario == "event-progress" ||
			scenario == "event-parse" || scenario == "event-initial-parse" || scenario == "event-creation" ||
			scenario == "event-support" || scenario == "event-dependency");
}
static bool event_write(const char *b)
{
#ifdef _WIN32
 return write_all(GetStdHandle(STD_OUTPUT_HANDLE),b);
#else
 return write_all(STDOUT_FILENO,b);
#endif
}
struct SyntheticReport
{
 bool enabled=true;
 SyntheticReport(unsigned w,unsigned a)
 {
  char line[Fossilize::ReplayEvent::MaxFrame];
  if(!fgets(line,sizeof(line),stdin)){enabled=false;return;}
  uint64_t worker=0,attempt=0;bool recognized=false;
  checks.require(Fossilize::ReplayEvent::offer(line,strlen(line),worker,attempt,recognized)&&worker==w&&attempt==a,"M18/synthetic-actual-offer");
  while(fgets(line,sizeof(line),stdin)){}
  if(scenario=="event-pre-report-death"||scenario=="event-drop-hello"){enabled=false;return;}
  char payload[96];snprintf(payload,sizeof(payload),"1 HELLO %u %u",w,a);Fossilize::ReplayEvent::frame(line,sizeof(line),payload);event_write(line);
 }
 ~SyntheticReport(){if(enabled){char line[Fossilize::ReplayEvent::MaxFrame];Fossilize::ReplayEvent::frame(line,sizeof(line),"1 FINAL 0 0");event_write(line);}}
};
static unsigned observed = 0;
static std::vector<ReplayWorkerOutcome> retained;
static std::string marker_path(unsigned index, unsigned attempt)
{
	return "worker-" + std::to_string(index) + "-" + std::to_string(attempt) + ".txt";
}
static bool orderly() { return scenario.find("orderly-") == 0; }
static bool mixed() { return scenario == "mixed-success-first" || scenario == "mixed-loss-first" || orderly(); }
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
	if (orderly() || event_real())
	{
		Fossilize::set_thread_log_level(Fossilize::LOG_NONE);
		// The fixed archive and actual production slave perform all parsing,
		// creation, outcome accumulation, queue draining and terminal writing.
		int result;
#ifdef _WIN32
		result = run_slave_process(Global::device_options, opts, Global::databases, nullptr, nullptr, nullptr);
#else
		result = run_slave_process(Global::device_options, opts, Global::databases);
#endif
		out << "orderly-return " << result << '\n';
		return result;
	}
 SyntheticReport synthetic(index,attempt);
 if(event_case())
 {
  synthetic.enabled=false;
  if(scenario=="event-pre-report-death")return 2;
  using namespace Fossilize::ReplayEvent;
  char final_frame[MaxFrame];
  if(scenario=="event-missing-final")return 0;
  if(scenario=="event-abnormal-final"){frame(final_frame,sizeof(final_frame),"1 FINAL 0 0");event_write(final_frame);return 2;}
  Event e;e.phase=Create;e.reason=CreationFailed;e.known=Raw|HasTag|HasHash|Worker|Attempt;e.domain=Vulkan;e.raw=VK_ERROR_DEVICE_LOST;e.tag=RESOURCE_COMPUTE_PIPELINE;e.hash=0x302;e.worker=index;e.attempt=attempt;
  char b[MaxFrame];event_frame(b,sizeof(b),e);event_write(b);
  if(scenario=="event-missing-final-origin"||scenario=="event-drop-hello")return 0;
  if(scenario.find("event-truncated")==0){frame(b,sizeof(b),"1 FINAL 0 0");size_t cut=scenario=="event-truncated-prefix"?4:scenario=="event-truncated-length"?8:scenario=="event-truncated-payload"?14:scenario=="event-truncated-newline"?strlen(b)-1:strlen(b)-3;b[cut]=0;event_write(b);return 0;}
  if(scenario=="event-unknown"){e.reason=999;event_frame(b,sizeof(b),e);event_write(b);}
  if(scenario=="event-unknown-version"){frame(b,sizeof(b),"2 EVENT 7");event_write(b);}
  if(scenario=="event-oversized")event_write((std::string(4096,'x')+"\n").c_str());
  frame(b,sizeof(b),"1 FINAL 0 0");event_write(b);
  if(scenario=="event-duplicate-final")event_write(b);
  return scenario=="event-abnormal-final"?2:0;
 }

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
	unsigned first = orderly() ? (scenario.find("success-first") != std::string::npos ? 0 : 1) : scenario == "mixed-success-first" ? 1 : 0;
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
	if (orderly() || event_real()) for (auto path : Global::databases) command += " \"" + std::string(path) + "\"";
}
template <typename Handles, typename Children> static DWORD test_wait(const Handles &handles, const Children &children)
{
	if (scenario == "status-error") { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
	if (mixed())
	{
		for (const auto &child : children)
			if (child.process) checks.require(WaitForSingleObject(child.process, 5000) == WAIT_OBJECT_0,
			                                 "mixed/both-workers-terminal");
		unsigned first = orderly() ? (scenario.find("success-first") != std::string::npos ? 0 : 1) : scenario == "mixed-success-first" ? 1 : 0;
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
		if (scenario == "event-support")
			c.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
		if (!recorder.record_compute_pipeline((VkPipeline)(uintptr_t)(30 + i), c, nullptr, 0, 0x300 + i)) return false;
		VkRayTracingPipelineCreateInfoKHR r = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
		stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR; r.stageCount = 1; r.pStages = &stage; r.layout = (VkPipelineLayout)2;
		if (!recorder.record_raytracing_pipeline((VkPipeline)(uintptr_t)(50 + i), r, nullptr, 0, 0x400 + i)) return false;
	}
	recorder.tear_down_recording_thread();
	return true;
}
static bool empty_fixture(const char *path)
{
	{
		std::unique_ptr<DatabaseInterface> db(create_database(path, DatabaseMode::OverWrite));
		if (!db || !db->prepare()) return false;
	}
	std::unique_ptr<DatabaseInterface> db(create_database(path, DatabaseMode::ReadOnly));
	if (!db || !db->prepare()) return false;
	for (unsigned tag = 0; tag < RESOURCE_COUNT; tag++)
	{
		size_t count = 1;
		if (!db->get_hash_list_for_resource_tag(ResourceTag(tag), &count, nullptr) || count != 0) return false;
	}
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
 if(event_case())
 {
  bool success=scenario=="event-clean"||scenario=="event-empty"||scenario=="event-progress";
  checks.require(result==(success?0:1)&&retained.size()==1,"M18/controller-native-contract");
  if(retained.size()!=1)return;
  const auto &o=retained[0];const auto &e=o.events;
  checks.require(o.attempts==1&&e.offered&&e.eof&&o.timeout.current.native_observed,"M18/controller-attempt-native-and-eof");
  checks.require(o.complete==success,"M18/missing-final-never-success");
  for(const auto &range:o.assigned)checks.require(range.begin==(scenario=="event-empty"?0u:2u)&&range.end==(scenario=="event-empty"?0u:6u),"M18/controller-known-original-assigned-range");
  if(success)checks.require(e.hello&&e.final&&e.successful()&&!e.count,"M18/clean-requires-valid-final");
#ifndef _WIN32
  else if(startup_close())checks.require(o.native_error==EPIPE&&e.lost&&!e.hello&&!e.count&&!e.final&&o.unknown_remainder&&!o.skipped(),"M18/closed-reader-parent-survives-retains-native-error-and-unknown-origin");
#endif
  else if (scenario == "event-support" || scenario == "event-dependency")
  {
	  using namespace Fossilize::ReplayEvent;
	  bool support = scenario == "event-support";
	  checks.require(e.hello && e.final && e.failed && e.categories_known && o.controlled_failure && !o.failed_attempts,
					 "M19/controller-actual-detector-native-final-attempt");
	  checks.require(e.count == (support ? 4u : 12u) && !e.omitted && !e.lost && !e.unsupported,
					 "M19/controller-exact-bounded-detail-count");
	  checks.require(e.categories[unsigned(ReplayFailure::Create)] == 0 &&
						 e.categories[unsigned(ReplayFailure::Unsupported)] == (support ? 4u : 0u) &&
						 e.categories[unsigned(ReplayFailure::Dependency)] == (support ? 0u : 12u) &&
						 e.categories[unsigned(ReplayFailure::Read)] == (support ? 0u : 1u),
					 "M19/controller-origin-blocked-counts-without-driver-faults");
	  for (unsigned i = 0; i < e.count; i++)
	  {
		  const auto &v = e.events[i];
		  checks.require(v.worker == 0 && v.attempt == 1 && !(v.known & Raw),
						 "M19/controller-worker-attempt-no-invented-Vulkan-result");
		  if (support)
			  checks.require(v.phase == Support && v.reason == Unsupported &&
								 v.support.kind == Fossilize::SupportKind::Feature &&
								 !strcmp(v.support.identifier, "descriptorBuffer") && v.support.known == 3 &&
								 v.support.required == 1 && v.support.available == 0 &&
								 v.tag == RESOURCE_COMPUTE_PIPELINE && v.hash >= 0x302 && v.hash < 0x306,
							 "M19/controller-support-origin-fields-prose-disabled");
		  else
			  checks.require(v.phase == Dependency && v.reason == Blocked && v.parent.tag == RESOURCE_SHADER_MODULE &&
								 v.parent.hash == 0x101 &&
								 v.parent.status == Fossilize::ReplayDependency::Status::Missing &&
								 v.parent.category == uint64_t(ReplayFailure::Read) + 1,
							 "M19/controller-dependency-source-identity-prose-disabled");
	  }
  }
  else if(scenario=="event-parse"||scenario=="event-initial-parse"||scenario=="event-creation")
  {
   checks.require(e.hello&&e.final&&e.failed&&e.count&&o.controlled_failure&&!o.failed_attempts,"M18/controlled-failure-no-crash-inflation");
   bool found=false;for(unsigned i=0;i<e.count;i++)
   {
    const auto &v=e.events[i];
    if(scenario=="event-creation")found|=v.phase==Fossilize::ReplayEvent::Create&&v.reason==Fossilize::ReplayEvent::CreationFailed&&v.raw==VK_ERROR_DEVICE_LOST&&v.domain==Fossilize::ReplayEvent::Vulkan&&v.tag==RESOURCE_COMPUTE_PIPELINE&&v.hash>=0x302&&v.hash<0x306;
    else found|=v.phase==Fossilize::ReplayEvent::Parse&&v.reason==Fossilize::ReplayEvent::InvalidJSON&&v.domain==Fossilize::ReplayEvent::RapidJSON&&v.raw==rapidjson::kParseErrorObjectMissName&&v.offset==1&&v.hash==(scenario=="event-initial-parse"?0x102u:0x302u);
    checks.require((v.known&(Fossilize::ReplayEvent::Worker|Fossilize::ReplayEvent::Attempt))==(Fossilize::ReplayEvent::Worker|Fossilize::ReplayEvent::Attempt)&&v.worker==0&&v.attempt==1,"M18/controller-worker-reported-identity");
   }
   checks.require(found,"M18/controller-origin-field-preservation");
  }
  else
  {
   checks.require(!o.complete&&o.unknown_remainder,"M18/uncertain-transport-never-success");
   if(scenario=="event-pre-report-death")checks.require(!e.hello&&!e.count&&!e.final&&e.lost&&!o.skipped(),"M18/pre-report-death-unknown-root-and-resource");
   else if(scenario=="event-abnormal-final")checks.require(e.final&&e.successful()&&!e.count&&o.termination!=0,"M18/final-cannot-override-native-failure");
   else checks.require(e.lost||e.unsupported,"M18/protocol-loss-retained");
   if(scenario=="event-missing-final")checks.require(e.hello&&!e.final&&!e.count&&e.lost,"M18/successful-work-without-final-remains-unknown");
   if(scenario=="event-unknown-version")checks.require(e.unsupported&&e.count==1&&e.events[0].reason==Fossilize::ReplayEvent::CreationFailed,"M18/unknown-wire-version-preserves-earlier-origin");
   if(scenario=="event-missing-final-origin"||scenario.find("event-truncated")==0)checks.require(e.count==1&&e.events[0].raw==VK_ERROR_DEVICE_LOST&&!e.final,"M18/origin-survives-final-loss");
  }
  return;
 }

	if (orderly())
	{
		checks.require(result == 1 && observed == 2, "orderly/direct-status-and-both-workers-retained");
		for (unsigned i = 0; i < 2; i++)
		{
			const auto &r = retained[i];
			checks.require(r.attempts == 1 && r.failed_attempts == 0, "orderly/no-crash-or-retry");
			checks.require(r.complete == (i == 0) && r.controlled_failure == (i == 1) && !r.unknown_remainder,
			               "orderly/queue-drained-distinct-from-required-success");
			checks.require(r.failure_mask == (i == 1 ? 1u << unsigned(ReplayFailure::Parse) : 0u), "orderly/identical-logical-reason-mask");
			checks.contains(read_marker(i,1), std::string("orderly-return ") + (i == 0 ? "0" : "1"), "orderly/siblings-reached-production-return");
		}
		return;
	}
	bool empty_archive = scenario == "empty-archive";
	bool success = scenario == "clean" || scenario == "zero" || empty_archive || scenario == "recover";
	checks.require(result == (success ? 0 : 1), "controller/direct-return-status");
	checks.require(observed == workers, "controller/retained-workers");
	for (unsigned i = 0; i < retained.size(); i++)
	{
		const auto &r = retained[i];
		unsigned begin = empty_archive ? 0 : zero ? 2 : 2 + i * 4 / workers;
		unsigned end = empty_archive ? 0 : zero ? 2 : 2 + (i + 1) * 4 / workers;
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
	static_assert(ControlBlockMagic == 0x19bcde1d, "base version cookie");
	static_assert(offsetof(SharedControlBlock, successful_modules) == 8, "base progress offset");
	static_assert(offsetof(SharedControlBlock, process_reserved_memory_mib) == 140, "base process stats offset");
	static_assert(offsetof(SharedControlBlock, ring_buffer_size) == 3240, "base ring size offset");
	static_assert(sizeof(SharedControlBlock) == 3244, "base shared layout");
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
 const char *uuid="000102030405060708090a0b0c0d0e0f";
 std::string uuid_record=std::string("MODULE_UUID ")+uuid;process.parse(uuid_record.c_str());checks.require(strcmp(process.module_uuid_path,uuid)==0,"M18/legacy-module-uuid-golden");
 process.parse("MODULE abc");char module_message[ControlBlockMessageSize]={};checks.require(Global::faulty_spirv_modules.count(0xabc)==1&&shared_control_block_read(&memory.control,module_message,sizeof(module_message))&&strcmp(module_message,"MODULE abc")==0,"M18/legacy-module-golden-and-ring-forward");
	// Category terminal records use the authoritative pipe, not the lossy ring.
	for (unsigned n = 0; n < 1000; n++) process.parse("COMPUTE_VERR abc");
	process.parse("REPLAY_FAILED 2 1");
	checks.require(process.outcome.orderly_reported && process.outcome.failure_mask == 2 && process.outcome.orderly_drained,
	               "protocol/full-ring-does-not-erase-terminal-outcome");
	ProcessProgress truncated;
	truncated.parse("REPLAY_FAILED 2");
	checks.require(!truncated.outcome.orderly_reported, "protocol/truncated-terminal-is-not-orderly-proof");
	truncated.parse("REPLAY_FAILED 0 1");
	checks.require(!truncated.outcome.orderly_reported, "protocol/empty-failure-summary-rejected");
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
	if (argc >= 12 && std::string(argv[1]) == "--test-worker")
	{
		scenario = argv[2];
		hold_name = argv[11];
		Global::device_options.null_device = true;
		for (int i = 12; i < argc; i++) Global::databases.push_back(argv[i]);
		ThreadedReplayer::Options opts;
		opts.start_graphics_index = unsigned(atoi(argv[5])); opts.end_graphics_index = unsigned(atoi(argv[6]));
		opts.start_compute_index = unsigned(atoi(argv[7])); opts.end_compute_index = unsigned(atoi(argv[8]));
		opts.start_raytracing_index = unsigned(atoi(argv[9])); opts.end_raytracing_index = unsigned(atoi(argv[10]));
		return test_worker(unsigned(atoi(argv[3])), unsigned(atoi(argv[4])), opts);
	}
#endif
	if (argc == 2 && std::string(argv[1]) == "--assertions-enabled")
	{
#ifdef NDEBUG
		return 1;
#else
		return 0;
#endif
	}
	if (argc == 3 && std::string(argv[1]) == "--empty") return empty_fixture(argv[2]) ? 0 : 1;
	if (argc == 3 && std::string(argv[1]) == "--malformed") return malformed_fixture(argv[2]) ? 0 : 1;
	if (argc == 3 && std::string(argv[1]) == "--fixture") return fixture(argv[2]) ? 0 : 1;
	if (argc < 4 || std::string(argv[1]) != "--controller") return 2;
	scenario = argv[2];
 if(event_case())
 {
  checks.require(scenario=="event-empty"?empty_fixture(argv[3]):fixture("event-good.foz"),"M18/dependency-complete-fixture-created");
  if(scenario!="event-empty")
  {
   std::unique_ptr<DatabaseInterface> input(create_database("event-good.foz",DatabaseMode::ReadOnly));
   std::unique_ptr<DatabaseInterface> output(create_database(argv[3],DatabaseMode::OverWrite));
   checks.require(input&&output&&input->prepare()&&output->prepare(),"M18/fixture-copy-open");
   for (unsigned t = 0; t < RESOURCE_COUNT; t++)
   {
	   size_t n = 0;
	   input->get_hash_list_for_resource_tag(ResourceTag(t), &n, nullptr);
	   std::vector<Hash> hashes(n);
	   if (n)
		   input->get_hash_list_for_resource_tag(ResourceTag(t), &n, hashes.data());
	   for (auto h : hashes)
	   {
		   if (scenario == "event-dependency" && t == RESOURCE_SHADER_MODULE)
			   continue;
		   size_t z = 0;
		   input->read_entry(ResourceTag(t), h, &z, nullptr, 0);
		   std::vector<char> b(z);
		   input->read_entry(ResourceTag(t), h, &z, b.data(), 0);
		   bool bad = (scenario == "event-parse" && t == RESOURCE_COMPUTE_PIPELINE && h == 0x302) ||
					  (scenario == "event-initial-parse" && t == RESOURCE_PIPELINE_LAYOUT && h == 0x102);
		   checks.require(output->write_entry(ResourceTag(t), h, bad ? "{" : b.data(), bad ? 1 : z, 0),
						  "M18/fixture-copy-with-controlled-input");
	   }
   }
  }
  std::unique_ptr<DatabaseInterface> db(create_database(argv[3],DatabaseMode::ReadOnly));checks.require(db&&db->prepare(),"M18/fixture-readback-open");
  if(db)for(unsigned t=0;t<RESOURCE_COUNT;t++){size_t n=0;checks.require(db->get_hash_list_for_resource_tag(ResourceTag(t),&n,nullptr),"M18/fixture-hash-discovery");std::vector<Hash> hashes(n);if(n)checks.require(db->get_hash_list_for_resource_tag(ResourceTag(t),&n,hashes.data()),"M18/fixture-hash-readback");for(auto h:hashes){size_t z=0;checks.require(db->read_entry(ResourceTag(t),h,&z,nullptr,0)&&z,"M18/fixture-payload-size");std::vector<char>b(z);checks.require(db->read_entry(ResourceTag(t),h,&z,b.data(),0),"M18/fixture-payload-readback");}}
 }
	if (scenario == "outcome-state") return state_and_protocol();
#ifndef _WIN32
 if(startup_close()){startup_signal_controls();if(scenario=="event-startup-hash-close")Global::faulty_spirv_modules.insert(0xabc);}
#endif
	VulkanDevice::Options device_opts;
	device_opts.null_device = true;
	ThreadedReplayer::Options opts;
	opts.num_threads = mixed() || scenario == "held-sibling" ? 2 : 1;

#ifndef _WIN32
	opts.disable_rate_limiter = true;
#endif
	opts.start_graphics_index = opts.start_compute_index = opts.start_raytracing_index = 2;
	opts.end_graphics_index = opts.end_compute_index = opts.end_raytracing_index = scenario == "zero" ? 2 : 6;
	if (orderly() || scenario=="event-empty")
	{
		opts.start_graphics_index = opts.start_compute_index = opts.start_raytracing_index = 0;
		opts.end_graphics_index = opts.end_compute_index = opts.end_raytracing_index = ~0u;
	}
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
	bool use_progress = scenario.find("progress") != std::string::npos || scenario == "lost-progress" || scenario == "held-sibling" ||
	                    scenario == "skip-graphics-progress" || scenario == "skip-graphics-final-progress";
#ifndef _WIN32
	ProgressMemory *shared_memory = nullptr;
	if (orderly() && use_progress)
	{
		void *mapping = mmap(nullptr, sizeof(ProgressMemory), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		checks.require(mapping != MAP_FAILED,"orderly/shared-progress-allocation");
		if (mapping == MAP_FAILED) return 99;
		shared_memory = new (mapping) ProgressMemory{};
	}
	auto &optional_progress = shared_memory ? shared_memory->control : memory.control;
#else
	auto &optional_progress = memory.control;
#endif
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
	if (orderly() && use_progress)
	{
		checks.require(optional_progress.clean_process_deaths.load() == 0 && optional_progress.dirty_process_deaths.load() == 0,
		               "orderly/no-false-crash-counters");
		checks.require(optional_progress.successful_compute.load() == 2 && optional_progress.parsed_compute_failures.load() == 1,
		               "orderly/every-valid-sibling-and-rejected-item-accounted");
	}
	Global::control_block = nullptr;
#ifndef _WIN32
	if (shared_memory) munmap(shared_memory,sizeof(ProgressMemory));
#endif
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
	return checks.failures ? 99 : event_case() ? 0 : result;
}
