// Hookshot VR: Controller markers and the bow reticle artwork swapchain.
// Included by vr/headset/vk_vr.cpp in the order shown there; not a separate compilation unit.

/// Static controller markers and a transparent bracket reticle. No game artwork is substituted or claimed extracted here.
bool make_marker(int h) {
    Eye& m = ctl.marker[h];
    if (ctl.marker_ready[h]) return true;
    if (!headset_gpu_free()) return false;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = swap_format;
    ci.sampleCount = 1;
    const uint32_t size = h==2 ? 64 : 4;
    ci.width = ci.height = size;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(s.session, &ci, &m.chain))) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(m.chain, 0, &n, nullptr);
    m.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(m.chain, n, &n, (XrSwapchainImageBaseHeader*)m.images.data());
    m.w = m.h = size;
    uint32_t index = 0;
    SwapTake take;
    // (a static image is acquired once: a failed one's chain is let go, made again at the next try, not left behind each frame)
    const auto drop_chain = [&] { xrDestroySwapchain(m.chain), m = Eye{}; };
    if (!swap_take(take, {m.chain}, &index, "marker")) return drop_chain(), false;
    if (!headset_record()) return swap_give(take), drop_chain(), false;
    const VkImage img = m.images[index].image;
    barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue colour = h==2 ? VkClearColorValue{{0,0,0,0}} : h==3 ? VkClearColorValue{{.1f,1,.2f,1}} :
        h ? VkClearColorValue{{1.0f, 0.25f, 0.2f, 1.0f}} : VkClearColorValue{{0.2f, 0.5f, 1.0f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
    barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkImageView reticle_view = VK_NULL_HANDLE;
    if (h==2) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image=img;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=(VkFormat)swap_format;
        vi.subresourceRange=range;
        if (vkCreateImageView(dev.device,&vi,nullptr,&reticle_view)!=VK_SUCCESS) {
            vkEndCommandBuffer(cmd);
            return swap_give(take), drop_chain(), false;
        }
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView=reticle_view;attachment.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent={64,64};rendering.layerCount=1;
        rendering.colorAttachmentCount=1;rendering.pColorAttachments=&attachment;
        vkCmdBeginRendering(cmd,&rendering);
        // Original BowPointer and BowPointerTen masks, extracted from the user's Common.bntx, with their own shadows.
        // Compose once, in premultiplied alpha. Runtime draw-distance contraction is independent of this static art.
        const auto sample=[](const auto& mask,int x,int y) -> int {
            return x>=0 && x<32 && y>=0 && y<(int)(sizeof(mask)/sizeof(mask[0])) ? mask[y][x] : 0;
        };
        const auto pixel=[&](int x,int y) {
            int white=sample(art::BowPointerTen_00,16+(x-32)*2,16+(y-32)*2);
            int shadow=sample(art::BowPointerTenSh_00,16+(x-32)*2,16+(y-32)*2);
            for (int side=0;side<4;side++) {
                const int cx=side<2 ? 32 : side==2 ? 10 : 54;
                const int cy=side>=2 ? 32 : side==0 ? 10 : 54;
                const int dx=x-cx,dy=y-cy;
                const int sx=16+2*(side<2 ? dx : dy),sy=32+2*(side<2 ? dy : dx);
                white=std::max(white,sample(art::BowPointer_00,sx,sy));
                shadow=std::max(shadow,sample(art::BowPointerSh_00,sx,sy));
            }
            return std::pair{(white+8)/17,(shadow+8)/17};
        };
        for (int y=0;y<64;y++) for (int x=0;x<64;) {
            const auto [white,shadow]=pixel(x,y);
            int end=x+1;while (end<64 && pixel(end,y)==std::pair{white,shadow}) ++end;
            if (white || shadow) {
                VkClearAttachment ink{};ink.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
                const float a=white/15.0f,b=shadow/15.0f;
                ink.clearValue.color={{a,a,a,a+(1-a)*b*.75f}};
                VkClearRect rect{{{x,y},{(uint32_t)(end-x),1}},0,1};
                vkCmdClearAttachments(cmd,1,&ink,1,&rect);
            }
            x=end;
        }
        vkCmdEndRendering(cmd);
    }
    // (the reticle's view destroyed once the submit is done, whenever that is: before, a failed one's leaked)
    const bool ok = headset_submit(take, VK_NULL_HANDLE, 0, "marker", [reticle_view] {
        if (reticle_view) vkDestroyImageView(dev.device, reticle_view, nullptr);
    });
    ctl.marker_ready[h] = ok;
    if (h==2) bow_art_ready.store(ok,std::memory_order_release);
    if (ok) logf("vr: marker %d ready",h);
    return ok;
}

/// The marker of hand h as a quad layer in its grip space (false: not shown this frame)
bool hand_marker(int h, XrCompositionLayerQuad& q) {
    // (Show hands is the VR menu's Debug tab's: without HOOKSHOT_VR_DEBUG=1 the tab is hidden, and an options file's
    // show_hands=1 from an earlier build draws nothing, since no row could turn it off)
    static const bool debug_tab = env_on("HOOKSHOT_VR_DEBUG");
    if (!debug_tab || !opts.vr_show_hands || !ctl.attached || !ctl.now[h].valid || !make_marker(h)) return false;
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.space = ctl.grip_space[h];
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = ctl.marker[h].chain;
    q.subImage.imageRect.extent = {4, 4};
    q.pose.orientation = {-0.7071068f, 0, 0, 0.7071068f};   // (lying in the hand's palm plane, facing up)
    q.size = {0.05f, 0.05f};
    return true;
}
