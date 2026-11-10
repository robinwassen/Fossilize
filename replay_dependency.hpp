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
struct Capture
{
	Detail result;
	Detail *previous;
	bool previous_boundary;
	explicit Capture(bool is_boundary = false) : previous(slot()), previous_boundary(boundary())
	{
		slot() = &result;
		boundary() = is_boundary;
	}
	~Capture()
	{
		slot() = previous;
		boundary() = previous_boundary;
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
