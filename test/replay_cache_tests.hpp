// Included only by the private production-replay fixture. All checks are active
// in Release. Files belong to the test's isolated CTest working directory.
static const ResourceTag cache_tags[] = {RESOURCE_GRAPHICS_PIPELINE, RESOURCE_COMPUTE_PIPELINE, RESOURCE_RAYTRACING_PIPELINE};
static std::string cache_uuid()
{
	Replay r;
	std::string uuid;
	const char hex[] = "0123456789abcdef";
	for (auto byte : r.device->get_gpu_properties().pipelineCacheUUID)
	{ uuid += hex[byte & 15]; uuid += hex[byte >> 4]; }
	return uuid;
}
static std::string policy_base(const std::string &path, bool null_device = true)
{ return path + ".replay-v1." + (null_device ? "null." : "vulkan.") + cache_uuid(); }
static bool cache_file_exists(const std::string &path)
{ FILE *f = fopen(path.c_str(),"rb"); if (!f) return false; fclose(f); return true; }
static std::string cache_bytes(const std::string &path)
{
	FILE *f = fopen(path.c_str(),"rb"); checks.require(f != nullptr,"M15/files/read-existing");
	std::string bytes; char buffer[1024]; size_t n;
	if (f) { while ((n = fread(buffer,1,sizeof(buffer),f))) bytes.append(buffer,n); fclose(f); }
	return bytes;
}
static void clear_cache(const std::string &base)
{
	remove((base + ".foz").c_str());
	for (unsigned n = 1; n < 256; n++) remove((base + "." + std::to_string(n) + ".foz").c_str());
}
static void seed_cache(const std::string &path, ResourceTag tag, Hash hash)
{
	std::unique_ptr<DatabaseInterface> db(create_database(path.c_str(),DatabaseMode::OverWrite));
	checks.require(db && db->prepare() && db->write_entry(tag,hash,nullptr,0,0),"M15/seed/write-real-archive");
}
static unsigned cache_entries(const std::string &path, ResourceTag tag, Hash hash, bool required = false)
{
	if (!cache_file_exists(path)) { checks.require(!required,"M15/readback/required-archive"); return 0; }
	std::unique_ptr<DatabaseInterface> db(create_database(path.c_str(),DatabaseMode::ReadOnly));
	if (!db || !db->prepare()) { checks.require(false,"M15/readback/open-archive"); return 0; }
	unsigned entries = 0;
	for (unsigned t = 0; t < RESOURCE_COUNT; t++)
	{
		size_t n = 0; checks.require(db->get_hash_list_for_resource_tag(ResourceTag(t),&n,nullptr),"M15/readback/enumerate-count");
		std::vector<Hash> hashes(n); checks.require(db->get_hash_list_for_resource_tag(ResourceTag(t),&n,hashes.data()),"M15/readback/enumerate-hashes");
		for (auto h : hashes)
		{
			checks.require(t == unsigned(tag) && h == hash,"M15/readback/exact-marker-identity");
			size_t size = 99; checks.require(db->read_entry(ResourceTag(t),h,&size,nullptr,0) && size == 0,"M15/readback/zero-payload-marker");
			entries++;
		}
	}
	return entries;
}
static unsigned harvest_cache(const std::string &base, ResourceTag tag, Hash hash)
{
	// Append lookup primes only base.foz. Harvest real fragments before reruns,
	// including failed runs; otherwise a rerun could pass without testing hints.
	std::vector<std::string> sources;
	for (unsigned n = 1; n < 256; n++)
	{
		auto path = base + "." + std::to_string(n) + ".foz";
		if (cache_file_exists(path)) sources.push_back(path);
	}
	std::vector<const char *> pointers;
	for (auto &s : sources) pointers.push_back(s.c_str());
	checks.require(merge_concurrent_databases((base + ".foz").c_str(),pointers.data(),pointers.size()),"M15/harvest/merge-fragments-into-read-base");
	for (auto &s : sources) remove(s.c_str());
	return cache_entries(base + ".foz",tag,hash,true);
}
static Hash cache_hash(unsigned kind) { return 0xc01 + kind; }
static void cache_input(const std::string &path, unsigned kind)
{
	remove(path.c_str());
	{
		std::unique_ptr<DatabaseInterface> db(create_database(path.c_str(),DatabaseMode::OverWrite));
		checks.require(db && db->prepare(),"M15/input/open");
		StateRecorder recorder;
		// Links are unrelated to selection; keep this fixture's exact dependency
		// closure small. The recorder's synthesized application context is checked.
		recorder.set_database_enable_application_feature_links(false);
		recorder.init_recording_synchronized(db.get());
		VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize = sizeof(valid_code); sm.pCode = valid_code;
		checks.require(recorder.record_shader_module((VkShaderModule)1,sm,0x101),"M15/input/record-shader");
		VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		checks.require(recorder.record_pipeline_layout((VkPipelineLayout)2,layout_info,0x102),"M15/input/record-layout");
		Pipeline p(kind,cache_hash(kind)); p.stage.module = (VkShaderModule)1; p.c.stage.module = (VkShaderModule)1;
		p.g.layout = p.c.layout = p.r.layout = (VkPipelineLayout)2;
		if (kind == 0) checks.require(recorder.record_graphics_pipeline((VkPipeline)3,p.g,nullptr,0,cache_hash(kind)),"M15/input/record-graphics");
		if (kind == 1) checks.require(recorder.record_compute_pipeline((VkPipeline)3,p.c,nullptr,0,cache_hash(kind)),"M15/input/record-compute");
		if (kind == 2) checks.require(recorder.record_raytracing_pipeline((VkPipeline)3,p.r,nullptr,0,cache_hash(kind)),"M15/input/record-raytracing");
		recorder.tear_down_recording_thread();
	}
	std::unique_ptr<DatabaseInterface> db(create_database(path.c_str(),DatabaseMode::ReadOnly));
	checks.require(db && db->prepare(),"M15/input/reopen");
	for (unsigned t = 0; t < RESOURCE_COUNT; t++)
	{
		size_t n = 0; checks.require(db->get_hash_list_for_resource_tag(ResourceTag(t),&n,nullptr),"M15/input/count");
		bool expected = t == RESOURCE_APPLICATION_INFO || t == RESOURCE_SHADER_MODULE || t == RESOURCE_PIPELINE_LAYOUT || t == unsigned(cache_tags[kind]);
		checks.require(n == (expected ? 1u : 0u),"M15/input/exact-resource-count");
		if (expected)
		{
			Hash h = 0; checks.require(db->get_hash_list_for_resource_tag(ResourceTag(t),&n,&h),"M15/input/hash");
			Hash expected_hash = t == RESOURCE_APPLICATION_INFO ? Hashing::compute_combined_application_feature_hash({}) :
			                     t == RESOURCE_SHADER_MODULE ? 0x101u : t == RESOURCE_PIPELINE_LAYOUT ? 0x102u : cache_hash(kind);
			checks.require(h == expected_hash,"M15/input/exact-known-hash");
			size_t size = 0; checks.require(db->read_entry(ResourceTag(t),h,&size,nullptr,0) && size > 0,"M15/input/payload-size");
			std::vector<uint8_t> bytes(size); checks.require(db->read_entry(ResourceTag(t),h,&size,bytes.data(),0),"M15/input/payload");
			if (t == RESOURCE_APPLICATION_INFO)
				checks.require(std::string(bytes.begin(),bytes.end()) == "{\"version\":6,\"applicationInfo\":{},\"physicalDeviceFeatures\":{}}",
				               "M15/input/exact-synthesized-null-application-and-features");
		}
	}
}
static unsigned run_cache_archive(const std::string &input, const std::string &cache, unsigned kind,
                                 VkResult result, unsigned expected_calls, bool physical = false,
                                 const std::string &whitelist = "", bool expected_whitelist = false)
{
	ThreadedReplayer::Options opts; opts.num_threads = 1; opts.replayer_cache_path = cache;
	opts.on_disk_validation_whitelist_path = whitelist;
	auto dev_opts = device_options(); dev_opts.null_device = !physical; physical_adapter = physical;
	Global::worker_thread_index = 0;
	ThreadedReplayer replay(dev_opts,opts); validation_target = &replay;
	results = {result}; calls = destroys = 0; marker_mode = true; observed_pipeline_flags.clear(); inject_validation = false;
	int status = run_normal_process(replay,{input.c_str()},nullptr,0,-1);
	checks.require(status == (result == VK_SUCCESS || !expected_calls ? 0 : 1),"M15/integration/exact-status");
	checks.require(calls == expected_calls,"M15/integration/exact-pipeline-attempts");
	checks.require(replay.work_drained,"M15/integration/queue-drained");
	checks.require(replay.device_opts.null_device == !physical && observed_null_device == !physical,"M15/provenance/production-device-mode");
	if (expected_calls && result != VK_SUCCESS) expect(replay,ReplayFailure::Create,cache_tags[kind],cache_hash(kind),result);
	checks.require(replay.has_resource_in_whitelist(cache_tags[kind],cache_hash(kind)) == expected_whitelist,"M15/whitelist/independent-prior-evidence");
	marker_mode = physical_adapter = false;
	return unsigned(status);
}
static void cache_creation_rerun()
{
	for (unsigned kind = 0; kind < 3; kind++)
	{
		auto name = "rerun-" + std::to_string(kind); auto base = policy_base(name); clear_cache(base);
		auto input = name + ".foz"; cache_input(input,kind);
		run_cache_archive(input,name,kind,VK_ERROR_DEVICE_LOST,1);
		checks.require(harvest_cache(base,cache_tags[kind],cache_hash(kind)) == 0,"M15/failed-creation/no-marker-after-teardown");
		run_cache_archive(input,name,kind,VK_ERROR_DEVICE_LOST,1);
		checks.require(harvest_cache(base,cache_tags[kind],cache_hash(kind)) == 0,"M15/failed-creation/same-cache-second-attempt-no-marker");
		run_cache_archive(input,name,kind,VK_SUCCESS,1);
		checks.require(harvest_cache(base,cache_tags[kind],cache_hash(kind)) == 1,"M15/success/one-real-marker");
		run_cache_archive(input,name,kind,VK_SUCCESS,0);
		checks.require(harvest_cache(base,cache_tags[kind],cache_hash(kind)) == 1,"M15/success/harvested-hint-actually-suppresses");
	}
}
static void cache_final_policy(bool loops, bool compile_required)
{
	for (unsigned kind = 0; kind < 3; kind++) for (unsigned scenario = 0; scenario < (compile_required ? 4u : loops ? 4u : 7u); scenario++)
	{
		SharedControlBlock shared = {}; Replay replay(0,&shared); Pipeline p(kind); PipelineResolver resolver;
		auto *cache = new BlobDatabase; cache->writable = true; replay.replayer_cache_db.reset(cache);
		auto *white = new BlobDatabase; white->writable = true; replay.validation_whitelist_db.reset(white);
		bool success = false; unsigned expected_calls = 1, successes = 0, primary = 0, required = 0;
		if (compile_required)
		{
			p.flags(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
			results = scenario == 1 ? std::vector<VkResult>{VK_PIPELINE_COMPILE_REQUIRED,VK_ERROR_DEVICE_LOST} :
			          scenario == 2 ? std::vector<VkResult>{VK_PIPELINE_COMPILE_REQUIRED} :
			          scenario == 3 ? std::vector<VkResult>{VK_PIPELINE_COMPILE_REQUIRED,VK_SUCCESS,VK_SUCCESS} :
			                          std::vector<VkResult>{VK_PIPELINE_COMPILE_REQUIRED,VK_SUCCESS};
			replay.opts.skip_pipelines_on_compile_required = scenario == 2; replay.loop_count = scenario == 3 ? 2 : 1;
			success = scenario == 0 || scenario == 3; expected_calls = unsigned(results.size()); required = 1; successes = success ? replay.loop_count : 0; primary = success;
		}
		else if (loops)
		{
			replay.loop_count = scenario == 3 ? 0 : 2;
			results = scenario == 1 ? std::vector<VkResult>{VK_ERROR_DEVICE_LOST,VK_SUCCESS} :
			          scenario == 2 ? std::vector<VkResult>{VK_SUCCESS,VK_ERROR_DEVICE_LOST} : std::vector<VkResult>{VK_SUCCESS,VK_SUCCESS};
			success = scenario == 0; expected_calls = replay.loop_count; successes = scenario == 3 ? 0 : scenario == 0 ? 2 : 1; primary = scenario == 0 || scenario == 2;
		}
		else
		{
			results = {scenario == 1 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS}; inject_validation = scenario == 2;
			if (scenario == 3) p.flags(0x80000000u);
			if (scenario == 4) p.item.create_info.graphics_create_info = nullptr;
			if (scenario == 5)
			{
				p.stage.module = VK_NULL_HANDLE; p.c.stage.module = VK_NULL_HANDLE;
				if (kind == 0) replay.enqueue_pipeline(p.item.hash,&p.g,&p.output,0,0);
				if (kind == 1) replay.enqueue_pipeline(p.item.hash,&p.c,&p.output,0,0);
				if (kind == 2) replay.enqueue_pipeline(p.item.hash,&p.r,&p.output,0,0);
				p.item = replay.pipeline_work_queue.front(); replay.pipeline_work_queue.pop();
			}
			if (scenario == 6 && kind != 1) p.flags(VK_PIPELINE_CREATE_LIBRARY_BIT_KHR);
			success = scenario == 0 || scenario == 6; expected_calls = scenario >= 3 && scenario <= 5 ? 0 : 1;
			successes = expected_calls && scenario != 1; primary = successes;
		}
		marker_mode = true; observed_pipeline_flags.clear(); replay.run_creation_work_item(resolver,p.item); marker_mode = false; inject_validation = false;
		checks.require(calls == expected_calls,"M15/final/exact-Vulkan-attempts");
		checks.require(cache->writes == unsigned(success) && white->writes == unsigned(success),"M15/final/exact-one-marker-and-independent-whitelist");
		checks.require(cache->has_entry(p.item.tag,p.item.hash) == success,"M15/final/exact-item-marker");
		bool zero = loops && scenario == 3;
		checks.require(replay.outcome.succeeded() == (success || zero),"M15/final/sticky-item-outcome-zero-loop-keeps-existing-status");
		unsigned completed = kind == 0 ? replay.graphics_pipeline_count.load() : kind == 1 ? replay.compute_pipeline_count.load() : replay.raytracing_pipeline_count.load();
		unsigned probes = kind == 0 ? replay.graphics_pipeline_compile_required_count.load() : kind == 1 ? replay.compute_pipeline_compile_required_count.load() : replay.raytracing_pipeline_compile_required_count.load();
		unsigned progress = kind == 0 ? shared.successful_graphics.load() : kind == 1 ? shared.successful_compute.load() : shared.successful_raytracing.load();
		checks.require(completed == successes && probes == required && progress == primary,"M15/final/iteration-probe-and-primary-progress-counts");
		bool retained = !loops && !compile_required && scenario == 6 && kind != 1;
		checks.require(destroys == successes - unsigned(retained) && bool(p.output) == retained && p.output == p.entry,"M15/final/handle-retention-and-destruction");
		if (retained) { destroy(replay.device->get_device(),p.entry,nullptr); checks.require(destroys == successes,"M15/final/library-owner-cleanup"); }
		if (compile_required)
		{
			checks.require(!observed_pipeline_flags.empty() && (observed_pipeline_flags.front() & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT),"M15/fallback/probe-flag");
			for (size_t n = 1; n < observed_pipeline_flags.size(); n++) checks.require(!(observed_pipeline_flags[n] & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT),"M15/fallback/allowed-fallback-removes-flag");
			checks.require(scenario != 2 || calls == 1,"M15/fallback/skip-never-falls-back");
		}
	}
	if (!loops && !compile_required) for (bool failed_first : {false,true})
	{
		Replay replay; PipelineResolver resolver; auto *cache = new BlobDatabase; cache->writable = true; replay.replayer_cache_db.reset(cache);
		for (unsigned n = 0; n < 2; n++)
		{
			bool failed = bool(n == 0) == failed_first; results = {failed ? VK_ERROR_DEVICE_LOST : VK_SUCCESS}; calls = 0;
			Pipeline p(1,0xd00 + n); replay.run_creation_work_item(resolver,p.item);
			checks.require(cache->has_entry(p.item.tag,p.item.hash) == !failed,"M15/final/per-item-reset-both-orders");
		}
		checks.require(cache->writes == 1 && !replay.outcome.succeeded(),"M15/final/aggregate-failure-and-distinct-success-marker");
	}
}
struct RejectedMarker : BlobDatabase
{
	unsigned attempts = 0;
	bool write_entry(ResourceTag,Hash,const void *,size_t,PayloadWriteFlags) override { attempts++; return false; }
};
static void cache_write_failure()
{
	for (unsigned kind = 0; kind < 3; kind++)
	{
		Replay replay; Pipeline p(kind); PipelineResolver resolver; auto *cache = new RejectedMarker; replay.replayer_cache_db.reset(cache);
		DiagnosticStderr capture; replay.run_creation_work_item(resolver,p.item); auto text = capture.finish();
		checks.contains(text,"Replay/marker write rejected: tag=" + std::to_string(unsigned(p.item.tag)),"M15/write/rejected-operation-and-tag");
		char hash[64]; snprintf(hash,sizeof(hash),"hash=0x%016" PRIx64,p.item.hash); checks.contains(text,hash,"M15/write/rejected-hash");
		checks.require(cache->attempts == 1 && cache->blobs.empty() && replay.outcome.succeeded() && calls == 1 && destroys == 1,"M15/write/optional-rejection-retains-Vulkan-success-and-cleanup");
	}
	std::string input = "write-input.foz", path = "absent-parent/markers"; cache_input(input,1);
	for (unsigned n = 0; n < 2; n++)
	{
		DiagnosticStderr capture; run_cache_archive(input,path,1,VK_SUCCESS,1); auto text = capture.finish();
		checks.contains(text,"Replay/marker write rejected: tag=7, hash=0x0000000000000c02","M15/write/filesystem-rejection-visible-on-fresh-rerun");
		checks.require(!cache_file_exists(policy_base(path) + ".foz") && !cache_file_exists(policy_base(path) + ".1.foz"),"M15/write/filesystem-no-false-marker");
	}
	// Accepting real-file control and selection control live in the same runner.
	cache_creation_rerun();
}
static void cache_legacy_provenance()
{
	std::string path = "legacy", old = path + "." + cache_uuid(), base = policy_base(path), input = "legacy-input.foz";
	clear_cache(old); clear_cache(base); cache_input(input,1);
	seed_cache(old + ".foz",cache_tags[1],cache_hash(1)); seed_cache(old + ".1.foz",cache_tags[1],cache_hash(1));
	std::string unknown = path + ".replay-v99.null." + cache_uuid() + ".foz"; seed_cache(unknown,cache_tags[1],cache_hash(1));
	auto before_base = cache_bytes(old + ".foz"), before_fragment = cache_bytes(old + ".1.foz"), before_unknown = cache_bytes(unknown);
	run_cache_archive(input,path,1,VK_ERROR_DEVICE_LOST,1);
	checks.require(harvest_cache(base,cache_tags[1],cache_hash(1)) == 0,"M15/legacy/no-promotion-or-suppression");
	run_cache_archive(input,path,1,VK_SUCCESS,1);
	checks.require(harvest_cache(base,cache_tags[1],cache_hash(1)) == 1,"M15/legacy/new-policy-control");
	run_cache_archive(input,path,1,VK_SUCCESS,0);
	checks.require(cache_bytes(old + ".foz") == before_base && cache_bytes(old + ".1.foz") == before_fragment && cache_bytes(unknown) == before_unknown,"M15/legacy/names-and-bytes-untouched-after-new-writes");
}
static void cache_null_provenance()
{
	std::string path = "modes", null_base = policy_base(path), physical_base = policy_base(path,false), input = "modes-input.foz";
	checks.require(null_base != physical_base,"M15/provenance/distinct-mode-namespace-with-same-UUID");
	clear_cache(null_base); clear_cache(physical_base); cache_input(input,1);
	run_cache_archive(input,path,1,VK_SUCCESS,1);
	checks.require(harvest_cache(null_base,cache_tags[1],cache_hash(1)) == 1,"M15/provenance/seed-null-success");
	run_cache_archive(input,path,1,VK_ERROR_DEVICE_LOST,1,true);
	checks.require(harvest_cache(physical_base,cache_tags[1],cache_hash(1)) == 0,"M15/provenance/null-hint-cannot-certify-Vulkan-mode");
	run_cache_archive(input,path,1,VK_SUCCESS,1,true);
	checks.require(harvest_cache(physical_base,cache_tags[1],cache_hash(1)) == 1,"M15/provenance/Vulkan-mode-CPU-control");
	run_cache_archive(input,path,1,VK_SUCCESS,0,true);
}
static void cache_whitelist()
{
	std::string path = "whitelist-markers", base = policy_base(path), white = "validation-only", input = "whitelist-input.foz";
	clear_cache(base); clear_cache(white); cache_input(input,1); seed_cache(white + ".foz",cache_tags[1],cache_hash(1));
	checks.require(cache_entries(white + ".foz",cache_tags[1],cache_hash(1),true) == 1,"M15/whitelist/readback-prior-entry");
	run_cache_archive(input,path,1,VK_ERROR_DEVICE_LOST,1,false,white,true);
	checks.require(harvest_cache(base,cache_tags[1],cache_hash(1)) == 0,"M15/whitelist/not-successful-creation-evidence");
	Replay replay; auto *db = new BlobDatabase; replay.validation_whitelist_db.reset(db);
	db->blobs[{cache_tags[1],cache_hash(1)}] = "";
	checks.require(replay.has_resource_in_whitelist(cache_tags[1],cache_hash(1)),"M15/whitelist/explicit-prior-meaning-preserved");
	replay.implicit_whitelist[cache_tags[0]].insert(cache_hash(0));
	checks.require(replay.has_resource_in_whitelist(cache_tags[0],cache_hash(0)),"M15/whitelist/implicit-prior-meaning-preserved");
	Pipeline p(0,cache_hash(0)); PipelineResolver resolver; auto *cache = new BlobDatabase; cache->writable = true; replay.replayer_cache_db.reset(cache);
	results = {VK_ERROR_DEVICE_LOST}; replay.run_creation_work_item(resolver,p.item);
	checks.require(calls == 1 && cache->writes == 0,"M15/whitelist/implicit-entry-still-attempts-and-cannot-mark-failure");
}
static void cache_legacy_invocation()
{
	std::string path = "cli-markers", base = policy_base(path), input = "cli-input.foz", whitelist = "cli-whitelist";
	clear_cache(base); clear_cache(whitelist); cache_input(input,1);
	marker_mode = true; results = {VK_SUCCESS}; calls = destroys = 0;
	std::vector<char *> arguments;
	for (const char *arg : {"private-replay", "--null-device", "--num-threads", "1", "--replayer-cache", path.c_str(),
	                       "--on-disk-validation-whitelist", whitelist.c_str(), input.c_str()}) arguments.push_back(const_cast<char *>(arg));
	int status = fossilize_replay_original_main(int(arguments.size()),arguments.data()); marker_mode = false;
	checks.require(status == 0 && calls == 1,"M15/compatibility/preserved-CLI-options-and-successful-creation");
	checks.require(harvest_cache(base,cache_tags[1],cache_hash(1)) == 1,"M15/compatibility/CLI-writes-current-policy-hint");
	std::unique_ptr<DatabaseInterface> white(create_database((whitelist + ".1.foz").c_str(),DatabaseMode::ReadOnly));
	checks.require(white && white->prepare() && white->has_entry(cache_tags[1],cache_hash(1)) &&
	               white->has_entry(RESOURCE_SHADER_MODULE,0x101),"M15/compatibility/validation-option-remains-independent");
}
static void cache_test(const std::string &test)
{
	if (test == "creation-rerun") cache_creation_rerun();
	else if (test == "success-policy") cache_final_policy(false,false);
	else if (test == "loop") cache_final_policy(true,false);
	else if (test == "compile-required") cache_final_policy(false,true);
	else if (test == "write-failure") cache_write_failure();
	else if (test == "legacy-provenance") cache_legacy_provenance();
	else if (test == "null-provenance") cache_null_provenance();
	else if (test == "whitelist") cache_whitelist();
	else if (test == "legacy-invocation") cache_legacy_invocation();
	else checks.require(false,"M15/unknown-test");
}
