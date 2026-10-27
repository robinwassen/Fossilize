# Fossilize — project descriptor

**Fossilize is a C++ library, a Vulkan capture layer, and a collection of CLI tools for recording and replaying Vulkan object creation.** Its scope includes shaders, pipelines, layouts, render passes, and samplers. [Project description](https://github.com/ValveSoftware/Fossilize#readme)

The three parts are:

| Part | Purpose |
|---|---|
| **Library** | Records Vulkan object descriptions and reads them back, usually through `.foz` databases. |
| **Capture layer** | Observes a running application’s Vulkan calls and records those descriptions. |
| **CLI tools** | Replay, inspect, merge, filter, and otherwise process the recordings. |

The important distinction is between **recorded inputs** and **compiled results**:

- A `.foz` database contains shader code and descriptions of how Vulkan objects should be created.
- `fossilize-replay` recreates those objects through Vulkan. The **graphics driver** performs compilation and can populate its compiled caches.

That lets Steam prepare pipelines without running the game itself.

There are also more than five tools: the current build lists **12 CLI targets**, including one platform-dependent tool. For our investigation, the relevant one is **`fossilize-replay`**. [Tool definitions](https://github.com/ValveSoftware/Fossilize/blob/master/cli/CMakeLists.txt)

---

Initial descriptor from the conversation on 27 October 2026. Tool counts describe the upstream build at that time.
