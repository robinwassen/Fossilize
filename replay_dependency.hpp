#pragma once
#include <cstdint>
namespace Fossilize
{
namespace ReplayDependency
{
enum class Status : uint64_t
{
	Unknown,
	Missing,
	Failed,
	ReadFailed
};
struct Detail
{
	uint64_t tag = 0, hash = 0, category = 0;
	Status status = Status::Unknown;
	uint64_t omitted = 0;
};
// Private lexical adapter: parser recursion reports only scalar identities. The
// output lives with the caller; no create-info or allocator pointer is retained.
inline void retain_first(Detail &first, const Detail &next)
{
	if (!next.hash)
		return;
	if (!first.hash)
		first = next;
	else if (first.omitted != UINT64_MAX)
		first.omitted++;
}
Detail *&slot();
bool &boundary();
bool &quiet_diagnostics();
struct Capture
{
	Detail result;
	Detail *previous;
	bool previous_boundary;
	bool previous_quiet;
	explicit Capture(bool is_boundary = false, bool quiet = false) : previous(slot()), previous_boundary(boundary()), previous_quiet(quiet_diagnostics())
	{
		slot() = &result;
		boundary() = is_boundary;
		quiet_diagnostics() = quiet || previous_quiet;
	}
	~Capture()
	{
		slot() = previous;
		boundary() = previous_boundary;
		quiet_diagnostics() = previous_quiet;
	}
};
inline void detected(uint64_t tag, uint64_t hash, Status status)
{
	auto *p = slot();
	if (p && !p->hash && hash)
	{
		p->tag = tag;
		p->hash = hash;
		p->status = status;
	}
}
} // namespace ReplayDependency
} // namespace Fossilize
