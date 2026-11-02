#include "replay_event.hpp"
namespace Fossilize { namespace ReplayEvent {
Event *&parse_slot() { static thread_local Event *slot = nullptr; return slot; }
} }
