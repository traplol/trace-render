# Managed allocation capture workload

This original synthetic .NET 8 workload keeps 2,048 `RetainedPayload` objects in a
static list, drops 4,096 `ReleasedPayload` objects, and performs compacting full
collections. A separate large byte array survives one collection before its
reference is removed. Allocation methods are not inlined. `checkpoints.json`
records the process ID, monotonic timestamps, collection counts and a weak
reference check proving that the released cohort's representative was collected.
The process keeps the retained cohort alive until recording stops.

On Windows with Visual Studio Diagnostic Tools installed, run:

```powershell
./scripts/capture_managed_memory.ps1 -OutputDirectory C:/Temp/trace-managed-capture
```

Use a fresh output directory. The script builds only this fixture and launches it
through `VSDiagnostics.exe` with the installed `DotNetObjectAllocBase.json`.
It saves the `.diagsession`, executable, DLL, portable PDB, workload checkpoints,
collector logs, config, collector version and hashes. The manual GitHub workflow
uses `windows-2022`; the dedicated `capture/managed-memory-fixture` branch also
triggers it so its first run does not require registration on the default branch.
Artifacts expire after seven days.

This is a capture candidate, not a validated allocation oracle yet. Inspect the
captured provider schemas, sampling settings, stacks, GC movement and survival
events before enabling allocation-lifetime import. An AllocationTick alone does
not describe every allocation, and a missing object after a GC does not establish
an exact free timestamp. The checkpoint counts describe this workload; they do
not imply that the profiler records every object.

The collection commands and config name follow Microsoft's
[command-line profiling documentation](https://learn.microsoft.com/en-us/visualstudio/profiling/profile-apps-from-command-line?view=visualstudio).
The .NET allocation collector requires launch rather than attach.
