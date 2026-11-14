# Vulkan GS renderer (Android first, later also PC/Linux)

Goal: the GS pixel pipeline on the phone's GPU, like the PC's Direct3D 11.3 renderer (gs_gpu.cpp), so
the CPU only runs the game (EE), VU1 and the GS front end. Measured need (Xiaomi 17, software
renderer): the first village scene draws 4-7 Mpixels per frame and falls to 4-20 fps; 80% of the
CPU time is in shade_t/sample_t.

## Device facts (Xiaomi 17, Adreno 840, Vulkan 1.4, vk_probe.cpp)

- VK_EXT_fragment_shader_interlock: NO (the PC's ROV path has no direct equivalent)
- VK_EXT_rasterization_order_attachment_access: color, depth, stencil: YES
- VK_KHR_dynamic_rendering(+_local_read), VK_KHR_push_descriptor, VK_EXT_extended_dynamic_state: yes
- R32_UINT: color attachment, storage, atomics: yes

## Design

- Same memory model as gs_gpu.cpp (shadow copy, per-block owner/shadow_ok/stamps, surfaces with
  cells, decoded texture cache, refresh/readback, batching rules, HUD transform). Kept as close to
  gs_gpu.cpp as possible so the two can later share one logic file with two backends.
- Surfaces: VkImage R32_UINT at S x native, usage color attachment | input attachment | sampled |
  storage | transfer.
- Ordered read-modify-write: the frame and z surfaces are color attachments that are also input
  attachments of the same subpass, with VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_
  ACCESS_BIT_EXT. The draw shader reads the current pixel with subpassLoad and writes the outputs
  (discard = keep). On tile GPUs this stays in tile memory.
- Render pass variants: frame only / frame + z (z must cover the frame surface size).
  Framebuffers cached per (frame, z) pair.
- A render pass instance stays open across batches while the targets stay the same; it ends for
  refresh/readback/copies of those surfaces, a batch sampling its own target (snapshot copy), a
  target change and present. (Ending it per batch would load/store whole attachments each time.)
- Descriptors: push descriptors per batch (texture, CLUT buffer, HD texture, mip levels).
- Refresh (shadow -> surface): staging buffer -> native scratch image -> compute upscale.
  Readback (surface -> shadow): compute downsample -> buffer -> wait for the fence (as D3D's Map).
- Present: composition pass (port of PRESENT_HLSL ps_merge) into the swapchain image, then the
  touch overlay (CPU-drawn RGBA texture) blended on top.
- Frames in flight: 2, each with its vertex ring, uniform ring, descriptor/command pools, fence.
- Shaders: GLSL ports of DRAW_HLSL / UTIL_HLSL / PRESENT_HLSL, compiled to SPIR-V at build time with
  the NDK's glslc (toolchain/sdk/ndk/29/shader-tools), embedded as arrays.
- Desktop later: VK_EXT_fragment_shader_interlock + storage images (the D3D design) as a second
  path in the same file, selected at init.

## Implementation details (decided after reading gs_gpu.cpp completely)

- File: runtime/android/gs_vk.cpp implements gs::gpu:: (replaces the stubs in platform_android.cpp);
  logic sections copied from gs_gpu.cpp (layouts, per-block state, surfaces/cells, refresh,
  readback, decoded textures, batching in on_draw, HUD transform, CLUT/upload/copy, state_vram).
- Images: all surfaces stay in VK_IMAGE_LAYOUT_GENERAL (attachment + input attachment feedback loop,
  sampling, storage, transfer): only memory/execution barriers, no layout transitions.
- Draw shader (GLSL port of DRAW_HLSL ps_main): subpassLoad(fb_in/zb_in) instead of fb[P]/zb[P];
  BOTH outputs are always written (old value where the ROV code skipped a write); "return before
  any write" becomes discard. Specialization constant HAS_Z: pipeline variant without z attachment.
  Pipeline flag VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT,
  subpass flag ..._RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT.
- Render pass instance open while (frame surface, z surface) stay the same. It ends only for:
  refresh/grow/snapshot of surfaces (rare after warm-up), readbacks (mid-frame submit + fence wait),
  target change, present. At every end: a full memory barrier.
- Things that must NOT end the pass:
  - decoded texture uploads: a texture that was already used this frame gets a NEW image (versions),
    uploads go into a pre-pass command buffer submitted before the main one
  - CLUT updates: written into a host-visible ring, the descriptor points at the new offset
  - per-batch constants: host-visible ring (UBO offset), vertices: host-visible ring
- Descriptors: VK_KHR_push_descriptor per batch (UBO, tex, CLUT, mips 1-6, HD texture, input
  attachments). Textures read with texelFetch through immutable nearest samplers.
- Presentation in GPU mode: gs_vk composes the PCRTC merge into an output image and hands it to
  vk_ctx (new entry: present an image instead of CPU pixels, same rectangle logic + overlay), on the
  renderer thread. The movie black-bar crop needs a CPU look at the picture: later (small readback).

## Progress

- 2026-11-02:
  - step 1 DONE (vk_ctx.cpp: device, swapchain at the window size with identity transform; SUBOPTIMAL is ignored, it comes every frame because the system rotates; picture + premultiplied touch overlay)
  - shaders DONE and compiling (draw.vert, draw.frag + HAS_Z variant SPV_DRAW_Z_FRAG, upscale.comp, downsample.comp, merge.frag, full.vert)
  - NEXT: gs_vk.cpp (steps 2-4)
  - vk_ctx needs accessors for the renderer (device, queue, memory types) and a present-an-image entry (wait on the renderer's semaphore)
- 2026-11-08:
  - gs_vk.cpp WRITTEN (steps 2-4), builds; NOT yet run on the phone (no adb device). Replaces the gpu stubs; Android default
    is now renderer = 1, internal_scale = 0 (automatic = x2 for now), keys renderer/internal_scale/sharp_text in grandia3.ini
  - vk_ctx: enables VK_EXT_rasterization_order_attachment_access + VK_KHR_push_descriptor, accessors, queue mutex
    (every submit/present/wait idle), present_image (GPU picture in GENERAL layout, same rectangles + overlay)
  - command buffers per frame (2 in flight): "pre" (texture uploads, UNDEFINED -> GENERAL of new images) and "main",
    submitted together; full memory barrier at the start/end of each and around every copy/compute outside a pass
  - one host-visible ring (48 MB) per frame for vertices, draw constants, CLUT copies, uploads; ring_reserve before
    allocations that must stay together (a full ring = submit + wait + restart)
  - readbacks: downsample -> native scratch -> buffer, submit + wait (mid-frame); the frame's resources are recycled then
  - decoded texture sampled by the open main buffer and decoded again: new image (old one deferred), counted "new versions"
  - movies: the composed picture is copied to a host buffer per frame and read 2 frames later for the letterbox detection
  - --shots in GPU mode: synchronous readback of the composed picture (platform::interval_shot_due / write_shot)
  - the SW HUD hooks (hud_sw.cpp) are switched off when the GPU renderer starts (it narrows the HUD itself; else twice)
  - not ported: HD textures (texrep), --gpu-timing/--gpu-profile, --frame-hashes, comparison mode
- 2026-11-08 (phone runs):
  - Adreno 840 does NOT keep the order between draws of one subpass despite the raster order flag: a self-dependency
    in the render pass + a BY_REGION barrier (color write -> input attachment read) after every draw fixes it
    (--gpu-compare x1: from ~200k to ~28k differing pixels per frame, rest = interpolation rounding (1-3 levels)
    and alpha-test edges in foliage, plus the HUD that only the GPU narrows). Within a draw the order holds.
  - --gpu-compare works (S=1; frame pairs as shots), --gpu-timing (timestamps per frame; the value includes waiting
    for presentation: same 36 ms at x1/x2/x3-ish, so not fill rate), --env=NAME=VALUE in args.txt
    (GRANDIA_VK_NO_DRAW_BARRIER, GRANDIA_VK_NO_MIPS); test switches --uav-overlap = a pass per batch,
    --frame-hashes = a draw per primitive (far too slow to use)
  - village (PC savestate Spiel/savestates/slot1.g3s, unpacked: XPRESS is Windows-only): SW 4.7 fps, GPU 10 -> 20 fps after
    removing system calls: SetEvent only with waiters (win32_shim), GS pipeline worker wakes the game only once the
    tail reaches its target (gspipe.cpp wait_tail), the renderer thread is woken every 256 draws (gs_draw.cpp seq_notify)
  - bottleneck now: the GS pipeline thread (VU1 AOT code 65%) at ~90% of a prime core (cpu6); the game sits at
    20 fps (every 3rd vblank); 30 needs the VU1 path ~1/3 faster. -O3 -march=armv8.2-a for generated/vu (G3_VU_FLAGS) gave nothing
  - VU1 on ARM: _mm_movemask_epi8/_mm_movemask_ps/_mm_cvttps_epi32 of the SSE shim were scalar lane loops; vsat (after
    almost every VU op) and mac_of call them. NEON versions (g3_sse.h, checked bit-exact on the phone by
    tools/neon_shim_test.cpp, 9M cases): village 20 -> 30 fps (the game's target there). GS pipeline thread still ~84%
    of a prime core at 30 fps (VU AOT code 70% of it): little headroom for weaker phones
  - VU1 headroom: NEON vsat (min of |x| bits with 0x7F7FFFFF, no branch) and mac_of (weighted lane sum) in
    runtime/android/include/g3_vu_neon.h, used by vu_internal.h on aarch64 (bit-exact, 15M cases on the phone);
    VIF unpack buffer no longer zero-filled per UNPACK. GS pipeline thread 84% -> 72-75% at 30 fps in the village.
    Profile method: G3_VU_FLAGS=-gline-tables-only build, simpleperf --sort symbol,vaddr_in_file, llvm-symbolizer
    --inlining per address (scratch script vu_lines.py) -> innermost source line and generated VU line.
    Remaining in the thread: VU code ~50% (MAC flag queue aot_push_mac/aot_slot ~15% of the thread), GS front end
    (gif::process, vertex_kick, draw_primitive, write_packed) ~20%, VIF unpack ~7%.
    Bigger steps left: fewer MAC pushes in tools/vu_recomp.py (flags nobody reads), or the GS front end on its own thread.
  - Fewer MAC pushes (tools/vu_recomp.py): JR/JALR targets resolved for the flag liveness (JALR: its constant, JR via a
    register only written by call links: the call return points), and "end MAC free": no run of the image can read
    the flags the previous run left (no MAC read reachable from an entry before an own push is visible, no status
    users) -> the latest push before the program end needs no commit. Village image b53ff08b26f584a9: live pushes
    88 -> 7. --vu-verify (interpreter vs translation per run) skips MAC/status at the end for such images
    (register_aot_end_mac_free in vu.cpp); 0 mismatches in the village. GS pipeline thread ~73% -> 66-70% at 30 fps.
    NOTE: generated/ is copied from the PC by sync_from_pc.sh: rerun
    `python tools/vu_recomp.py tools/vuimg/vu1img_<hash>.bin generated/vu/vu1_<hash>.cpp` after a sync
    (or regenerate on the PC with this vu_recomp.py once the branches merge).
    Now in the thread: VU code ~44%, GS front end ~30% (draw_primitive, write_packed, vertex_kick, gif::process,
    enqueue_draw, setup memcmp), VIF unpack ~11%. Next lever: the GS front end on its own thread.
  - GS front end thread (gspipe.cpp, second ring): the pipeline worker keeps VIF1/VU1 and forwards GIF packets
    (PATH1/2/3, copied), GIF/GS register writes, vblank and re-present in order; the front end thread runs GIF
    parsing, GS registers, primitive setup and presentation. sync() drains both stages; the worker stays at most one
    vblank ahead of the front end (one more frame in flight). post_intc accepts both threads. Android only
    (gsp::enable_front_end in platform_android.cpp; GRANDIA_NO_GS_FE=1 switches it off); the PC keeps one thread.
    Found on the way: CreateThread in win32_shim returned the pthread_t as thread id (now pthread_gettid_np), so
    post_intc never recognised the pipeline worker and raised VIF1 interrupts from the wrong thread.
    Village at 30 fps: worker 66-70% -> 45-48%, front end 30-33%, game waits 33-40% -> 9-11%.
    Checked: --gpu-compare unchanged (~27.9k rounding pixels), savestate save/load in the village, boot/intro/title
    (title matches SW; its black areas are the game's design, the earlier "broken" title shots were its fade-in).
  - Battles (2026-11-09): PC savestates of the battle tutorial (port/build/release slot7, unpacked with g3s_unpack.py)
    run at 59-60 fps (battles run at 60): VU1 worker ~51%, front end ~30%, EE idle ~45%, GPU ~16 ms per frame.
    --gpu-compare: 3D picture identical, differences only in the narrowed HUD (and the command-wheel spotlight that
    belongs to it). Other test states (test/deck_textmix/*) are field scenes; slot1 there is the old text-mix repro:
    its broken window is baked into the saved RAM (the 0D FF 00 fix is in text_hook.cpp), not a new bug.
  - Night forest field (deck_textmix/slot1_c): 27.4 fps, GPU-bound (36 ms/frame; x1 26 ms, x3 56 ms).
    New --gpu-profile (timestamps around passes, refresh copies/uploads, snapshots; per second the split and the 8
    slowest passes with size and draw count): 10.6 ms per frame were refresh copies. The game reads its 512x512
    buffer at 0x2a00 in another layout (lay 4 written, lay 1 read; a depth-based effect), and the renderer moved it with
    one vkCmdCopyImage per run of 2 cells (~5000 cells per frame). Now refresh collects the cells and does one compute
    dispatch per source surface (shaders/cellcopy.comp, cell list in the ring): 10.6 -> 0.5 ms, scene at 30 fps,
    --gpu-compare unchanged (HUD only). Remaining: one 1024x960 pass with ~46 draws takes ~18 ms (fill/shader cost).
    CPU side of the renderer thread in that scene: uploads of unchanged texture data every frame. gs.cpp
    exec_upload_rows_m writes CT32/CT16/T8 in runs inside a block (tools/upload_fast_test.cpp: bit-exact against the
    per-pixel version), on_upload keeps the stamp of blocks whose content did not change (no rehash later):
    upload 16% -> 8%, rehash 2225 -> 894 per second. tools/build_android.py: G3_RT_FLAGS (e.g. -gline-tables-only).
  - old NEXT: VU1 thread on ARM (profile the AOT code / split VU1 and GS front end), title screen check (it was broken
    before the draw barrier fix, not rechecked); old note: first phone run (log: "gpu: Vulkan renderer active", stats line "passes"), screenshots vs SW renderer,
    fps in the village scene at x2 and x3; then validation layer run if something is off

## Steps

1. Vulkan device + swapchain presenting the software renderer's frame + touch overlay (replaces the
   CPU blit; testable on its own).
2. Surfaces, refresh/readback, uploads, CLUT, decoded textures.
3. Draw shader + batches (render pass reuse), self-sampling snapshots, mips.
4. Present composition (PCRTC merge, both circuits), widescreen HUD transform (from gs_gpu.cpp).
5. HD textures (optional on phones), sharp text, profiling.
