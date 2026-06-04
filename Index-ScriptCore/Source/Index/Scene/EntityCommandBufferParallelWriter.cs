using System;
using System.Runtime.CompilerServices;
using Index.Components;

namespace Index;

public sealed partial class EntityCommandBuffer
{
    /// <summary>Per-worker ECB recorder; EntityRef indices are LOCAL to the producing thread and remapped at Playback — passing a ref across threads silently corrupts the batch.</summary>
    public readonly struct ParallelWriter
    {
        private readonly EntityCommandBuffer m_Parent;

        internal ParallelWriter(EntityCommandBuffer parent)
        {
            m_Parent = parent;
        }

        /// <summary>Records a new entity into this thread's sub-buffer; the returned index is local to this worker slot and remapped at Playback.</summary>
        public EntityRef Create()
        {
            WorkerSlot slot = m_Parent.GetOrCreateSlotForCurrentThread();
            uint localIndex = slot.EntityCount;
            slot.EntityCount = localIndex + 1;
            return new EntityRef(localIndex);
        }

        /// <summary>Records entity destruction into this worker's sub-buffer.</summary>
        public void Destroy(EntityRef entity)
        {
            WorkerSlot slot = m_Parent.GetOrCreateSlotForCurrentThread();
            if (entity.IsCommandBufferEntity && entity.Index >= slot.EntityCount)
            {
                throw new ArgumentException(
                    $"EntityRef index {entity.Index} is out of range for this worker slot " +
                    $"(thread {slot.ManagedThreadId} entityCount = {slot.EntityCount}). " +
                    "Did you pass an EntityRef across worker threads, or call Create on a different ECB?",
                    nameof(entity));
            }

            EcbWire.WriteDestroyEntityRecord(
                ref slot.Commands,
                ref slot.CommandsLen,
                ref slot.CommandCount,
                entity);
        }

        /// <summary>Records a component add into this thread's sub-buffer. <paramref name="e"/> MUST originate from <see cref="Create"/> on this same thread; a cross-thread ref silently corrupts the batch.</summary>
        public unsafe void AddComponent<T>(EntityRef e, in T data) where T : unmanaged, IComponent
        {
            int payloadSize = sizeof(T);
            if (payloadSize > ushort.MaxValue)
            {
                throw new ArgumentException(
                    $"Component '{typeof(T).Name}' sizeof = {payloadSize} exceeds the ECB's u16 payload limit.",
                    nameof(data));
            }

            WorkerSlot slot = m_Parent.GetOrCreateSlotForCurrentThread();
            if (!e.IsCommandBufferEntity || e.Index >= slot.EntityCount)
            {
                // Catches the common cross-thread misuse: an EntityRef
                // created on thread A is passed to thread B, whose slot
                // hasn't reserved that index. Cannot catch the case where
                // thread B coincidentally has enough entities of its own
                // — see the contract note on ParallelWriter.
                throw new ArgumentException(
                    $"EntityRef index {e.Index} is out of range for this worker slot " +
                    $"(thread {slot.ManagedThreadId} entityCount = {slot.EntityCount}). " +
                    "Did you pass an EntityRef across worker threads, or call Create on a different ECB?",
                    nameof(e));
            }

            EcbWire.WriteAddComponentRecord(
                ref slot.Commands,
                ref slot.CommandsLen,
                ref slot.CommandCount,
                e.Index,
                ComponentTypes<T>.NativeId,
                (ushort)payloadSize,
                Unsafe.AsPointer(ref Unsafe.AsRef(in data)));
        }

        /// <summary>Records a prefab instantiation into this thread's sub-buffer and returns the root EntityRef (children created at playback). Same per-thread locality contract as <see cref="Create"/>.</summary>
        public unsafe EntityRef Instantiate(ulong prefabGuid)
        {
            if (prefabGuid == 0)
            {
                throw new ArgumentException(
                    "Instantiate called with prefabGuid == 0; pass a valid prefab asset GUID.",
                    nameof(prefabGuid));
            }

            WorkerSlot slot = m_Parent.GetOrCreateSlotForCurrentThread();
            uint localIndex = slot.EntityCount;
            slot.EntityCount = localIndex + 1;

            EcbWire.WriteInstantiatePrefabRecord(
                ref slot.Commands,
                ref slot.CommandsLen,
                ref slot.CommandCount,
                localIndex,
                prefabGuid);

            return new EntityRef(localIndex);
        }

        /// <summary>Equivalent to <c>Instantiate(prefabAsset.PrefabGUID)</c>; requires a prefab-asset Entity.</summary>
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

        // Per-worker CreateEntityWith/CreateEntitiesWith: records Ecb_DefaultConstructComponent so C++ default-initializers fire (e.g. Scale=(1,1), Color=white). Same thread-locality rules as Create.
        private void RecordDefaultConstruct<T>(EntityRef e) where T : unmanaged, IComponent
        {
            WorkerSlot slot = m_Parent.GetOrCreateSlotForCurrentThread();
            if (!e.IsCommandBufferEntity || e.Index >= slot.EntityCount)
            {
                throw new ArgumentException(
                    $"EntityRef index {e.Index} is out of range for this worker slot " +
                    $"(thread {slot.ManagedThreadId} entityCount = {slot.EntityCount}). " +
                    "Did you pass an EntityRef across worker threads, or call Create on a different ECB?",
                    nameof(e));
            }

            EcbWire.WriteDefaultConstructRecord(
                ref slot.Commands,
                ref slot.CommandsLen,
                ref slot.CommandCount,
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

        /// <summary>Records <paramref name="length"/> entities (each with default-constructed components) into this thread's sub-buffer; writes refs into <paramref name="output"/> (must be at least <paramref name="length"/> long).</summary>
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
    }
}
