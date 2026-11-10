#pragma once
#include "replay_item_outcome.hpp"
#include "replay_support.hpp"
#include "replay_dependency.hpp"
#include <cstring>
#include <cstdio>

// Private attempt-local diagnostics. All retained storage is fixed; observations
// neither allocate nor print. Operational object caches are separate.
class ReplayWarnings
{
public:
	static constexpr unsigned MaxGroups = 32, MaxExamples = 8, MaxPerGroup = 2, MaxOrigins = 64;
	static constexpr unsigned MaxLines = 42, MaxBytes = 32768;
	struct Key
	{
		Fossilize::SupportDetail support;
		uint64_t status = 0, category = 0;
		unsigned context = 0;
		bool dependency = false, escaped_fallback = false;
	};
	struct Example
	{
		Fossilize::ResourceTag tag = Fossilize::RESOURCE_COUNT;
		uint64_t hash = 0;
		Fossilize::ReplayDependency::Detail parent;
		Fossilize::SupportDetail support;
	};
	struct Group
	{
		Key key;
		std::array<uint64_t, Fossilize::RESOURCE_COUNT + 1> resources = {};
		uint64_t count = 0;
		unsigned examples = 0;
		bool saturated = false;
	};
	struct Snapshot
	{
		std::array<Group, MaxGroups> groups;
		unsigned count = 0, examples = 0;
		uint64_t overflow = 0;
		bool saturated = false;
	};
	static const char *resource(unsigned tag)
	{
		using namespace Fossilize;
		switch (tag)
		{
		case RESOURCE_SAMPLER: return "sampler";
		case RESOURCE_DESCRIPTOR_SET_LAYOUT: return "descriptor-set-layout";
		case RESOURCE_PIPELINE_LAYOUT: return "pipeline-layout";
		case RESOURCE_SHADER_MODULE: return "shader-module";
		case RESOURCE_RENDER_PASS: return "render-pass";
		case RESOURCE_COMPUTE_PIPELINE: return "compute-pipeline";
		case RESOURCE_GRAPHICS_PIPELINE: return "graphics-pipeline";
		case RESOURCE_RAYTRACING_PIPELINE: return "raytracing-pipeline";
		default: return "resource";
		}
	}
	static void increment(uint64_t &value, bool &saturated)
	{
		if (value == UINT64_MAX) saturated = true;
		else value++;
	}
	static Key key(Fossilize::ResourceTag tag, const Fossilize::SupportDetail &support)
	{
		Key k;
		k.support = support;
		k.support.identifier[sizeof(k.support.identifier) - 1] = '\0';
		for (auto c : k.support.identifier)
			if (c && (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126)) k.escaped_fallback = true;
		if (k.escaped_fallback)
		{
			char input[160]; memcpy(input, k.support.identifier, sizeof(input));
			Fossilize::copy_support_identifier(k.support.identifier, input);
		}
		k.support.required = k.support.available = 0;
		// Numeric sType/capability/enum identity is a cause; varying limits aren't.
		k.support.known &= Fossilize::SupportDetail::Raw;
		if (!(k.support.known & Fossilize::SupportDetail::Raw)) k.support.raw = 0;
		if (support.kind == Fossilize::SupportKind::Constraint) k.context = unsigned(tag) + 1;
		return k;
	}
	static bool equal(const Key &a, const Key &b)
	{
		return a.dependency == b.dependency && a.escaped_fallback == b.escaped_fallback && a.status == b.status && a.category == b.category &&
		       a.context == b.context && a.support.kind == b.support.kind &&
		       a.support.known == b.support.known && a.support.raw == b.support.raw &&
		       !strcmp(a.support.identifier, b.support.identifier);
	}
	void observe(ReplayFailure reason, Fossilize::ResourceTag tag, uint64_t hash,
	             const Fossilize::SupportDetail &support, const Fossilize::ReplayDependency::Detail &parent)
	{
		using namespace Fossilize;
		std::lock_guard<std::mutex> hold(mutex);
		if (finished) return; // finalization is only legal after producers drain.
		SupportDetail example_support = support;
		Key k = key(tag, support);
		if (reason == ReplayFailure::Dependency)
		{
			k.dependency = true; k.status = uint64_t(parent.status); k.category = parent.category;
			for (unsigned i = 0; i < origin_count; i++)
				if (origins[i].tag == parent.tag && origins[i].hash == parent.hash)
				{
					k.support = origins[i].key.support; k.context = origins[i].key.context;
					k.escaped_fallback = origins[i].key.escaped_fallback;
					example_support = origins[i].support;
					break;
				}
		}
		// Retain bounded source context only; lack of a retained cause stays unknown.
		if ((reason == ReplayFailure::Unsupported || reason == ReplayFailure::Dependency) && hash)
		{
			bool found = false;
			for (unsigned i = 0; i < origin_count; i++)
				if (origins[i].tag == unsigned(tag) && origins[i].hash == hash) { found = true; break; }
			if (!found && origin_count < MaxOrigins) origins[origin_count++] = {unsigned(tag), hash, k, example_support};
		}
		unsigned index = 0;
		for (; index < state.count; index++) if (equal(state.groups[index].key, k)) break;
		if (index == state.count)
		{
			if (state.count == MaxGroups) { increment(state.overflow, state.saturated); return; }
			state.groups[state.count++].key = k;
		}
		auto &g = state.groups[index];
		increment(g.count, g.saturated);
		increment(g.resources[tag < RESOURCE_COUNT ? unsigned(tag) : unsigned(RESOURCE_COUNT)], g.saturated);
		if (g.examples < MaxPerGroup && state.examples < MaxExamples)
		{
			examples[state.examples] = {tag, hash, parent, example_support};
			example_groups[state.examples++] = index;
			g.examples++;
		}
	}
	Snapshot snapshot() const { std::lock_guard<std::mutex> hold(mutex); return state; }
	template <typename Emit> void finish(uint64_t worker, uint64_t attempt, Emit emit)
	{
		std::lock_guard<std::mutex> hold(mutex);
		if (finished) return;
		finished = true;
		if (!state.count && !state.overflow) return;
		// Worst case: 32 <=936-byte prefixed causes, 8 <=280-byte
		// examples, and two <=250-byte header/overflow lines.
		static_assert(MaxGroups * 936 + MaxExamples * 280 + 500 < MaxBytes, "warning byte bound");
		char line[920];
		snprintf(line, sizeof(line), "Replay warning summary: worker=%llu attempt=%llu; counts are attempt replay observations (including repeats).", ull(worker), ull(attempt));
		emit(line);
		for (unsigned i = 0; i < state.count; i++)
		{
			const auto &g = state.groups[i];
			char resources[500] = {}; size_t offset = 0;
			for (unsigned r = 0; r < g.resources.size(); r++) if (g.resources[r] && offset < sizeof(resources) - 1)
				offset += size_t(snprintf(resources + offset, sizeof(resources) - offset, "%s%llu %s", offset ? ", " : "", ull(g.resources[r]), resource(r)));
			char cause[256], raw[48] = {};
			if (g.key.support.known & Fossilize::SupportDetail::Raw)
				snprintf(raw, sizeof(raw), "; raw=%llu", ull(g.key.support.raw));
			snprintf(cause, sizeof(cause), "%s%s%s %s%s%s", g.key.context ? resource(g.key.context - 1) : "",
			         g.key.context ? " " : "", kind(g.key.support.kind), g.key.support.identifier, raw, g.key.escaped_fallback ? " (escaped)" : "");
			if (g.key.dependency)
				snprintf(line, sizeof(line), "Replay cause %u: %llu observations [%s] could not be replayed because a dependency is %s (origin=%s; %s%s)%s.", i + 1, ull(g.count), resources, status(g.key.status), category(g.key.category), g.key.support.kind == Fossilize::SupportKind::None ? "support cause unknown" : "unsupported ", g.key.support.kind == Fossilize::SupportKind::None ? "" : cause, g.saturated ? "; counts saturated (lower bounds)" : "");
			else
				snprintf(line, sizeof(line), "Replay cause %u: %llu observations [%s] could not be replayed because %s is not supported%s.", i + 1, ull(g.count), resources, cause, g.saturated ? "; counts saturated (lower bounds)" : "");
			emit(line);
			for (unsigned j = 0; j < state.examples; j++) if (example_groups[j] == i)
			{
				const auto &e = examples[j];
				char required[32], available[32], raw_value[32];
				value(required, e.support.known & Fossilize::SupportDetail::Required, e.support.required);
				value(available, e.support.known & Fossilize::SupportDetail::Available, e.support.available);
				value(raw_value, e.support.known & Fossilize::SupportDetail::Raw, e.support.raw);
				snprintf(line, 264, "Replay example %u: %s %016llx; parent=%s:%016llx; required=%s available=%s raw=%s.", i + 1, resource(e.tag), ull(e.hash), resource(unsigned(e.parent.tag)), ull(e.parent.hash), required, available, raw_value);
				emit(line);
			}
		}
		if (state.overflow)
		{
			snprintf(line, sizeof(line), "Replay overflow: %llu additional observations outside the 32 retained cause groups%s; omitted cause cardinality unknown.", ull(state.overflow), state.saturated ? " (saturated lower bound)" : "");
			emit(line);
		}
	}
private:
	static void value(char (&out)[32], bool known, uint64_t n)
	{ if (known) snprintf(out, sizeof(out), "%llu", ull(n)); else snprintf(out, sizeof(out), "unknown"); }
	static unsigned long long ull(uint64_t n) { return static_cast<unsigned long long>(n); }
	static const char *status(uint64_t n)
	{ return n == 1 ? "missing" : n == 2 ? "failed" : n == 3 ? "read-failed" : "unknown"; }
	static const char *category(uint64_t n)
	{ static const char *names[] = {"unknown", "read", "parse", "create", "unsupported", "validation", "setup", "dependency"}; return n < 8 ? names[n] : "unknown"; }
	static const char *kind(Fossilize::SupportKind k)
	{ static const char *names[] = {"unknown", "feature", "extension", "property/API boundary", "unknown sType", "SPIR-V capability", "malformed", "constraint"}; return uint64_t(k) < 8 ? names[unsigned(k)] : "unknown"; }
	friend struct ReplayWarningTestAccess;
	mutable std::mutex mutex;
	Snapshot state;
	std::array<Example, MaxExamples> examples = {};
	std::array<unsigned, MaxExamples> example_groups = {};
	struct Origin { unsigned tag; uint64_t hash; Key key; Fossilize::SupportDetail support; };
	std::array<Origin, MaxOrigins> origins = {};
	unsigned origin_count = 0;
	bool finished = false;
};
