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
std::array<double,7> nativeGatherStepMilliseconds{};

void EcstasyVulkanDevice::Storage::Gather(const EcstasyFrame& frame,
    std::vector<VkVertex>& output,std::vector<std::uint32_t>& elementIndices,
    std::vector<VkDrawBatch>& batches)
{
    // BeginGather also identifies validation performed during overlap
    // preflight. The sealed frame cannot mutate between that check and here.
    residentGeometry.Begin(gatherSerial);residentIndexCache.Begin(gatherSerial);
    residentFrameVertices=0;residentFrameIndices=0;gatheredResidentIndexSpans.clear();
    nativeStripJobs.clear();nativeWorkRanges.clear();nativeFrameShading.Clear();
    using Clock=std::chrono::steady_clock;
    auto& profile=statistics.renderProfile;
    profile.shadowStrips={};profile.shadowTriangles={};
    const bool sampleShadowGather=platform::nativeDiagnosticsEnabled && nextSubmission%16u==0;
    profile.shadowGatherSamples=sampleShadowGather ? 1u:0u;
    profile.shadowGatherMilliseconds={};
    const auto nativeWorkers=NativeSceneEffectiveHardwareConcurrency();
    const auto validateStarted=Clock::now();
    residentGeometry.ValidateSources(frame.Commands(),nativeWorkers);
    profile.gatherValidationMilliseconds+=std::chrono::duration<double,std::milli>(Clock::now()-validateStarted).count();
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
        } else batches.push_back(batch);
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
    const auto prepareSceneryPacket=[&](const EcstasyDrawCommand& draw) {
        if(draw.kind!=EcstasyDrawKind::SolidStrip || !draw.instance || !draw.strip ||
            !draw.texture || draw.sampledTarget || draw.orderingTexture ||
            draw.carShadow!=EcstasyCarShadow::None || !draw.target ||
            draw.target->id<0 || draw.target->id>14 ||
            draw.target->width<=0 || draw.target->height<=0)return false;
        const auto textureIndex=resolveTexture(*draw.texture,NativeDrawClampsTexture(draw));
        auto& texture=textures[textureIndex];
        const auto* wire=draw.texture->descriptor->authoredRecord;
        // Scrolling materials retain the normal dynamic preparation path.
        if(wire[0x79])return false;
        const auto destination=draw.target->id>=7 ? draw.target->id:0;
        const auto& raster=rasterFor(*draw.target,destination);
        if(!raster.scissor.extent.width || !raster.scissor.extent.height)return false;
        const auto& strip=*draw.strip;
        const auto count=strip.positions.size();
        Require(strip.textureCoordinates.size()==count && strip.colors.size()==count,
            "Original strip vertex/color/UV extents differ");
        Require(count<=48,"Vulkan strip exceeds the original packet vertex extent");
        Require(std::isfinite(draw.textureQScale),"Native solid texture Q scale is nonfinite");
        if(draw.drawFlags&0xc0u)
            Require(draw.LightContext() && draw.lightMaterial && strip.normals.size()==count,
                "Lit strip lacks its source context, material or signed normal payload");
        const auto identity=reinterpret_cast<std::uintptr_t>(draw.instance);
        auto& validated=validatedSceneryInstances[((identity>>4)^(identity>>12))&
            (validatedSceneryInstances.size()-1)];
        if(validated!=draw.instance) {
            Require(std::all_of(draw.LocalToClip().begin(),draw.LocalToClip().end(),
                [](float value){return std::isfinite(value);}),"Native solid projection matrix is nonfinite");
            validated=draw.instance;
        }
        VkSolidStripJob job;job.draw=&draw;
        job.lod={{draw.textureQScale,static_cast<std::uint8_t>(strip.autoK)*0.0625f}};
        job.lineTopology=(draw.nativeRenderEggState&EcstasyRenderEggWireframe)!=0;
        bool prepare=false;
        auto* resident=residentGeometry.Acquire(job,prepare);
        if(!resident)return false;
        const auto state=draw.nativeRenderEggState |
            (static_cast<std::uint32_t>(draw.texture->drawFlag4Enabled)<<3) |
            (static_cast<std::uint32_t>(draw.target->id==14)<<4);
        VkDrawBatch batch;
        if(const auto* cached=sceneryPackets.Find(*resident,texture.generation,state)) {
            batch=*cached;
            ++statistics.sceneryPacketHits;
        } else {
            const auto& material=ResolveOpaqueSceneryMaterial(texture,wire,options.textureFilter);
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
        // Frame-local ranges, shading, camera and viewport never enter the cache.
        batch.texture=textureIndex;batch.target=destination;
        batch.viewport=raster.viewport;batch.scissor=raster.scissor;
        applyDepthRange(draw,batch);
        batch.nativeLocalToClip=draw.LocalToClip();
        batch.nativeClipMapping={{static_cast<float>(raster.clipScale[0]),static_cast<float>(raster.clipScale[1]),
            static_cast<float>(raster.clipOffset[0]),static_cast<float>(raster.clipOffset[1])}};
        batch.nativeTextureQScale=draw.textureQScale;
        batch.nativeShadingIndex=nativeFrameShading.Capture(job,gatheredGlyphBounds);
        Require(residentFrameIndices+resident->indexCount<=UINT32_MAX,
            "Native resident frame index count exceeds32-bit draw range");
        Require(gatheredResidentIndexSpans.size()<UINT32_MAX,"Native resident index span count exceeds32-bit range");
        batch.first=static_cast<std::uint32_t>(residentFrameIndices);
        batch.nativeSpanFirst=static_cast<std::uint32_t>(gatheredResidentIndexSpans.size());
        gatheredResidentIndexSpans.push_back(resident);
        residentFrameIndices+=resident->indexCount;residentFrameVertices+=job.count;
        job.residentOutput=resident->shadow.data();
        job.workUnits=prepare ? NativePendingVertexWork(job):0;
        if(job.workUnits) {
            if(directSourcePreparation)PrepareNativeSolidStripVertices(job,job.residentOutput);
            else {nativePendingWork+=job.workUnits;nativeStripJobs.push_back(job);}
        }
        if(batch.count)appendBatch(batch);
        return true;
    };
    const bool sampleCategories=platform::nativeDiagnosticsEnabled && nextSubmission%32u==0;
    if(sampleCategories)++profile.gatherCategorySamples;
    NativeGatherCategoryTimer categoryTimer(sampleCategories ? &profile:nullptr);
    if(sampleCategories)nativeGatherStepMilliseconds[6]+=1.0;
    Clock::time_point stepMark{};
    const auto step=[&](std::size_t index) {
        if(!sampleCategories)return;
        const auto now=Clock::now();
        nativeGatherStepMilliseconds[index]+=std::chrono::duration<double,std::milli>(now-stepMark).count();
        stepMark=now;
    };
    for (const auto& draw:frame.Commands()) {
        categoryTimer.Enter(draw);
        if(sampleCategories)stepMark=Clock::now();
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
            if(prepareSceneryPacket(draw))continue;
            ++statistics.sceneryPacketFallbacks;
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
        const bool particle=draw.kind==EcstasyDrawKind::ParticleGroup;
        const bool mirrorOverlay=draw.kind==EcstasyDrawKind::MirrorOverlayPolygon;
        const bool ui=draw.kind==EcstasyDrawKind::FlailerPolygon || rearSample || mirrorOverlay || filter || remap || screenPolygon;
        const bool layeredUi=ui && (draw.rasterState[2]&255u)!=0;
        Require(elementIndices.size()<=std::numeric_limits<std::uint32_t>::max(),
            "Vulkan frame index count exceeds32-bit draw range");
        VkDrawBatch batch;batch.indexed=true;batch.cachedOpaqueScenery=draw.cachedOpaqueScenery;
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
            VkSolidStripJob job;job.draw=&draw;
            job.scroll={{scroll[0],scroll[1]}};job.lod=textureLod;
            job.glyphBoundsIndex=glyphBoundsIndex;job.reflection=reflection;job.targetSample=targetSample;
            job.lineTopology=batch.wireframe;
            step(3);
            batch.nativeShadingIndex=nativeFrameShading.Capture(job,gatheredGlyphBounds);
            step(1);
            bool prepare=false;auto* resident=residentGeometry.Acquire(job,prepare);
            step(2);
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
                gatheredResidentIndexSpans.push_back(resident);residentFrameIndices+=resident->indexCount;
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
            for(const auto& quad:*draw.particleGroup) {
                const auto firstVertex=output.size();
                const auto particleColor=vulkanColor(quad.color,draw.alphaScale);
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
            step(solidStep ? 3:5);
            // A Vulkan draw is only a transport boundary. Concatenate adjacent
            // triangles when every consumed GPU state value agrees. Original
            // packet order, texture versions, alpha/depth, and target changes
            // are untouched; no sorting or transparency approximation occurs.
            appendBatch(batch);
            step(4);
        }
        if(shadowStarted!=Clock::time_point{}) {
            const auto shadow=static_cast<std::size_t>(draw.carShadow)-1;
            profile.shadowGatherMilliseconds[shadow]+=
                std::chrono::duration<double,std::milli>(Clock::now()-shadowStarted).count();
        }
    }
    categoryTimer.Finish();
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
    profile.gatherVertexJobsMilliseconds+=
        std::chrono::duration<double,std::milli>(indicesStarted-finishStarted).count();
    ResolveNativeResidentIndices(residentIndexCache,gatheredResidentIndexSpans,elementIndices,batches);
    profile.gatherIndicesMilliseconds+=std::chrono::duration<double,std::milli>(Clock::now()-indicesStarted).count();
    profile.gatherFinishMilliseconds+=std::chrono::duration<double,std::milli>(Clock::now()-finishStarted).count();
}
