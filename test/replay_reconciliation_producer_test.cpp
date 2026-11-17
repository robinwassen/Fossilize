// Reuse the null-device dispatch and cache fixtures. Replay, discovery and
// terminal reporting are the production standalone paths in this executable.
#define FOSSILIZE_OUTCOME_NO_MAIN
#include "replay_outcome_test.cpp"
#include <fstream>

using Terminal = ReplayTerminal;

static void expect_result(const ThreadedReplayer &replay,
                          std::initializer_list<std::pair<Terminal, uint64_t>> counts, const std::string &id)
{
	ReplayReconciliation::Counts expected = {};
	for (auto entry : counts)
		expected[unsigned(entry.first)] = entry.second;
	checks.require(replay.reconciliation.counts() == expected, id);
}

static void outcome_matrix()
{
	struct Case
	{
		const char *input;
		unsigned loops;
		std::vector<VkResult> results;
		bool validation, setup_failure;
		Terminal first, second;
		unsigned calls;
		int exit_status;
	};
	const Case cases[] = {
		{"good", 2, {}, false, false, Terminal::Created, Terminal::Created, 4, 0},
		{"good", 0, {}, false, false, Terminal::NoExecution, Terminal::NoExecution, 0, 1},
		{"good", 2, {VK_SUCCESS, VK_ERROR_DEVICE_LOST, VK_SUCCESS, VK_SUCCESS}, false, false,
		 Terminal::CreateFailed, Terminal::Created, 4, 1},
		{"good", 1, {}, true, false, Terminal::ValidationFailed, Terminal::ValidationFailed, 2, 1},
		{"good", 1, {}, false, true, Terminal::Unknown, Terminal::Unknown, 0, 1},
		{"unsupported", 1, {}, false, false, Terminal::Unsupported, Terminal::Created, 1, 1},
		{"missing", 1, {}, false, false, Terminal::Dependency, Terminal::Dependency, 0, 1},
		{"mixed", 1, {}, false, false, Terminal::Created, Terminal::Created, 2, 1},
	};
	unsigned row = 0;
	for (const auto &test : cases)
	{
		std::string path = "case-" + std::to_string(row++) + ".foz";
		checks.require(fixture(test.input, path.c_str()), "R21/producer/recorded-outcome-fixture");
		auto opts = Replay::options(1, nullptr);
		opts.loop_count = test.loops;
		ThreadedReplayer replay(device_options(), opts);
		marker_mode = true;
		calls = 0;
		results = test.results;
		validation_target = &replay;
		inject_validation = test.validation;
		fail_device = test.setup_failure;
		int status = run_normal_process(replay, {path.c_str()}, nullptr, 0, -1);
		inject_validation = fail_device = marker_mode = false;

		bool bad_parse = std::string(test.input) == "mixed";
		ReplayReconciliation::Counts expected = {};
		expected[unsigned(test.first)]++;
		expected[unsigned(test.second)]++;
		expected[unsigned(Terminal::ParseFailed)] += bad_parse;
		checks.require(status == test.exit_status && calls == test.calls && replay.reconciliation.selected_total_known &&
		               replay.reconciliation.counts() == expected, "R21/producer/exact-outcome-and-create-call-count/" + path);
		auto *first = replay.reconciliation.item(RESOURCE_COMPUTE_PIPELINE, 0, 0x201);
		auto *second = replay.reconciliation.item(RESOURCE_COMPUTE_PIPELINE, 1, 0x202);
		checks.require(first && first->status() == test.first && second && second->status() == test.second,
		               "R21/producer/per-item-outcome/" + path);
		if (bad_parse)
		{
			auto *item = replay.reconciliation.item(RESOURCE_COMPUTE_PIPELINE, 2, 0x203);
			checks.require(item && item->status() == Terminal::ParseFailed, "R21/producer/actual-parser-rejection");
		}
	}
}

static void hints()
{
	std::string input = "hint.foz", cache = "markers", base = policy_base(cache);
	clear_cache(base);
	cache_input(input, 1);
	run_cache_archive(input, cache, 1, VK_SUCCESS, 1);
	checks.require(harvest_cache(base, RESOURCE_COMPUTE_PIPELINE, cache_hash(1)) == 1, "R21/producer/compatible-v1-marker");
	auto opts = Replay::options(1, nullptr);
	opts.replayer_cache_path = cache;
	ThreadedReplayer replay(device_options(), opts);
	marker_mode = true;
	calls = 0;
	results.clear();
	int status = run_normal_process(replay, {input.c_str()}, nullptr, 0, -1);
	marker_mode = false;
	checks.require(status == 0 && calls == 0 && replay.work_drained, "R21/producer/compatible-marker-actually-skips");
	expect_result(replay, {{Terminal::CacheHint, 1}}, "R21/producer/hint-is-its-own-terminal-class");
}

static void read_failure()
{
	checks.require(fixture("good", "read-source.foz"), "R21/producer/read-failure-source");
	std::unique_ptr<DatabaseInterface> source(create_database("read-source.foz", DatabaseMode::ReadOnly));
	checks.require(source->prepare(), "R21/producer/read-source-prepares");
	{
		std::unique_ptr<DatabaseInterface> dest(create_database("read.foz", DatabaseMode::OverWrite));
		checks.require(dest->prepare(), "R21/producer/read-destination-prepares");
		for (const auto &entry : source->get_preparation_report().discovered)
		{
			size_t size = 0;
			if (!source->read_entry(entry.tag, entry.hash, &size, nullptr, 0))
				return checks.require(false, "R21/producer/read-source-size");
			std::vector<char> data(size);
			checks.require(source->read_entry(entry.tag, entry.hash, &size, data.data(), 0) &&
			               dest->write_entry(entry.tag, entry.hash, data.data(), size, PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT),
			               "R21/producer/write-checksummed-payload");
		}
	}
	std::fstream file("read.foz", std::ios::binary | std::ios::in | std::ios::out);
	const std::string bytes((std::istreambuf_iterator<char>(file)), {});
	char key[41];
	snprintf(key, sizeof(key), "%024x%016x", unsigned(RESOURCE_COMPUTE_PIPELINE), 0x201);
	auto header = bytes.find(key);
	checks.require(header != std::string::npos && header + 56 < bytes.size(), "R21/producer/checksummed-pipeline-entry");
	if (header == std::string::npos || header + 56 >= bytes.size())
		return;
	file.clear();
	file.seekp(std::streamoff(header + 56));
	file.put(char(bytes[header + 56] ^ 1));
	file.close();

	ThreadedReplayer replay(device_options(), Replay::options(1, nullptr));
	marker_mode = true;
	calls = 0;
	results.clear();
	int status = run_normal_process(replay, {"read.foz"}, nullptr, 0, -1);
	marker_mode = false;
	checks.require(status == EXIT_FAILURE && calls == 1 && replay.reconciliation.discovery_complete,
	               "R21/producer/checksum-error-with-complete-discovery");
	expect_result(replay, {{Terminal::ReadFailed, 1}, {Terminal::Created, 1}}, "R21/producer/payload-read-failure-is-terminal");
}

static void discovery()
{
	checks.require(fixture("good", "complete.foz") && fixture("empty", "empty.foz"), "R21/discovery/source-fixtures");
	std::remove("absent.foz");
	std::ifstream input("complete.foz", std::ios::binary);
	std::ofstream prefix("prefix.foz", std::ios::binary);
	prefix << input.rdbuf();
	prefix.put('x');
	prefix.close();

	for (unsigned row = 0; row < 6; row++)
	{
		auto opts = Replay::options(1, nullptr);
		std::vector<const char *> databases;
		if (row == 0)
			databases = {"empty.foz"};
		else if (row == 1 || row == 5)
			databases = {"complete.foz", "absent.foz"};
		else if (row == 2)
			databases = {"absent.foz"};
		else
			databases = {"prefix.foz"};
		opts.accept_recovered_prefix = row == 4;
		if (row == 5)
			opts.start_compute_index = 1;
		ThreadedReplayer replay(device_options(), opts);
		marker_mode = true;
		results.clear();
		calls = 0;
		int status = run_normal_process(replay, databases, nullptr, 0, -1);
		marker_mode = false;

		bool succeeded = row == 0 || row == 4;
		checks.require(status == (succeeded ? 0 : 1) && replay.reconciliation.selected_total_known == succeeded &&
		               replay.reconciliation.discovery_complete == (row == 0) &&
		               calls == (row == 4 ? 2u : 0u), "R21/discovery/separate-known-scope-and-input-discovery");
		if (row == 4)
			expect_result(replay, {{Terminal::Created, 2}}, "R21/discovery/accepted-prefix-known-selected-scope");
		else if (row == 1 || row == 3)
			expect_result(replay, {{Terminal::Unknown, 2}}, "R21/discovery/known-retained-work-with-unknown-extent");
		else
			expect_result(replay, {}, "R21/discovery/empty-or-indeterminate-range");
	}
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	std::string mode = argv[1];
	if (mode == "outcomes")
		outcome_matrix();
	else if (mode == "hints")
		hints();
	else if (mode == "read")
		read_failure();
	else if (mode == "discovery")
		discovery();
	else
		return 2;
	printf("Executed R21/producer-%s failures=%d\n", argv[1], checks.failures);
	return checks.failures ? 1 : 0;
}
