// (OoT VR, research) RX_VR_PIPE_LOG=1: each graphics pipeline asked of the driver, and each new pipeline entry, logged with
// everything its key is made from, so two runs' pipelines can be told apart field by field (the pipeline-key stability
// question: the same draw keyed differently from run to run, or a pipeline keyed the same and still compiled again).
// Off by default; included by vk_draw.cpp after the pipeline key's helpers (key_inputs, pipeline_key).
//
// Lines (one pipeline: its "pipe build" and "pipe state" lines share their number):
//   vr: pipe new <key> fresh <0|1> predicted <0|1> own <0|1> prog <p.key> variant <p.variant>
//       a pipeline entry made by a draw (graphics_pipeline): fresh 1 = the key was not in hookshot_pipelines.bin (new to this
//       install), own 1 = keyed by its program object (an address: never the same in another run)
//   vr: pipe build #<n> <draw|worker> <ask|quick|full|lib<part>> r <VkResult> how <0-3> fb <feedback flags> <ms> ms key ...
//       one build_graphics call: draw = a thread that records draws (blocking it), worker = a job thread; how = UseCounters::
//       made_how (1 the driver's own cache, 2 the start's cache, 3 compiled); fb bit 1 = the application's cache had it;
//       why (a draw's readiness check, -1 elsewhere): 0 the draw may be left out, 1 its targets are not redrawn, 2 it writes
//       memory, 4 RX_VK_PIPE_SKIP=0, 3 the second half of a routed draw that may be left out (the first that applies); 5 the
//       both-eyes check's ask for a second half
//   vr: pipe state #<n> ...: the key's fields, the VR salt's parts and each stage's SPIR-V hash (handed, and its program's own)

#ifdef HOOKSHOT_VR_RESEARCH   // (research builds only: a player's build has the stubs at the end)
thread_local bool vr_pipe_draw_thread = false;   // (set by graphics_pipeline: this thread records draws)
thread_local int vr_pipe_why = -1;   // (the draw's readiness check: why its build may block, logged as "why"; -1 elsewhere)
inline bool vr_pipe_log_on() {
    static const bool on = vr_research_on("RX_VR_PIPE_LOG");
    return on;
}
/// (graphics_pipeline) a new entry: its key, and whether the notes had it
inline void vr_pipe_log_new(uint64_t key, bool fresh, bool predicted, bool own, const Program& p) {
    logf("vr: pipe new %016llx fresh %d predicted %d own %d prog %016llx variant %016llx", (unsigned long long)key, (int)fresh,
         (int)predicted, (int)own, (unsigned long long)p.key, (unsigned long long)p.variant);
}
/// (build_graphics, after the driver answered)
inline void vr_pipe_log_build(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, int part, bool ask,
                              bool quick, VkResult r, const VkGraphicsPipelineCreateInfo& ci, const VkPipelineCreationFeedback& fb,
                              double ms) {
    static std::atomic<int64_t> seq{0};
    const int64_t n = ++seq;
    const KeyInputs k = key_inputs(p, s, at, topo);
    const uint64_t key = pipeline_key(p, s, at, topo);
    char kind[8];
    if (part >= 0) snprintf(kind, sizeof kind, "lib%d", part);
    else snprintf(kind, sizeof kind, "%s", ask ? "ask" : quick ? "quick" : "full");
    logf("vr: pipe build #%lld %s %s r %d how %d fb %x %.1f ms key %016llx prog %016llx variant %016llx fp %016llx obj %llx "
         "mesh %d fallback %d flags %x layout %llx why %d",
         (long long)n, vr_pipe_draw_thread ? "draw" : "worker", kind, (int)r, (int)use_counters.made_how, (unsigned)fb.flags, ms,
         (unsigned long long)key, (unsigned long long)p.key, (unsigned long long)p.variant, (unsigned long long)p.fp,
         (unsigned long long)(uintptr_t)&p, (int)p.mesh, (int)p.fallback, (unsigned)ci.flags,
         (unsigned long long)(uintptr_t)ci.layout, vr_pipe_why);
    char b[900];
    int o = snprintf(b, sizeof b, "topo %llu samples %llu n %d", (unsigned long long)k.topo_class, (unsigned long long)k.samples, k.n);
    const auto add = [&](const char* fmt, auto... v) {
        if (o > 0 && o < (int)sizeof b) o += snprintf(b + o, sizeof b - o, fmt, v...);
    };
    for (int i = 0; i < k.n; i++) add(" c%d %llu/%016llx", i, (unsigned long long)k.color_format[i], (unsigned long long)k.blend[i]);
    add(" depth %llu blend_en %llx wmask %llx poly %llu clamp %llu a2c %llu", (unsigned long long)k.depth_format,
        (unsigned long long)k.blend_en, (unsigned long long)k.write_masks, (unsigned long long)k.poly_mode,
        (unsigned long long)k.depth_clamp, (unsigned long long)k.a2c);
    if (k.vertex_input) {
        add(" loads %x nvattr %d", k.loads, k.nvattr);
        for (int i = 0; i < 16; i++)
            if (k.loads >> i & 1) add(" a%d %llx", i, (unsigned long long)k.vattr[i]);
        for (int i = 0; i < 16; i++)
            if (k.divisor[i]) add(" d%d %llu", i, (unsigned long long)k.divisor[i]);
    }
    uint64_t sb = 0;
    for (const uint64_t w : at.eyes_sample_bindings) sb = mix(sb, w);
    uint32_t hud;
    std::memcpy(&hud, &at.hud, 4);
    add(" | eyes %d mask %d samp %016llx sbind %016llx cam %016llx hud %x kx %016llx salt %016llx", (int)at.eyes,
        (int)vr_eyes_mask(at), (unsigned long long)at.eyes_sampling, (unsigned long long)sb, (unsigned long long)at.eyes_cam, hud,
        (unsigned long long)vr::eyes_key(), (unsigned long long)vr_eyes_salt(at));
    add(" | mods");
    for (uint32_t i = 0; i < ci.stageCount; i++) {
        uint64_t own = 0;   // (its program's own module, before any VR patch)
        for (const Stage& st : p.stages)
            if (st.vk_stage == ci.pStages[i].stage) own = vr::module_words_hash(st.module);
        const uint64_t handed = vr::module_words_hash(ci.pStages[i].module);
        add(" %x:%016llx", (unsigned)ci.pStages[i].stage, (unsigned long long)handed);
        if (own != handed) add("<%016llx", (unsigned long long)own);
    }
    logf("vr: pipe state #%lld %s", (long long)n, b);
}
#else
thread_local bool vr_pipe_draw_thread = false;
thread_local int vr_pipe_why = -1;
constexpr bool vr_pipe_log_on() { return false; }
inline void vr_pipe_log_new(uint64_t, bool, bool, bool, const Program&) {}
inline void vr_pipe_log_build(const Program&, const NvnState&, const Targets&, VkPrimitiveTopology, int, bool, bool, VkResult,
                              const VkGraphicsPipelineCreateInfo&, const VkPipelineCreationFeedback&, double) {}
#endif
