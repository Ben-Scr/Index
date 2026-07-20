using System;
using Index;
using Index.Components;
using Index.Jobs;
using Index.Native;

// End-to-end sample + regression test for the Ecb_SetEntityEnabled opcode:
//   1. ecb.SetEnabled on a command-buffer ref (spawn already-disabled).
//   2. ecb.SetEnabled on a live scene ref via Entity.ToCommandBufferRef().
//   3. Last-write-wins within one batch (disable then re-enable).
//   4. Parallel spawn-disabled through AsParallelWriter() + a job.
//
// Drop this SceneSystem on any scene and toggle RunOnStart. It self-runs once,
// logs PASS / FAIL per case, then destroys everything it spawned so the scene
// is left as it was found. Mirrors PrefabSpawnJobSample's harness.
public class EcbSetEnabledSample : SceneSystem
{
    [ShowInEditor("Entities Per Pass")] public int EntitiesPerPass = 256;
    [ShowInEditor("Run On Start")]      public bool RunOnStart = false;

    private bool m_Ran;
    private int  m_TotalCases;
    private int  m_PassedCases;

    public override void OnStart()
    {
        if (!RunOnStart || m_Ran) return;
        m_Ran = true;
        RunAllCases();
    }

    // ── The parallel job ────────────────────────────────────────────────
    private struct SpawnDisabledJob : IJobParallelFor
    {
        public JobContext Ctx;

        public void Execute(int i)
        {
            EntityRef e = Ctx.Ecb.Create();
            Ctx.Ecb.AddComponent(e, new NativeTransform2D { LocalPosition = new Vector2(i, 0f) });
            Ctx.Ecb.SetEnabled(e, false);
        }
    }

    // ── Driver ──────────────────────────────────────────────────────────
    private void RunAllCases()
    {
        m_TotalCases = 0;
        m_PassedCases = 0;
        int n = EntitiesPerPass < 1 ? 1 : EntitiesPerPass;

        Case("Spawn disabled (command-buffer ref)", () => Case_SpawnDisabled(n));
        Case("Disable live entity (ToCommandBufferRef)", () => Case_DisableLive(n));
        Case("Re-enable wins within one batch", Case_ReEnableLastWins);
        Case("Parallel spawn disabled", () => Case_ParallelSpawnDisabled(n));

        Log.Info($"[EcbSetEnabledSample] {m_PassedCases}/{m_TotalCases} cases passed");
    }

    // ── Cases ────────────────────────────────────────────────────────────

    // Create N entities, mark each disabled BEFORE playback, then verify the
    // DisabledTag landed (IsEnabled == false) once they exist.
    private void Case_SpawnDisabled(int n)
    {
        using var ecb = new EntityCommandBuffer(initialCapacity: 64 * 1024);
        for (int i = 0; i < n; i++)
        {
            EntityRef e = ecb.CreateWith<NativeTransform2D>();
            ecb.SetEnabled(e, false);
        }

        int created = ecb.Playback();
        if (created != n)
            throw new InvalidOperationException($"Playback created {created}, expected {n}");

        for (int i = 0; i < created; i++)
        {
            Entity e = ecb.GetCreatedEntity(i);
            if (!e) throw new InvalidOperationException($"entity #{i} invalid after playback");
            if (e.IsEnabled) throw new InvalidOperationException($"entity #{i} should be disabled");
        }
        DestroySpawned(ecb, created);
    }

    // Spawn enabled, then disable the now-live entities through a second ECB
    // using Entity.ToCommandBufferRef() (the live-scene-ref path).
    private void Case_DisableLive(int n)
    {
        using var spawnEcb = new EntityCommandBuffer(initialCapacity: 64 * 1024);
        for (int i = 0; i < n; i++)
            spawnEcb.CreateWith<NativeTransform2D>();
        int created = spawnEcb.Playback();
        if (created != n)
            throw new InvalidOperationException($"spawn Playback created {created}, expected {n}");

        using (var disableEcb = new EntityCommandBuffer(initialCapacity: 64 * 1024))
        {
            for (int i = 0; i < created; i++)
            {
                Entity e = spawnEcb.GetCreatedEntity(i);
                if (e.IsEnabled == false)
                    throw new InvalidOperationException($"entity #{i} unexpectedly already disabled");
                disableEcb.SetEnabled(e.ToCommandBufferRef(), false);
            }
            disableEcb.Playback();
        }

        for (int i = 0; i < created; i++)
        {
            Entity e = spawnEcb.GetCreatedEntity(i);
            if (e.IsEnabled) throw new InvalidOperationException($"live entity #{i} should be disabled");
        }
        DestroySpawned(spawnEcb, created);
    }

    // disable then re-enable the same ref in one batch — playback applies in
    // stream order, so the final enable must win.
    private void Case_ReEnableLastWins()
    {
        using var ecb = new EntityCommandBuffer(initialCapacity: 1024);
        EntityRef e = ecb.CreateWith<NativeTransform2D>();
        ecb.SetEnabled(e, false);
        ecb.SetEnabled(e, true);

        int created = ecb.Playback();
        if (created != 1)
            throw new InvalidOperationException($"Playback created {created}, expected 1");

        Entity spawned = ecb.GetCreatedEntity(0);
        if (!spawned.IsEnabled)
            throw new InvalidOperationException("entity should be enabled (re-enable must win)");
        DestroySpawned(ecb, created);
    }

    private void Case_ParallelSpawnDisabled(int n)
    {
        using var ecb = new EntityCommandBuffer(initialCapacity: 64 * 1024);
        var job = new SpawnDisabledJob { Ctx = JobContext.ForFrame(ecb.AsParallelWriter(), baseSeed: 1u) };
        Job.ScheduleParallelFor(job, length: n).Complete();

        if (ecb.EntityCount != n)
            throw new InvalidOperationException($"recorded entity count {ecb.EntityCount}, expected {n}");

        int created = ecb.Playback();
        if (created != n)
            throw new InvalidOperationException($"Playback created {created}, expected {n}");

        for (int i = 0; i < created; i++)
        {
            Entity e = ecb.GetCreatedEntity(i);
            if (!e) throw new InvalidOperationException($"entity #{i} invalid after playback");
            if (e.IsEnabled) throw new InvalidOperationException($"entity #{i} should be disabled");
        }
        DestroySpawned(ecb, created);
    }

    // ── Harness helpers ──────────────────────────────────────────────────

    private void Case(string name, Action body)
    {
        m_TotalCases++;
        try
        {
            body();
            m_PassedCases++;
            Log.Info($"[EcbSetEnabledSample] PASS  {name}");
        }
        catch (Exception ex)
        {
            Log.Error($"[EcbSetEnabledSample] FAIL  {name}: {ex.Message}");
        }
    }

    private static void DestroySpawned(EntityCommandBuffer ecb, int count)
    {
        for (int i = 0; i < count; i++)
        {
            Entity e = ecb.GetCreatedEntity(i);
            if (e) e.Destroy();
        }
    }
}
