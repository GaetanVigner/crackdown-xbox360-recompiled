<#
.SYNOPSIS
  HLE renderer milestone 1: compares a game-side D3D device capture with the GPU command processor's draw trace.

.DESCRIPTION
  F8 with crackdown_hle_capture on writes logs\hle_game_<n>.bin (src/hle_capture.cpp: the device shadow after every
  D3D draw call, one record per DRAW packet) and logs\hle_gpu_<n>.bin (gpu_draw_trace, SDK patch 0106: the registers
  the command processor sees for every DRAW it executes). Both are "GDTR" version 2 files with the same records.

  Draws are paired by the DRAW packet's guest physical address and draw initiator; every tile of a predicated replay
  pairs with the same game draw. Each GPU frame is aligned with the game frame that shares the most packet addresses
  (the game submits ahead of the GPU). Then the shaders (dword count and first eight ucode dwords) and every register
  the game side knows are compared. Expected differences are reported apart: VGT_INDX_OFFSET (never shadowed),
  the per-tile registers of predicated draws, fetch slot 31 (BeginVertices' UP fetch) and float constants (shader
  literal constants are loaded by the GPU from the shader object, not the shadow).

.EXAMPLE
  .\tools\hle-trace-compare.ps1 1          # build\logs\hle_game_1.bin vs hle_gpu_1.bin
  .\tools\hle-trace-compare.ps1 -Game a.bin -Gpu b.bin
#>
param(
    [int]$Index = 0,
    [string]$Game,
    [string]$Gpu,
    [int]$Top = 30
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Game -or -not $Gpu) {
    $logs = Join-Path $root 'build\logs'
    if ($Index -le 0) {
        $last = Get-ChildItem $logs -Filter 'hle_game_*.bin' | Sort-Object LastWriteTime | Select-Object -Last 1
        if (-not $last) { throw "No hle_game_*.bin in $logs" }
        $Index = [int]($last.BaseName -replace '^hle_game_', '')
    }
    $Game = Join-Path $logs "hle_game_$Index.bin"
    $Gpu = Join-Path $logs "hle_gpu_$Index.bin"
}

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

public sealed class TraceRecord {
    public uint Kind, Frame, Sequence, Packet, Initiator, IndexBase, IndexCount, IndexFormat, Predicated, BinSelect, BinMask, Killed;
    public uint VsDwords, PsDwords;
    public uint[] VsHead = new uint[8], PsHead = new uint[8];
    public uint[] Registers;  // all blocks, concatenated
}

public sealed class Trace {
    public List<KeyValuePair<uint, uint>> Blocks = new List<KeyValuePair<uint, uint>>();
    public List<TraceRecord> Draws = new List<TraceRecord>();
    // Command buffers replayed with RunCommandBuffer (game side): [start, end) physical ranges.
    public List<KeyValuePair<uint, uint>> IndirectBuffers = new List<KeyValuePair<uint, uint>>();
    public int Frames;
    public int RegisterCount;

    public static Trace Load(string path) {
        var trace = new Trace();
        using (var reader = new BinaryReader(new BufferedStream(File.OpenRead(path), 1 << 20))) {
            if (Encoding.ASCII.GetString(reader.ReadBytes(4)) != "GDTR") throw new InvalidDataException(path + ": not a draw trace");
            uint version = reader.ReadUInt32();
            if (version != 2) throw new InvalidDataException(path + ": version " + version + ", expected 2");
            uint blocks = reader.ReadUInt32();
            for (uint i = 0; i < blocks; ++i) {
                uint first = reader.ReadUInt32(), count = reader.ReadUInt32();
                trace.Blocks.Add(new KeyValuePair<uint, uint>(first, count));
                trace.RegisterCount += (int)count;
            }
            long length = reader.BaseStream.Length;
            while (reader.BaseStream.Position + 12 <= length) {
                uint kind = reader.ReadUInt32();
                uint frame = reader.ReadUInt32();
                uint sequence = reader.ReadUInt32();
                if (kind == 3) { trace.Frames = (int)frame + 1; continue; }
                if (kind == 4) {  // RunCommandBuffer replay: physical address, dwords
                    uint address = reader.ReadUInt32(), dwords = reader.ReadUInt32();
                    trace.IndirectBuffers.Add(new KeyValuePair<uint, uint>(address, address + dwords * 4));
                    continue;
                }
                var r = new TraceRecord { Kind = kind, Frame = frame, Sequence = sequence };
                r.Packet = reader.ReadUInt32(); r.Initiator = reader.ReadUInt32(); r.IndexBase = reader.ReadUInt32();
                r.IndexCount = reader.ReadUInt32(); r.IndexFormat = reader.ReadUInt32(); r.Predicated = reader.ReadUInt32();
                r.BinSelect = reader.ReadUInt32(); r.BinMask = reader.ReadUInt32(); r.Killed = reader.ReadUInt32();
                r.VsDwords = reader.ReadUInt32(); for (int i = 0; i < 8; ++i) r.VsHead[i] = reader.ReadUInt32();
                r.PsDwords = reader.ReadUInt32(); for (int i = 0; i < 8; ++i) r.PsHead[i] = reader.ReadUInt32();
                r.Registers = new uint[trace.RegisterCount];
                byte[] bytes = reader.ReadBytes(trace.RegisterCount * 4);
                if (bytes.Length < trace.RegisterCount * 4) break;  // truncated tail
                Buffer.BlockCopy(bytes, 0, r.Registers, 0, bytes.Length);
                trace.Draws.Add(r);
            }
        }
        return trace;
    }

    // Register index of position i in Registers.
    public uint RegisterAt(int i) {
        foreach (var block in Blocks) { if (i < block.Value) return block.Key + (uint)i; i -= (int)block.Value; }
        return 0;
    }
}

public static class TraceCompare {
    const uint Unknown = 0xCDCDCDCD;
    static ulong Key(TraceRecord r) { return ((ulong)r.Packet << 32) | r.Initiator; }
    static readonly HashSet<uint> TileRegisters = new HashSet<uint> { 0x2000, 0x2104, 0x2203, 0x2080, 0x2081, 0x2082, 0x2208, 0x2180, 0x2181 };

    static string Category(uint reg, TraceRecord gpu) {
        if (reg == 0x2102) return "VGT_INDX_OFFSET (never shadowed)";
        if (gpu.Predicated != 0 && TileRegisters.Contains(reg)) return "per-tile register of a predicated draw";
        if (reg >= 0x48BA && reg <= 0x48BF) return "fetch slot 31 (UP vertex fetch)";
        if (reg >= 0x4000 && reg < 0x4400) return "VS float constant";
        if (reg >= 0x4400 && reg < 0x4800) return "PS float constant";
        if (reg >= 0x4800 && reg < 0x48C0) return "fetch constant";
        if (reg >= 0x4900 && reg < 0x4928) return "bool/loop constant";
        return "render state";
    }

    public static string Run(Trace game, Trace gpu, int top) {
        var o = new StringBuilder();
        o.AppendFormat("Game side: {0} draws in {1} frames. GPU side: {2} draws/resolves in {3} frames.\n",
            game.Draws.Count, game.Frames, gpu.Draws.Count, gpu.Frames);
        if (game.RegisterCount != gpu.RegisterCount) return o.Append("Register blocks differ between the two files.\n").ToString();

        // Game draws by frame and key.
        var gameByFrame = new Dictionary<uint, Dictionary<ulong, TraceRecord>>();
        foreach (var r in game.Draws) {
            if (r.Packet == 0) continue;
            Dictionary<ulong, TraceRecord> frame;
            if (!gameByFrame.TryGetValue(r.Frame, out frame)) gameByFrame[r.Frame] = frame = new Dictionary<ulong, TraceRecord>();
            if (!frame.ContainsKey(Key(r))) frame[Key(r)] = r;
        }
        int unparsedGame = game.Draws.Count(r => r.Packet == 0);

        // Align each GPU frame with the game frame sharing the most keys.
        var alignment = new Dictionary<uint, uint>();
        foreach (var frame in gpu.Draws.GroupBy(r => r.Frame)) {
            var keys = new HashSet<ulong>(frame.Where(r => r.Kind == 1).Select(Key));
            uint best = uint.MaxValue; int bestHits = -1;
            foreach (var g in gameByFrame) {
                int hits = keys.Count(k => g.Value.ContainsKey(k));
                if (hits > bestHits) { bestHits = hits; best = g.Key; }
            }
            alignment[frame.Key] = best;
            o.AppendFormat("GPU frame {0}: {1} distinct draw packets, aligned with game frame {2} ({3} shared).\n",
                frame.Key, keys.Count, best, bestHits);
        }

        int gpuDraws = 0, resolves = 0, matched = 0, unmatched = 0, killed = 0, inReplayedBuffers = 0;
        int vsMismatch = 0, psMismatch = 0, vsUnknown = 0, psUnknown = 0, cleanDraws = 0;
        var usedGame = new HashSet<TraceRecord>();
        var mismatchCount = new Dictionary<uint, int>();
        var mismatchExample = new Dictionary<uint, string>();
        var categoryCount = new Dictionary<string, int>();
        var unmatchedSamples = new List<string>();
        var unmatchedByInitiator = new Dictionary<string, int>();
        var unmatchedByRegion = new Dictionary<uint, int>();
        foreach (var r in gpu.Draws) {
            if (r.Kind == 2) { ++resolves; continue; }
            ++gpuDraws;
            if (r.Killed != 0) ++killed;
            Dictionary<ulong, TraceRecord> frame; TraceRecord g;
            uint aligned;
            if (!alignment.TryGetValue(r.Frame, out aligned) || !gameByFrame.TryGetValue(aligned, out frame) || !frame.TryGetValue(Key(r), out g)) {
                ++unmatched;
                uint packet = r.Packet;
                if (game.IndirectBuffers.Any(b => packet >= b.Key && packet < b.Value)) ++inReplayedBuffers;
                // Primitive type, source select and whether the packet is predicated; the count is left out.
                string shape = string.Format("prim {0,2} source {1} predicated {2}", r.Initiator & 0x3F, (r.Initiator >> 6) & 3, r.Predicated);
                int s; unmatchedByInitiator.TryGetValue(shape, out s); unmatchedByInitiator[shape] = s + 1;
                int a; unmatchedByRegion.TryGetValue(r.Packet >> 20, out a); unmatchedByRegion[r.Packet >> 20] = a + 1;
                if (unmatchedSamples.Count < 10)
                    unmatchedSamples.Add(string.Format("    GPU frame {0} #{1}: packet {2:X8} initiator {3:X8} predicated {4}", r.Frame, r.Sequence, r.Packet, r.Initiator, r.Predicated));
                continue;
            }
            ++matched; usedGame.Add(g);
            if (g.VsDwords == 0) ++vsUnknown;
            else if (g.VsDwords != r.VsDwords || !g.VsHead.SequenceEqual(r.VsHead)) ++vsMismatch;
            if (g.PsDwords == 0) ++psUnknown;
            else if (g.PsDwords != r.PsDwords || !g.PsHead.SequenceEqual(r.PsHead)) ++psMismatch;
            bool clean = true;
            for (int i = 0; i < r.Registers.Length; ++i) {
                uint expected = g.Registers[i];
                if (expected == Unknown || expected == r.Registers[i]) continue;
                uint reg = gpu.RegisterAt(i);
                string category = Category(reg, r);
                int c; categoryCount.TryGetValue(category, out c); categoryCount[category] = c + 1;
                if (category != "render state" && category != "fetch constant" && category != "bool/loop constant") continue;
                clean = false;
                int n; mismatchCount.TryGetValue(reg, out n); mismatchCount[reg] = n + 1;
                if (!mismatchExample.ContainsKey(reg))
                    mismatchExample[reg] = string.Format("device {0:X8} vs GPU {1:X8} (packet {2:X8})", expected, r.Registers[i], r.Packet);
            }
            if (clean) ++cleanDraws;
        }
        int gameNotExecuted = game.Draws.Count(r => r.Packet != 0 && alignment.ContainsValue(r.Frame) && !usedGame.Contains(r));

        o.AppendFormat("\nGPU draws: {0} ({1} killed by a visibility query), resolves: {2}.\n", gpuDraws, killed, resolves);
        o.AppendFormat("Paired with a device draw: {0} ({1:F1}%). Unpaired GPU draws: {2}. Device draws in aligned frames never executed: {3}. Device calls without a parsed packet: {4}.\n",
            matched, gpuDraws > 0 ? 100.0 * matched / gpuDraws : 0, unmatched, gameNotExecuted, unparsedGame);
        o.AppendFormat("Of the unpaired GPU draws, {0} lie in the {1} command buffer replays (RunCommandBuffer) the game side logged.\n",
            inReplayedBuffers, game.IndirectBuffers.Count);
        o.AppendFormat("Shaders: VS differ in {0}, PS differ in {1} (device shader unknown: VS {2}, PS {3}).\n", vsMismatch, psMismatch, vsUnknown, psUnknown);
        o.AppendFormat("Paired draws whose render state, fetch and bool/loop constants all match: {0} ({1:F1}%).\n",
            cleanDraws, matched > 0 ? 100.0 * cleanDraws / matched : 0);
        o.Append("\nDiffering registers by category (register-draw pairs):\n");
        foreach (var c in categoryCount.OrderByDescending(p => p.Value)) o.AppendFormat("  {0,10}  {1}\n", c.Value, c.Key);
        o.AppendFormat("\nTop {0} differing registers (render state, fetch, bool/loop):\n", top);
        foreach (var m in mismatchCount.OrderByDescending(p => p.Value).Take(top))
            o.AppendFormat("  0x{0:X4}  {1,8} draws  e.g. {2}\n", m.Key, m.Value, mismatchExample[m.Key]);
        if (unmatchedSamples.Count > 0) {
            o.Append("\nUnpaired GPU draws by shape:\n");
            foreach (var p in unmatchedByInitiator.OrderByDescending(p => p.Value).Take(12)) o.AppendFormat("  {0,8}  {1}\n", p.Value, p.Key);
            o.Append("Unpaired GPU draws by packet region (physical address >> 20, in MB):\n");
            foreach (var p in unmatchedByRegion.OrderByDescending(p => p.Value).Take(12)) o.AppendFormat("  {0,8}  0x{1:X3}xxxxx\n", p.Value, p.Key);
            o.Append("Unpaired GPU draws (first 10):\n"); foreach (var s in unmatchedSamples) o.AppendLine(s);
        }
        return o.ToString();
    }
}
'@

Write-Host "Game side: $Game"
Write-Host "GPU side:  $Gpu"
$gameTrace = [Trace]::Load($Game)
$gpuTrace = [Trace]::Load($Gpu)
Write-Host ([TraceCompare]::Run($gameTrace, $gpuTrace, $Top))
