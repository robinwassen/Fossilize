#pragma once
#include <cstdint>
#include <cstdio>
namespace Fossilize
{
// Caller-owned values: no borrowed input strings or pointers. These identifiers
// describe the existing detector; they do not add device support policy.
enum class SupportKind : uint64_t
{
	None,
	Feature,
	Extension,
	Property,
	UnknownSType,
	Capability,
	Malformed,
	Constraint
};
struct SupportDetail
{
	SupportKind kind = SupportKind::None;
	enum : uint64_t
	{
		Required = 1,
		Available = 2,
		Raw = 4
	};
	uint64_t known = 0, required = 0, available = 0, raw = 0;
	char identifier[160] = {};
};
} // namespace Fossilize
