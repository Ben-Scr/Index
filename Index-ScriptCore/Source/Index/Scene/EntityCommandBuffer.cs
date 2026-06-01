using System;
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Threading;
using Index.Components;
using Index.Interop;

namespace Index;

/// <summary>
/// Batch-records entity creation and component additions, then replays the whole batch in one P/Invoke (typically 50–100× faster than per-entity calls).
/// Components must be unmanaged IComponent structs with layouts matching their C++ counterparts; components with scene-bound runtime state need a native emplaceFromBytes callback.
/// Single-threaded by default; use <see cref="AsParallelWriter"/> for job-worker parallel recording.
/// </summary>
public sealed partial class EntityCommandBuffer : IDisposable
{
    internal const int HEADER_BYTES = EcbWire.HEADER_BYTES;
    internal const int COMMAND_PREFIX_BYTES = EcbWire.COMMAND_PREFIX_BYTES;

    private byte[] m_Commands;
    private int m_CommandsLen;
    private int m_CommandCount;
    private uint m_EntityCount;

    // Buffer the native playback writes runtime IDs into. Allocated
    // lazily on first Playback so a Clear()-and-reuse loop keeps the
    // backing storage when the entity count is stable.
    private ulong[]? m_CreatedIds;
    private int m_CreatedCount;

    private byte[]? m_WireBuffer;

    // Per-thread sub-recorders for parallel writes. Allocated lazily on
    // the first AsParallelWriter() call — single-threaded callers never
    // pay the dictionary cost.
    private ConcurrentDictionary<int, WorkerSlot>? m_ParallelSlots;

    // Version-gated cache: m_SlotSnapshotVersion bumps only when a new worker slot is added; Playback rebuilds m_CachedSortedSlots only on drift.
    private WorkerSlot[]? m_CachedSortedSlots;
    private int m_CachedSortedSlotsVersion;
    private int m_SlotSnapshotVersion;

    /// <summary>Construct a recorder with an initial command-stream capacity in bytes; buffer grows geometrically.</summary>
    public EntityCommandBuffer(int initialCapacity = 1024)
    {
        if (initialCapacity < HEADER_BYTES) initialCapacity = HEADER_BYTES;
        m_Commands = new byte[initialCapacity];
    }

    /// <summary>Total entities queued (main + parallel workers). Allocates a snapshot — avoid in per-frame hot loops.</summary>
    public int EntityCount
    {
        get
        {
            uint total = m_EntityCount;
            if (m_ParallelSlots != null)
            {
                foreach (WorkerSlot s in m_ParallelSlots.Values)
                    total += s.EntityCount;
            }
            return (int)total;
        }
    }

    /// <summary>Total commands recorded (main + parallel workers). Allocates a snapshot — see <see cref="EntityCount"/>.</summary>
    public int CommandCount
    {
        get
        {
            int total = m_CommandCount;
            if (m_ParallelSlots != null)
            {
                foreach (WorkerSlot s in m_ParallelSlots.Values)
                    total += s.CommandCount;
            }
            return total;
        }
    }

    /// <summary>Records a new entity and returns a handle valid until the next <see cref="Clear"/> or <see cref="Dispose"/>.</summary>
    public EntityRef Create()
    {
        EntityRef r = new EntityRef(m_EntityCount);
        m_EntityCount++;
        return r;
    }

    /// <summary>Records "attach component T with the given value to entity e"; bytes are copied immediately so the source struct can be reused.</summary>
    public unsafe void AddComponent<T>(EntityRef e, in T data) where T : unmanaged, IComponent
    {
        int payloadSize = sizeof(T);
        // The wire format reserves a u16 for payloadSize, so 65535 is
        // the hard upper bound — defensive even though no real built-in
        // component approaches that size.
        if (payloadSize > ushort.MaxValue)
        {
            throw new ArgumentException(
                $"Component '{typeof(T).Name}' sizeof = {payloadSize} exceeds the ECB's u16 payload limit.",
                nameof(data));
        }
        if (e.Index >= m_EntityCount)
        {
            throw new ArgumentException(
                $"EntityRef index {e.Index} is out of range for this ECB (entityCount = {m_EntityCount}). " +
                "Did you call Create on a different ECB?",
                nameof(e));
        }

        EcbWire.WriteAddComponentRecord(
            ref m_Commands,
            ref m_CommandsLen,
            ref m_CommandCount,
            e.Index,
            ComponentTypes<T>.NativeId,
            (ushort)payloadSize,
            Unsafe.AsPointer(ref Unsafe.AsRef(in data)));
    }

    /// <summary>
    /// Records a prefab spawn and returns an <see cref="EntityRef"/> for its root. Children are reachable only AFTER <see cref="Playback"/>.
    /// First spawn bakes a memcpy-ready template; subsequent spawns are 50–200× faster. Prefabs with internal entity references fall back to a slow path in v1.
    /// </summary>
    public unsafe EntityRef Instantiate(ulong prefabGuid)
    {
        if (prefabGuid == 0)
        {
            throw new ArgumentException(
                "Instantiate called with prefabGuid == 0; pass a valid prefab asset GUID.",
                nameof(prefabGuid));
        }

        EntityRef root = new EntityRef(m_EntityCount);
        m_EntityCount++;

        EcbWire.WriteInstantiatePrefabRecord(
            ref m_Commands,
            ref m_CommandsLen,
            ref m_CommandCount,
            root.Index,
            prefabGuid);

        return root;
    }

    /// <summary>Convenience overload accepting a prefab-asset <see cref="Entity"/> (from <see cref="Entity.FromPrefabGUID"/> or an inspector-wired field).</summary>
    public EntityRef Instantiate(Entity prefabAsset)
    {
        if (prefabAsset is null || !prefabAsset.IsPrefabAsset)
        {
            throw new ArgumentException(
                "Instantiate requires a prefab-asset Entity (Entity.FromPrefabGUID). " +
                "Pass a runtime entity? Use Clone instead.",
                nameof(prefabAsset));
        }
        return Instantiate(prefabAsset.PrefabGUID);
    }

    // ── CreateWith / CreateEntitiesWith ──────────────────────────────
    // Uses Ecb_DefaultConstructComponent so C++ default-member-initializers fire (e.g. Transform2D Scale = (1,1)) instead of being overwritten by C#'s zero-init default(T).

    private void RecordDefaultConstruct<T>(EntityRef e) where T : unmanaged, IComponent
    {
        if (e.Index >= m_EntityCount)
        {
            throw new ArgumentException(
                $"EntityRef index {e.Index} is out of range for this ECB (entityCount = {m_EntityCount}). " +
                "Did you call Create on a different ECB?",
                nameof(e));
        }

        EcbWire.WriteDefaultConstructRecord(
            ref m_Commands,
            ref m_CommandsLen,
            ref m_CommandCount,
            e.Index,
            ComponentTypes<T>.NativeId);
    }

    public EntityRef CreateWith<T1>()
        where T1 : unmanaged, IComponent
    {
        EntityRef e = Create();
        RecordDefaultConstruct<T1>(e);
        return e;
    }

    public EntityRef CreateWith<T1, T2>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
    {
        EntityRef e = Create();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        return e;
    }

    public EntityRef CreateWith<T1, T2, T3>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
    {
        EntityRef e = Create();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        RecordDefaultConstruct<T3>(e);
        return e;
    }

    public EntityRef CreateWith<T1, T2, T3, T4>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
    {
        EntityRef e = Create();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        RecordDefaultConstruct<T3>(e);
        RecordDefaultConstruct<T4>(e);
        return e;
    }

    public EntityRef CreateWith<T1, T2, T3, T4, T5>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
    {
        EntityRef e = Create();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        RecordDefaultConstruct<T3>(e);
        RecordDefaultConstruct<T4>(e);
        RecordDefaultConstruct<T5>(e);
        return e;
    }

    public EntityRef CreateWith<T1, T2, T3, T4, T5, T6>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
    {
        EntityRef e = Create();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        RecordDefaultConstruct<T3>(e);
        RecordDefaultConstruct<T4>(e);
        RecordDefaultConstruct<T5>(e);
        RecordDefaultConstruct<T6>(e);
        return e;
    }

    public EntityRef CreateWith<T1, T2, T3, T4, T5, T6, T7>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        where T7 : unmanaged, IComponent
    {
        EntityRef e = Create();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        RecordDefaultConstruct<T3>(e);
        RecordDefaultConstruct<T4>(e);
        RecordDefaultConstruct<T5>(e);
        RecordDefaultConstruct<T6>(e);
        RecordDefaultConstruct<T7>(e);
        return e;
    }

    public EntityRef CreateWith<T1, T2, T3, T4, T5, T6, T7, T8>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        where T7 : unmanaged, IComponent
        where T8 : unmanaged, IComponent
    {
        EntityRef e = Create();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        RecordDefaultConstruct<T3>(e);
        RecordDefaultConstruct<T4>(e);
        RecordDefaultConstruct<T5>(e);
        RecordDefaultConstruct<T6>(e);
        RecordDefaultConstruct<T7>(e);
        RecordDefaultConstruct<T8>(e);
        return e;
    }

    /// <summary>Records <paramref name="length"/> entities with one default-constructed component each; refs written to <paramref name="output"/> in order (must be at least <paramref name="length"/> long).</summary>
    public void CreateEntitiesWith<T1>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            output[i] = e;
        }
    }

    public void CreateEntitiesWith<T1, T2>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            output[i] = e;
        }
    }

    public void CreateEntitiesWith<T1, T2, T3>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            output[i] = e;
        }
    }

    public void CreateEntitiesWith<T1, T2, T3, T4>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            output[i] = e;
        }
    }

    public void CreateEntitiesWith<T1, T2, T3, T4, T5>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            RecordDefaultConstruct<T5>(e);
            output[i] = e;
        }
    }

    public void CreateEntitiesWith<T1, T2, T3, T4, T5, T6>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            RecordDefaultConstruct<T5>(e);
            RecordDefaultConstruct<T6>(e);
            output[i] = e;
        }
    }

    public void CreateEntitiesWith<T1, T2, T3, T4, T5, T6, T7>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        where T7 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            RecordDefaultConstruct<T5>(e);
            RecordDefaultConstruct<T6>(e);
            RecordDefaultConstruct<T7>(e);
            output[i] = e;
        }
    }

    public void CreateEntitiesWith<T1, T2, T3, T4, T5, T6, T7, T8>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        where T7 : unmanaged, IComponent
        where T8 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            RecordDefaultConstruct<T5>(e);
            RecordDefaultConstruct<T6>(e);
            RecordDefaultConstruct<T7>(e);
            RecordDefaultConstruct<T8>(e);
            output[i] = e;
        }
    }

    internal static void ValidateCreateEntitiesArgs(int length, int outputLength)
    {
        if (length < 0)
            throw new ArgumentOutOfRangeException(nameof(length),
                "length must be non-negative.");
        if (outputLength < length)
            throw new ArgumentException(
                $"output span length ({outputLength}) is smaller than entity count ({length}).",
                "output");
    }

    /// <summary>
    /// Returns a per-thread sub-buffer writer; the ECB merges all workers at <see cref="Playback"/> time.
    /// MUST be quiescent before Playback. An <see cref="EntityRef"/> from one worker is valid ONLY in that same worker's AddComponent calls.
    /// </summary>
    public ParallelWriter AsParallelWriter()
    {
        m_ParallelSlots ??= new ConcurrentDictionary<int, WorkerSlot>();
        return new ParallelWriter(this);
    }

    /// <summary>Ships the batch to the active scene. Returns entity count; IDs available via <see cref="GetCreatedEntityId"/> indexed by EntityRef order (main first, then workers by ascending thread ID).</summary>
    public unsafe int Playback()
    {
        PlaybackTotals totals = ComputeTotals();
        if (totals.EntityCount == 0)
        {
            m_CreatedCount = 0;
            return 0;
        }

        EnsureCreatedIdsCapacity((int)totals.EntityCount);

        int created;
        fixed (ulong* outIds = m_CreatedIds)
        {
            created = PlaybackDispatch(outIds, m_CreatedIds.Length, totals);
        }

        m_CreatedCount = created;
        return created;
    }

    /// <summary>
    /// Variant of <see cref="Playback"/> that writes IDs into a caller-owned span, avoiding the internal m_CreatedIds allocation.
    /// Does NOT update m_CreatedCount, so <see cref="GetCreatedEntityId"/> still reflects the last <see cref="Playback"/> call.
    /// </summary>
    public unsafe int PlaybackInto(Span<ulong> destination)
    {
        PlaybackTotals totals = ComputeTotals();
        if (totals.EntityCount == 0)
        {
            return 0;
        }

        if ((uint)destination.Length < totals.EntityCount)
        {
            throw new ArgumentException(
                $"Destination span (length {destination.Length}) is smaller than " +
                $"the batch entity count ({totals.EntityCount}).",
                nameof(destination));
        }

        int created;
        fixed (ulong* outIds = destination)
        {
            created = PlaybackDispatch(outIds, destination.Length, totals);
        }
        return created;
    }

    private struct PlaybackTotals
    {
        public uint EntityCount;
        public int CommandsLen;
        public int CommandCount;
        public WorkerSlot[]? Slots; // null when there are no parallel slots
    }

    private PlaybackTotals ComputeTotals()
    {
        PlaybackTotals t;
        t.EntityCount = m_EntityCount;
        t.CommandsLen = m_CommandsLen;
        t.CommandCount = m_CommandCount;
        t.Slots = null;

        if (m_ParallelSlots != null && !m_ParallelSlots.IsEmpty)
        {
            t.Slots = SortedSlotSnapshot();
            for (int i = 0; i < t.Slots.Length; i++)
            {
                t.EntityCount += t.Slots[i].EntityCount;
                t.CommandsLen += t.Slots[i].CommandsLen;
                t.CommandCount += t.Slots[i].CommandCount;
            }
        }

        return t;
    }

    private unsafe int PlaybackDispatch(ulong* outIds, int maxOut, PlaybackTotals totals)
    {
        return totals.Slots == null
            ? PlaybackMainOnlyImpl(outIds, maxOut, totals)
            : PlaybackMergedImpl(outIds, maxOut, totals);
    }

    private unsafe int PlaybackMainOnlyImpl(ulong* outIds, int maxOut, PlaybackTotals totals)
    {
        int entityTableBytes = (int)totals.EntityCount * sizeof(uint);
        int totalBytes = HEADER_BYTES + entityTableBytes + totals.CommandsLen;
        EnsureWireCapacity(totalBytes);

        int created;
        fixed (byte* wirePtr = m_WireBuffer)
        fixed (byte* cmdSrc = m_Commands)
        {
            // Header.
            uint entityCountU = totals.EntityCount;
            uint commandCountU = (uint)totals.CommandCount;
            Unsafe.CopyBlockUnaligned(wirePtr, &entityCountU, 4);
            Unsafe.CopyBlockUnaligned(wirePtr + 4, &commandCountU, 4);

            // Entity table — every slot is NO_NAME in v1. NO_NAME is
            // 0xFFFFFFFF, so a single InitBlock fills four uints per
            // store instead of one and vectorizes to `rep stos`.
            Unsafe.InitBlockUnaligned(wirePtr + HEADER_BYTES, 0xFF, (uint)entityTableBytes);

            // Command stream.
            if (totals.CommandsLen > 0)
            {
                Unsafe.CopyBlockUnaligned(
                    wirePtr + HEADER_BYTES + entityTableBytes,
                    cmdSrc,
                    (uint)totals.CommandsLen);
            }

            created = InternalCalls.Ecb_Playback(wirePtr, totalBytes, outIds, maxOut);
        }

        ThrowOnPlaybackError(created);
        return created;
    }

    private unsafe int PlaybackMergedImpl(ulong* outIds, int maxOut, PlaybackTotals totals)
    {
        WorkerSlot[] slots = totals.Slots!;
        int entityTableBytes = (int)totals.EntityCount * sizeof(uint);
        int totalBytes = HEADER_BYTES + entityTableBytes + totals.CommandsLen;
        EnsureWireCapacity(totalBytes);

        int created;
        fixed (byte* wirePtr = m_WireBuffer)
        {
            // Header.
            uint entityCountU = totals.EntityCount;
            uint commandCountU = (uint)totals.CommandCount;
            Unsafe.CopyBlockUnaligned(wirePtr, &entityCountU, 4);
            Unsafe.CopyBlockUnaligned(wirePtr + 4, &commandCountU, 4);

            // Entity table — every slot is NO_NAME.
            Unsafe.InitBlockUnaligned(wirePtr + HEADER_BYTES, 0xFF, (uint)entityTableBytes);

            // Command stream — main recorder first (baseOffset 0), then
            // each worker slot at its running base offset.
            byte* cmdDst = wirePtr + HEADER_BYTES + entityTableBytes;
            uint baseOffset = 0;

            if (m_CommandsLen > 0)
            {
                EcbWire.CopyAndRemapCommands(cmdDst, m_Commands, m_CommandsLen, baseOffset);
                cmdDst += m_CommandsLen;
            }
            baseOffset += m_EntityCount;

            for (int i = 0; i < slots.Length; i++)
            {
                WorkerSlot s = slots[i];
                if (s.CommandsLen > 0)
                {
                    EcbWire.CopyAndRemapCommands(cmdDst, s.Commands, s.CommandsLen, baseOffset);
                    cmdDst += s.CommandsLen;
                }
                baseOffset += s.EntityCount;
            }

            created = InternalCalls.Ecb_Playback(wirePtr, totalBytes, outIds, maxOut);
        }

        ThrowOnPlaybackError(created);
        return created;
    }

    private static void ThrowOnPlaybackError(int created)
    {
        if (created >= 0) return;
        string reason = created switch
        {
            -1 => "wire buffer was truncated",
            -2 => "no active scene to play back into",
            -3 => "output ID buffer was too small (internal bug — please report)",
            -4 => "an AddComponent referenced a component type id with no native " +
                  "emplacer registered. Either the component isn't registered on the " +
                  "native side, or it holds non-memcpy-safe state (smart pointers / " +
                  "owning containers / scene-bound handles) and needs a custom " +
                  "ComponentInfo::emplaceFromBytes registered at engine startup. See " +
                  "the engine log for the offending typeId",
            -5 => "the batch would exceed the EnTT entity cap configured for this " +
                  "build. Open Project Settings > Entity ID bits, raise the value " +
                  "(24 -> 16M, 28 -> 268M), then regenerate project files and " +
                  "rebuild. See the engine log for the current count / cap",
            -6 => "an Ecb_InstantiatePrefab record referenced a prefab GUID that " +
                  "is unknown OR whose template can't be baked into a memcpy-ready " +
                  "form (e.g. the prefab contains internal entity references — a " +
                  "v1 limitation; fall back to Entity.Instantiate(prefab) for those). " +
                  "See the engine log for the offending GUID",
            _ => $"native error code {created}",
        };
        throw new InvalidOperationException(
            $"EntityCommandBuffer.Playback failed: {reason}.");
    }

    [System.Diagnostics.CodeAnalysis.MemberNotNull(nameof(m_WireBuffer))]
    private void EnsureWireCapacity(int needed)
    {
        if (m_WireBuffer != null && m_WireBuffer.Length >= needed) return;
        int newCap = m_WireBuffer?.Length ?? 0;
        if (newCap < needed)
        {
            newCap = Math.Max(needed, newCap * 2);
        }
        m_WireBuffer = new byte[newCap];
    }

    [System.Diagnostics.CodeAnalysis.MemberNotNull(nameof(m_CreatedIds))]
    private void EnsureCreatedIdsCapacity(int needed)
    {
        if (m_CreatedIds != null && m_CreatedIds.Length >= needed) return;
        int newCap = m_CreatedIds?.Length ?? 0;
        if (newCap < needed)
        {
            newCap = Math.Max(needed, newCap + (newCap >> 1));
        }
        m_CreatedIds = new ulong[newCap];
    }

    private WorkerSlot[] SortedSlotSnapshot()
    {
        int version = Volatile.Read(ref m_SlotSnapshotVersion);
        if (m_CachedSortedSlots != null && m_CachedSortedSlotsVersion == version)
        {
            return m_CachedSortedSlots;
        }

        WorkerSlot[] arr = new WorkerSlot[m_ParallelSlots!.Count];
        int idx = 0;
        foreach (WorkerSlot s in m_ParallelSlots.Values)
        {
            if (idx < arr.Length) arr[idx++] = s;
        }
        if (idx != arr.Length) Array.Resize(ref arr, idx);
        Array.Sort(arr, static (a, b) => a.ManagedThreadId.CompareTo(b.ManagedThreadId));

        m_CachedSortedSlots = arr;
        m_CachedSortedSlotsVersion = version;
        return arr;
    }

    /// <summary>Runtime ID of the i-th entity from the most recent <see cref="Playback"/>; throws if out of range or before any playback.</summary>
    public ulong GetCreatedEntityId(int index)
    {
        if ((uint)index >= (uint)m_CreatedCount || m_CreatedIds == null)
        {
            throw new ArgumentOutOfRangeException(nameof(index),
                $"No playback result at index {index} (last playback created {m_CreatedCount} entities).");
        }
        return m_CreatedIds[index];
    }

    public Entity GetCreatedEntity(int index) => new Entity(GetCreatedEntityId(index));

    /// <summary>Resets all counters without releasing backing buffers; reuse the instance each frame to avoid allocation.</summary>
    public void Clear()
    {
        m_CommandsLen = 0;
        m_CommandCount = 0;
        m_EntityCount = 0;
        m_CreatedCount = 0;
        if (m_ParallelSlots != null)
        {
            foreach (WorkerSlot s in m_ParallelSlots.Values)
            {
                s.CommandsLen = 0;
                s.CommandCount = 0;
                s.EntityCount = 0;
            }
        }
    }

    /// <summary>Releases all buffers; the instance is unusable afterwards.</summary>
    public void Dispose()
    {
        Clear();
        m_Commands = Array.Empty<byte>();
        m_CreatedIds = null;
        m_WireBuffer = null;
        m_ParallelSlots = null;
    }

    // ── Parallel-writer plumbing (called from ParallelWriter) ────────

    internal WorkerSlot GetOrCreateSlotForCurrentThread()
    {
        int tid = Environment.CurrentManagedThreadId;
        return m_ParallelSlots!.GetOrAdd(
            tid,
            static (id, self) =>
            {
                        Interlocked.Increment(ref self.m_SlotSnapshotVersion);
                return new WorkerSlot(id);
            },
            this);
    }

    internal sealed class WorkerSlot
    {
        public byte[] Commands = new byte[256];
        public int CommandsLen;
        public int CommandCount;
        public uint EntityCount;
        public readonly int ManagedThreadId;

        public WorkerSlot(int managedThreadId)
        {
            ManagedThreadId = managedThreadId;
        }
    }
}

// Wire-format constants and record writers; single source of truth for the byte layout shared with the native side.
internal static class EcbWire
{
    // Mirrors EcbOpcode in EntityCommandBufferWire.hpp.
    public const byte OP_ADD_COMPONENT      = 1;
    // OP_SET_COMPONENT = 2 (reserved on the native side; no managed
    // emitter today — call sites add components via OP_ADD_COMPONENT).
    public const byte OP_INSTANTIATE_PREFAB = 3;
    // Payload-free opcode; native side calls defaultEmplace so C++ member-initializers (e.g. Transform2D Scale=(1,1)) fire instead of being overwritten by C#'s zero-init default(T).
    public const byte OP_DEFAULT_CONSTRUCT_COMPONENT = 4;

    // Sentinel "no name" matching kEcbNoName on the native side.
    public const uint NO_NAME = 0xFFFFFFFFu;

    public const int HEADER_BYTES = 8;
    public const int COMMAND_PREFIX_BYTES = 11; // u8 opcode + u32 entityIndex + u32 typeId + u16 payloadSize
    // u64 prefabGuid — the only payload Ecb_InstantiatePrefab carries today.
    public const int INSTANTIATE_PREFAB_PAYLOAD_BYTES = 8;

    public static unsafe void WriteAddComponentRecord(
        ref byte[] commands,
        ref int commandsLen,
        ref int commandCount,
        uint entityIndex,
        uint typeId,
        ushort payloadSize,
        void* payload)
    {
        int recordSize = COMMAND_PREFIX_BYTES + payloadSize;
        EnsureCapacity(ref commands, commandsLen + recordSize);

        fixed (byte* basePtr = commands)
        {
            byte* w = basePtr + commandsLen;
            *w = OP_ADD_COMPONENT; w += 1;
            Unsafe.CopyBlockUnaligned(w, &entityIndex, 4); w += 4;
            Unsafe.CopyBlockUnaligned(w, &typeId, 4); w += 4;
            Unsafe.CopyBlockUnaligned(w, &payloadSize, 2); w += 2;
            if (payloadSize > 0)
            {
                Unsafe.CopyBlockUnaligned(w, payload, payloadSize);
            }
        }

        commandsLen += recordSize;
        commandCount++;
    }

    // No payload; native default-constructs T via the registered defaultEmplace callback. Same 11-byte prefix as other opcodes so the remap walker needs no branch.
    public static unsafe void WriteDefaultConstructRecord(
        ref byte[] commands,
        ref int commandsLen,
        ref int commandCount,
        uint entityIndex,
        uint typeId)
    {
        const int recordSize = COMMAND_PREFIX_BYTES;
        EnsureCapacity(ref commands, commandsLen + recordSize);

        fixed (byte* basePtr = commands)
        {
            byte* w = basePtr + commandsLen;
            *w = OP_DEFAULT_CONSTRUCT_COMPONENT; w += 1;
            Unsafe.CopyBlockUnaligned(w, &entityIndex, 4); w += 4;
            Unsafe.CopyBlockUnaligned(w, &typeId, 4); w += 4;
            // payloadSize = 0 — no bytes follow.
            ushort payloadSize = 0;
            Unsafe.CopyBlockUnaligned(w, &payloadSize, 2);
        }

        commandsLen += recordSize;
        commandCount++;
    }

    // Shares the fixed 11-byte prefix with AddComponent so the merge walker splices prefab records without per-opcode branching.
    public static unsafe void WriteInstantiatePrefabRecord(
        ref byte[] commands,
        ref int commandsLen,
        ref int commandCount,
        uint entityIndex,
        ulong prefabGuid)
    {
        const int recordSize = COMMAND_PREFIX_BYTES + INSTANTIATE_PREFAB_PAYLOAD_BYTES;
        EnsureCapacity(ref commands, commandsLen + recordSize);

        fixed (byte* basePtr = commands)
        {
            byte* w = basePtr + commandsLen;
            *w = OP_INSTANTIATE_PREFAB; w += 1;
            Unsafe.CopyBlockUnaligned(w, &entityIndex, 4); w += 4;
            // typeId slot is unused for this opcode; emit zero so a future
            // wire reader that doesn't know about OP_INSTANTIATE_PREFAB
            // sees a benign sentinel rather than uninitialized bytes.
            uint typeIdZero = 0;
            Unsafe.CopyBlockUnaligned(w, &typeIdZero, 4); w += 4;
            ushort payloadSize = INSTANTIATE_PREFAB_PAYLOAD_BYTES;
            Unsafe.CopyBlockUnaligned(w, &payloadSize, 2); w += 2;
            Unsafe.CopyBlockUnaligned(w, &prefabGuid, INSTANTIATE_PREFAB_PAYLOAD_BYTES);
        }

        commandsLen += recordSize;
        commandCount++;
    }

    public static void EnsureCapacity(ref byte[] buf, int needed)
    {
        if (needed <= buf.Length) return;
        int newCap = buf.Length * 2;
        if (newCap < needed) newCap = needed;
        Array.Resize(ref buf, newCap);
    }

    // Copies the command stream to dst, adding baseOffset to every entityIndex; used by merged-playback to splice worker slots into the unified range.
    public static unsafe void CopyAndRemapCommands(
        byte* dst,
        byte[] src,
        int srcLen,
        uint baseOffset)
    {
        if (srcLen <= 0) return;

        fixed (byte* srcPtr = src)
        {
            byte* r = srcPtr;
            byte* end = srcPtr + srcLen;
            byte* w = dst;

            while (r < end)
            {
                byte op = *r;
                uint entityIndex;
                Unsafe.CopyBlockUnaligned(&entityIndex, r + 1, 4);
                uint typeId;
                Unsafe.CopyBlockUnaligned(&typeId, r + 5, 4);
                ushort payloadSize;
                Unsafe.CopyBlockUnaligned(&payloadSize, r + 9, 2);
                int recordSize = COMMAND_PREFIX_BYTES + payloadSize;

                uint remappedIndex = entityIndex + baseOffset;
                *w = op;
                Unsafe.CopyBlockUnaligned(w + 1, &remappedIndex, 4);
                Unsafe.CopyBlockUnaligned(w + 5, &typeId, 4);
                Unsafe.CopyBlockUnaligned(w + 9, &payloadSize, 2);
                if (payloadSize > 0)
                {
                    Unsafe.CopyBlockUnaligned(w + 11, r + 11, payloadSize);
                }

                r += recordSize;
                w += recordSize;
            }
        }
    }
}
