#define FOSSILIZE_REPLAY_WARNING_NO_MAIN
#include "replay_support_dependency_test.cpp"
#ifndef _WIN32
#include <sys/wait.h>

#endif

// Count diagnostic-loop allocations without changing logging or detectors.
static std::atomic<bool> track_allocations{false};
static std::atomic<uint64_t> allocation_count{0};
void *operator new(size_t n)
{
	if(track_allocations.load(std::memory_order_relaxed)) allocation_count.fetch_add(1,std::memory_order_relaxed);
	void *p=malloc(n ? n : 1); if(!p) std::abort(); return p;
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p,size_t) noexcept { free(p); }
void operator delete[](void *p,size_t) noexcept { free(p); }
static unsigned occurrences(const std::string &s, const std::string &what)
{
	unsigned n = 0; size_t p = 0;
	while ((p = s.find(what, p)) != std::string::npos) { n++; p += what.size(); }
	return n;
}
static void bounded(const std::string &text)
{
	checks.require(text.size() <= ReplayWarnings::MaxBytes, "M19/text-output-byte-bound");
	checks.require(std::count(text.begin(), text.end(), '\n') <= ReplayWarnings::MaxLines + 1,
	               "M19/text-output-line-bound-including-one-fixed-setup-line");
	checks.require(occurrences(text, "Replay warning summary:") == 1, "M19/text-flush-once");
	for (const char *old : {"Skipping.", "is not supported by current device", "Replay/support:",
	                        "Replay/dependency:", "Did not replay blob", "Unrecognized pNext", "warning spam",
	                        "Invalid derivative", "Invalid library", "common cache"})
		checks.require(text.find(old) == std::string::npos, std::string("M19/no-legacy-bypass/") + old);
}
static void limited(Replay &replay)
{
	non_lazy = true;
	VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	VkPhysicalDeviceProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	p.properties.limits.maxPushConstantsSize = 16;
	p.properties.limits.maxBoundDescriptorSets = 4;
	replay.device->legacy_filter.init(VK_API_VERSION_1_0, nullptr, 0, &f, &p);
}
static void rejected(Replay &r, unsigned kind, uint64_t hash, unsigned cause = 0)
{
	Pipeline p(kind, hash);
	VkBaseInStructure unknown = {VkStructureType(0x60000000u + cause), nullptr};
	if (cause)
	{
		p.c.pNext = p.g.pNext = p.r.pNext = &unknown;
	}
	else p.flags(VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT);
	PipelineResolver resolver;
	r.run_creation_work_item(resolver, p.item);
}
static std::string repeated(unsigned n)
{
	DiagnosticStderr capture;
	{
		Replay replay; limited(replay);
		long before = ftell(capture.file);
		rejected(replay,1,0x10000); // warm detector/collector state
		allocation_count=0; track_allocations=true;
		for (unsigned i = 1; i < n; i++) rejected(replay, 1, 0x10000 + i);
		track_allocations=false;
		checks.require(allocation_count==0,"M19/warmed-production-warning-loop-no-diagnostic-allocation");
		checks.require(ftell(capture.file) == before, "M19/no-observation-stdio");
		auto s = replay.warnings.snapshot();
		checks.require(s.count == 1 && s.groups[0].count == n && s.examples == 2 &&
		               s.groups[0].resources[RESOURCE_COMPUTE_PIPELINE] == n,
		               "M19/same-cause-exact-resource-observations");
		checks.require(count(replay, ReplayFailure::Unsupported) == n && !calls && replay.outcome.exit_status() == 1,
		               "M19/same-cause-outcome-zero-create");
		replay.finish_events(1); replay.finish_events(1);
	}
	return capture.finish();
}
static void same_cause()
{
	auto small = repeated(7), large = repeated(100000);
	bounded(small); bounded(large);
	checks.contains(large, "100000 compute-pipeline", "M19/same-cause-human-resource-count");
	checks.contains(large, "feature descriptorBuffer", "M19/same-cause-meaningful-identifier");
	checks.contains(large, "required=1 available=0", "M19/same-cause-relevant-values");
	checks.contains(large, "compute-pipeline 0000000000010000", "M19/same-cause-representative-source");
	checks.require(large.size() < small.size() + 40, "M19/output-independent-of-input-size");
	checks.require(sizeof(ReplayWarnings) < 65536, "M19/fixed-bounded-diagnostic-storage");
}
static void grouping()
{
	DiagnosticStderr capture;
	{
		Replay r; limited(r);
		for (unsigned k = 0; k < 3; k++) for (unsigned n = 0; n < 3; n++) rejected(r, k, 0xabc);
		// Same actual property predicate with differing requested values.
		for (unsigned n = 0; n < 4; n++)
		{
			VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 + n * 4};
			VkPipelineLayoutCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			info.pushConstantRangeCount = 1; info.pPushConstantRanges = &range;
			r.get_per_thread_data().expected_tag = RESOURCE_PIPELINE_LAYOUT;
			VkPipelineLayout out;
			r.enqueue_create_pipeline_layout(0x500 + n, &info, &out);
		}
		for (unsigned k = 0; k < 3; k++)
		{
			Pipeline p(k); p.flags(0x80000000u); PipelineResolver resolver;
			r.run_creation_work_item(resolver, p.item);
		}
		for (unsigned n = 0; n < 2; n++) rejected(r, 1, 0x900 + n, n + 1);
		auto s = r.warnings.snapshot();
		checks.require(s.count == 7 && s.examples == 8 && s.groups[0].count == 9 && s.groups[1].count == 4,
		               "M19/semantic-groups-values-coalesce-generic-context-distinct");
		for (unsigned k = 0; k < 3; k++) checks.require(s.groups[0].resources[Pipeline(k).item.tag] == 3,
		               "M19/mixed-resource-counts-repeated-hash-observations");
		r.finish_events(1);
	}
	auto text = capture.finish(); bounded(text);
	checks.contains(text, "3 compute-pipeline", "M19/mixed-human-resource-counts");
	checks.contains(text, "maxPushConstantsSize", "M19/property-identifier");
	checks.contains(text, "required=32 available=16", "M19/property-values");
}
static void shader_grouping()
{
	DiagnosticStderr capture;
	{
		Replay r; limited(r);
		uint32_t code[]={0x07230203,0x10000,0,1,0,0x20011,10};
		VkShaderModuleCreateInfo info={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.pCode=code;info.codeSize=sizeof(code);
		auto emit=[&](uint64_t hash) {
			r.get_per_thread_data().expected_tag=RESOURCE_SHADER_MODULE;r.get_per_thread_data().expected_hash=hash;
			VkShaderModule module; r.enqueue_create_shader_module(hash,&info,&module);
			checks.require(!module,"M19/shader-detector-reject-before-create");
		};
		emit(0x700);code[6]=0xf000;emit(0x701);code[6]=0xf001;emit(0x702);
		code[1]=0x10600;emit(0x703);code[1]=0x10500;emit(0x704);
		std::string name="SPV_missing_"+std::string(170,'x')+"\n\t";
		std::vector<uint32_t> extension(6+(name.size()+1+3)/4);extension[0]=code[0];extension[1]=0x10000;extension[3]=1;
		extension[5]=uint32_t((extension.size()-5)<<16)|10;
		memcpy(extension.data()+6,name.c_str(),name.size()+1);info.pCode=extension.data();info.codeSize=extension.size()*4;emit(0x705);
		// Include control bytes before the truncation boundary too.
		name="SPV_missing_\n\t"+std::string(140,'z');std::fill(extension.begin()+6,extension.end(),0);memcpy(extension.data()+6,name.c_str(),name.size()+1);emit(0x706);
		auto snap=r.warnings.snapshot();
		checks.require(snap.count==6&&snap.groups[3].count==2&&snap.groups[3].key.support.kind==SupportKind::Property,
		               "M19/API-values-coalesce-capability-raw-and-extensions-distinct");
		checks.require(snap.groups[0].key.support.raw==10&&snap.groups[1].key.support.raw==0xf000&&snap.groups[2].key.support.raw==0xf001,
		               "M19/semantic-capability-identities");
		r.finish_events(1);
	}
	auto text=capture.finish();bounded(text);
	checks.contains(text,"CapabilityFloat64","M19/known-capability-human-name");
	checks.contains(text,"apiVersion","M19/API-human-boundary");
	checks.contains(text,"SPV_missing_??","M19/actual-extension-controls-sanitized");
	checks.require(text.find("Unrecognized SPIR-V")==std::string::npos&&text.find("ignoring shader module")==std::string::npos,
	               "M19/no-capability-extension-detector-bypass");
}

static void overflow()
{
	DiagnosticStderr capture;
	{
		Replay r; limited(r);
		for (unsigned n = 1; n <= 40; n++) rejected(r, 1, 0x100 + n, n);
		for (unsigned n = 0; n < 100000; n++) rejected(r, 1, 0x10000 + n, 40);
		for (unsigned n = 0; n < 9; n++) rejected(r, 1, 1, 1);
		auto s = r.warnings.snapshot();
		checks.require(s.count == 32 && s.examples == 8 && s.overflow == 100008 && s.groups[0].count == 10,
		               "M19/overflow-exact-observations-retained-groups-continue");
		uint64_t sum = s.overflow; for (unsigned i = 0; i < s.count; i++) sum += s.groups[i].count;
		checks.require(sum == 100049 && !calls, "M19/overflow-all-observations-conserved");
		uint64_t maximum = UINT64_MAX; bool saturated = false;
		ReplayWarnings::increment(maximum, saturated);
		checks.require(maximum == UINT64_MAX && saturated, "M19/uint64-explicit-saturation");
		r.finish_events(1);
	}
	auto text = capture.finish(); bounded(text);
	checks.contains(text, "100008 additional observations", "M19/overflow-human-count");
	checks.contains(text, "omitted cause cardinality unknown", "M19/overflow-honest-scope");
}
static void dependencies()
{
	for (auto mode : {"missing-module", "missing-layout", "missing-static-set", "missing-static-sampler",
	                  "missing-static-pass", "missing-parent", "missing-library", "invalid-layout"})
	{
		DiagnosticStderr capture;
		run_graph(mode, 4, std::string(mode) == "missing-module" ? 4096 : 4, (std::string(mode).find("library") != std::string::npos || std::string(mode) == "missing-static-pass") ? 2 : 1);
		auto text = capture.finish();
		// The malformed origin is unrelated parser evidence; its one line is retained.
		checks.require(text.size() < 32768 && occurrences(text, "Replay warning summary:") == 1,
		               std::string("M19/FOZ-bounded-summary/") + mode);
		checks.require(occurrences(text,"Did not replay blob") == (std::string(mode)=="invalid-layout" ? 1u : 0u) && text.find("common cache") == std::string::npos,
		               "M19/FOZ-no-parser-dependency-bypass");
	}
	DiagnosticStderr capture;
	run_graph("good-static", 4, 4, 1);
	auto text = capture.finish();
	checks.require(text.find("Replay warning summary:") == std::string::npos, "M19/good-FOZ-no-warning-report");
}
static void unsupported_sources()
{
	DiagnosticStderr capture;
	{
		graph_unsupported_capability = true;
		checks.require(graph("unsupported.foz", "bounded-origins", 1100, 1), "M19/unsupported-source-FOZ");
		graph_unsupported_capability = false;
		ThreadedReplayer::Options o; o.num_threads = 4;
		ThreadedReplayer r(device_options(),o);
		active_replay = &r; graph_mode = "bounded-origins";
		additional_dispatch = graph_dispatch; module_result = VK_SUCCESS; fail_parent_hash = 0;
		pipeline_calls = module_calls = 0; edges.clear(); non_lazy = true;
		int status = run_normal_process(r,{"unsupported.foz"},nullptr,0,-1);
		checks.require(status == 1 && count(r,ReplayFailure::Unsupported) == 70 &&
		               count(r,ReplayFailure::Dependency) == 1100 && !pipeline_calls && !module_calls,
		               "M19/known-unsupported-origin-fanout-exact-counts");
		auto snap = r.warnings.snapshot(); uint64_t known=0, unknown=0;
		for(unsigned i=0;i<snap.count;i++) if(snap.groups[i].key.dependency)
		{
			if(snap.groups[i].key.support.kind==SupportKind::None) unknown += snap.groups[i].count;
			else known += snap.groups[i].count;
		}
		checks.require(known > 0 && unknown > 0 && known + unknown == 1100,
		               "M19/bounded-origin-retention-honest-known-unknown-root-causes");
		r.finish_events(status); additional_dispatch=nullptr; non_lazy=false;
	}
	auto text=capture.finish();
	checks.contains(text,"unsupported SPIR-V capability CapabilityFloat64", "M19/known-root-support-context-readable");
	checks.contains(text,"support cause unknown", "M19/unretained-root-stays-unknown");
	checks.require(text.size()<32768 && text.find("Did not replay blob")==std::string::npos,
	               "M19/known-root-no-duplicate-detector-parser-output");
}

static void concurrency()
{
	DiagnosticStderr capture;
	{
		Replay r(4); limited(r);
		BlobDatabase db; StateReplayer parser;
		r.global_database = &db; r.global_replayer = &parser;
		struct Queued
		{
			Pipeline pipeline;
			VkBaseInStructure unknown;
			Queued(unsigned n) : pipeline(1,n+1), unknown{VkStructureType(0x60000001u+n%40),nullptr}
			{ pipeline.c.pNext = &unknown; pipeline.item.memory_context_index = n % NUM_MEMORY_CONTEXTS; }
		};
		std::vector<std::unique_ptr<Queued>> items;
		for(unsigned n=0;n<8000;n++) items.emplace_back(new Queued(n));
		r.start_worker_threads();
		std::mutex lock; std::condition_variable cv; unsigned ready = 0; bool go = false;
		std::vector<std::thread> producers;
		for (unsigned t = 0; t < 4; t++) producers.emplace_back([&,t]() {
			{ std::unique_lock<std::mutex> h(lock); ready++; cv.notify_all(); cv.wait(h,[&](){return go;}); }
			for (unsigned n = t; n < items.size(); n += 4) r.enqueue_work_item(items[n]->pipeline.item);
		});
		{ std::unique_lock<std::mutex> h(lock); cv.wait(h,[&](){return ready == 4;}); go = true; cv.notify_all(); }
		for (auto &t : producers) t.join();
		r.sync_worker_threads();
		for(unsigned c=0;c<NUM_MEMORY_CONTEXTS;c++)
			checks.require(r.completed_count[c] == r.queued_count[c] && r.completed_count[c] > 0,
			               "M19/actual-production-worker-queue-context-drain");
		Global::worker_thread_index = 0;
		auto s = r.warnings.snapshot();
		checks.require(s.count == 32 && s.examples == 8 && s.overflow == 1600 && count(r, ReplayFailure::Unsupported) == 8000,
		               "M19/concurrent-exact-overflow-and-outcome");
		for (unsigned i = 0; i < s.count; i++) checks.require(s.groups[i].count == 200, "M19/concurrent-exact-retained-cause-count");
		std::thread a([&](){r.finish_events(1);}), b([&](){r.finish_events(1);}); a.join(); b.join();
	}
	bounded(capture.finish());
}
static void lifecycle()
{
	for (unsigned scenario = 0; scenario < 4; scenario++)
	{
		DiagnosticStderr capture;
		{
			Replay r; limited(r);
			if (scenario < 2) rejected(r, 1, 0xabc);
			if (scenario == 0) { r.finish_events(1); r.finish_events(1); }
			if (scenario == 2) { Pipeline p(1); PipelineResolver resolver; r.run_creation_work_item(resolver,p.item); }
		}
		auto text = capture.finish();
		checks.require(occurrences(text,"Replay warning summary:") == (scenario < 2 ? 1u : 0u), "M19/standalone-finish-destructor-empty-success");
	}
	{
		DiagnosticStderr pending_capture;
		{
			Replay r(1); limited(r);
			std::mutex lock; std::condition_variable cv; bool ready=false, go=false;
			r.thread_pool.emplace_back([&]() {
				Global::worker_thread_index=1;
				{std::unique_lock<std::mutex> hold(lock);ready=true;cv.notify_all();cv.wait(hold,[&](){return go;});}
				rejected(r,1,0xdef);
			});
			{std::unique_lock<std::mutex> hold(lock);cv.wait(hold,[&](){return ready;});}
			std::thread finalizer([&](){r.finish_events(1);});
			{std::lock_guard<std::mutex> hold(lock);go=true;cv.notify_all();}
			finalizer.join();
			checks.require(count(r,ReplayFailure::Unsupported)==1,"M19/orderly-early-failure-joins-pending-producer");
		}
		auto text=pending_capture.finish();bounded(text);
		checks.contains(text,"1 compute-pipeline","M19/pending-producer-in-final-summary");
	}

#ifndef _WIN32
	DiagnosticStderr capture;
	pid_t pid = fork();
	if (pid == 0) { Replay r; limited(r); rejected(r,1,0xabc); _exit(7); }
	int status = 0; checks.require(pid > 0 && waitpid(pid,&status,0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7,
	                            "M19/hard-exit-native-status");
	auto text=capture.finish();
	checks.require(text.find("Replay warning summary:")==std::string::npos && text.find("FINAL")==std::string::npos, "M19/hard-exit-no-completed-summary");
#endif
}
static void legacy_controls()
{
	Replay r; limited(r);
	VkBaseInStructure unknown = {VkStructureType(0x60000001),nullptr};
	Pipeline valid(1);
	VkComputePipelineCreateInfo info = valid.c; info.pNext = &unknown;
	DiagnosticStderr capture;
	SupportDetail detail;
	checks.require(!r.device->legacy_filter.compute_pipeline_is_supported(&info,detail), "M19/detailed-unknown-reject");
	checks.require(ftell(capture.file)==0,"M19/detailed-filter-no-duplicate-log");
	checks.require(!r.device->legacy_filter.compute_pipeline_is_supported(&info),"M19/bool-decision-preserved");
	auto text = capture.finish(); checks.contains(text,"Unrecognized pNext", "M19/legacy-bool-log-preserved");
	DiagnosticStderr parser_capture;
	StateReplayer parser; BlobDatabase db;
	std::string missing = "{\"version\":6,\"computePipelines\":{\"201\":{\"flags\":0,\"basePipelineHandle\":\"0\",\"basePipelineIndex\":-1,\"layout\":\"0\",\"stage\":{\"flags\":0,\"stage\":32,\"module\":\"101\",\"name\":\"main\"}}}}";
	checks.require(!parser.parse(r,&db,missing.data(),missing.size()),"M19/non-replay-parser-reject");
	text = parser_capture.finish(); checks.contains(text,"common cache", "M19/non-replay-parser-advice-preserved");
	DiagnosticStderr malformed;
	VkShaderModuleCreateInfo shader = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; shader.codeSize=3; shader.pCode=valid_code;
	checks.require(!r.device->legacy_filter.shader_module_is_supported(&shader,detail) && detail.kind==SupportKind::Malformed,
	               "M19/malformed-stays-parse");
	checks.contains(malformed.finish(),"not aligned", "M19/unrelated-malformed-output-retained");
	{
		DiagnosticStderr generic;
		Replay clean; BlobDatabase empty;
		empty.blobs[{RESOURCE_SHADER_MODULE,0x101}]="{\"version\":6}";
		clean.global_database=&empty; StateReplayer no_callback;std::vector<uint8_t> bytes;
		checks.require(!clean.run_parse_work_item(no_callback,bytes,parse_item(0x101)),"M19/generic-parse-no-captured-reason-fails");
		checks.contains(generic.finish(),"Did not replay blob","M19/generic-parse-sole-text-preserved");
	}
	{
		DiagnosticStderr oom;
		Replay clean; Pipeline p(1); results={VK_ERROR_OUT_OF_HOST_MEMORY};p.run(clean,1);
		checks.require(count(clean,ReplayFailure::Create)==1&&!count(clean,ReplayFailure::Unsupported),"M19/OOM-remains-create");
		checks.contains(oom.finish(),"VkResult=-1","M19/OOM-native-error-visible");
	}

}
int main(int argc, char **argv)
{
	set_thread_log_level(LOG_WARNING);
	if (argc == 3 && std::string(argv[1]) == "--fixture") return graph(argv[2],"missing-module",64,1) ? 0 : 1;
	if (argc != 2) return 2;
	std::string c=argv[1];
	if(c=="same-cause")same_cause(); else if(c=="grouping"){grouping();shader_grouping();} else if(c=="overflow")overflow();
	else if(c=="dependencies"){dependencies();unsupported_sources();} else if(c=="concurrency")concurrency(); else if(c=="lifecycle")lifecycle();
	else if(c=="legacy-controls")legacy_controls(); else return 2;
	printf("M19 warning %s failures=%d\n",c.c_str(),checks.failures);
	return checks.failures ? 1 : 0;
}
