using System.Runtime.CompilerServices;
using System.Text.Json;

// Original synthetic workload. It never reads repository files, user data, or the network.
internal static class Program
{
    private sealed class RetainedPayload { public readonly byte[] Bytes = new byte[4096]; }
    private sealed class ReleasedPayload { public readonly byte[] Bytes = new byte[2048]; }
    private static readonly List<RetainedPayload> Retained = new();
    private static byte[]? HeldDuringGc;
    private static readonly List<object> Checkpoints = new();

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void AllocateRetained()
    {
        for (int i = 0; i < 2048; ++i) Retained.Add(new RetainedPayload());
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference AllocateReleased()
    {
        var released = new List<ReleasedPayload>();
        for (int i = 0; i < 4096; ++i) released.Add(new ReleasedPayload());
        return new WeakReference(released[0]);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Collect(string name, WeakReference? released = null)
    {
        GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
        GC.WaitForPendingFinalizers();
        GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
        Checkpoints.Add(new { name, timestamp = System.Diagnostics.Stopwatch.GetTimestamp(),
            retainedPayloads = Retained.Count, releasedObjectAlive = released?.IsAlive,
            generation2Collections = GC.CollectionCount(2) });
        File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "checkpoints.json"),
            JsonSerializer.Serialize(new { frequency = System.Diagnostics.Stopwatch.Frequency,
                pid = Environment.ProcessId, checkpoints = Checkpoints }, new JsonSerializerOptions { WriteIndented = true }));
    }

    private static int Main()
    {
        var deadline = DateTime.UtcNow.AddSeconds(90);
        while (!File.Exists(Path.Combine(AppContext.BaseDirectory, "start-workload")))
        {
            if (DateTime.UtcNow > deadline) return 2;
            Thread.Sleep(100);
        }
        Collect("baseline");
        AllocateRetained();
        var released = AllocateReleased();
        Collect("retained-and-released", released);
        // Keep another cohort alive over a compacting GC, then remove the reference.
        HeldDuringGc = new byte[128 * 1024];
        Collect("large-object-survived", released);
        HeldDuringGc = null;
        Collect("large-object-released", released);
        File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "workload-complete"), "complete");
        // Collection stops while the deliberately retained cohort is still rooted.
        while (!File.Exists(Path.Combine(AppContext.BaseDirectory, "stop-workload")))
        {
            if (DateTime.UtcNow > deadline) return 3;
            Thread.Sleep(100);
        }
        GC.KeepAlive(Retained);
        return released.IsAlive ? 4 : 0;
    }
}
