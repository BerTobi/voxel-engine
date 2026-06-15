# Voxel Engine Architecture Document — Design Phase

I'm designing a voxel-based survival/progression game in C with OpenGL, targeting a Dell XPS M170 (2005) running Windows XP. I need you to produce a comprehensive architecture document — **no code yet**, just design decisions, data structures sketched in pseudocode, and the reasoning behind them.

## Core Design Premise

The game's central fantasy is **emergent technological progression through voxel material interaction**. Unlike Minecraft, items and machines are not magic objects with hardcoded functions. A furnace is not a "furnace block that smelts" — it's a cavity of stone voxels containing a heat source, where heat actually propagates through materials based on their thermal properties, metal voxels actually melt at their melting points, liquid metal pools and resolidifies, etc. Game mechanics emerge from physics rather than being scripted on top.

The closest reference is **Noita's cellular automaton**, but in 3D, at Minecraft-scale, with an industrial/progression focus rather than alchemy.

## World Scale

- A few square kilometers, not thousands. Roughly 2048×2048 footprint × 256 vertical (~1m voxels)
- ~1 billion voxels total, only thousands actively simulated at any moment
- Single-player, no networking concerns

## Hard Hardware Constraints

Target machine: Dell XPS M170
- **CPU**: Pentium M 780 — single core, 2.26GHz, no hyperthreading. **The simulation must be single-threaded.**
- **GPU**: GeForce Go 7800 GTX (G70), 256MB GDDR3. **OpenGL 2.0 + reliable ARB extensions only.** No compute shaders, no geometry shaders, no UBOs, no VAOs. GLSL 1.10/1.20 only.
- **RAM**: up to 2GB DDR2, 32-bit Windows XP. Process gets ~2GB virtual address space practically.
- **OS**: Windows XP SP3

These constraints are non-negotiable and should drive architectural decisions throughout. The constraint is a feature — it forces good discipline.

## Toolchain

Cross-compile from a modern Linux/Windows development machine, deploy binaries to the M170. The architecture doc should address:
- Recommended cross-compilation toolchain (likely MinGW-w64 targeting i686-w64-mingw32, but justify)
- C standard target (C99 is probably the ceiling that MSVC on XP and old MinGW agree on)
- Build system (Make? CMake? Something simpler?)
- How to test/profile remotely on the target machine
- Static linking strategy to avoid DLL hell on XP

## What the Document Must Cover

### 1. System architecture overview
High-level component diagram and data flow. What are the major subsystems and how do they communicate? Memory ownership model.

### 2. Voxel data model
- Per-voxel struct layout (target: 4 bytes, justify if different)
- Global material property table (`MaterialDef`) — what properties matter for emergence
- Chunk size decision (defend 16³ vs alternatives given the hardware)
- World addressing: how chunk coordinates map to memory, hash table vs flat array vs paging

### 3. Cellular automaton simulation
- Active voxel tracking — how voxels wake up and go to sleep
- Tick ordering and determinism (does order matter? double-buffer or in-place?)
- Heat propagation algorithm (this is the first interaction to design, since most mechanically rich)
- Fluid simulation approach (falling sand style? something cheaper?)
- State transitions (melting, solidifying, evaporation)
- Per-frame simulation budget and how to enforce it

### 4. Meshing
- Greedy meshing is mandatory — describe the algorithm at a high level
- Dirty-chunk tracking and remesh budget per frame
- Mesh data layout in VBO (interleaved attributes, how to encode material ID per vertex)
- Memory budget: how much CPU-side mesh data, how much VRAM

### 5. Rendering pipeline
- GL 2.0 forward renderer design
- Frustum culling approach
- Draw call batching strategy without instancing
- Texture atlasing for materials (G70 has limited texture units)
- Lighting model — what's actually feasible (ambient + directional, vertex AO?)
- Liquid/transparent rendering pass

### 6. Frame scheduling
- Single-threaded cooperative time-slicing of sim/mesh/render per frame
- Target framerate justification (30fps vs 60fps tradeoff on this hardware)
- What gets cut when the frame budget overruns

### 7. Memory budget
- Concrete allocation table — how the ~2GB process space is divided
- Loaded chunk window — how many chunks live in RAM at once around the player
- Chunk eviction/streaming strategy (saves to disk? regenerate from seed?)

### 8. World persistence
- Save format for modified chunks
- Procedural generation vs stored chunks
- How material composition of structures persists across save/load

### 9. Progression layer
- How "discoveries" are tracked when mechanics emerge from physics rather than recipes
- What the player-facing progression actually looks like given an emergent system
- This is the most open part of the design — I want your thinking, not just a checklist

### 10. Risk register
- What are the 5 biggest technical risks given these constraints?
- For each, what's the contingency plan?

## Style and Tone

- Be opinionated. I want defended decisions, not menus of options.
- When you suggest a number (chunk size, active voxel limit, memory budget), justify it from the hardware.
- Sketch data structures in C-style pseudocode where it clarifies the design.
- Flag anywhere you're uncertain or where a decision should be deferred until prototyping reveals more.
- Do not write the engine itself. This is architecture only. Code samples should be illustrative struct definitions or short algorithm sketches, not implementations.

Length: as long as it needs to be. This is a foundational document I'll return to repeatedly.
