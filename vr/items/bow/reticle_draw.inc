// Hookshot VR: The game's flat bow reticle hidden in first person: its textures found by their whole compressed
// mip, only their sampled views replaced.
//
// A piece of gpu/vk/vk_draw.cpp (the draw replay), included there at its VR HOOK: it uses that file's types and helpers
// (Targets, NvnState, Program, stream_copy, logf, ...). Not compiled on its own.
// Reticle assets are identified by their ENTIRE compressed mip, not by shape or shader. Replacing only their sampled
// views preserves other UI textures even when they share a batch. Zero-swizzle views are owned by the texture cache.
static VkImageView vr_reticle_view(Texture* tex, VkImageView original) {
    if (!tex || tex->format!=VK_FORMAT_BC4_UNORM_BLOCK || tex->width!=32 ||
        (tex->height!=32 && tex->height!=64) || tex->levels!=1 || tex->layers!=1 || !vr::native_bow_reticle_hidden()) return original;
    ParLock lock;
    static std::unordered_map<uint32_t,bool> matches;
    auto found=matches.find(tex->id);
    if (found==matches.end()) {
        uint8_t raw[1024]; const size_t n=tex->height==64 ? 1024 : 512;
        if (!ub_read(tex->gpu,raw,n)) return original; // retry a temporarily unavailable upload
        uint64_t hash=14695981039346656037ull;
        for (size_t i=0;i<n;i++) hash=(hash^raw[i])*1099511628211ull;
        const bool match=hash==0xb21e07c5b9ac6e60ull || hash==0xa818490e80c585d4ull ||
                         hash==0x307aa2d484a23300ull || hash==0x71e624b24e9934d8ull;
        if (matches.size()>4096) matches.clear();
        found=matches.emplace(tex->id,match).first;
        if (match) logf("vr: identified original bow reticle texture #%u %ux%u hash %016llx",tex->id,tex->width,tex->height,(unsigned long long)hash);
    }
    if (!found->second) return original;
    constexpr uint64_t key=0x565252455449434cull; // private sampled all-zero component mapping
    if (auto view=tex->views.find(key);view!=tex->views.end()) return view->second;
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image=tex->image;ci.viewType=VK_IMAGE_VIEW_TYPE_2D;ci.format=tex->format;
    ci.components={VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO};
    ci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView view=VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device,&ci,nullptr,&view)!=VK_SUCCESS) return original;
    tex->views.emplace(key,view);
    return view;
}
