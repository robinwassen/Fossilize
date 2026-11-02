#pragma once

// Private scalar evidence; not a public API or shared-memory layout.
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

namespace Fossilize { namespace ReplayEvent {
enum Phase : uint64_t { Parse = 1, Create = 2, Timeout = 3 };
enum Reason : uint64_t { GenericParse = 1, InvalidJSON = 2, IncompatibleVersion = 3, MalformedEnvelope = 4,
                        InvalidShader = 5, InvalidRecord = 6, CreationFailed = 16, NoProgress = 32, Heartbeat = 33, Cleanup = 34 };
enum Domain : uint64_t { Vulkan = 1, RapidJSON = 2, Native = 3 };
enum Known : uint64_t { Raw = 1, HasTag = 2, HasHash = 4, Worker = 8, Attempt = 16, Range = 32, Offset = 64, Duration = 128 };
struct Event
{
	uint64_t phase = 0, reason = 0, domain = 0, known = 0;
	int64_t raw = 0;
	uint64_t tag = 0, hash = 0, worker = 0, attempt = 0, begin = 0, end = 0, offset = 0, threshold = 0, elapsed = 0;
};
// A caller-owned capture, scoped independently on each parsing thread. Impl
// recursion uses the same capture, retaining the first detected originating fact.
Event *&parse_slot();
struct ParseCapture
{
	Event result;
	Event *previous;
	ParseCapture() : previous(parse_slot()) { parse_slot() = &result; }
	~ParseCapture() { parse_slot() = previous; }
};
inline void detected(const Event &e) { auto *p = parse_slot(); if (p && !p->reason) *p = e; }

static constexpr size_t MaxFrame = 512;
static constexpr unsigned MaxEvents = 16;
// Length excludes prefix and terminating LF. Every frame is one printable ASCII
// line; explicit lengths and counts are checked before numeric interpretation.
inline size_t frame(char *out, size_t size, const char *payload)
{
	int n = std::snprintf(out, size, "FZ18 %zu:%s\n", std::strlen(payload), payload);
	return n > 0 && size_t(n) < size && size_t(n) < MaxFrame ? size_t(n) : 0;
}
inline size_t event_frame(char *out, size_t size, const Event &e)
{
	char payload[MaxFrame];
	int n = std::snprintf(payload, sizeof(payload),
	    "1 EVENT %llu %llu %llu %llu %lld %llu %llu %llu %llu %llu %llu %llu %llu %llu",
	    (unsigned long long)e.phase, (unsigned long long)e.reason, (unsigned long long)e.domain, (unsigned long long)e.known,
	    (long long)e.raw, (unsigned long long)e.tag, (unsigned long long)e.hash, (unsigned long long)e.worker,
	    (unsigned long long)e.attempt, (unsigned long long)e.begin, (unsigned long long)e.end, (unsigned long long)e.offset,
	    (unsigned long long)e.threshold, (unsigned long long)e.elapsed);
	return n > 0 && size_t(n) < sizeof(payload) ? frame(out, size, payload) : 0;
}
struct Numbers
{
	const char *p, *end;
	bool number(uint64_t &value)
	{
		if (p == end || *p < '0' || *p > '9') return false;
		value = 0;
		do { unsigned d = unsigned(*p++ - '0'); if (value > (UINT64_MAX - d) / 10) return false; value = value * 10 + d; }
		while (p != end && *p >= '0' && *p <= '9');
		return true;
	}
	bool next(uint64_t &v) { if (p == end || *p++ != ' ') return false; return number(v); }
	bool signed_next(int64_t &v)
	{
		if (p == end || *p++ != ' ') return false;
		bool negative = p != end && *p == '-'; if (negative) p++;
		uint64_t u; if (!number(u) || u > uint64_t(INT64_MAX) + unsigned(negative)) return false;
		v = negative ? (u == uint64_t(INT64_MAX) + 1 ? INT64_MIN : -int64_t(u)) : int64_t(u); return true;
	}
};
inline bool payload(const char *line, size_t n, const char *&p, size_t &length)
{
	if (n < 8 || n >= MaxFrame || std::memcmp(line, "FZ18 ", 5)) return false;
	for (size_t i = 0; i < n; i++) if (line[i] < 32 || line[i] > 126) return false;
	Numbers nums{line + 5, line + n}; uint64_t v;
	if (!nums.number(v) || nums.p == nums.end || *nums.p++ != ':' || v != uint64_t(nums.end - nums.p)) return false;
	p = nums.p; length = size_t(v); return true;
}
inline bool valid(const Event &e)
{
	if (e.known & ~uint64_t(255)) return false;
	if (!(e.known & Raw) && (e.raw || e.domain)) return false;
	if ((e.known & Raw) && !e.domain) return false;
	if (!(e.known & HasTag) && e.tag) return false;
	if (!(e.known & HasHash) && e.hash) return false;
	if (!(e.known & Worker) && e.worker) return false;
	if (!(e.known & Attempt) && e.attempt) return false;
	if (!(e.known & Range) && (e.begin || e.end)) return false;
	if ((e.known & Range) && e.end < e.begin) return false;
	if (!(e.known & Offset) && e.offset) return false;
	if (!(e.known & Duration) && (e.threshold || e.elapsed)) return false;
	if ((e.known & Duration) && (!e.threshold || e.elapsed < e.threshold)) return false;
	return true;
}
inline bool supported(const Event &e)
{
	return (e.phase == Parse && e.reason >= GenericParse && e.reason <= InvalidRecord) ||
	       (e.phase == Create && e.reason == CreationFailed) ||
	       (e.phase == Timeout && e.reason >= NoProgress && e.reason <= Cleanup);
}
struct State
{
	std::array<Event, MaxEvents> events = {};
	unsigned count = 0;
	uint64_t omitted = 0, received = 0;
	bool offered = false, hello = false, final = false, failed = false, lost = false, unsupported = false, eof = false;
	uint64_t worker = 0, attempt = 0;
	void begin(uint64_t w, uint64_t a) { *this = {}; offered = true; worker = w; attempt = a; }
	bool successful() const { return !offered || (hello && final && !failed && !lost && !unsupported && !omitted && !count); }
	void accept(const char *p, size_t n)
	{
		if (eof) { lost = true; return; }
		Numbers nums{p,p+n}; uint64_t version;
		if (!nums.number(version) || nums.p == nums.end || *nums.p++ != ' ') { lost = true; return; }
		if (version != 1) { unsupported = true; return; }
		const char *kind = nums.p; while (nums.p != nums.end && *nums.p != ' ') nums.p++;
		size_t k = size_t(nums.p - kind);
		if (k == 5 && !std::memcmp(kind,"HELLO",5))
		{
			uint64_t w,a; if (!nums.next(w) || !nums.next(a) || nums.p != nums.end || !offered || hello || final || w != worker || a != attempt) lost = true;
			else hello = true;
		}
		else if (k == 5 && !std::memcmp(kind,"EVENT",5))
		{
			Event e;
			if (!nums.next(e.phase) || !nums.next(e.reason) || !nums.next(e.domain) || !nums.next(e.known) || !nums.signed_next(e.raw) ||
			    !nums.next(e.tag) || !nums.next(e.hash) || !nums.next(e.worker) || !nums.next(e.attempt) || !nums.next(e.begin) || !nums.next(e.end) ||
			    !nums.next(e.offset) || !nums.next(e.threshold) || !nums.next(e.elapsed) || nums.p != nums.end || !valid(e) || !hello || final ||
			    ((e.known & Worker) && e.worker != worker) || ((e.known & Attempt) && e.attempt != attempt)) { lost = true; return; }
			received++;
			if (!supported(e) || ((e.known & Raw) && e.domain > Native)) unsupported = true;
			if (count < events.size()) events[count++] = e; else { omitted++; lost = true; }
		}
		else if (k == 5 && !std::memcmp(kind,"FINAL",5))
		{
			uint64_t status, dropped;
			if (!nums.next(status) || !nums.next(dropped) || nums.p != nums.end || status > 1 || !hello || final) lost = true;
			else { final = true; failed = status != 0; omitted += dropped; if (dropped) lost = true; }
		}
		else { unsupported = true; }
	}
};
struct Stream
{
	std::array<char, MaxFrame> buffer = {};
	size_t used = 0;
	bool discard = false;
	template <typename Legacy, typename Rich> void feed_frames(State &state, const char *bytes, size_t n, Legacy legacy, Rich rich)
	{
		for (size_t i = 0; i < n; i++)
		{
			unsigned char c = static_cast<unsigned char>(bytes[i]);
			if (c == '\n')
			{
				if (!discard)
				{
					buffer[used] = 0;
					if (used >= 4 && !std::memcmp(buffer.data(),"FZ18",4))
					{
						const char *p; size_t length; if (payload(buffer.data(),used,p,length)) rich(p,length); else state.lost = true;
					}
					else legacy(buffer.data());
				}
				used = 0; discard = false;
			}
			else if (!discard)
			{
				if (used + 1 < buffer.size() && c >= 32 && c <= 126) buffer[used++] = char(c);
				else { used = 0; discard = true; state.lost = true; }
			}
		}
	}
	template <typename Legacy> void feed(State &state, const char *bytes, size_t n, Legacy legacy)
	{ feed_frames(state,bytes,n,legacy,[&state](const char *p,size_t n) {state.accept(p,n);}); }
	void finish(State &s) { if (used || discard) s.lost = true; used = 0; discard = false; s.eof = true; if (s.offered && (!s.hello || !s.final)) s.lost = true; }
};
// Startup only: first offer line, followed by legacy module hashes and EOF.
inline bool select_offer(const char *p, size_t length, uint64_t &worker, uint64_t &attempt)
{
	if (length < 10 || std::memcmp(p,"1 OFFER ",8)) return false;
	Numbers nums{p+8,p+length}; uint64_t versions;
	return nums.number(versions) && nums.next(worker) && nums.next(attempt) && nums.p == nums.end && (versions & 2u);
}
inline bool offer(const char *line, size_t n, uint64_t &worker, uint64_t &attempt, bool &recognized)
{
	recognized = n >= 4 && !std::memcmp(line,"FZ18",4); if (!recognized) return false;
	if (n && line[n-1] == '\n') n--;
	const char *p; size_t length; if (!payload(line,n,p,length)) return false;
	return select_offer(p,length,worker,attempt);
}
} }
