# Repeated shader processing with unclear failure and cache-reuse outcomes

The Sims 4 repeatedly delayed startup with “Processing Vulkan shaders,” despite previous processing having finished.

Steam launched Fossilize’s replayer before Proton, with a **10-second limit without pipeline completion**. Controlled tests showed workers timing out and compilation being interrupted. Allowing 60 seconds let the tested workload finish; its resulting cache was reusable.

The investigation was complicated by several observability gaps:

- **Hidden failure context:** Steam’s normal logs showed progress and completion without an actionable explanation of worker failures. Separate, unsuppressed replay runs exposed the timeouts.
- **Ambiguous crash reporting:** Timeouts used SIGABRT and the crash-recovery path. A debugger was needed to establish that an interrupted worker was executing inside NVIDIA’s shader compiler; this did not prove a spontaneous driver crash.
- **Unclear completion:** One replay returned exit status zero despite reporting “Cannot continue,” making successful termination insufficient evidence of completed work.
- **Unclear reuse:** The logs did not clearly connect replay triggers, previously processed entries, skipped/failed work and persisted results. A shader-bucket update also triggered processing of an unchanged generic database.

**The integration gap:** Fossilize needs to expose explicit failure reasons and partial outcomes even in quiet mode. Steam needs to retain and present those results alongside why replay was scheduled and what reuse information is available.

The investigation did **not** establish that the entire cache was discarded. Cache reuse worked in controlled tests, while deliberately forcing a warm replay still incurred substantial processing overhead.

## References

Investigated 26–27 October 2026.

- [Upstream issue: steam-for-linux #13810](https://github.com/ValveSoftware/steam-for-linux/issues/13810)
- [Sanitized supporting evidence](https://gist.github.com/robinwassen/6d8cd12e4040cf409cb3553f4fb87b61)
