#include "diagnostic_test.hpp"
#include "../cli/replay_worker_outcome.hpp"
#include "../cli/replay_warnings.hpp"
#include <numeric>

using namespace Fossilize;

static DiagnosticChecks checks;
using Terminal = ReplayTerminal;

static ReplaySelectionReport small_selection()
{
	ReplaySelectionReport report;
	report.stable_hashes[0] = {0x11, 0x22};
	report.stable_hashes[1] = {0x11, 0x33};
	report.ranges[0].effective_begin = 5;
	report.ranges[1].effective_begin = 10;
	report.selected_total_known = true;
	return report;
}

static void permutations()
{
	auto selection = small_selection();
	const std::array<ReplayTerminalFact, 7> history = {{
		{0, 1, RESOURCE_GRAPHICS_PIPELINE, 5, 0x11, Terminal::CreateFailed},
		{0, 2, RESOURCE_GRAPHICS_PIPELINE, 5, 0x11, Terminal::Created},
		{0, 1, RESOURCE_COMPUTE_PIPELINE, 10, 0x11, Terminal::Unsupported},
		{1, 1, RESOURCE_GRAPHICS_PIPELINE, 6, 0x22, Terminal::CacheHint},
		{1, 1, RESOURCE_GRAPHICS_PIPELINE, 6, 0x22, Terminal::Created},
		{1, 1, RESOURCE_COMPUTE_PIPELINE, 11, 0x33, Terminal::Dependency},
		{1, 2, RESOURCE_COMPUTE_PIPELINE, 11, 0x33, Terminal::Dependency}
	}};
	const std::array<Terminal, 4> expected = {
		Terminal::Created, Terminal::Unsupported, Terminal::Unknown, Terminal::Dependency
	};
	std::array<unsigned, history.size()> order;
	std::iota(order.begin(), order.end(), 0u);
	unsigned permutations = 0;
	do
	{
		for (unsigned duplicates : {0u, 1u, 0x2au, 0x7fu})
		{
			ReplayReconciliation ledger;
			ledger.initialize(selection);
			for (unsigned i : order)
			{
				const auto &fact = history[i];
				auto range = ledger.assignment(fact.worker, 2);
				checks.require(ledger.accept(fact.worker, fact.attempt, range, range, fact), "R21/property-valid-fact");
				if (duplicates & (1u << i))
					checks.require(ledger.accept(fact.worker, fact.attempt, range, range, fact), "R21/property-equivalent-duplicate");
			}
			auto *first = ledger.item(RESOURCE_GRAPHICS_PIPELINE, 5, 0x11);
			auto *second = ledger.item(RESOURCE_COMPUTE_PIPELINE, 10, 0x11);
			auto *third = ledger.item(RESOURCE_GRAPHICS_PIPELINE, 6, 0x22);
			auto *fourth = ledger.item(RESOURCE_COMPUTE_PIPELINE, 11, 0x33);
			const ReplayReconciliation::Item *items[] = {first, second, third, fourth};
			for (unsigned i = 0; i < expected.size(); i++)
				checks.require(items[i] && items[i]->status() == expected[i], "R21/property-independent-item-oracle");
			checks.require(first && first->attempt[0] == 2 && first->attempt[1] == 1 && first->failure_attempt == 1 &&
			               first->failure_mask == 1u << unsigned(Terminal::CreateFailed),
			               "R21/property-prior-failure-survives-newer-creation");
			auto counts = ledger.counts();
			ReplayReconciliation::Counts oracle = {};
			for (auto status : expected)
				oracle[unsigned(status)]++;
			checks.require(counts == oracle && ledger.conflicted_items() == 1, "R21/property-final-counts-and-conflict");
		}
		permutations++;
	} while (std::next_permutation(order.begin(), order.end()));
	checks.require(permutations == 5040, "R21/property-all-permutations");
}

static std::string wire(const char *payload)
{
	char frame[ReplayEvent::MaxFrame];
	checks.require(ReplayEvent::frame(frame, sizeof(frame), payload) != 0, "R21/codec-encodes");
	return frame;
}

static std::string wire(const ReplayTerminalFact &fact)
{
	char frame[128];
	checks.require(replay_terminal_frame(frame, sizeof(frame), fact) != 0, "R21/codec-encodes-item");
	return frame;
}

static void feed(ReplayEvent::Stream &stream, ReplayWorkerOutcome &worker, const std::string &bytes)
{
	for (char byte : bytes)
		stream.feed_frames(worker.events, &byte, 1, [](const char *) {},
		                   [&](const char *p, size_t n) { worker.accept_frame(p, n); });
}

static void codec()
{
	auto selection = small_selection();
	ReplayReconciliation ledger;
	ledger.initialize(selection);
	ReplayWorkerOutcome worker;
	worker.assign(5, 6, 10, 11, 0, 0);
	worker.reconciliation = &ledger;
	worker.begin_attempt(0, worker.original);
	worker.begin_attempt(0, worker.original);
	ReplayEvent::Stream stream;

	ReplayTerminalFact valid{0, 2, RESOURCE_GRAPHICS_PIPELINE, 5, 0x11, Terminal::Created};
	feed(stream, worker, wire(valid));
	checks.require(worker.events.lost && ledger.item(valid.tag, valid.index, valid.hash)->status() == Terminal::Unknown,
	               "R21/codec-before-hello-is-untrusted");
	feed(stream, worker, wire("1 HELLO 0 2"));
	feed(stream, worker, wire(valid));

	const std::vector<std::string> malformed = {
		"FZ18 9999999999999999999999999999999:1 ITEM\n",
		"FZ18 1:1 ITEM\n",
		std::string(4096, 'x') + "\n",
		wire("1 ITEM 0 0 6 5 17 1"),
		wire("1 ITEM 0 4294967296 6 5 17 1"),
		wire("1 ITEM 0 2 6 5 18446744073709551616 1"),
		wire("1 ITEM 0 2 6 5 17 0"),
		wire("1 ITEM 0 2 6 5 17 10"),
		wire("1 ITEM 0 2 6 5 17 256"),
		wire("1 ITEM 0 2 6 5 17 1 garbage"),
		wire({1, 2, RESOURCE_COMPUTE_PIPELINE, 10, 0x11, Terminal::Created}),
		wire({0, 1, RESOURCE_COMPUTE_PIPELINE, 10, 0x11, Terminal::Created}),
		wire({0, 2, RESOURCE_SHADER_MODULE, 10, 0x11, Terminal::Created}),
		wire({0, 2, RESOURCE_COMPUTE_PIPELINE, 10, 0x33, Terminal::Created}),
		wire({0, 2, RESOURCE_COMPUTE_PIPELINE, 11, 0x33, Terminal::Created}),
		wire({0, 2, RESOURCE_GRAPHICS_PIPELINE, 4, 0x11, Terminal::Created}),
		wire({0, 2, RESOURCE_RAYTRACING_PIPELINE, 0, 0x11, Terminal::Created})
	};
	for (auto &bytes : malformed)
	{
		feed(stream, worker, bytes);
		auto counts = ledger.counts();
		checks.require(worker.events.lost && counts[unsigned(Terminal::Created)] == 1 &&
		               counts[unsigned(Terminal::Unknown)] == 3, "R21/codec-rejects-invalid-identity-and-framing");
	}
	ReplayTerminalFact later{0, 2, RESOURCE_COMPUTE_PIPELINE, 10, 0x11, Terminal::ReadFailed};
	feed(stream, worker, wire(later));
	auto truncated = wire({0, 2, RESOURCE_COMPUTE_PIPELINE, 10, 0x11, Terminal::Created});
	truncated.pop_back();
	feed(stream, worker, truncated);
	stream.finish(worker.events);
	checks.require(worker.events.lost && stream.used == 0 && stream.buffer.size() == ReplayEvent::MaxFrame &&
	               ledger.item(later.tag, later.index, later.hash)->status() == Terminal::ReadFailed,
	               "R21/codec-recovers-after-bad-frame-and-rejects-truncated-conflict");

	int resume = -1;
	Hash hash = 0;
	for (const char *bad : {" 6 11junk", " 4294967296 11", " 6 10000000000000000", " 6 ", " 6 11 1"})
		checks.require(!replay_crash_cursor(bad, resume, hash), "R21/codec-rejects-malformed-crash-marker");
	checks.require(replay_crash_cursor(" 6 11", resume, hash) && resume == 6 && hash == 0x11,
	               "R21/codec-valid-crash-marker");
}

static void stress()
{
	constexpr unsigned count = 35000;
	ReplaySelectionReport selection;
	for (unsigned k = 0; k < 3; k++)
	{
		selection.ranges[k].effective_begin = 17 + k;
		for (unsigned i = 0; i < count; i++)
			selection.stable_hashes[k].push_back(0x1000 + i);
	}
	ReplayReconciliation::Counts expected = {};
	ReplayReconciliation::Counts observed[2];
	for (bool emit : {false, true})
	{
		ReplayReconciliation ledger;
		ledger.initialize(selection);
		ReplayWarnings warnings;
		const auto capacity = ledger.storage_bytes();
		checks.require(ledger.item_capacity() == 105000 && capacity == sizeof(ReplayReconciliation) + 28 * 105000,
		               "R21/stress-documented-capacity");
		for (unsigned w = 0; w < 4; w++)
		{
			auto ranges = ledger.assignment(w, 4);
			ReplayWorkerOutcome worker;
			worker.reconciliation = &ledger;
			worker.assign(ranges[0].begin, ranges[0].end, ranges[1].begin, ranges[1].end, ranges[2].begin, ranges[2].end);
			for (unsigned attempt = 1; attempt <= 8; attempt++)
				worker.begin_attempt(w, ranges);
			checks.require(worker.previous_attempt_count == 4 && worker.omitted_attempts == 3,
			               "R21/stress-bounded-attempt-history");
			ReplayEvent::Stream stream;
			feed(stream, worker, wire(("1 HELLO " + std::to_string(w) + " 8").c_str()));
			feed(stream, worker, std::string(8192, 'x') + "\n");
			for (unsigned k = 0; k < 3; k++)
			{
				auto tag = replay_pipeline_tags[k];
				for (unsigned index = ranges[k].end; index-- > ranges[k].begin;)
				{
					unsigned offset = index - selection.ranges[k].effective_begin;
					unsigned bucket = offset % 10;
					Hash hash = 0x1000 + offset;
					auto status = bucket == 0 ? Terminal::Unknown :
					              bucket < 3 ? Terminal::Dependency : bucket < 6 ? Terminal::CacheHint : Terminal::Created;
					if (!emit)
						expected[unsigned(status)]++;
					if (status == Terminal::Unknown)
						continue;

					// Exercise the production framing on a subset of the large run.
					ReplayTerminalFact fact{w, 8, tag, index, hash, status};
					if (offset % 17 == 0)
					{
						auto encoded = wire(fact);
						feed(stream, worker, encoded);
						feed(stream, worker, encoded);
					}
					else
					{
						ledger.accept(w, 8, ranges, ranges, fact);
						ledger.accept(w, 8, ranges, ranges, fact);
					}
					for (unsigned stale : {2u, 5u, 3u, 5u, 1u, 2u})
						ledger.accept(w, stale, ranges, ranges, {w, stale, tag, index, hash, Terminal::ReadFailed});
					if (emit && status == Terminal::Dependency)
						warnings.observe(ReplayFailure::Dependency, tag, hash, {}, {});
					auto *item = ledger.item(tag, index, hash);
					checks.require(item && item->attempt[0] == 8 && item->attempt[1] == 5 &&
					               item->failure_attempt == (status == Terminal::Dependency ? 8u : 5u) &&
					               item->history_omitted && item->status() == status, "R21/stress-bounded-stale-duplicates");
				}
			}
			stream.finish(worker.events);
		}
		size_t warning_lines = 0;
		warnings.finish(0, 0, [&](const char *) { warning_lines++; });
		checks.require(emit ? warning_lines > 0 && warning_lines <= 42 : warning_lines == 0,
		               "R21/stress-ordinary-warning-sampling-control");
		observed[unsigned(emit)] = ledger.counts();
		checks.require(ledger.storage_bytes() == capacity && ledger.omitted_item_histories() == 94500,
		               "R21/stress-bad-size-and-duplicates-cannot-grow-retention");
	}
	checks.require(observed[0] == expected && observed[1] == expected,
	               "R21/stress-exact-final-oracle-independent-of-warning-sampling");
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	std::string mode = argv[1];
	if (mode == "properties")
		permutations();
	else if (mode == "codec")
		codec();
	else if (mode == "stress")
		stress();
	else
		return 2;
	printf("Executed R21/%s failures=%d\n", argv[1], checks.failures);
	return checks.failures ? 1 : 0;
}
