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
// Lossless ASCII escaping distinguishes control bytes, literal '?' and literal
// backslashes. Long identifiers retain an explicit truncation marker.
inline void copy_support_identifier(char (&out)[160], const char *input)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n = 0;
	for (; *input; input++)
	{
		unsigned char c = static_cast<unsigned char>(*input);
		bool escaped = c < 32 || c > 126 || c == '\\';
		size_t width = escaped ? 4 : 1;
		if (n + width >= sizeof(out))
		{
			static const char marker[] = "...[truncated]";
			if (n + sizeof(marker) > sizeof(out)) n = sizeof(out) - sizeof(marker);
			for (size_t i = 0; i < sizeof(marker); i++) out[n + i] = marker[i];
			return;
		}
		if (escaped) { out[n++] = '\\'; out[n++] = 'x'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
		else out[n++] = char(c);
	}
	out[n] = 0;
}
struct SupportDetail
{
	SupportKind kind = SupportKind::None;
	enum : uint64_t
	{
		Required = 1,
		Available = 2,
		Raw = 4
	};
	// For property checks, required is the recorded requirement/value and
	// available is the device or replay boundary value. The identifier defines
	// a maximum, minimum, mask or alignment relation, not always <=.
	// Input-only constraints name their relation and compare its input values.
	uint64_t known = 0, required = 0, available = 0, raw = 0;
	char identifier[160] = {};
};
} // namespace Fossilize
