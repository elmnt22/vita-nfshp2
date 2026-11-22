// Shared CPU frame preparation (Gather) for the Vulkan and vitaGL Storage.
// Moved verbatim from ecstasye.cpp; it records API-neutral VkVertex/VkDrawBatch.
namespace {
// Native presentation of an explicitly selected centred 640x480 layout.
// Ordinary race HUD/menu placement does not enter this adaptation.
std::array<float,2> nativeUiCanvasScale(float width,float height)
{
    Require(std::isfinite(width) && std::isfinite(height) && width>0.0f && height>0.0f,
        "Native UI canvas has no finite viewport");
    const double horizontal=static_cast<double>(width)/640.0;
    const double vertical=static_cast<double>(height)/480.0;
    const double uniform=std::min(horizontal,vertical);
    return {static_cast<float>(uniform/horizontal),static_cast<float>(uniform/vertical)};
}
}

// Diagnostic (sampled frames): solid-draw gather steps, milliseconds summed
// since the last read: 0 preamble, 1 shading, 2 resident acquire, 3 rest of
// the solid branch, 4 batch append, 5 non-solid draws; [6] = sampled frames.
// [10] template continuation total, [11] its resident acquire (one call in
// eight timed), [12] timed continuations, [13] model runs, [14] run strips,
// [15] run strips continued through the template.
std::array<double,24> nativeGatherStepMilliseconds{};
// gather_detail: [16..20] sampled cached-scenery first-strip steps (texture+raster, acquire,
// packet, shading, rest), [21] timed calls; snapshot taken at Gather start.
std::array<double,24> nativeGatherStepBefore{};
// Diagnostic: generic solid draws by kind (plain, reflection, specular,
// shadow), resident misses, resident re-prepares.
std::array<double,6> nativeSolidKinds{};
std::array<double,12> nativeBatchBreaks{};
// Last Gather's phases (ms): validation, command loop, vertex jobs, indices.
std::array<double,4> nativeGatherFrameMs{};
// ecstasyGatherDetail: this Gather's category split (only the two category arrays are used).
EcstasyVulkanRenderProfile nativeGatherDetail;

void EcstasyVulkanDevice::Storage::Gather(const EcstasyFrame& frame,
    std::vector<VkVertex>& output,std::vector<std::uint32_t>& elementIndices,
    std::vector<VkDrawBatch>& batches)
{
    // BeginGather also identifies validation performed during overlap
    // preflight. The sealed frame cannot mutate between that check and here.
    nativeGatherResidentEvents={};
    residentGeometry.Begin(gatherSerial);residentIndexCache.Begin(gatherSerial);
    residentFrameVertices=0;residentFrameIndices=0;gatheredResidentIndexSpans.clear();gatheredResidentSpanTokens.clear();
    nativeStripJobs.clear();nativeWorkRanges.clear();nativeFrameShading.Clear();
    using Clock=std::chrono::steady_clock;
    auto& profile=statistics.renderProfile;
    profile.shadowStrips={};profile.shadowTriangles={};
    const bool sampleShadowGather=platform::nativeDiagnosticsEnabled && nextSubmission%16u==0;
    profile.shadowGatherSamples=sampleShadowGather ? 1u:0u;
    profile.shadowGatherMilliseconds={};
    const auto nativeWorkers=NativeSceneEffectiveHardwareConcurrency();
    const auto validateStarted=Clock::now();
    residentGeometry.ValidateSources(frame.Commands(),frame.SceneryRunStrips(),nativeWorkers);
    // Per-strip copies of run commands that leave the cached scenery path.
    // Stable addresses: generic jobs keep pointers until this Gather ends.
    sceneryRunFallbacks.clear();
    const auto loopStarted=Clock::now();
    nativeGatherFrameMs[0]=std::chrono::duration<double,std::milli>(loopStarted-validateStarted).count();
    profile.gatherValidationMilliseconds+=nativeGatherFrameMs[0];
    const bool directSourcePreparation=nativeWorkers<=1;
    std::size_t nativePendingWork=0;
    // Cold raw uploads use the shared native pool after ordered state
    // capture. Warm source validation above needs no vertex repacking.
    // There is no private renderer thread pool and no CPU vertex shader.
    if(!directSourcePreparation)nativeStripJobs.reserve(frame.Commands().size());

    std::array<float,2> textureLod{},lastDepthRange{};
    std::array<float,5> lastDepthProjection{};bool depthRangeValid=false;
    std::uint32_t glyphBoundsIndex{};
    auto appendValidated=[&](const VkVertex& vertex) {
        Require(output.size()<std::numeric_limits<std::uint32_t>::max(),
            "Vulkan frame vertex count exceeds32-bit index range");
        const auto index=static_cast<std::uint32_t>(output.size());
        output.push_back(vertex);output.back().lod=textureLod;output.back().glyphBoundsIndex=glyphBoundsIndex;
        return index;
    };
    auto append=[&](const VkVertex& vertex) {
        // Validate every component before publishing a vertex. Construct
        // diagnostics only on failure; Require's std::string parameter
        // otherwise allocates eleven temporary strings per emitted vertex.
        for(auto value:vertex.position)if(!std::isfinite(value))throw std::runtime_error("Nonfinite original vertex position");
        for(auto value:vertex.uvq)if(!std::isfinite(value))throw std::runtime_error("Nonfinite original texture coordinate");
        for(auto value:vertex.color)if(!std::isfinite(value))throw std::runtime_error("Nonfinite original vertex color");
        return appendValidated(vertex);
    };
    // A sealed frame borrows one stable texture identity/descriptor per
    // source. Validate through the normal upload path on its first use for
    // each clamp context; repeated strips then need no associative lookup.
    // This scratch cache never crosses Gather, owns no resource and keeps
    // separate clamp/repeat indices for authored NPOT textures. Indices stay
    // valid when the texture vector grows; retirement happens before Gather.
    struct FrameTextureResolution {
        const TextureInfo* source{};
        std::array<std::size_t,2> indices{};
        std::uint8_t contexts{};
    };
    std::array<FrameTextureResolution,256> frameTextures{};
    const auto resolveTexture=[&](const TextureInfo& texture,bool forcedClamp) {
        const auto identity=reinterpret_cast<std::uintptr_t>(&texture);
        const auto slot=((identity>>4)^(identity>>12)^(identity>>20))&(frameTextures.size()-1);
        auto& resolved=frameTextures[slot];
        const auto context=static_cast<std::size_t>(forcedClamp);
        const auto mask=static_cast<std::uint8_t>(1u<<context);
        if(resolved.source==&texture && (resolved.contexts&mask))return resolved.indices[context];
        const auto key=NativeTextureKey(texture,forcedClamp);
        auto found=textureIndices.find(key);
        if(found==textureIndices.end()) {
            const auto index=textures.size();textures.emplace_back();
            found=textureIndices.emplace(key,index).first;
            UploadTexture(texture,index,key.repeatedPadding);textures[index].validatedGather=gatherSerial;
        } else if(textures[found->second].validatedGather!=gatherSerial) {
            const auto index=found->second;
            ++statistics.textureValidations;
            if(!textures[index].Matches(texture))UploadTexture(texture,index,key.repeatedPadding);
            textures[index].validatedGather=gatherSerial;
        }
        if(resolved.source!=&texture) {resolved.source=&texture;resolved.contexts=0;}
        resolved.indices[context]=found->second;resolved.contexts|=mask;
        return found->second;
    };
    // Texture prewarm: the ordinary resolve/upload path, ahead of first draw.
    if(PrewarmAllowed())for(const auto& [texture,forcedClamp]:frame.PrewarmTextures()) {
        const auto before=textures.size();
        (void)resolveTexture(*texture,forcedClamp);
        if(platform::nativeDiagnosticsEnabled) {
            char text[128];std::snprintf(text,sizeof(text),"key=%08x ptr=%p clamp=%d new=%d",
                texture->descriptor->key,static_cast<const void*>(texture),forcedClamp ? 1:0,textures.size()!=before ? 1:0);
            platform::RecordSessionEvent("render.texture_prewarm_resolve",text);
        }
    }
    if(PrewarmAllowed())EcstasyConfirmTexturePrewarm(frame.PrewarmTextures());
    // Logical targets can share one physical attachment. Use the original
    // source identity and keep the raster cache local to this Gather.
    struct TargetRaster {
        const EcstasyRenderTarget* source{};
        VkViewport originalViewport{},viewport{};
        VkRect2D scissor{};
        float displayWidth{},displayHeight{};
        std::uint32_t outputWidth{},outputHeight{};
        std::array<double,2> translation{},clipScale{{1.0,1.0}},clipOffset{};
    };
    std::array<TargetRaster,15> targetRasters{};
    const auto rasterFor=[&](const EcstasyRenderTarget& target,std::int32_t destination) -> const TargetRaster& {
        auto& raster=targetRasters[static_cast<std::size_t>(target.id)];
        if(raster.source==&target)return raster;
        raster={};
        raster.source=&target;
        auto& viewport=raster.viewport;
        // Source targets are authored in the original display coordinates;
        // viewport scaling maps them to the selected native output resolution.
        raster.displayWidth=static_cast<float>(target.width);
        raster.displayHeight=static_cast<float>(target.height);
        const auto* attachment=destination ? &renderedTargets.at(destination):nullptr;
        raster.outputWidth=attachment ? attachment->width:options.width;
        raster.outputHeight=attachment ? attachment->height:options.height;
        const auto outputWidth=raster.outputWidth,outputHeight=raster.outputHeight;
        // Offscreen attachments may have a native mirror/reflection extent.
        // Source vertices and camera matrices remain in authored coordinates.
        const float sx=static_cast<float>(outputWidth)/
            (attachment ? raster.displayWidth:static_cast<float>(options.logicalWidth));
        const float sy=static_cast<float>(outputHeight)/
            (attachment ? raster.displayHeight:static_cast<float>(options.logicalHeight));
        viewport={static_cast<float>(target.left)*sx,static_cast<float>(target.top)*sy,
            raster.displayWidth*sx,raster.displayHeight*sy,0.0f,1.0f};
        raster.originalViewport=viewport;
        const auto left=std::clamp(static_cast<std::int32_t>(viewport.x),0,static_cast<std::int32_t>(outputWidth));
        const auto top=std::clamp(static_cast<std::int32_t>(viewport.y),0,static_cast<std::int32_t>(outputHeight));
        const auto right=std::clamp(static_cast<std::int32_t>(viewport.x+viewport.width),left,static_cast<std::int32_t>(outputWidth));
        const auto bottom=std::clamp(static_cast<std::int32_t>(viewport.y+viewport.height),top,static_cast<std::int32_t>(outputHeight));
        raster.scissor={{left,top},{static_cast<std::uint32_t>(right-left),static_cast<std::uint32_t>(bottom-top)}};
        // Display calibration translates the final raster, not the rear
        // image a second time before that image is composited.
        if(!destination)raster.translation=frame.DisplayTranslation();
        const auto& translation=raster.translation;
        if(translation[0]!=0.0 || translation[1]!=0.0) {
            // Shift the final display rectangle, then crop it to the actual
            // target before forming Vulkan integers or viewport coordinates.
            // A huge persisted word can move a view fully offscreen without
            // overflowing a cast or violating viewportBoundsRange.
            const double x=static_cast<double>(viewport.x)+translation[0]*outputWidth;
            const double y=static_cast<double>(viewport.y)+translation[1]*outputHeight;
            const double width=viewport.width,height=viewport.height;
            const double l=std::clamp(x,0.0,static_cast<double>(outputWidth));
            const double t=std::clamp(y,0.0,static_cast<double>(outputHeight));
            const double r=std::clamp(x+width,0.0,static_cast<double>(outputWidth));
            const double b=std::clamp(y+height,0.0,static_cast<double>(outputHeight));
            if(r<=l || b<=t) {raster.scissor={};return raster;}
            const double w=r-l,h=b-t;
            // Homogeneous affine conversion into that cropped viewport.
            // Its image is exactly the translated original view intersected
            // with the client area; z/w, UV and source colors stay intact.
            raster.clipScale={{width/w,height/h}};
            raster.clipOffset={{(2.0*(x-l)+width-w)/w,(2.0*(y-t)+height-h)/h}};
            viewport={static_cast<float>(l),static_cast<float>(t),static_cast<float>(w),
                static_cast<float>(h),viewport.minDepth,viewport.maxDepth};
            const auto il=static_cast<std::int32_t>(std::floor(l));
            const auto it=static_cast<std::int32_t>(std::floor(t));
            const auto ir=static_cast<std::int32_t>(std::ceil(r));
            const auto ib=static_cast<std::int32_t>(std::ceil(b));
            raster.scissor={{il,it},{static_cast<std::uint32_t>(ir-il),static_cast<std::uint32_t>(ib-it)}};
        }
        return raster;
    };
    const auto appendBatch=[&](const VkDrawBatch& batch) {
        if(!batches.empty() && batch.Continues(batches.back())) {
            batches.back().count+=batch.count;
            batches.back().nativeSpanCount+=batch.nativeSpanCount;
        } else {
            if(!batches.empty()) {
                // Why adjacent draws stay separate (diagnostic only).
                const auto& previous=batches.back();
                std::size_t reason=5;
                if(previous.texture!=batch.texture)reason=0;
                else if(!batch.SameDrawState(previous))reason=1;
                else if(previous.first+previous.count!=batch.first ||
                    (batch.residentGeometry && previous.nativeSpanFirst+previous.nativeSpanCount!=batch.nativeSpanFirst))reason=2;
                else if(batch.nativeLocalToClip!=previous.nativeLocalToClip || batch.nativeClipMapping!=previous.nativeClipMapping ||
                    batch.nativeTextureQScale!=previous.nativeTextureQScale)reason=3;
                else if(batch.nativeShadingIndex!=previous.nativeShadingIndex)reason=4;
                nativeBatchBreaks[reason+(batch.cachedOpaqueScenery ? 6:0)]+=1.0;
            }
            batches.push_back(batch);
        }
    };
    const auto applyDepthRange=[&](const EcstasyDrawCommand& draw,VkDrawBatch& batch) {
        const auto* projection=draw.ClipToScreen().data();
        const std::array<float,5> key{{projection[10],projection[11],projection[14],projection[15],
            static_cast<float>(draw.target->format)}};
        if(!depthRangeValid || key!=lastDepthProjection) {
            lastDepthRange=ecstasyNativeDepthRange(projection,draw.target->format);
            lastDepthProjection=key;depthRangeValid=true;
        }
        batch.viewport.minDepth=lastDepthRange[0];batch.viewport.maxDepth=lastDepthRange[1];
    };
    std::array<const EcstasyDrawInstance*,1024> validatedSceneryInstances{};
    // Template of the last scenery packet (all fields but its index range),
    // reused by later strips of the same run command: same texture, target,
    // material state, transform and shading, so only geometry differs.
    VkDrawBatch sceneryTemplate;const EcstasyDrawCommand* sceneryTemplateDraw=nullptr;
    // batches.size() right after the template's last append: batches.back()
    // then has the template's draw state (appendBatch only merges equal
    // state), so a contiguous next strip extends it without the compare.
    std::size_t sceneryTemplateBatches=0;
    if(ecstasyGatherDetail)nativeGatherStepBefore=nativeGatherStepMilliseconds;
    bool timeScenery=false;std::uint32_t sceneryTimeIndex=0;Clock::time_point sceneryMark{};
    const auto sceneryStep=[&](std::size_t index) {
        if(!timeScenery)return;
        const auto now=Clock::now();
        nativeGatherStepMilliseconds[index]+=std::chrono::duration<double,std::milli>(now-sceneryMark).count();
        sceneryMark=now;
    };
    const auto prepareSceneryPacket=[&](const EcstasyDrawCommand& draw,const EditorMarkerSolidGeometry::Strip* runStrip) {
        sceneryTemplateDraw=nullptr;
        timeScenery=ecstasyGatherDetail && (sceneryTimeIndex++&7u)==0;
        if(timeScenery) {sceneryMark=Clock::now();nativeGatherStepMilliseconds[21]+=1.0;}
        if(draw.kind!=EcstasyDrawKind::SolidStrip || !draw.instance || !runStrip ||
            !draw.texture || draw.sampledTarget || draw.orderingTexture ||
            draw.carShadow!=EcstasyCarShadow::None || !draw.target ||
            draw.target->id<0 || draw.target->id>14 ||
            draw.target->width<=0 || draw.target->height<=0)return false;
        const bool clamp=NativeDrawClampsTexture(draw);
        const auto textureIndex=resolveTexture(*draw.texture,clamp);
        auto& texture=textures[textureIndex];
        const auto destination=draw.target->id>=7 ? draw.target->id:0;
        const auto& raster=rasterFor(*draw.target,destination);
        if(!raster.scissor.extent.width || !raster.scissor.extent.height)return false;
        const auto& strip=*runStrip;
        const auto state=draw.nativeRenderEggState |
            (static_cast<std::uint32_t>(draw.texture->drawFlag4Enabled)<<3) |
            (static_cast<std::uint32_t>(draw.target->id==14)<<4);
        // A first-strip record exists only for a revisioned strip whose
        // scroll byte was zero and whose extents passed the checks below;
        // the unchanged strip epoch (no revisioned strip restamped, reset,
        // reassigned or destroyed) and texture identity/generation keep both.
        VkSceneryFirstRecord* first=nullptr;bool firstHit=false;
        VkResidentGeometryEntry* resident=nullptr;
        if(ecstasySceneryFirstFast) {
            if(sceneryFirstRecords.empty())sceneryFirstRecords.resize(8192);
            const auto id=reinterpret_cast<std::uintptr_t>(runStrip);
            first=&sceneryFirstRecords[((id>>4)^(id>>13))&(sceneryFirstRecords.size()-1)];
            firstHit=first->strip==runStrip && first->epoch==residentGeometry.FrameEpoch() &&
                first->texture==draw.texture && first->state==state && first->clamp==clamp &&
                first->textureIndex==textureIndex && first->textureGeneration==texture.generation &&
                (resident=residentGeometry.ReacquireFast(first->slot,first->serial))!=nullptr;
        }
        Require(std::isfinite(draw.textureQScale),"Native solid texture Q scale is nonfinite");
        if(!firstHit) {
            const auto* wire=draw.texture->descriptor->authoredRecord;
            // Scrolling materials retain the normal dynamic preparation path.
            if(wire[0x79])return false;
            const auto count=strip.positions.size();
            Require(strip.textureCoordinates.size()==count && strip.colors.size()==count,
                "Original strip vertex/color/UV extents differ");
            Require(count<=48,"Vulkan strip exceeds the original packet vertex extent");
            if(draw.drawFlags&0xc0u)
                Require(draw.LightContext() && draw.lightMaterial && strip.normals.size()==count,
                    "Lit strip lacks its source context, material or signed normal payload");
        } else if(draw.drawFlags&0xc0u)
            Require(draw.LightContext() && draw.lightMaterial,"Lit strip lacks its source context or material");
        const auto identity=reinterpret_cast<std::uintptr_t>(draw.instance);
        auto& validated=validatedSceneryInstances[((identity>>4)^(identity>>12))&
            (validatedSceneryInstances.size()-1)];
        if(validated!=draw.instance) {
            Require(std::all_of(draw.LocalToClip().begin(),draw.LocalToClip().end(),
                [](float value){return std::isfinite(value);}),"Native solid projection matrix is nonfinite");
            validated=draw.instance;
        }
        VkSolidStripJob job;job.draw=&draw;job.strip=runStrip;
        job.lineTopology=(draw.nativeRenderEggState&EcstasyRenderEggWireframe)!=0;
        sceneryStep(16);
        bool prepare=false;
        VkDrawBatch batch;
        if(firstHit) {
            {
                batch=first->packet;job.count=static_cast<std::uint8_t>(first->count);
                ++nativeSceneryFirstCounters[0];
                if(ecstasySceneryFirstFast==2) {
                    // Verify: the ordinary checks, Acquire and packet lookup must agree.
                    VkSolidStripJob check=job;bool checkPrepare=false;
                    check.lod={{draw.textureQScale,static_cast<std::uint8_t>(strip.autoK)*0.0625f}};
                    const auto count=strip.positions.size();
                    const bool extents=!draw.texture->descriptor->authoredRecord[0x79] && strip.nativeVertexRevision &&
                        strip.textureCoordinates.size()==count && strip.colors.size()==count && count<=48 &&
                        (!(draw.drawFlags&0xc0u) || strip.normals.size()==count);
                    auto* expected=residentGeometry.Acquire(check,checkPrepare);
                    const auto* cached=expected ? sceneryPackets.Find(*expected,texture.generation,state):nullptr;
                    const bool same=extents && expected==resident && !checkPrepare && check.count==job.count && cached &&
                        cached->SameDrawState(batch) && cached->count==batch.count && cached->nativeSpanCount==batch.nativeSpanCount &&
                        cached->residentGeometry==batch.residentGeometry && cached->nativeGpuPosition==batch.nativeGpuPosition;
                    ++nativeSceneryFirstCounters[2];
                    if(!same)++nativeSceneryFirstCounters[3];
                    if((nativeSceneryFirstCounters[2]&16383u)==0 || (!same && nativeSceneryFirstCounters[3]<=4)) {
                        char text[128];std::snprintf(text,sizeof(text),"checks=%llu mismatches=%llu",
                            (unsigned long long)nativeSceneryFirstCounters[2],(unsigned long long)nativeSceneryFirstCounters[3]);
                        platform::RecordSessionEvent("render.scenery_first_verify",text);
                    }
                }
                ++statistics.sceneryPacketHits;
            }
        }
        if(!resident) {
        job.lod={{draw.textureQScale,static_cast<std::uint8_t>(strip.autoK)*0.0625f}};
        resident=residentGeometry.Acquire(job,prepare);
        sceneryStep(17);
        if(!resident)return false;
        if(const auto* cached=sceneryPackets.Find(*resident,texture.generation,state)) {
            batch=*cached;
            ++statistics.sceneryPacketHits;
        } else {
            const auto& material=ResolveOpaqueSceneryMaterial(texture,draw.texture->descriptor->authoredRecord,options.textureFilter);
            batch.indexed=true;batch.cachedOpaqueScenery=true;batch.residentGeometry=true;
            batch.depth=true;batch.depthWrite=material.depthWrite;batch.blend=material.blend;
            batch.blendConstant=material.blendConstant;batch.lodScale=material.lodScale;
            batch.textureFunction=material.textureFunction;batch.minification=material.minification;
            batch.alphaTest=material.fullAlpha || draw.texture->drawFlag4Enabled || draw.target->id==14 ? 0:2;
            batch.wireframe=job.lineTopology;
            batch.untextured=(draw.nativeRenderEggState&EcstasyRenderEggTexturing)==0;
            batch.clampTopLeft=NativeDrawClampsTexture(draw);
            batch.nativeGpuPosition=true;batch.count=resident->indexCount;batch.nativeSpanCount=1;
            Require(batch.textureFunction>=0 && batch.textureFunction<=3,"Invalid authored GS texture function");
            sceneryPackets.Store(*resident,texture.generation,state,batch);
            ++statistics.sceneryPacketMisses;
        }
        if(first && strip.nativeVertexRevision) {
            *first={runStrip,draw.texture,texture.generation,resident->serial,residentGeometry.FrameEpoch(),
                textureIndex,resident->slot,state,static_cast<std::uint32_t>(job.count),clamp,batch};
            ++nativeSceneryFirstCounters[1];
        }
        }
        sceneryStep(18);
        // Frame-local ranges, shading, camera and viewport never enter the cache.
        batch.texture=textureIndex;batch.target=destination;
        batch.viewport=raster.viewport;batch.scissor=raster.scissor;
        applyDepthRange(draw,batch);
        batch.nativeLocalToClip=draw.LocalToClip();
        batch.nativeClipMapping={{static_cast<float>(raster.clipScale[0]),static_cast<float>(raster.clipScale[1]),
            static_cast<float>(raster.clipOffset[0]),static_cast<float>(raster.clipOffset[1])}};
        batch.nativeTextureQScale=draw.textureQScale;
        batch.nativeShadingIndex=nativeFrameShading.Capture(job,gatheredGlyphBounds);
        sceneryStep(19);
        Require(residentFrameIndices+resident->indexCount<=UINT32_MAX,
            "Native resident frame index count exceeds32-bit draw range");
        Require(gatheredResidentIndexSpans.size()<UINT32_MAX,"Native resident index span count exceeds32-bit range");
        batch.first=static_cast<std::uint32_t>(residentFrameIndices);
        batch.nativeSpanFirst=static_cast<std::uint32_t>(gatheredResidentIndexSpans.size());
        gatheredResidentIndexSpans.push_back(resident);gatheredResidentSpanTokens.push_back(resident->indexToken);
        residentFrameIndices+=resident->indexCount;residentFrameVertices+=job.count;
        job.residentOutput=resident->shadow.data();
        job.workUnits=prepare ? NativePendingVertexWork(job):0;
        if(job.workUnits) {
            if(directSourcePreparation)PrepareNativeSolidStripVertices(job,job.residentOutput);
            else {nativePendingWork+=job.workUnits;nativeStripJobs.push_back(job);}
        }
        if(batch.count)appendBatch(batch);
        sceneryTemplate=batch;sceneryTemplateDraw=&draw;sceneryTemplateBatches=batch.count ? batches.size():0;
        sceneryStep(20);
        return true;
    };
    // A later strip of the run whose template was just built. Only the
    // per-strip resident geometry, its validation and its index span differ;
    // the packet cache, raster, depth range and shading resolve identically.
    // Resident entry of the last fast-path continuation (null otherwise):
    // the scenery run replay records it.
    VkResidentGeometryEntry* sceneryLastResident=nullptr;
    const auto continueSceneryRun=[&](const EcstasyDrawCommand& draw,const EditorMarkerSolidGeometry::Strip* runStrip) {
        sceneryLastResident=nullptr;
        if(!ecstasySceneryRunFastPath || sceneryTemplateDraw!=&draw)return prepareSceneryPacket(draw,runStrip);
#if !defined(__vita__) && !defined(__SWITCH__)
        // PC HP2_VERIFY_RUN_FAST=1: take the full path and compare its batch
        // with the template the fast path would reuse (all but index range).
        static const bool verifyRunFast=[] {const char* value=std::getenv("HP2_VERIFY_RUN_FAST");return value && *value=='1';}();
        if(verifyRunFast) {
            const auto expected=sceneryTemplate;
            const bool taken=prepareSceneryPacket(draw,runStrip);
            if(taken) {
                const auto& actual=sceneryTemplate;
                const bool same=actual.SameDrawState(expected) && actual.cachedOpaqueScenery==expected.cachedOpaqueScenery &&
                    actual.nativeLocalToClip==expected.nativeLocalToClip && actual.nativeClipMapping==expected.nativeClipMapping &&
                    actual.nativeTextureQScale==expected.nativeTextureQScale && actual.nativeShadingIndex==expected.nativeShadingIndex &&
                    actual.nativeTransformIndex==expected.nativeTransformIndex;
                static std::uint64_t checks=0,mismatches=0;
                ++checks;if(!same)++mismatches;
                if((checks&16383u)==0) {
                    char text[96];std::snprintf(text,sizeof(text),"checks=%llu mismatches=%llu",
                        (unsigned long long)checks,(unsigned long long)mismatches);
                    platform::RecordSessionEvent("render.run_fast_verify",text);
                }
            }
            return taken;
        }
#endif
        const auto& strip=*runStrip;
        const auto count=strip.positions.size();
        Require(strip.textureCoordinates.size()==count && strip.colors.size()==count,
            "Original strip vertex/color/UV extents differ");
        Require(count<=48,"Vulkan strip exceeds the original packet vertex extent");
        if(draw.drawFlags&0xc0u)
            Require(strip.normals.size()==count,"Lit strip lacks its signed normal payload");
        VkSolidStripJob job;job.draw=&draw;job.strip=runStrip;
        job.lod={{draw.textureQScale,static_cast<std::uint8_t>(strip.autoK)*0.0625f}};
        job.lineTopology=(draw.nativeRenderEggState&EcstasyRenderEggWireframe)!=0;
        bool prepare=false;
        auto* resident=residentGeometry.Acquire(job,prepare);
        if(!resident) {sceneryTemplateDraw=nullptr;return false;}
        sceneryLastResident=resident;
        Require(residentFrameIndices+resident->indexCount<=UINT32_MAX,
            "Native resident frame index count exceeds32-bit draw range");
        Require(gatheredResidentIndexSpans.size()<UINT32_MAX,"Native resident index span count exceeds32-bit range");
        auto& batch=sceneryTemplate;
        batch.count=resident->indexCount;batch.nativeSpanCount=1;
        batch.first=static_cast<std::uint32_t>(residentFrameIndices);
        batch.nativeSpanFirst=static_cast<std::uint32_t>(gatheredResidentIndexSpans.size());
        gatheredResidentIndexSpans.push_back(resident);gatheredResidentSpanTokens.push_back(resident->indexToken);
        residentFrameIndices+=resident->indexCount;residentFrameVertices+=job.count;
        job.residentOutput=resident->shadow.data();
        job.workUnits=prepare ? NativePendingVertexWork(job):0;
        if(job.workUnits) {
            if(directSourcePreparation)PrepareNativeSolidStripVertices(job,job.residentOutput);
            else {nativePendingWork+=job.workUnits;nativeStripJobs.push_back(job);}
        }
        ++statistics.sceneryPacketHits;
        if(batch.count) {
            if(ecstasySceneryFastMerge && sceneryTemplateBatches && sceneryTemplateBatches==batches.size()) {
                auto& previous=batches.back();
                if(previous.first+previous.count==batch.first && previous.nativeSpanFirst+previous.nativeSpanCount==batch.nativeSpanFirst) {
#if !defined(__vita__) && !defined(__SWITCH__)
                    Require(batch.Continues(previous),"Scenery template merge differs from appendBatch");
#endif
                    previous.count+=batch.count;previous.nativeSpanCount+=batch.nativeSpanCount;
                    return true;
                }
            }
            appendBatch(batch);sceneryTemplateBatches=batches.size();
        }
        return true;
    };
    const bool sampleCategories=platform::nativeDiagnosticsEnabled && nextSubmission%32u==0;
    if(sampleCategories)++profile.gatherCategorySamples;
    if(ecstasyGatherDetail) {nativeGatherDetail.gatherCategoryCommands={};nativeGatherDetail.gatherCategoryMilliseconds={};}
    NativeGatherCategoryTimer categoryTimer(sampleCategories ? &profile:ecstasyGatherDetail ? &nativeGatherDetail:nullptr);
    if(sampleCategories)nativeGatherStepMilliseconds[6]+=1.0;
    Clock::time_point stepMark{};
    // Time one draw in eight (scaled at report) so clock reads do not
    // dominate the sampled frame; [7] counts timed segments for calibration.
    std::uint32_t stepDrawIndex=0;bool timeDraw=false;std::uint32_t templateTimeIndex=0;
    const auto step=[&](std::size_t index) {
        if(!timeDraw)return;
        nativeGatherStepMilliseconds[7]+=1.0;
        const auto now=Clock::now();
        nativeGatherStepMilliseconds[index]+=std::chrono::duration<double,std::milli>(now-stepMark).count();
        stepMark=now;
    };
    // Solid strip templates (see ecstasySolidTemplate): small direct-mapped
    // table keyed by instance, so base/specular strips and several models
    // interleaved in one view each keep their own template.
    struct SolidTemplate {
        const EcstasyDrawCommand* draw{};VkDrawBatch batch;
        std::array<float,2> scroll{};float lod1{};
    };
    std::array<SolidTemplate,64> solidTemplates{};
    const auto solidTemplateSlot=[&](const EcstasyDrawCommand& draw)->SolidTemplate& {
        const auto identity=reinterpret_cast<std::uintptr_t>(draw.instance)^
            (reinterpret_cast<std::uintptr_t>(draw.texture)>>2)^static_cast<std::uintptr_t>(draw.kind);
        return solidTemplates[((identity>>4)^(identity>>11))&(solidTemplates.size()-1)];
    };
#if !defined(__vita__) && !defined(__SWITCH__)
    static const bool verifySolidTemplate=[] {const char* value=std::getenv("HP2_VERIFY_SOLID_TEMPLATE");return value && *value=='1';}();
#else
    constexpr bool verifySolidTemplate=false;
#endif
    std::array<float,2> solidScroll{};
    const auto solidTemplateEligible=[&](const EcstasyDrawCommand& draw) {
        return (draw.kind==EcstasyDrawKind::SolidStrip || draw.kind==EcstasyDrawKind::SolidSpecularStrip) &&
            !draw.cachedOpaqueScenery && !draw.sceneryRunCount && draw.strip && !draw.sampledTarget;
    };
    // Per-strip remainder of the solid path for a template hit: geometry
    // acquisition, index range, vertex preparation and append. Mirrors the
    // general solid branch below for everything that depends on the strip.
    const auto continueSolidTemplate=[&](const EcstasyDrawCommand& draw,const SolidTemplate& source,
        const EditorMarkerSolidGeometry::Strip* runStrip=nullptr,VkResidentGeometryEntry** residentOut=nullptr) {
        const bool timeTemplate=sampleCategories && (templateTimeIndex++&7u)==0;
        const auto templateStarted=timeTemplate ? Clock::now():Clock::time_point{};
        struct TemplateTimer {
            bool active;Clock::time_point started;
            ~TemplateTimer() {
                if(!active)return;
                nativeGatherStepMilliseconds[10]+=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
                nativeGatherStepMilliseconds[12]+=1.0;
            }
        } templateTimer{timeTemplate,templateStarted};
        const auto& strip=runStrip ? *runStrip:*draw.strip;
        const auto count=strip.positions.size();
        Require(strip.textureCoordinates.size()==count && strip.colors.size()==count,
            "Original strip vertex/color/UV extents differ");
        Require(count<=48,"Vulkan strip exceeds the original packet vertex extent");
        if(draw.drawFlags&0xc0u)Require(strip.normals.size()==count,
            "Lit strip lacks its source context, material or signed normal payload");
        VkDrawBatch batch=source.batch;
        batch.residentGeometry=false;batch.nativeSpanFirst=0;batch.nativeSpanCount=0;
        batch.first=static_cast<std::uint32_t>(elementIndices.size());batch.count=0;
        textureLod[0]=draw.textureQScale;
        textureLod[1]=draw.kind==EcstasyDrawKind::SolidStrip ? static_cast<std::uint8_t>(strip.autoK)*0.0625f:source.lod1;
        glyphBoundsIndex=0;
        VkSolidStripJob job;job.draw=&draw;job.strip=runStrip;
        job.scroll=source.scroll;job.lod=textureLod;
        job.glyphBoundsIndex=0;job.reflection=false;job.targetSample=false;
        job.lineTopology=batch.wireframe;
        const auto acquireStarted=timeTemplate ? Clock::now():Clock::time_point{};
        bool prepare=false;auto* resident=residentGeometry.Acquire(job,prepare);
        if(residentOut)*residentOut=resident;
        if(timeTemplate)nativeGatherStepMilliseconds[11]+=std::chrono::duration<double,std::milli>(Clock::now()-acquireStarted).count();
        nativeSolidKinds[draw.kind==EcstasyDrawKind::SolidSpecularStrip ? 2:draw.carShadow!=EcstasyCarShadow::None ? 3:0]+=1;
        if(!resident)nativeSolidKinds[4]+=1; else if(prepare)nativeSolidKinds[5]+=1;
        VkSolidTopology topology;
        if(!resident) {
            topology=DecodeNativeSolidTopology(strip,job.lineTopology);
            job.count=topology.count;job.sourceOrder=topology.sourceOrder;
        }
        std::uint32_t firstVertex;
        if(resident) {
            firstVertex=resident->slot*static_cast<std::uint32_t>(VkResidentGeometryCache::VerticesPerSlot);
            batch.residentGeometry=true;job.destination=0;job.residentOutput=resident->shadow.data();
            residentFrameVertices+=job.count;
            Require(residentFrameIndices+resident->indexCount<=std::numeric_limits<std::uint32_t>::max(),
                "Native resident frame index count exceeds32-bit draw range");
            Require(gatheredResidentIndexSpans.size()<std::numeric_limits<std::uint32_t>::max(),
                "Native resident index span count exceeds32-bit range");
            batch.first=static_cast<std::uint32_t>(residentFrameIndices);batch.count=resident->indexCount;
            batch.nativeSpanFirst=static_cast<std::uint32_t>(gatheredResidentIndexSpans.size());batch.nativeSpanCount=1;
            gatheredResidentIndexSpans.push_back(resident);gatheredResidentSpanTokens.push_back(resident->indexToken);residentFrameIndices+=resident->indexCount;
        } else {
            Require(output.size()+job.count<std::numeric_limits<std::uint32_t>::max(),
                "Vulkan frame vertex count exceeds32-bit index range");
            firstVertex=static_cast<std::uint32_t>(output.size());job.destination=firstVertex;
            output.resize(output.size()+job.count);prepare=true;
            for(std::size_t index=0;index<topology.indexCount;++index)
                elementIndices.push_back(firstVertex+topology.indices[index]);
        }
        job.workUnits=prepare ? NativePendingVertexWork(job):0;
        if(job.workUnits) {
            if(directSourcePreparation)
                PrepareNativeSolidStripVertices(job,job.residentOutput ? job.residentOutput:output.data());
            else {nativePendingWork+=job.workUnits;nativeStripJobs.push_back(job);}
        }
        Require(elementIndices.size()<=std::numeric_limits<std::uint32_t>::max(),"Vulkan frame index count exceeds32-bit draw range");
        if(!batch.residentGeometry)batch.count=static_cast<std::uint32_t>(elementIndices.size())-batch.first;
        if(batch.count) {
            if(draw.carShadow!=EcstasyCarShadow::None) {
                const auto shadow=static_cast<std::size_t>(draw.carShadow)-1;
                ++profile.shadowStrips[shadow];
                profile.shadowTriangles[shadow]+=batch.count/3u;
            }
            appendBatch(batch);
        }
        ++statistics.solidTemplateHits;++nativeGatherCounters[0];
    };
    const auto& sourceCommands=frame.Commands();
    const auto& runStrips=frame.SceneryRunStrips();
    // Model-run cache. A recorded run replays only when every continuation
    // strip is the same strip pointer and its earlier resident entry is
    // still that exact, prepared entry with exactly validated source bytes.
    // The per-strip path would then append the same spans and merge the same
    // contiguous batches (equal state, adjacent index and span ranges), so
    // one combined span range and batch is the identical output.
#if !defined(__vita__) && !defined(__SWITCH__)
    static const int modelRunCacheMode=[] {const char* value=std::getenv("HP2_RUN_CACHE");return value ? std::atoi(value):1;}();
#else
    const int modelRunCacheMode=ecstasyModelRunCache;
#endif
    const EcstasyDrawCommand* runRecording=nullptr;
    std::vector<const EditorMarkerSolidGeometry::Strip*>& recordStrips=modelRunScratchStrips;
    std::vector<VkResidentGeometryEntry*>& recordEntries=modelRunScratchEntries;
    recordStrips.clear();recordEntries.clear();
    struct ModelRunVerify {
        const EcstasyDrawCommand* run{};
        std::size_t spansBefore{},batchesBefore{};
        VkDrawBatch backBefore,expected;bool hadBack{};
        std::vector<VkResidentGeometryEntry*> entries;
    } modelRunVerify;
    const auto runLod1=[](const EcstasyDrawCommand& draw,const SolidTemplate& slot,const EditorMarkerSolidGeometry::Strip& strip) {
        return draw.kind==EcstasyDrawKind::SolidStrip ? static_cast<std::uint8_t>(strip.autoK)*0.0625f:slot.lod1;
    };
    // Per-strip replay data shared by model and scenery run records.
    const auto fillRunRecord=[&](VkModelRunRecord& record,const std::vector<const EditorMarkerSolidGeometry::Strip*>& strips,
        const std::vector<VkResidentGeometryEntry*>& entries) {
        const auto n=entries.size();
        record.strips=strips;record.slots.resize(n);record.tokens.resize(n);record.serials.resize(n);
        record.indexCounts.resize(n);record.counts.resize(n);record.revisioned=true;
        for(std::size_t at=0;at<n;++at) {
            const auto& entry=*entries[at];
            record.slots[at]=entry.slot;record.tokens[at]=entry.indexToken;record.serials[at]=entry.serial;
            record.indexCounts[at]=entry.indexCount;record.counts[at]=entry.count;
            if(!strips[at]->nativeVertexRevision)record.revisioned=false;
        }
        record.epoch=residentGeometry.FrameEpoch();
    };
    // A recorded run's entries. Fast: the strip epoch is unchanged since the
    // record was last exactly validated and every strip is revisioned, so
    // each slot assignment (serial) still holds exactly those prepared bytes
    // and only compact slot state is read. Otherwise Reacquire validates
    // each source exactly and refreshes the record. Sums index counts.
    const auto resolveRunRecord=[&](VkModelRunRecord& record,const EditorMarkerSolidGeometry::Strip* const* strips,
        std::size_t count,const auto& keyFor,std::size_t& indices)->bool {
        auto& entries=modelRunScratchReplay;entries.resize(count);indices=0;
        if(ecstasyReplayFast && record.revisioned && record.epoch==residentGeometry.FrameEpoch() && record.serials.size()==count) {
            for(std::size_t at=0;at<count;++at) {
                auto* entry=residentGeometry.ReacquireFast(record.slots[at],record.serials[at]);
                if(!entry)return false;
                entries[at]=entry;indices+=record.indexCounts[at];
            }
            nativeReplayFastCounters[0]+=count;
            return true;
        }
        bool revisioned=true;
        for(std::size_t at=0;at<count;++at) {
            auto* entry=residentGeometry.Reacquire(strips[at],record.slots[at],record.tokens[at],keyFor(at));
            if(!entry || !entry->indexCount)return false;
            entries[at]=entry;indices+=entry->indexCount;
            if(!strips[at]->nativeVertexRevision)revisioned=false;
        }
        record.serials.resize(count);record.indexCounts.resize(count);record.counts.resize(count);
        for(std::size_t at=0;at<count;++at) {
            record.serials[at]=entries[at]->serial;record.indexCounts[at]=entries[at]->indexCount;record.counts[at]=entries[at]->count;
        }
        record.revisioned=revisioned;record.epoch=residentGeometry.FrameEpoch();
        nativeReplayFastCounters[1]+=count;
        return true;
    };
    const auto replayModelRun=[&](const EcstasyDrawCommand& run,const EcstasyDrawCommand& draw,const SolidTemplate& slot)->bool {
        const std::size_t count=run.sceneryRunCount-1u;
        const auto* strips=&runStrips[run.sceneryRunFirst+1u];
        const auto found=modelRunRecords.find(strips[0]);
        const bool solidKind=draw.kind==EcstasyDrawKind::SolidStrip;
        if(found==modelRunRecords.end()) {++nativeModelRunCounters[1];return false;}
        auto& record=found->second;
        if(record.strips.size()!=count || std::memcmp(record.strips.data(),strips,count*sizeof(strips[0])) ||
            record.wireframe!=slot.batch.wireframe || record.solidKind!=solidKind ||
            (!solidKind && std::memcmp(&record.lod1,&slot.lod1,sizeof(float)))) {++nativeModelRunCounters[1];return false;}
        std::size_t indices=0;
        const auto keyFor=[&](std::size_t at) {
            VkResidentGeometryKey key;
            key.words[0]=slot.batch.wireframe ? 1u:0u;
            const float lod1=runLod1(draw,slot,*strips[at]);
            std::memcpy(&key.words[1],&lod1,sizeof(float));key.words[2]=0;
            return key;
        };
        if(!resolveRunRecord(record,strips,count,keyFor,indices)) {++nativeModelRunCounters[1];return false;}
        const auto& entries=modelRunScratchReplay;
        VkDrawBatch batch=slot.batch;
        batch.residentGeometry=true;batch.first=static_cast<std::uint32_t>(residentFrameIndices);
        Require(gatheredResidentIndexSpans.size()+count<std::numeric_limits<std::uint32_t>::max(),
            "Native resident index span count exceeds32-bit range");
        batch.nativeSpanFirst=static_cast<std::uint32_t>(gatheredResidentIndexSpans.size());
        batch.nativeSpanCount=static_cast<std::uint32_t>(count);
        Require(residentFrameIndices+indices<=std::numeric_limits<std::uint32_t>::max(),
            "Native resident frame index count exceeds32-bit draw range");
        batch.count=static_cast<std::uint32_t>(indices);
        if(modelRunCacheMode==2) {
            // Verify: the ordinary path runs and must produce this.
            modelRunVerify.run=&run;modelRunVerify.spansBefore=gatheredResidentIndexSpans.size();
            modelRunVerify.batchesBefore=batches.size();modelRunVerify.hadBack=!batches.empty();
            if(modelRunVerify.hadBack)modelRunVerify.backBefore=batches.back();
            modelRunVerify.expected=batch;modelRunVerify.entries=entries;
            return true;
        }
        for(std::size_t at=0;at<count;++at) {
            gatheredResidentIndexSpans.push_back(entries[at]);gatheredResidentSpanTokens.push_back(record.tokens[at]);
            residentFrameVertices+=record.counts[at];
        }
        residentFrameIndices+=indices;
        textureLod[0]=draw.textureQScale;textureLod[1]=runLod1(draw,slot,*strips[count-1]);glyphBoundsIndex=0;
        appendBatch(batch);
        const auto kind=draw.kind==EcstasyDrawKind::SolidSpecularStrip ? 2:draw.carShadow!=EcstasyCarShadow::None ? 3:0;
        nativeSolidKinds[kind]+=static_cast<double>(count);
        if(draw.carShadow!=EcstasyCarShadow::None) {
            const auto shadow=static_cast<std::size_t>(draw.carShadow)-1;
            profile.shadowStrips[shadow]+=count;profile.shadowTriangles[shadow]+=batch.count/3u;
        }
        statistics.solidTemplateHits+=count;nativeGatherCounters[0]+=count;
        ++nativeModelRunCounters[0];nativeModelRunCounters[2]+=count;
        return true;
    };
    const auto finishModelRunRecord=[&](const EcstasyDrawCommand& run,const EcstasyDrawCommand& draw,const SolidTemplate& slot) {
        if(recordStrips.size()+1u!=run.sceneryRunCount)return;
        if(modelRunRecords.size()>=4096)modelRunRecords.clear();
        auto& record=modelRunRecords[recordStrips[0]];
        fillRunRecord(record,recordStrips,recordEntries);
        record.wireframe=slot.batch.wireframe;record.solidKind=draw.kind==EcstasyDrawKind::SolidStrip;record.lod1=slot.lod1;
    };
    const auto checkModelRunVerify=[&] {
        auto& verify=modelRunVerify;
        bool same=gatheredResidentIndexSpans.size()==verify.spansBefore+verify.entries.size();
        for(std::size_t at=0;same && at<verify.entries.size();++at)
            same=gatheredResidentIndexSpans[verify.spansBefore+at]==verify.entries[at] &&
                gatheredResidentSpanTokens[verify.spansBefore+at]==verify.entries[at]->indexToken;
        VkDrawBatch expected=verify.expected;
        std::size_t expectedSize=verify.batchesBefore+1;
        if(verify.hadBack && expected.Continues(verify.backBefore)) {
            auto merged=verify.backBefore;merged.count+=expected.count;merged.nativeSpanCount+=expected.nativeSpanCount;
            expected=merged;expectedSize=verify.batchesBefore;
        }
        if(same)same=batches.size()==expectedSize && !batches.empty();
        if(same) {
            const auto& actual=batches.back();
            same=actual.SameDrawState(expected) && actual.first==expected.first && actual.count==expected.count &&
                actual.residentGeometry==expected.residentGeometry && actual.nativeSpanFirst==expected.nativeSpanFirst &&
                actual.nativeSpanCount==expected.nativeSpanCount && actual.nativeLocalToClip==expected.nativeLocalToClip &&
                actual.nativeClipMapping==expected.nativeClipMapping && actual.nativeShadingIndex==expected.nativeShadingIndex &&
                actual.nativeTextureQScale==expected.nativeTextureQScale && actual.nativeTransformIndex==expected.nativeTransformIndex;
        }
        ++nativeModelRunCounters[3];
        if(!same)++nativeModelRunCounters[4];
        if((nativeModelRunCounters[3]&4095u)==0 || (!same && nativeModelRunCounters[4]==1)) {
            char text[128];
            std::snprintf(text,sizeof(text),"checks=%llu mismatches=%llu replay_hits=%llu misses=%llu",
                (unsigned long long)nativeModelRunCounters[3],(unsigned long long)nativeModelRunCounters[4],
                (unsigned long long)nativeModelRunCounters[0],(unsigned long long)nativeModelRunCounters[1]);
            platform::RecordSessionEvent("render.model_run_verify",text);
        }
        verify.run=nullptr;
    };
    // Scenery run replay (ecstasySceneryRunReplay). After a cached scenery
    // run's first strip built the template, its remaining strips replay the
    // resident entries recorded for that exact strip sequence when each is
    // still the same prepared entry with exactly validated source bytes
    // (Reacquire). Every per-strip continuation would append the template
    // with that entry's index range: equal state and adjacent index/span
    // ranges, so the ordinary path merges them into one batch, the same one
    // the replay appends. Any changed entry takes the per-strip path.
    const EcstasyDrawCommand* sceneryRecording=nullptr;
    std::uint32_t sceneryRecordFrom=0;
    std::vector<const EditorMarkerSolidGeometry::Strip*> sceneryRecordStrips;
    std::vector<VkResidentGeometryEntry*> sceneryRecordEntries;
    const auto replaySceneryRun=[&](const EcstasyDrawCommand& run,std::uint32_t from)->bool {
        const std::size_t count=run.sceneryRunCount-from;
        const auto* strips=&runStrips[run.sceneryRunFirst+from];
        const bool wireframe=(run.nativeRenderEggState&EcstasyRenderEggWireframe)!=0;
        const auto found=sceneryRunRecords.find(strips[0]);
        if(found==sceneryRunRecords.end()) {++nativeSceneryReplayCounters[1];return false;}
        auto& record=found->second;
        if(record.strips.size()!=count || std::memcmp(record.strips.data(),strips,count*sizeof(strips[0])) ||
            record.wireframe!=wireframe) {++nativeSceneryReplayCounters[1];return false;}
        std::size_t indices=0;
        const auto keyFor=[&](std::size_t at) {
            VkResidentGeometryKey key;
            key.words[0]=wireframe ? 1u:0u;
            const float lod1=static_cast<std::uint8_t>(strips[at]->autoK)*0.0625f;
            std::memcpy(&key.words[1],&lod1,sizeof(float));key.words[2]=0;
            return key;
        };
        if(!resolveRunRecord(record,strips,count,keyFor,indices)) {++nativeSceneryReplayCounters[1];return false;}
        const auto& entries=modelRunScratchReplay;
        VkDrawBatch batch=sceneryTemplate;
        Require(gatheredResidentIndexSpans.size()+count<std::numeric_limits<std::uint32_t>::max(),
            "Native resident index span count exceeds32-bit range");
        Require(residentFrameIndices+indices<=std::numeric_limits<std::uint32_t>::max(),
            "Native resident frame index count exceeds32-bit draw range");
        batch.first=static_cast<std::uint32_t>(residentFrameIndices);
        batch.nativeSpanFirst=static_cast<std::uint32_t>(gatheredResidentIndexSpans.size());
        batch.nativeSpanCount=static_cast<std::uint32_t>(count);batch.count=static_cast<std::uint32_t>(indices);
        if(ecstasySceneryRunReplay==2) {
            // Verify: the per-strip path runs and must produce this.
            modelRunVerify.run=&run;modelRunVerify.spansBefore=gatheredResidentIndexSpans.size();
            modelRunVerify.batchesBefore=batches.size();modelRunVerify.hadBack=!batches.empty();
            if(modelRunVerify.hadBack)modelRunVerify.backBefore=batches.back();
            modelRunVerify.expected=batch;modelRunVerify.entries=entries;
            return false;
        }
        for(std::size_t at=0;at<count;++at) {
            gatheredResidentIndexSpans.push_back(entries[at]);gatheredResidentSpanTokens.push_back(record.tokens[at]);
            residentFrameVertices+=record.counts[at];
        }
        residentFrameIndices+=indices;
        statistics.sceneryPacketHits+=count;
        appendBatch(batch);sceneryTemplateBatches=batches.size();
        ++nativeSceneryReplayCounters[0];nativeSceneryReplayCounters[2]+=count;
        return true;
    };
    const auto finishSceneryRecord=[&](const EcstasyDrawCommand& run) {
        if(sceneryRecordFrom+sceneryRecordStrips.size()!=run.sceneryRunCount || sceneryRecordStrips.empty())return;
        if(sceneryRunRecords.size()>=8192)sceneryRunRecords.clear();
        auto& record=sceneryRunRecords[sceneryRecordStrips[0]];
        fillRunRecord(record,sceneryRecordStrips,sceneryRecordEntries);
        record.wireframe=(run.nativeRenderEggState&EcstasyRenderEggWireframe)!=0;
        ++nativeSceneryReplayCounters[3];
    };
    // Scenery reuse census (HP2_SCENERY_REUSE=1 / scenery_reuse.txt): counts
    // only, no output change. Per view (0 main, 1 other targets): cached
    // singletons, run commands and run strips; strips whose geometry sequence
    // already appeared this Gather; strips whose sequence (any instance) or
    // sequence+instance appeared in the previous Gather.
    struct SceneryReuseCensus {
        std::unordered_set<std::uint64_t> frameSeq,frameSeqInst,lastSeq,lastSeqInst;
        std::array<std::array<double,7>,2> sum{};unsigned frames=0;
    };
    static SceneryReuseCensus reuseCensus;
    const auto censusSequence=[&](const EcstasyDrawCommand& draw,const EditorMarkerSolidGeometry::Strip* const* strips,std::uint32_t count) {
        std::uint64_t hash=1469598103934665603ull^static_cast<std::uint64_t>(draw.target ? draw.target->id:-1);
        for(std::uint32_t at=0;at<count;++at) {
            hash^=static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(strips[at]));hash*=1099511628211ull;
        }
        const std::uint64_t withInstance=(hash^static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(draw.instance)))*0x9e3779b97f4a7c15ull;
        auto& row=reuseCensus.sum[draw.target && draw.target->id<7 ? 0:1];
        if(count>1 || draw.sceneryRunCount) {row[1]+=1;row[2]+=count;} else row[0]+=1;
        if(!reuseCensus.frameSeq.insert(hash).second)row[3]+=count;
        if(reuseCensus.lastSeq.count(hash))row[4]+=count;
        if(reuseCensus.lastSeqInst.count(withInstance))row[5]+=count;
        reuseCensus.frameSeqInst.insert(withInstance);
        row[6]+=count;
    };
    std::uint32_t runResume=0; // next unexpanded strip of sourceCommands[commandAt]
    const EcstasyDrawCommand* modelRunSingle=nullptr; // first-strip command of the current model run
#if defined(__GNUC__)
    std::size_t prefetchedCommand=0;
#endif
    for (std::size_t commandAt=0;commandAt<sourceCommands.size();) {
        const EcstasyDrawCommand* current=&sourceCommands[commandAt];
#if defined(__GNUC__)
        // Vita main RAM is latency bound (~176 MB/s streaming without
        // prefetch): request the command and instance a few draws ahead.
        if(commandAt+3<sourceCommands.size() && prefetchedCommand<commandAt+3) {
            prefetchedCommand=commandAt+3;
            const auto* ahead=reinterpret_cast<const char*>(&sourceCommands[prefetchedCommand]);
            for(std::size_t line=0;line<sizeof(EcstasyDrawCommand);line+=32)__builtin_prefetch(ahead+line);
            if(const auto* instance=sourceCommands[commandAt+2].instance)
                for(std::size_t line=0;line<sizeof(float)*16;line+=32)
                    __builtin_prefetch(reinterpret_cast<const char*>(instance)+line);
        }
#endif
        if(current->sceneryRunCount && !current->cachedOpaqueScenery) {
            // Model run (car parts etc.): its first strip takes the general
            // path as the per-strip command it replaced; later strips reuse
            // that command's solid template (the same state, another strip).
            const auto& run=*current;
            const auto stripIndex=runResume;
            const auto* strip=runStrips[run.sceneryRunFirst+runResume];
            const bool first=runResume==0;
            if(++runResume>=run.sceneryRunCount) {runResume=0;++commandAt;}
            else residentGeometry.Prefetch(runStrips[run.sceneryRunFirst+runResume]);
            if(sampleCategories) {nativeGatherStepMilliseconds[14]+=1.0;if(first)nativeGatherStepMilliseconds[13]+=1.0;}
            if(first)runRecording=nullptr;
            if(!first && modelRunSingle && ecstasySolidTemplate && !verifySolidTemplate) {
                const auto& slot=solidTemplateSlot(*modelRunSingle);
                if(slot.draw==modelRunSingle) {
                    if(sampleCategories)nativeGatherStepMilliseconds[15]+=1.0;
                    categoryTimer.Enter(run);
                    if(stripIndex==1 && modelRunCacheMode) {
                        const bool replayed=replayModelRun(run,*modelRunSingle,slot);
                        if(replayed && modelRunCacheMode==1) {
                            if(runResume) {runResume=0;++commandAt;}
                            continue;
                        }
                        runRecording=&run;recordStrips.clear();recordEntries.clear();
                    }
                    VkResidentGeometryEntry* resident=nullptr;
                    continueSolidTemplate(*modelRunSingle,slot,strip,runRecording==&run ? &resident:nullptr);
                    if(runRecording==&run) {
                        if(!resident || !resident->indexCount)runRecording=nullptr;
                        else {recordStrips.push_back(strip);recordEntries.push_back(resident);}
                        if(runRecording && stripIndex+1==run.sceneryRunCount) {
                            finishModelRunRecord(run,*modelRunSingle,slot);runRecording=nullptr;
                        }
                    }
                    if(stripIndex+1==run.sceneryRunCount && modelRunVerify.run==&run)checkModelRunVerify();
                    continue;
                }
            }
            if(runRecording==&run)runRecording=nullptr;
            if(modelRunVerify.run==&run)modelRunVerify.run=nullptr;
            sceneryRunFallbacks.push_back(run);
            auto& single=sceneryRunFallbacks.back();
            single.strip=strip;single.sceneryRunCount=0;single.sceneryRunFirst=0;
            modelRunSingle=&single;current=&single;
        } else if(current->sceneryRunCount) {
            // Each strip of the run is the per-strip command it replaced.
            // Cached strips need no copy; a strip that leaves the cached
            // path continues below as its own stable per-strip command.
            const auto& run=*current;
            const EcstasyDrawCommand* fallback=nullptr;
            categoryTimer.Enter(run);
            if(ecstasySceneryReuseCensus && runResume==0)
                censusSequence(run,&runStrips[run.sceneryRunFirst],run.sceneryRunCount);
            while(runResume<run.sceneryRunCount) {
                const auto stripAt=runResume;
                const auto* strip=runStrips[run.sceneryRunFirst+runResume++];
                if(runResume<run.sceneryRunCount)residentGeometry.Prefetch(runStrips[run.sceneryRunFirst+runResume]);
#if defined(HP2_ENABLE_VITAGL) && !defined(HP2_VITA_ENVIRONMENT_MAP)
                if(run.target && run.target->id==14)continue;
#endif
                const bool fastPath=ecstasySceneryRunFastPath && sceneryTemplateDraw==&run;
                if(continueSceneryRun(run,strip)) {
                    if(ecstasySceneryRunReplay) {
                        if(sceneryRecording==&run) {
                            // Only fast-path continuations with indices are recordable.
                            if(fastPath && sceneryLastResident && sceneryLastResident->indexCount) {
                                sceneryRecordStrips.push_back(strip);sceneryRecordEntries.push_back(sceneryLastResident);
                                if(runResume==run.sceneryRunCount) {finishSceneryRecord(run);sceneryRecording=nullptr;}
                            } else sceneryRecording=nullptr;
                        }
                        if(stripAt==0 && runResume<run.sceneryRunCount && sceneryTemplateDraw==&run && ecstasySceneryRunFastPath) {
                            if(replaySceneryRun(run,runResume)) {runResume=run.sceneryRunCount;break;}
                            sceneryRecording=&run;sceneryRecordFrom=runResume;
                            sceneryRecordStrips.clear();sceneryRecordEntries.clear();
                        }
                    }
                    if(modelRunVerify.run==&run && runResume==run.sceneryRunCount)checkModelRunVerify();
                    continue;
                }
                if(sceneryRecording==&run)sceneryRecording=nullptr;
                if(modelRunVerify.run==&run)modelRunVerify.run=nullptr;
                ++statistics.sceneryPacketFallbacks;
                sceneryRunFallbacks.push_back(run);
                auto& single=sceneryRunFallbacks.back();
                single.strip=strip;single.sceneryRunCount=0;single.sceneryRunFirst=0;single.cachedOpaqueScenery=false;
                fallback=&single;break;
            }
            if(runResume>=run.sceneryRunCount) {runResume=0;++commandAt;}
            if(!fallback)continue;
            current=fallback;
        } else ++commandAt;
        const auto& draw=*current;
        const auto drawVertexStart=output.size();
        categoryTimer.Enter(draw);
        timeDraw=(sampleCategories || ecstasyGatherDetail) && (stepDrawIndex++&7u)==0;
        if(timeDraw)stepMark=Clock::now();
#if defined(HP2_ENABLE_VITAGL) && !defined(HP2_VITA_ENVIRONMENT_MAP)
        // The vitaGL passes drop environment-map (target 14) draws and their
        // readers; do not gather what would be discarded.
        if((draw.target && draw.target->id==14) || (draw.sampledTarget && draw.sampledTarget->id==14))continue;
#endif
        if(draw.kind==EcstasyDrawKind::SceneHistoryBoundary || draw.kind==EcstasyDrawKind::SceneHistorySource) {
            // Ordinary racing retains its single pass and does no history copy.
            if(preparedFrame.historyReaders.display) {
                VkDrawBatch marker;marker.target=0;
                marker.textureFunction=draw.kind==EcstasyDrawKind::SceneHistorySource ? 12:11;
                batches.push_back(marker);
            }
            continue;
        }
        if(draw.cachedOpaqueScenery) {
            if(ecstasySceneryReuseCensus && !draw.sceneryRunCount && draw.strip)censusSequence(draw,&draw.strip,1);
            if(prepareSceneryPacket(draw,draw.strip))continue;
            ++statistics.sceneryPacketFallbacks;
        }
        const SolidTemplate* solidMatch=nullptr;
        if(draw.kind==EcstasyDrawKind::SolidStrip || draw.kind==EcstasyDrawKind::SolidSpecularStrip) {
            ++nativeGatherCounters[1];
            if(draw.carShadow!=EcstasyCarShadow::None)++nativeGatherCounters[2];
            if(draw.depthSorted)++nativeGatherCounters[4];
        }
        if(ecstasySolidTemplate && solidTemplateEligible(draw)) {
            const auto& slot=solidTemplateSlot(draw);
            if(slot.draw && NativeSolidCommandStateEqual(*slot.draw,draw)) {
                if(!verifySolidTemplate) {continueSolidTemplate(draw,slot);continue;}
                solidMatch=&slot;
            }
        }
        const auto shadowStarted=sampleShadowGather && draw.carShadow!=EcstasyCarShadow::None ?
            Clock::now():Clock::time_point{};
        if(draw.kind==EcstasyDrawKind::BlackAndWhitePass || draw.kind==EcstasyDrawKind::DestinationFilterPass) {
            Require(draw.target && draw.target->id>=0 && draw.target->id<7 && draw.feedbackGroup,
                "Colour feedback lost its original view or source-stage group");
            const auto region=ecstasyNativeFeedbackRegion(*draw.target,options.width,options.height,
                options.logicalWidth,options.logicalHeight,frame.DisplayTranslation());
            if(!region.scissor[2] || !region.scissor[3])continue;
            VkDrawBatch feedback;feedback.target=0;feedback.feedbackGroup=draw.feedbackGroup;
            feedback.textureFunction=draw.kind==EcstasyDrawKind::BlackAndWhitePass ? 9:10;
            feedback.blend=0x00000202u; // source overwrite; RGB equation is evaluated by the shader.
            feedback.viewport={region.viewport[0],region.viewport[1],region.viewport[2],region.viewport[3],0,1};
            feedback.scissor={{region.scissor[0],region.scissor[1]},
                {static_cast<std::uint32_t>(region.scissor[2]),static_cast<std::uint32_t>(region.scissor[3])}};
            std::array<float,4> tint{{1,1,1,1}};
            std::array<float,2> sourceLod{};
            if(feedback.textureFunction==10) {
                Require(draw.texture && draw.screenFilter.textureFunction==0 &&
                    draw.screenFilter.blend==std::array<std::uint8_t,4>{{1,0,2,1}},
                    "Destination filter lost its released TFX/ALPHA inputs");
                feedback.texture=resolveTexture(*draw.texture,false);
                feedback.lodScale=static_cast<float>(draw.screenFilter.fixedAlpha)/128.0f;
                const auto* wire=draw.texture->descriptor->authoredRecord;
                feedback.minification=options.textureFilter==EcstasyTextureFilter::Authored ? wire[0x70]:-2;
                std::int32_t mipK=ReadU16(wire+0x72)&0xfff;if(mipK&0x800)mipK-=0x1000;
                sourceLod={{static_cast<float>(1u<<wire[0x71]),static_cast<float>(mipK)*0.0625f}};
                for(std::size_t lane=0;lane<4;++lane)tint[lane]=static_cast<float>(draw.screenFilter.color[lane])/128.0f;
            }
            feedback.first=static_cast<std::uint32_t>(output.size());feedback.count=3;
            const auto& uv=region.textureRectangle;
            output.push_back({{{-1,-1,0,1}},{{uv[0],uv[1],1}},tint,sourceLod});
            output.push_back({{{3,-1,0,1}},{{uv[0]+2*(uv[2]-uv[0]),uv[1],1}},tint,sourceLod});
            output.push_back({{{-1,3,0,1}},{{uv[0],uv[1]+2*(uv[3]-uv[1]),1}},tint,sourceLod});
            batches.push_back(feedback);continue;
        }
        if(draw.kind==EcstasyDrawKind::FogPass || draw.kind==EcstasyDrawKind::FocusMaskPass) {
            Require(draw.depthPalette && draw.target && draw.target->id>=0 && draw.target->id<7 &&
                draw.target->width>0 && draw.target->height>0,"Fog packet lost its source view/palette");
            VkDrawBatch fog;fog.target=0;fog.textureFunction=draw.kind==EcstasyDrawKind::FogPass ? 5:6;
            fog.depth=fog.depthWrite=false;
            fog.blend=draw.depthPalette->blend;fog.blendConstant=draw.depthPalette->blendConstant;
            fog.minification=static_cast<std::int32_t>(DepthPalette(*draw.depthPalette));
            fog.lodScale=draw.target->format==0x31 ? 16777215.0f:65535.0f;
            const double sx=static_cast<double>(options.width)/options.logicalWidth;
            const double sy=static_cast<double>(options.height)/options.logicalHeight;
            const auto translation=frame.DisplayTranslation();
            const double x=draw.target->left*sx+translation[0]*options.width;
            const double y=draw.target->top*sy+translation[1]*options.height;
            const double left=std::clamp(x,0.0,static_cast<double>(options.width));
            const double top=std::clamp(y,0.0,static_cast<double>(options.height));
            const double right=std::clamp(x+draw.target->width*sx,0.0,static_cast<double>(options.width));
            const double bottom=std::clamp(y+draw.target->height*sy,0.0,static_cast<double>(options.height));
            if(right<=left || bottom<=top)continue;
            fog.viewport={static_cast<float>(left),static_cast<float>(top),static_cast<float>(right-left),static_cast<float>(bottom-top),0,1};
            const auto l=static_cast<std::int32_t>(std::floor(left)),t=static_cast<std::int32_t>(std::floor(top));
            fog.scissor={{l,t},{static_cast<std::uint32_t>(std::ceil(right)-l),static_cast<std::uint32_t>(std::ceil(bottom)-t)}};
            fog.first=static_cast<std::uint32_t>(output.size());fog.count=3;
            output.push_back({{{-1,-1,0,1}},{{0,0,1}},{{1,1,1,1}}});
            output.push_back({{{3,-1,0,1}},{{0,0,1}},{{1,1,1,1}}});
            output.push_back({{{-1,3,0,1}},{{0,0,1}},{{1,1,1,1}}});
            batches.push_back(fog);continue;
        }
        if(draw.kind==EcstasyDrawKind::DepthOfFieldPass) {
            Require(draw.focus && draw.target && draw.target->id>=0 && draw.target->id<7 &&
                frame.SourceDisplayExtent()[0] && frame.SourceDisplayExtent()[1],
                "Depth of field lost its source display/settings");
            const auto& focus=*draw.focus;
            // Complete48-byte JitterOffsets PAL2DA750, independently
            // matched to Beta2D1D20. Adjacent bytes belong to FogPalette.
            static constexpr float jitter[6][2]={{0,0},{2,2},{2,0},{2,2},{0,2},{0,0}};
            Require(focus.numPasses>=0 && focus.numPasses<=6,"Focus tap count exceeds its original JitterOffsets table");
            const auto& extent=frame.SourceDisplayExtent();
            VkDrawBatch composite;composite.target=0;composite.textureFunction=8;
            composite.blend=draw.rasterState[0];composite.blendConstant=static_cast<float>(focus.compositeBlend[4])/128.0f;
            composite.viewport={0,0,static_cast<float>(options.width),static_cast<float>(options.height),0,1};
            composite.scissor={{0,0},{options.width,options.height}};
            composite.focusFirst=static_cast<std::uint32_t>(output.size());
            composite.focusPassCount=focus.numPasses;composite.focusBlend=static_cast<float>(focus.jitterBlend)/128.0f;
            const auto triangle=[&](float u,float v) {
                output.push_back({{{-1,-1,0,1}},{{u,v,1}},{{1,1,1,1}}});
                output.push_back({{{3,-1,0,1}},{{u+2,v,1}},{{1,1,1,1}}});
                output.push_back({{{-1,3,0,1}},{{u,v+2,1}},{{1,1,1,1}}});
            };
            for(std::int32_t index=0;index<focus.numPasses;++index)
                // GS window centers are integers; texture centers n+.5.
                // Half-size XY0/UV0 therefore samples source2*j, whereas
                // a native full-rectangle draw samples2*j+1. Keep this
                // one-source-pixel phase in the selected PC raster scale.
                triangle((jitter[index][0]-1.0f)/extent[0],(jitter[index][1]-1.0f)/extent[1]);
            composite.first=static_cast<std::uint32_t>(output.size());composite.count=3;
            // Native source-pixel displacement; the default full rectangle
            // has zero displacement. The earlier half-size tap phase is
            // independent and remains part of the filter's sampling pattern.
            Require(std::isfinite(focus.compositeOffsetPixels[0]) && std::isfinite(focus.compositeOffsetPixels[1]),
                "Focus composite displacement is not finite");
            triangle(focus.compositeOffsetPixels[0]/static_cast<float>(extent[0]),
                focus.compositeOffsetPixels[1]/static_cast<float>(extent[1]));
            batches.push_back(composite);continue;
        }
        glyphBoundsIndex=0;
        if(draw.kind==EcstasyDrawKind::FlailerPolygon && draw.polygon) {
            const auto& bounds=draw.polygon->glyphSamplingBounds;
            if(bounds[2]>0.0f && bounds[3]>0.0f) {
                Require(std::all_of(bounds.begin(),bounds.end(),[](float v){return std::isfinite(v);}) &&
                    bounds[0]>0 && bounds[1]>0 && bounds[2]>=bounds[0] && bounds[3]>=bounds[1] &&
                    bounds[2]<=1.0f && bounds[3]<=1.0f,"Glyph sampling bounds exceed their actual atlas cell");
                Require(gatheredGlyphBounds.size()<std::numeric_limits<std::uint32_t>::max(),"Glyph sampling table exceeds native range");
                glyphBoundsIndex=static_cast<std::uint32_t>(gatheredGlyphBounds.size());
                gatheredGlyphBounds.push_back(bounds);
            }
        }
        const bool targetSample=draw.sampledTarget!=nullptr;
        const bool rearSample=draw.kind==EcstasyDrawKind::RenderTargetPolygon;
        if(rearSample && draw.sampledTarget)nativeMirrorTargetId=draw.sampledTarget->id;
        const bool remap=draw.kind==EcstasyDrawKind::RenderTargetStrip;
        const bool reflection=draw.kind==EcstasyDrawKind::SolidReflectionStrip;
        const bool specular=draw.kind==EcstasyDrawKind::SolidSpecularStrip;
        const bool filter=draw.kind==EcstasyDrawKind::ScreenFilterPolygon;
        const bool screenPolygon=draw.kind==EcstasyDrawKind::ScreenPolygon;
        Require(draw.target && draw.target->id>=0 && draw.target->id<=14 && draw.target->width>0 && draw.target->height>0 &&
            (targetSample ? !draw.texture:draw.texture!=nullptr),"Vulkan draw lost its actual image or render target");
        const auto destination=draw.target->id>=7 ? draw.target->id:0;
        const auto sampled=targetSample ? (draw.sampledTarget->id>=7 ? draw.sampledTarget->id:0):-1;
        if(targetSample) {
            const auto& owner=renderedTargets.at(sampled);
            Require((draw.sampledPreviousFrame ? owner.previousDescriptor:owner.descriptor)!=VK_NULL_HANDLE,
                "Vulkan target draw lost its current/previous image owner");
            Require(draw.sampledPreviousFrame || sampled!=destination,
                "A render target cannot sample its own current attachment");
        }
        const std::uint8_t* wire=nullptr;std::size_t textureIndex{};
        if(!targetSample) {
            textureIndex=resolveTexture(*draw.texture,NativeDrawClampsTexture(draw));wire=draw.texture->descriptor->authoredRecord;
        }
        step(8);
        const bool particle=draw.kind==EcstasyDrawKind::ParticleGroup;
        const bool mirrorOverlay=draw.kind==EcstasyDrawKind::MirrorOverlayPolygon;
        const bool ui=draw.kind==EcstasyDrawKind::FlailerPolygon || rearSample || mirrorOverlay || filter || remap || screenPolygon;
        const bool layeredUi=ui && (draw.rasterState[2]&255u)!=0;
        Require(elementIndices.size()<=std::numeric_limits<std::uint32_t>::max(),
            "Vulkan frame index count exceeds32-bit draw range");
        VkDrawBatch batch;batch.indexed=true;batch.cachedOpaqueScenery=draw.cachedOpaqueScenery;batch.particle=particle;
        batch.first=static_cast<std::uint32_t>(elementIndices.size());batch.texture=textureIndex;
        // AttachMicroCode's TME/primitive controls feed the solid VU
        // templates; UI and immediate particle packets have independent PRIM.
        const bool solidPrimitive=draw.kind==EcstasyDrawKind::SolidStrip || reflection || specular;
        batch.wireframe=solidPrimitive && (draw.nativeRenderEggState&EcstasyRenderEggWireframe)!=0;
        batch.untextured=solidPrimitive && (draw.nativeRenderEggState&EcstasyRenderEggTexturing)==0;
        batch.showcaseEnvironmentBlend=reflection && draw.showcaseEnvironmentBlend && !batch.untextured;
        // CreateTextureContext108F98 consumes the clamp override for the
        // actual authored texture. Native image/history passes own samplers.
        batch.clampTopLeft=NativeDrawClampsTexture(draw);
        batch.target=destination;batch.sampledTarget=sampled;batch.sampledPreviousFrame=draw.sampledPreviousFrame;
        textureLod[0]=draw.textureQScale;
        if(batch.cachedOpaqueScenery) {
            const auto& material=ResolveOpaqueSceneryMaterial(textures[textureIndex],wire,options.textureFilter);
            batch.blend=material.blend;batch.blendConstant=material.blendConstant;
            batch.lodScale=material.lodScale;batch.textureFunction=material.textureFunction;
            batch.minification=material.minification;
            batch.depth=true;batch.depthWrite=material.depthWrite;
            batch.alphaTest=material.fullAlpha || draw.texture->drawFlag4Enabled || draw.target->id==14?0:2;
            textureLod[1]=static_cast<std::uint8_t>(draw.strip->autoK)*0.0625f;
        } else {
            batch.depth=(!ui || layeredUi || rearSample || mirrorOverlay || filter || screenPolygon);
            batch.depthWrite=targetSample || filter ? draw.zWriteEnabled:
                layeredUi ? draw.zWriteEnabled && wire[0x50]!=0:!ui && draw.zWriteEnabled && !draw.depthSorted;
            if(reflection || specular || remap)batch.depthWrite=false;
            batch.blend=draw.rasterState[0];batch.blendConstant=static_cast<float>(draw.rasterState[1]&255u)/128.0f;
            batch.textureFunction=targetSample || reflection ? 0:wire[0x4e];
            batch.alphaTest=rearSample ? ecstasyNativeMirrorCompositeAlphaTest() :
                targetSample || reflection || specular || filter ? 0:draw.texture->requiresDepthSort ? 1:0;
            batch.minification=targetSample ? 2:wire[0x70];
            // UI coordinates are authored in640x480 and scaled by the PC
            // output extent. Keep their atlas sampling linear at level0; the
            // selectable world filter must not pixelate fonts or movie panels.
            if(draw.kind==EcstasyDrawKind::FlailerPolygon)batch.minification=-1;
            else if(options.textureFilter!=EcstasyTextureFilter::Authored)batch.minification=-2;
            batch.lodScale=targetSample ? 1.0f:static_cast<float>(1u<<wire[0x71]);
            std::int32_t mipK=targetSample ? 0:ReadU16(wire+0x72)&0xfff;
            if(mipK&0x800)mipK-=0x1000;
            if(draw.kind==EcstasyDrawKind::SolidStrip) {
                Require(draw.strip,"Mip-mapped model lost its original strip");
                // CalcAutoK10C0F0 retains16 bits, but SetMipMapK11C328 gets
                // lbu+3C and VU01E8..0220 copies that unsigned byte into TEX1 K.
                mipK=static_cast<std::uint8_t>(draw.strip->autoK);
            } else if(draw.kind==EcstasyDrawKind::WorldPolygon || draw.kind==EcstasyDrawKind::WorldStrip) {
                // Immediate polygon110040 and strip110564 headers replicate
                // zero into this same K lane before the common VU program.
                mipK=0;
            }
            textureLod[1]=static_cast<float>(mipK)*0.0625f;
            if(draw.kind==EcstasyDrawKind::SolidStrip || draw.kind==EcstasyDrawKind::WorldStrip ||
                draw.kind==EcstasyDrawKind::WorldPolygon || particle || mirrorOverlay || screenPolygon) {
                // Particle direct packets carry PRIM0x54 (ABE1) and pass through
                // the very same AttachSolid/Alpha texture contexts. Their own
                // packet does not override ALPHA, TEST or ZBUF.
                // Final AttachMicroCode10AD78 enables PRIM.ABE for these packets.
                // CreateTextureContext108CC0 writes ALPHA from descriptor76/77;
                // descriptor4D only controls ordering, including unsorted shadows.
                const auto alpha=wire[0x76];
                if(!screenPolygon || !(draw.rasterState[1]&0x00ff0000u)) {
                    batch.blend=(alpha&3u)|(((alpha>>2)&3u)<<8)|
                        (((alpha>>4)&3u)<<16)|(((alpha>>6)&3u)<<24);
                    batch.blendConstant=static_cast<float>(wire[0x77])/128.0f;
                }
                // Final AttachSolidPolys10BC00..10BC38 and AttachAlphaPolys10BA24..10BA4C
                // choose context bit0 from descriptor4F OR runtime74, independently
                // of the sorting lane4D. Contexts0/2 carry TEST GREATER with AREF48;
                // contexts1/3 carry GEQUAL0 (PAL105AC0 UpdateGifTags). Alpha is
                // expressed in the same128-unit GS range by this native shader.
                const bool fullAlpha=(wire[0x4f]!=0) || draw.texture->drawFlag4Enabled;
                batch.alphaTest=fullAlpha || draw.target->id==14 ? 0:2;
                // The same source context bit1 is descriptor50 gated by the
                // captured original Z-write state, not by depth sorting.
                batch.depthWrite=draw.zWriteEnabled && wire[0x50]!=0;
            }
        }
        Require(batch.textureFunction>=0 && batch.textureFunction<=3,"Invalid authored GS texture function");
        step(9);
        const auto& raster=rasterFor(*draw.target,destination);
        const auto displayWidth=raster.displayWidth,displayHeight=raster.displayHeight;
        const auto outputWidth=raster.outputWidth,outputHeight=raster.outputHeight;
        const auto& originalViewport=raster.originalViewport;
        const auto& translation=raster.translation;
        const auto& clipScale=raster.clipScale;
        const auto& clipOffset=raster.clipOffset;
        batch.viewport=raster.viewport;
        batch.scissor=raster.scissor;
        std::array<float,2> uiCanvasScale{{1.0f,1.0f}};
        if(draw.kind==EcstasyDrawKind::FlailerPolygon &&
           draw.uiPlacement==EcstasyUiPlacement::CenteredCanvas &&
           (draw.rasterState[1]&0x01000000u))
            uiCanvasScale=nativeUiCanvasScale(originalViewport.width,originalViewport.height);
        const double maximumDepth=draw.target->format==0x31 ? 16777215.0:65535.0;
        if(!ui && !particle)applyDepthRange(draw,batch);
        if(rearSample) {
            // B24: the camera image is sampled into the authored PAL
            // 160..352 x 20..84 quadrilateral. Constrain Vulkan raster
            // to those pixel centres after the native viewport mapping,
            // avoiding a visible camera strip beyond the mirror's edge.
            // The original mask, textured overlay, UV and view remain live.
            const auto& extent=frame.SourceDisplayExtent();
            const auto mirror=ecstasyNativeMirrorScissor(
                originalViewport.x,originalViewport.y,
                originalViewport.width,originalViewport.height,
                extent[0],extent[1],outputWidth,outputHeight,
                translation[0]*outputWidth,translation[1]*outputHeight);
            const auto x0=std::max(batch.scissor.offset.x,mirror.left);
            const auto y0=std::max(batch.scissor.offset.y,mirror.top);
            const auto x1=std::min(batch.scissor.offset.x+
                static_cast<std::int32_t>(batch.scissor.extent.width),mirror.right);
            const auto y1=std::min(batch.scissor.offset.y+
                static_cast<std::int32_t>(batch.scissor.extent.height),mirror.bottom);
            batch.scissor={{x0,y0},{static_cast<std::uint32_t>(std::max(x0,x1)-x0),
                static_cast<std::uint32_t>(std::max(y0,y1)-y0)}};
        }
        // The final draw submission already rejects a zero-area scissor.
        // Do it before transforming/lighting every vertex and building an
        // index buffer: no pixel can be emitted, and target/texture ownership
        // has already been processed exactly as in the existing path.
        if (!batch.scissor.extent.width || !batch.scissor.extent.height) continue;
        // Feed dynamic world geometry to the same vertex shader projection
        // as resident models on Switch. Keep the calibrated CPU path's
        // double-precision viewport translation and the UI/particle paths.
#ifdef __SWITCH__
        const bool gpuWorldProjection=!ui && !particle && !solidPrimitive &&
            translation[0]==0.0 && translation[1]==0.0;
#else
        constexpr bool gpuWorldProjection=false;
#endif
        if(gpuWorldProjection) {
            batch.nativeGpuPosition=true;
            batch.nativeLocalToClip=draw.LocalToClip();
            batch.nativeTextureQScale=textureLod[0];
        }
        auto position=[&](const float input[3]) -> std::array<float,4> {
            if(gpuWorldProjection) {
                ++statistics.gpuWorldVertices;
                return {input[0],input[1],input[2],1.0f};
            }
            std::array<float,4> result{};
            if(ui) {
                // ScaleCoordinates is byte97. Flailer authored coordinates
                // use its640x480 design space; the target decides pixel size.
                const bool scaled=(draw.rasterState[1]&0x01000000u)!=0;
                const auto& sourceExtent=frame.SourceDisplayExtent();
                const float width=rearSample ? static_cast<float>(sourceExtent[0]):scaled ? 640.0f:displayWidth;
                const float height=rearSample ? static_cast<float>(sourceExtent[1]):scaled ? 480.0f:displayHeight;
                float projectedDepth=0.0f;
                if(layeredUi || rearSample || mirrorOverlay || filter || screenPolygon) {
                    // Final ePoly::Render10FC74..10FCE4: byte98 selects the
                    // logical layer table2C9E70. Background20..24 uses its
                    // small direct GS Z; all foreground layers use MaxZ-table.
                    // Keeping every UI primitive at z=0 with depth disabled
                    // incorrectly covered the preceding opaque frontend car.
                    static constexpr float layerDepths[29]={10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,
                        5,6,7,8,5,4,3,2,1,1,2,3,4};
                    Require(draw.view && draw.view->matricesCalculated,
                        "Polygon source depth lacks its calculated view");
                    float sourceDepth=input[2];
                    if(layeredUi) {
                        const auto layer=convertFloatToInt32(input[2]);
                        Require(layer>=0 && layer<29,"Flailer logical depth has no authored layer");
                        const float maximum=draw.target->format==0x31 ? 16777215.0f:65535.0f;
                        sourceDepth=layer>=20 && layer<=24 ? layerDepths[layer]:maximum-layerDepths[layer];
                    }
                    // Byte98==0 (RVM/Sun/filters) uses direct Z. Preserve all
                    // original layer distinctions in the same reversed GS-Z
                    // interval as world/particles, including polygons ahead
                    // of the world's near plane. Clamping them to that plane
                    // makes priority0 movie panels disappear behind layer10.
                    projectedDepth=static_cast<float>(1.0-static_cast<double>(sourceDepth)/maximumDepth);
                }
                const float x=filter ? input[0]-static_cast<float>(draw.target->left):input[0];
                const float y=filter ? input[1]-static_cast<float>(draw.target->top):input[1];
                result={(x*2.0f/width-1.0f)*uiCanvasScale[0],
                    (y*2.0f/height-1.0f)*uiCanvasScale[1],projectedDepth,1.0f};
            } else {
                eMulVector3(result.data(),draw.LocalToClip().data(),input);
                // Original perspective clip Z is[-W,+W]; Vulkan is[0,W].
                result[2]=(result[2]+result[3])*0.5f;
            }
            if(translation[0]!=0.0 || translation[1]!=0.0) {
                result[0]=static_cast<float>(result[0]*clipScale[0]+result[3]*clipOffset[0]);
                result[1]=static_cast<float>(result[1]*clipScale[1]+result[3]*clipOffset[1]);
            }
            return result;
        };
        const bool solidStep=draw.kind==EcstasyDrawKind::SolidStrip || reflection || specular;
        step(solidStep ? 0:5);
        if(draw.kind==EcstasyDrawKind::SolidStrip || reflection || specular) {
            batch.nativeGpuPosition=true;
            Require(std::isfinite(draw.textureQScale),"Native solid texture Q scale is nonfinite");
            batch.nativeTextureQScale=draw.textureQScale;
            batch.nativeLocalToClip=draw.LocalToClip();
            Require(std::all_of(batch.nativeLocalToClip.begin(),batch.nativeLocalToClip.end(),
                [](float v){return std::isfinite(v);}),"Native solid projection matrix is nonfinite");
            batch.nativeClipMapping={{static_cast<float>(clipScale[0]),static_cast<float>(clipScale[1]),
                static_cast<float>(clipOffset[0]),static_cast<float>(clipOffset[1])}};
            Require(draw.strip!=nullptr,"Vulkan solid draw has no actual strip");const auto& strip=*draw.strip;
            const auto count=strip.positions.size();Require(strip.textureCoordinates.size()==count && strip.colors.size()==count,
                "Original strip vertex/color/UV extents differ");
            Require(count<=48,"Vulkan strip exceeds the original packet vertex extent");
            const bool lit=(draw.drawFlags&0xc0u)!=0;
            if(lit || reflection)Require(draw.LightContext() && draw.lightMaterial && strip.normals.size()==count,
                "Lit strip lacks its source context, material or signed normal payload");
            float scroll[2]{};
            const auto* textureRecord=draw.orderingTexture ? draw.orderingTexture->descriptor->authoredRecord:
                draw.texture ? draw.texture->descriptor->authoredRecord:nullptr;
            const auto scrollMode=textureRecord && !reflection ? static_cast<std::int8_t>(textureRecord[0x79]):0;
            if(scrollMode) {
                // CreateTextureContext108D80..108E4C writes S,T,T,S to its
                //+70 vector; UNPACK at10913C places it atVU30F. The ordinary
                //strip program0E08/0F40 adds it before perspective division.
                const bool override=textureAnimationScrollSpeed[0]!=0.0f || textureAnimationScrollSpeed[1]!=0.0f;
                for(std::size_t axis=0;axis<2;++axis) {
                    const auto rate=override ? textureAnimationScrollSpeed[axis]/
                        static_cast<float>(static_cast<std::int16_t>(ReadU16(textureRecord+0x24+axis*2))):
                        real(textureRecord+0x7c+axis*4);
                    scroll[axis]=TextureInfo::GetScroll(frame.TextureTime(),rate,
                        override ? textureAnimationScrollMode:scrollMode,
                        override ? textureAnimationScrollInterval:real(textureRecord+0x84));
                }
            }
            // A resident hit already owns validated first-use topology.
            // The public source snapshot is still checked once per Gather.
            VkSolidStripJob job;job.draw=&draw;solidScroll={{scroll[0],scroll[1]}};
            job.scroll={{scroll[0],scroll[1]}};job.lod=textureLod;
            job.glyphBoundsIndex=glyphBoundsIndex;job.reflection=reflection;job.targetSample=targetSample;
            job.lineTopology=batch.wireframe;
            step(3);
            batch.nativeShadingIndex=nativeFrameShading.Capture(job,gatheredGlyphBounds);
            step(1);
            bool prepare=false;auto* resident=residentGeometry.Acquire(job,prepare);
            step(2);
            nativeSolidKinds[reflection ? 1:specular ? 2:draw.carShadow!=EcstasyCarShadow::None ? 3:0]+=1;
            if(!resident)nativeSolidKinds[4]+=1; else if(prepare)nativeSolidKinds[5]+=1;
            VkSolidTopology topology;
            if(!resident) {
                topology=DecodeNativeSolidTopology(strip,job.lineTopology);
                job.count=topology.count;job.sourceOrder=topology.sourceOrder;
            }
            std::uint32_t firstVertex;
            if(resident) {
                firstVertex=resident->slot*static_cast<std::uint32_t>(VkResidentGeometryCache::VerticesPerSlot);
                batch.residentGeometry=true;job.destination=0;job.residentOutput=resident->shadow.data();
                residentFrameVertices+=job.count;
                Require(residentFrameIndices+resident->indexCount<=std::numeric_limits<std::uint32_t>::max(),
                    "Native resident frame index count exceeds32-bit draw range");
                Require(gatheredResidentIndexSpans.size()<std::numeric_limits<std::uint32_t>::max(),
                    "Native resident index span count exceeds32-bit range");
                batch.first=static_cast<std::uint32_t>(residentFrameIndices);batch.count=resident->indexCount;
                batch.nativeSpanFirst=static_cast<std::uint32_t>(gatheredResidentIndexSpans.size());batch.nativeSpanCount=1;
                gatheredResidentIndexSpans.push_back(resident);gatheredResidentSpanTokens.push_back(resident->indexToken);residentFrameIndices+=resident->indexCount;
            } else {
                Require(output.size()+job.count<std::numeric_limits<std::uint32_t>::max(),
                    "Vulkan frame vertex count exceeds32-bit index range");
                firstVertex=static_cast<std::uint32_t>(output.size());job.destination=firstVertex;
                output.resize(output.size()+job.count);prepare=true;
            }
            if(!resident)for(std::size_t index=0;index<topology.indexCount;++index)
                elementIndices.push_back(firstVertex+topology.indices[index]);

            job.workUnits=prepare ? NativePendingVertexWork(job):0;
            if(job.workUnits) {
                if(directSourcePreparation)
                    PrepareNativeSolidStripVertices(job,job.residentOutput ? job.residentOutput:output.data());
                else {nativePendingWork+=job.workUnits;nativeStripJobs.push_back(job);}
            }
        } else if(particle) {
            Require(draw.particleGroup,"Particle draw lost its contiguous group");
            const double coverageScale=static_cast<double>(batch.viewport.width)*batch.viewport.height/
                (4.0*static_cast<double>(options.width)*options.height);
            auto& coverage=nativeParticleCoverage[destination==0 ? 0:1];
            const bool mirrorCulled=ecstasyParticleMirrorCull && destination!=0 && destination==nativeMirrorTargetId;
            const bool fade=ecstasyParticleNearFadeEnd>ecstasyParticleNearFadeStart && ecstasyParticleNearFadeStart>0.0f;
            if(!mirrorCulled)for(const auto& quad:*draw.particleGroup) {
                float fadeScale=1.0f;
                {
                    float x0=quad.clipXY[0][0],x1=x0,y0=quad.clipXY[0][1],y1=y0;
                    for(std::size_t corner=1;corner<4;++corner) {
                        x0=std::min(x0,quad.clipXY[corner][0]);x1=std::max(x1,quad.clipXY[corner][0]);
                        y0=std::min(y0,quad.clipXY[corner][1]);y1=std::max(y1,quad.clipXY[corner][1]);
                    }
                    if(fade) {
                        // Unclipped quad area as a fraction of the viewport: a
                        // puff this large sits at the lens and is mostly fill.
                        const float fraction=(x1-x0)*(y1-y0)*0.25f;
                        if(fraction>ecstasyParticleNearFadeStart)
                            fadeScale=std::max(0.0f,(ecstasyParticleNearFadeEnd-fraction)/
                                (ecstasyParticleNearFadeEnd-ecstasyParticleNearFadeStart));
                    }
                    if(fadeScale<=0.0f)continue;
                    x0=std::max(x0,-1.0f);y0=std::max(y0,-1.0f);x1=std::min(x1,1.0f);y1=std::min(y1,1.0f);
                    if(x1>x0 && y1>y0)coverage+=static_cast<double>(x1-x0)*(y1-y0)*coverageScale;
                    nativeParticleCoverage[2]+=1.0;
                }
                const auto firstVertex=output.size();
                auto particleColor=vulkanColor(quad.color,draw.alphaScale);
                particleColor[3]*=fadeScale;
                for(std::size_t corner=0;corner<4;++corner) {
                    std::array<float,4> point{{quad.clipXY[corner][0],quad.clipXY[corner][1],quad.depth,1.0f}};
                    if(translation[0]!=0.0 || translation[1]!=0.0) {
                        point[0]=static_cast<float>(point[0]*clipScale[0]+clipOffset[0]);
                        point[1]=static_cast<float>(point[1]*clipScale[1]+clipOffset[1]);
                    }
                    append({point,{{quad.uv[corner][0],quad.uv[corner][1],1.0f}},particleColor});
                }
                for(const auto corner:{0u,1u,2u,1u,3u,2u})
                    elementIndices.push_back(static_cast<std::uint32_t>(firstVertex+corner));
            }
            statistics.particles+=draw.particleGroup->size();
        } else if(draw.kind==EcstasyDrawKind::WorldStrip || remap) {
            Require(draw.worldStrip,"Vulkan world strip has no retained vertices");
            const auto& strip=*draw.worldStrip;
            const auto firstVertex=output.size();
            if(strip.size()>=3)for(const auto& source:strip) {
                auto vertexColor=vulkanColor(source.color,draw.alphaScale);
                if(targetSample)vertexColor[3]*=128.0f/255.0f;
                append({position(source.position.data()),source.textureCoordinate,vertexColor});
            }
            for(std::size_t triangle=2;triangle<strip.size();++triangle) {
                const std::size_t triangleIndices[]={triangle-2+(triangle&1u),triangle-1-(triangle&1u),triangle};
                for(auto index:triangleIndices)elementIndices.push_back(static_cast<std::uint32_t>(firstVertex+index));
            }
        } else {
            Require(draw.polygon!=nullptr,"Vulkan polygon draw has no actual polygon");
            // ePoly stores perimeter vertices; native triangles retain the
            // source's0,1,3,2 packet strip winding without a crossed diagonal.
            const std::size_t polygonIndices[]={0,1,3,1,2,3};
            std::array<std::uint32_t,4> decoded{};unsigned decodedMask{};
            for(auto index:polygonIndices) {
                if(decodedMask&(1u<<index)) {elementIndices.push_back(decoded[index]);continue;}
                const auto& source=draw.polygon->vertices[index];
                auto vertexColor=vulkanColor(source.color,draw.alphaScale);
                // Uploaded GS textures store alpha in raw8-bit units; the
                // native color attachment already stores the shader's logical
                // alpha. Undo only that upload-specific alpha conversion when
                // sampling this GPU target through the common TFX0 shader.
                if(targetSample)vertexColor[3]*=128.0f/255.0f;
                auto sampledUV=source.textureCoordinate;
                if(rearSample)sampledUV[0]=ecstasyNativeMirrorMaskedSampleU(sampledUV[0]);
                decoded[index]=append({position(source.position.data()),sampledUV,vertexColor});
                decodedMask|=1u<<index;elementIndices.push_back(decoded[index]);
            }
        }
        Require(elementIndices.size()<=std::numeric_limits<std::uint32_t>::max(),"Vulkan frame index count exceeds32-bit draw range");
        if(!batch.residentGeometry)batch.count=static_cast<std::uint32_t>(elementIndices.size())-batch.first;
        if(batch.count && batch.scissor.extent.width && batch.scissor.extent.height) {
            if(draw.carShadow!=EcstasyCarShadow::None) {
                const auto shadow=static_cast<std::size_t>(draw.carShadow)-1;
                ++profile.shadowStrips[shadow];
                profile.shadowTriangles[shadow]+=batch.count/3u;
            }
            if(!batch.residentGeometry && !batch.nativeGpuPosition && output.size()>drawVertexStart) {
                bool unit=true;
                for(auto at=drawVertexStart;unit && at<output.size();++at)unit=output[at].uvq[2]==1.0f;
                batch.affineUV=unit;
            }
            step(solidStep ? 3:5);
            // A Vulkan draw is only a transport boundary. Concatenate adjacent
            // triangles when every consumed GPU state value agrees. Original
            // packet order, texture versions, alpha/depth, and target changes
            // are untouched; no sorting or transparency approximation occurs.
            appendBatch(batch);
            step(4);
        }
        if(ecstasySolidTemplate && solidTemplateEligible(draw) && batch.nativeGpuPosition &&
            batch.scissor.extent.width && batch.scissor.extent.height) {
#if !defined(__vita__) && !defined(__SWITCH__)
            if(solidMatch) {
                // Verify: the full path's state must equal the template's.
                const auto& expected=solidMatch->batch;
                const float lod1=draw.kind==EcstasyDrawKind::SolidStrip ?
                    static_cast<std::uint8_t>(draw.strip->autoK)*0.0625f:solidMatch->lod1;
                const bool same=batch.SameDrawState(expected) && batch.cachedOpaqueScenery==expected.cachedOpaqueScenery &&
                    batch.nativeGpuPosition==expected.nativeGpuPosition &&
                    batch.nativeLocalToClip==expected.nativeLocalToClip && batch.nativeClipMapping==expected.nativeClipMapping &&
                    batch.nativeTextureQScale==expected.nativeTextureQScale && batch.nativeShadingIndex==expected.nativeShadingIndex &&
                    batch.nativeTransformIndex==expected.nativeTransformIndex && batch.affineUV==expected.affineUV &&
                    batch.wireframe==expected.wireframe && batch.particle==expected.particle &&
                    solidScroll==solidMatch->scroll && textureLod[1]==lod1;
                static std::uint64_t checks=0,mismatches=0;
                ++checks;if(!same)++mismatches;
                if((checks&4095u)==0 || (!same && mismatches<8)) {
                    char text[96];std::snprintf(text,sizeof(text),"checks=%llu mismatches=%llu",
                        (unsigned long long)checks,(unsigned long long)mismatches);
                    platform::RecordSessionEvent("render.solid_template_verify",text);
                }
            }
#endif
#if defined(__vita__) || defined(__SWITCH__)
            (void)solidMatch;
#endif
            auto& slot=solidTemplateSlot(draw);
            slot.draw=&draw;slot.batch=batch;slot.scroll=solidScroll;slot.lod1=textureLod[1];
        }
        if(shadowStarted!=Clock::time_point{}) {
            const auto shadow=static_cast<std::size_t>(draw.carShadow)-1;
            profile.shadowGatherMilliseconds[shadow]+=
                std::chrono::duration<double,std::milli>(Clock::now()-shadowStarted).count();
        }
    }
    categoryTimer.Finish();
    if(ecstasySceneryRunReplay) {
        static unsigned replayFrames=0;
        if(++replayFrames==300) {
            const auto& c=nativeSceneryReplayCounters;
            const auto epoch=ecstasyNativeStripEpoch.load(std::memory_order_relaxed);
            static std::uint64_t lastEpoch=epoch;
            char text[256];std::snprintf(text,sizeof(text),"per_frame hits=%.1f misses=%.1f strips=%.1f records=%.1f table=%u fast_strips=%.1f exact_strips=%.1f epoch_bumps=%llu",
                c[0]/300.0,c[1]/300.0,c[2]/300.0,c[3]/300.0,static_cast<unsigned>(sceneryRunRecords.size()),
                nativeReplayFastCounters[0]/300.0,nativeReplayFastCounters[1]/300.0,(unsigned long long)(epoch-lastEpoch));
            const auto used=std::strlen(text);
            std::snprintf(text+used,sizeof(text)-used," first_fast=%.1f first_records=%.1f",
                nativeSceneryFirstCounters[0]/300.0,nativeSceneryFirstCounters[1]/300.0);
            nativeSceneryFirstCounters[0]=nativeSceneryFirstCounters[1]=0;
            lastEpoch=epoch;nativeReplayFastCounters={};
            platform::RecordSessionEvent("render.scenery_replay",text);
            nativeSceneryReplayCounters={};replayFrames=0;
        }
    }
    if(ecstasySceneryReuseCensus) {
        auto& census=reuseCensus;
        std::swap(census.lastSeq,census.frameSeq);std::swap(census.lastSeqInst,census.frameSeqInst);
        census.frameSeq.clear();census.frameSeqInst.clear();
        if(++census.frames==300) {
            char text[320];int used=0;
            for(std::size_t view=0;view<2;++view) {
                const auto& r=census.sum[view];const double f=census.frames;
                used+=std::snprintf(text+used,sizeof(text)-used,"%s singles=%.0f runs=%.0f run_strips=%.0f strips=%.0f same_frame_rep=%.0f prev_seq=%.0f prev_seq_inst=%.0f ",
                    view ? "other":"main",r[0]/f,r[1]/f,r[2]/f,r[6]/f,r[3]/f,r[4]/f,r[5]/f);
            }
            platform::RecordSessionEvent("render.scenery_reuse",text);
            census.sum={};census.frames=0;
        }
    }
    const auto finishStarted=Clock::now();
    profile.coldVertexJobs+=nativeStripJobs.size();profile.coldVertices+=nativePendingWork;
    if(!directSourcePreparation) {
        const auto prepareJob=[&](std::size_t jobIndex) {
            const auto& job=nativeStripJobs[jobIndex];
            PrepareNativeSolidStripVertices(job,job.residentOutput ? job.residentOutput:output.data());
        };
        if(nativePendingWork<8192 || nativeStripJobs.size()<64) {
            for(std::size_t index=0;index<nativeStripJobs.size();++index)prepareJob(index);
        } else {
            // Chunk by actual cold vertices and available processors. The
            // shared pool drains all disjoint writes, including failures,
            // before resident publication or unwinding borrowed frame input.
            const auto shares=static_cast<std::size_t>(nativeWorkers)*4;
            const auto budget=std::clamp<std::size_t>(nativePendingWork/shares+(nativePendingWork%shares!=0),512,8192);
            std::size_t first=0,work=0;
            for(std::size_t index=0;index<nativeStripJobs.size();++index) {
                work+=nativeStripJobs[index].workUnits;
                if(work>=budget) {nativeWorkRanges.push_back({first,index+1});first=index+1;work=0;}
            }
            if(first<nativeStripJobs.size())nativeWorkRanges.push_back({first,nativeStripJobs.size()});
            const auto workers=static_cast<unsigned>(std::min<std::size_t>(nativeWorkers,nativeWorkRanges.size()));
            RunNativeSceneJobs(nativeWorkRanges.size(),[&](std::size_t rangeIndex) {
                const auto& range=nativeWorkRanges[rangeIndex];
                for(auto index=range.first;index<range.last;++index)prepareJob(index);
            },1,workers);
        }
    }
    residentGeometry.CompletePreparation();
    const auto indicesStarted=Clock::now();
    nativeGatherFrameMs[1]=std::chrono::duration<double,std::milli>(finishStarted-loopStarted).count();
    nativeGatherFrameMs[2]=std::chrono::duration<double,std::milli>(indicesStarted-finishStarted).count();
    profile.gatherVertexJobsMilliseconds+=nativeGatherFrameMs[2];
    ResolveNativeResidentIndices(residentIndexCache,gatheredResidentIndexSpans,gatheredResidentSpanTokens,elementIndices,batches);
    nativeGatherFrameMs[3]=std::chrono::duration<double,std::milli>(Clock::now()-indicesStarted).count();
    profile.gatherIndicesMilliseconds+=nativeGatherFrameMs[3];
    profile.gatherFinishMilliseconds+=std::chrono::duration<double,std::milli>(Clock::now()-finishStarted).count();
}
