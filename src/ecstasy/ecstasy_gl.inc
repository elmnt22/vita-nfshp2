// vitaGL submission backend (PS Vita) for the shared native Gather.
// Included from ecstasye.cpp inside namespace hp2 in place of the Vulkan
// Storage. The CPU frame preparation (Gather, resident geometry/index caches,
// texture decode) is the unchanged shared code; only GPU submission differs.
//
// Stage-2 scope: the main display target (0). Offscreen targets (mirror,
// environment, remap), feedback/filter, fog-depth and focus passes are
// recorded by Gather but not yet drawn here. Specular response tables use the
// programmatic pow fallback. See ps2/docs or the spike memory for the plan.

// Diagnostic: bytes of live GL texture images (renderer.vitagl.memory).
inline std::atomic<std::uint64_t> glLiveTextureBytes{0},glLiveTextureCount{0},glTargetBuilds{0},glBufferBuilds{0};
namespace {
template<class T> T glToken(GLuint name) noexcept
{return reinterpret_cast<T>(static_cast<std::uintptr_t>(name));}
template<class T> GLuint glName(T token) noexcept
{return static_cast<GLuint>(reinterpret_cast<std::uintptr_t>(token));}

// One persistently mapped vitaGL buffer. vitaGL returns its own GPU-visible
// storage from glMapBuffer, so writes land in place without a staging copy.
struct GlMappedBuffer {
    GLuint name{};
    GLenum target{};
    std::size_t size{};
    std::uint8_t* mapped{};
    void Destroy() noexcept {
        if(name)glDeleteBuffers(1,&name);
        name=0;size=0;mapped=nullptr;
    }
    void Ensure(GLenum bufferTarget,std::size_t required) {
        if(name && size>=required)return;
        Destroy();++glBufferBuilds;
        target=bufferTarget;
        // Grow by half again: each regrowth leaves the old block allocated
        // until vitaGL's deferred free, so small steps churn GPU memory.
        const auto grown=size ? size+size/2:std::size_t(0);
        size=std::max<std::size_t>((std::max(required,grown)+65535u)&~std::size_t(65535u),65536u);
        glGenBuffers(1,&name);
        glBindBuffer(target,name);
        glBufferData(target,static_cast<GLsizeiptr>(size),nullptr,GL_DYNAMIC_DRAW);
        mapped=static_cast<std::uint8_t*>(glMapBuffer(target,GL_WRITE_ONLY));
        Require(mapped!=nullptr,"vitaGL did not map a native geometry buffer");
    }
};

// A GL texture object owned through a stable heap handle: the preparing
// thread refers to it while only the GL thread creates, fills or deletes it.
struct GlTexture {
    GLuint name{};
    std::uint32_t bytes{};
};
struct GlPendingTexture {
    GlTexture* texture{};
    std::vector<std::uint8_t> rgba;
    std::uint32_t width{},height{};
    GLint filter{},wrapS{},wrapT{};
};
// Everything one frame's GL submission consumes. Gather fills it on the
// render thread; the GL thread draws it while the next frame is gathered.
// It holds no pointer into the source EcstasyFrame.
struct GlFramePacket {
    std::vector<VkVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<VkDrawBatch> batches;
    std::vector<std::array<float,4>> glyphBounds;
    std::vector<GlPendingTexture> uploads;
    std::vector<GlTexture*> deletes;
    struct TextureRef {GlTexture* texture{};std::uint32_t width{},height{};};
    std::vector<TextureRef> textures;
    std::array<std::array<std::uint32_t,2>,16> targets{};
    std::array<bool,16> targetUsed{};
    std::vector<std::uint8_t> residentVertexBytes,residentIndexBytes;
    std::vector<std::array<std::size_t,2>> residentVertexRanges,residentIndexRanges;
    std::size_t residentVertexCapacity{},residentIndexCapacity{};
    std::uint32_t width{},height{};
    double gatherMilliseconds{};
    double particleScreens{}; // main-target particle coverage this frame
    void Clear() {
        uploads.clear();deletes.clear();textures.clear();targetUsed={};
        residentVertexBytes.clear();residentIndexBytes.clear();
        residentVertexRanges.clear();residentIndexRanges.clear();
        residentVertexCapacity=residentIndexCapacity=0;
    }
};

constexpr const char* kGlVertexShader=R"glsl(
attribute vec4 a_pos;
attribute vec3 a_uvq;
attribute vec4 a_col;
attribute vec3 a_nrm;
uniform vec4 u_m0;
uniform vec4 u_m1;
uniform vec4 u_m2;
uniform vec4 u_m3;
uniform vec4 u_clipmap;
uniform float u_yflip;
#if SHADE
uniform vec4 u_shade[19];
uniform vec4 u_mat[4];
uniform vec4 u_flags;
uniform vec4 u_flags2;
#endif
varying vec3 v_uvq;
varying vec4 v_col;
#if SHADE
vec3 hp2Reflection(vec3 p,vec3 n)
{
    vec3 rel=p-u_shade[3].xyz;
    float twiceDot=2.0*dot(rel,n);
    vec3 r=rel-n*twiceDot;
    float sq=dot(r,r);
    return sq>0.0 ? r*inversesqrt(sq) : vec3(0.0);
}
float hp2Specular(float base,bool alphaMaterial)
{
    float limited=max(base,u_mat[0].z);
    if(alphaMaterial)return u_mat[3].y*pow(max(limited,0.0),u_mat[3].x);
    return u_mat[0].y*pow(max(limited,0.0),u_mat[0].x);
}
vec4 hp2Lit(vec3 p,vec3 n,vec3 authored,vec3 prepared,bool hasPrepared)
{
    bool materialAlpha=u_flags.z>0.5;
    bool lit=u_flags.x>0.5;
    bool alphaLit=u_flags.y>0.5;
    bool rgbSpecular=u_mat[2].x!=0.0 || u_mat[2].y!=0.0 || u_mat[2].z!=0.0;
    bool litSpecular=lit && (rgbSpecular || (materialAlpha && u_mat[3].w!=0.0));
    bool alphaSpecular=alphaLit && u_mat[3].w!=0.0;
    vec3 r=vec3(0.0);
    if(litSpecular || alphaSpecular)r=hasPrepared ? prepared : hp2Reflection(p,n);
    vec4 o=vec4(authored,1.0);
    if(lit) {
        vec3 diffuse=max(u_shade[4].xyz*n.x+u_shade[5].xyz*n.y+u_shade[6].xyz*n.z,vec3(0.0));
        vec3 specular=vec3(0.0);
        if(litSpecular) {
            vec3 rd=u_shade[4].xyz*r.x+u_shade[5].xyz*r.y+u_shade[6].xyz*r.z;
            specular=vec3(hp2Specular(rd.x,materialAlpha),hp2Specular(rd.y,materialAlpha),
                hp2Specular(rd.z,materialAlpha));
        }
        vec4 value=u_shade[12]+u_shade[13]*diffuse.x+u_shade[14]*diffuse.y+u_shade[15]*diffuse.z+
            u_shade[16]*specular.x+u_shade[17]*specular.y+u_shade[18]*specular.z;
        value.rgb=value.rgb*(vec3(1.0)+(authored*128.0-vec3(255.0))*u_mat[0].w);
        o=floor(clamp(value,0.0,255.0))/128.0;
    }
    if(alphaLit) {
        float rdot=alphaSpecular ? u_shade[4].x*r.x+u_shade[5].x*r.y+u_shade[6].x*r.z : 0.0;
        float sa=alphaSpecular ? hp2Specular(rdot,true) : 0.0;
        float al=u_mat[3].z+u_mat[3].w*sa;
        o.a=floor(clamp(al,0.0,255.0))/128.0;
    }
    return o;
}
#endif
void main()
{
#if GPUPOS
    vec4 lp=a_pos;
#if SHADE
    lp=vec4(a_pos.xyz+u_shade[2].xyz,u_shade[2].w);
#endif
    vec4 p=u_m0*lp.x+u_m1*lp.y+u_m2*lp.z+u_m3*lp.w;
    p.xy=p.xy*u_clipmap.xy+p.w*u_clipmap.zw;
#else
    vec4 p=a_pos;
    p.z=p.z*2.0-p.w;
#endif
    p.y=p.y*u_yflip;
    gl_Position=p;
    v_uvq=a_uvq;
    v_col=a_col;
#if SHADE
    vec4 parameters=u_shade[1];
    vec3 coordinates=vec3(a_uvq.xy+parameters.yz,a_uvq.z);
    vec4 result=vec4(a_col.rgb,parameters.x);
    if(u_flags.w>0.5) {
        vec3 reflection=hp2Reflection(a_pos.xyz,a_nrm);
        vec3 mapped=(u_shade[8].xyz*reflection.x+u_shade[9].xyz*reflection.y+
            u_shade[10].xyz*reflection.z)*vec3(-1.0,1.0,-1.0);
        float z=mapped.z+1.0;
        float sq=mapped.x*mapped.x+mapped.y*mapped.y+z*z;
        float inv=sq>0.0 ? inversesqrt(sq) : 0.0;
        coordinates=vec3(0.5+mapped.x*inv*0.5,0.5+mapped.y*inv*0.5,1.0);
        result=vec4(1.0);
        if(u_flags.z>0.5) {
            if(u_flags.x>0.5)result=hp2Lit(a_pos.xyz,a_nrm,a_col.rgb,reflection,true);
            else result=vec4(a_col.rgb,1.0);
        }
        float alpha=u_mat[1].w+u_mat[2].w*mapped.z;
        float whole=alpha<0.0 ? ceil(alpha) : floor(alpha);
        result.a=(whole-256.0*floor(whole/256.0))/128.0;
        if(u_flags2.x>0.5)result.a=result.a*(128.0/255.0);
    } else if(u_flags.x>0.5 || u_flags.y>0.5) {
        result=hp2Lit(a_pos.xyz,a_nrm,a_col.rgb,vec3(0.0),false);
        result.a=result.a*parameters.x;
    }
    v_uvq=coordinates;
    v_col=result;
#endif
}
)glsl";

constexpr const char* kGlFragmentShader=R"glsl(
uniform sampler2D u_tex;
uniform vec4 u_fparam;
#if DEPTHTEX
uniform sampler2D u_depth;
uniform vec4 u_depthmap;
#endif
varying vec3 v_uvq;
varying vec4 v_col;
void main()
{
#if DEPTHTEX
    // Half-resolution particle pass: LEQUAL against the scene depth.
    if(gl_FragCoord.z>texture2D(u_depth,gl_FragCoord.xy*u_depthmap.xy+u_depthmap.zw).r)discard;
#endif
#if TEXTURING
#if AFFINE
    vec2 uv=v_uvq.xy;
#else
    vec2 uv=v_uvq.xy/v_uvq.z;
#endif
#if CLAMPTL
    uv=clamp(uv,u_fparam.yz*0.5,u_fparam.yz*1.5);
#endif
    vec4 s=texture2D(u_tex,uv);
    s.a=s.a*(255.0/128.0);
#if FUNC==0
    vec4 r=s*v_col;
#elif FUNC==1
    vec4 r=s;
#elif FUNC==2
    vec4 r=vec4(s.rgb*v_col.rgb+v_col.a,s.a+v_col.a);
#else
    vec4 r=vec4(s.rgb*v_col.rgb+v_col.a,s.a);
#endif
#else
    vec4 r=vec4(v_col.rgb*(128.0/255.0),v_col.a);
#endif
#if ATEST==1
    if(r.a==0.0)discard;
#elif ATEST==2
    // floor(clamp(a*128,0,255))<=48 <=> a*128<49 <=> a<49/128 (power-of-two
    // scale is exact in any float precision).
    if(r.a<0.3828125)discard;
#endif
#if SHOWCASE
    vec3 source=floor(clamp(r.rgb*255.0,vec3(0.0),vec3(255.0)))/255.0;
    float sourceAlpha=floor(clamp(r.a*128.0,0.0,255.0))/128.0;
    r.rgb=source*sourceAlpha;
#endif
#if AFIX
    r.a=u_fparam.x;
#endif
    gl_FragColor=r;
}
)glsl";

struct GlProgram {
    GLuint program{};
    GLint m0{-1},m1{-1},m2{-1},m3{-1},clipmap{-1},shade{-1},material{-1},flags{-1},flags2{-1};
    GLint fparam{-1},texture{-1},yflip{-1},depth{-1},depthmap{-1};
    bool failed{},depthmapValid{};std::uint32_t depthmapDivisor{};
    // Last values uploaded to this program (GL keeps uniforms per program).
    // vitaGL re-reserves and copies a program's uniform buffer on any
    // change, so identical consecutive values are not set again.
    std::array<float,20> transform{};
    std::array<float,4> parameters{};
    float flip{};
    bool transformValid{},parametersValid{},flipValid{};
    std::uint32_t shading{UINT32_MAX};
};

struct GlBlendState {
    bool enable{};
    GLenum source{GL_ONE},destination{GL_ZERO},equation{GL_FUNC_ADD};
    bool fixedAlpha{};
    bool operator==(const GlBlendState& other) const noexcept {
        return enable==other.enable && source==other.source && destination==other.destination &&
            equation==other.equation && fixedAlpha==other.fixedAlpha;
    }
};

// GS ALPHA (A-B)*C+D with A/B/D lanes 0=Cs 1=Cd 2=0 and C 0=As 1=Ad 2=FIX.
// Same decomposition as the Vulkan pipeline; FIX rides in source alpha since
// GXM has no constant blend factor (the shader writes FIX as its alpha).
GlBlendState glBlendFor(const VkDrawBatch& batch)
{
    GlBlendState state;
    if(batch.showcaseEnvironmentBlend) {
        state.enable=true;state.source=GL_ONE;state.destination=GL_ONE;return state;
    }
    const int a=static_cast<int>(batch.blend&255u),b=static_cast<int>((batch.blend>>8)&255u),
        c=static_cast<int>((batch.blend>>16)&255u),d=static_cast<int>(batch.blend>>24);
    const GLenum factor=c==1 ? GL_DST_ALPHA:GL_SRC_ALPHA;
    const GLenum inverse=c==1 ? GL_ONE_MINUS_DST_ALPHA:GL_ONE_MINUS_SRC_ALPHA;
    bool usesFactor=false;
    const auto coefficient=[&](int lane,bool& negative)->GLenum {
        const int variable=(a==lane ? 1:0)-(b==lane ? 1:0),constant=d==lane ? 1:0;
        negative=variable<0 && constant==0;
        if(!variable)return constant ? GL_ONE:GL_ZERO;
        usesFactor=true;
        if(variable==1 && !constant)return factor;
        if(variable==-1)return constant ? inverse:factor;
        return GL_ONE; // (Cx-B)*C+Cx needs destination feedback; approximate.
    };
    bool sourceNegative{},destinationNegative{};
    state.source=coefficient(0,sourceNegative);state.destination=coefficient(1,destinationNegative);
    state.equation=sourceNegative ? GL_FUNC_REVERSE_SUBTRACT:destinationNegative ? GL_FUNC_SUBTRACT:GL_FUNC_ADD;
    state.enable=!(state.source==GL_ONE && state.destination==GL_ZERO && state.equation==GL_FUNC_ADD);
    state.fixedAlpha=state.enable && usesFactor && c==2;
    return state;
}
}

struct EcstasyVulkanDevice::Storage {
    EcstasyVulkanOptions options;
    std::uint64_t completedFrameSerial{};
    std::uint64_t nextSubmission{};
    EcstasyVulkanFrameStatistics statistics;
    std::map<std::int32_t,VkDrawTarget> renderedTargets;
    VkResidentGeometryCache residentGeometry;
    VkResidentIndexCache residentIndexCache;
    VkSceneryPacketCache sceneryPackets;
    std::uint64_t nextTextureGeneration{};
    std::vector<const VkResidentGeometryEntry*> gatheredResidentIndexSpans;
    std::vector<std::uint64_t> gatheredResidentSpanTokens; // indexToken of each span, recorded while hot
    std::vector<VkResidentIndexEntry*> residentIndexPendingUploads;
    std::vector<std::uint32_t> residentPendingUploads;
    std::uint64_t residentFrameVertices{};
    std::uint64_t residentFrameIndices{};
    std::vector<VkDrawTexture> textures;
    std::unordered_map<VkNativeTextureKey,std::size_t,VkNativeTextureKeyHash> textureIndices;
    std::vector<VkNativeTextureKey> referencedTextures;
    std::uint64_t gatherSerial{};
    std::vector<VkVertex> gatheredVertices;
    std::vector<std::uint32_t> gatheredIndices;
    std::vector<VkDrawBatch> gatheredBatches;
    std::vector<std::array<float,4>> gatheredGlyphBounds;
    struct NativePreparedFrame {
        EcstasyNativeHistoryReaders historyReaders;
    } preparedFrame;
    struct NativeDepthPalette {
        const EcstasyDepthPalettePass* source{};
        std::array<std::uint32_t,256> colors{};
        std::array<std::array<float,4>,256> converted{};
        std::uint64_t gathered{};
        std::uint32_t offset{};
        bool valid{};
    };
    std::array<NativeDepthPalette,8> depthPaletteCache{};
    std::vector<VkSolidStripJob> nativeStripJobs;
    std::deque<EcstasyDrawCommand> sceneryRunFallbacks;
    VkNativeFrameShading nativeFrameShading;
    std::vector<VkNativeStripWorkRange> nativeWorkRanges;

    // GL objects.
    GlMappedBuffer residentVertices,residentIndices;
    static constexpr std::size_t StreamRing=3;
    std::array<GlMappedBuffer,StreamRing> streamVertices,streamIndices;
    std::size_t streamSlot{};
    std::unordered_map<std::uint32_t,GlProgram> programs;
    // GL thread: colour texture + framebuffer for each offscreen target.
    struct GlTarget {GLuint texture{},framebuffer{};std::uint32_t width{},height{};};
    std::array<GlTarget,16> glTargets{};
    // Half-resolution particle pass: premultiplied colour target, the
    // display depth exposed as a texture, and the composite program/quad.
    // One half buffer (GXM runs scenes in order, so reuse is safe). Each
    // buffer owns a vitaGL render target + depth, which is memory-heavy:
    // more caused GPU allocation stalls, so one half run per frame.
    // [0] half resolution, [1] quarter resolution (screen-filling smoke).
    std::array<GlTarget,2> halfTargets{};
    std::size_t halfTargetCurrent{};
    std::uint32_t halfDivisor{2},depthmapDivisor{};std::uint64_t quarterFrames{};
    GLuint halfDepthTexture{},halfCompositeProgram{},halfQuad{};
    // Hardware-depth half pass: one fullscreen copy of the scene depth into
    // the half buffer's depth, then particles use the fixed-function LEQUAL
    // test (same compare as the DEPTHTEX discard, no per-fragment discard).
    GLuint halfDepthCopyProgram{};GLint halfDepthCopyMap{-1};bool halfDepthCopyFailed{},halfHardwareDepth{};
    GLint halfCompositeFlip{-1};
    bool halfCompositeFailed{};
    std::uint64_t halfRuns{},halfBatches{},halfDepthWrites{};
    std::size_t nextScreenshot{};
    int displayTargetScenes{};
    std::size_t nextDisplayShot{};
    std::uint64_t halfFramesTotal{};
    bool lastFrameHalf{};
    std::uint32_t displayShotPhase{};
    std::vector<std::pair<std::uint64_t,std::vector<std::uint32_t>>> displayShots;
    std::array<std::uint32_t,2> displayShotSize{};
    void BeginHalfParticles(std::uint32_t width,std::uint32_t height);
    void EndHalfParticles(std::uint32_t width,std::uint32_t height);
    void SaveScreenshot(std::uint32_t width,std::uint32_t height,std::uint64_t frame);
    // Two packets: one being gathered, one being drawn.
    std::array<GlFramePacket,2> packets;
    GlFramePacket* building{&packets[0]};
    std::size_t nextPacket{};
    SceUID freeSema_{-1},readySema_{-1};
    pthread_t glThread_{};
    bool glThreadStarted_{};
    std::atomic<bool> glStopping_{};
    std::exception_ptr glError_;
    std::atomic<int> vsyncRequest{-1};
    // Last completed GL frame, published for the statistics of later frames.
    std::mutex glStatsMutex;
    double lastSubmitMs{},lastPresentMs{},lastUploadMs{};
    std::uint64_t lastDrawn{};
    std::uint64_t journalFrames{},gatherJournalFrames{};
    bool residentVerticesAllocated{},residentIndicesAllocated{};
    SceUInt64 lastSlowPrepareLog{},lastSlowSubmitLog{};
    std::uint64_t skippedBatches{},drawnBatches{};

    explicit Storage(const EcstasyVulkanOptions& value):options(value)
    {
        // Vita budgets: 24 MB resident vertices, 6 MB resident indices.
        constexpr std::size_t geometryBudget=24u*1024u*1024u;
        residentGeometry.Configure(geometryBudget/VkResidentGeometryCache::SlotBytes);
        sceneryPackets.Configure(residentGeometry.Capacity());
        residentIndexCache.Configure(static_cast<std::uint32_t>((geometryBudget/4)/sizeof(std::uint32_t)));
        glDisable(GL_CULL_FACE);
        glDepthFunc(GL_LEQUAL);
        glClearColor(0.0f,0.0f,0.0f,1.0f);
        glClearDepthf(1.0f);
        platform::RecordSessionEvent("renderer.vitagl","initialized");
        freeSema_=sceKernelCreateSema("hp2_gl_free",0,static_cast<int>(packets.size()),static_cast<int>(packets.size()),nullptr);
        readySema_=sceKernelCreateSema("hp2_gl_ready",0,0,static_cast<int>(packets.size())+1,nullptr);
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes,512u*1024u);
        glThreadStarted_=freeSema_>=0 && readySema_>=0 && pthread_create(&glThread_,&attributes,
            [](void* self) -> void* {static_cast<Storage*>(self)->GlThread();return nullptr;},this)==0;
        pthread_attr_destroy(&attributes);
        platform::RecordSessionEvent("renderer.vitagl.gl_thread",glThreadStarted_ ? "started":"unavailable: inline submission");
    }
    ~Storage() {Close();}
    void GlThread();
    void Submit(GlFramePacket& packet);
    void RethrowGlError()
    {
        if(glError_) {auto error=glError_;glError_=nullptr;std::rethrow_exception(error);}
    }
    // Waits until every queued packet has been drawn (GL thread idle).
    void WaitIdle()
    {
        if(!glThreadStarted_)return;
        for(std::size_t i=0;i<packets.size();++i)sceKernelWaitSema(freeSema_,1,nullptr);
        sceKernelSignalSema(freeSema_,static_cast<int>(packets.size()));
        RethrowGlError();
    }
    void StopGlThread() noexcept
    {
        if(glThreadStarted_) {
            for(std::size_t i=0;i<packets.size();++i)sceKernelWaitSema(freeSema_,1,nullptr);
            glStopping_=true;sceKernelSignalSema(readySema_,1);
            pthread_join(glThread_,nullptr);glThreadStarted_=false;
        }
        if(freeSema_>=0)sceKernelDeleteSema(freeSema_);
        if(readySema_>=0)sceKernelDeleteSema(readySema_);
        freeSema_=readySema_=-1;
    }
    void Close() noexcept
    {
        StopGlThread();
        for(auto& packet:packets) {
            for(auto* texture:packet.deletes) {if(texture->name)glDeleteTextures(1,&texture->name);delete texture;}
            packet.Clear();
        }
        for(auto& target:glTargets) {
            if(target.framebuffer)glDeleteFramebuffers(1,&target.framebuffer);
            if(target.texture)glDeleteTextures(1,&target.texture);
            target={};
        }
        for(auto& texture:textures) {
            if(auto* handle=glTextureOf(texture)) {if(handle->name) {glDeleteTextures(1,&handle->name);glLiveTextureBytes-=handle->bytes;--glLiveTextureCount;}delete handle;}
            texture={};
        }
        textures.clear();textureIndices.clear();
        residentVertices.Destroy();residentIndices.Destroy();
        for(auto& buffer:streamVertices)buffer.Destroy();
        for(auto& buffer:streamIndices)buffer.Destroy();
        for(auto& entry:programs)if(entry.second.program)glDeleteProgram(entry.second.program);
        programs.clear();
    }
    static GlTexture* glTextureOf(const VkDrawTexture& texture) noexcept
    {return reinterpret_cast<GlTexture*>(texture.image.image);}
    // Preparing thread: the GL object is deleted after the in-flight frame.
    void Destroy(VkDrawTexture& texture)
    {
        if(auto* handle=glTextureOf(texture))building->deletes.push_back(handle);
        for(auto& variant:texture.variants)
            if(auto* handle=glTextureOf(variant))building->deletes.push_back(handle);
        texture={};
    }
    bool CanRenderPrepared() const noexcept {return false;}
    void InvalidatePrepared() noexcept {}

    std::uint32_t DepthPalette(const EcstasyDepthPalettePass& palette)
    {
        for(auto& cached:depthPaletteCache)
            if(cached.source==&palette && cached.gathered==gatherSerial)return cached.offset;
        auto* entry=&depthPaletteCache.front();
        bool matched=false;
        for(auto& cached:depthPaletteCache) {
            if(cached.valid && cached.colors==palette.palette) {entry=&cached;matched=true;break;}
            if(cached.gathered<entry->gathered)entry=&cached;
        }
        if(!matched) {
            entry->colors=palette.palette;entry->valid=true;
            for(std::size_t index=0;index<256;++index) {
                const auto paletteColor=entry->colors[index];
                entry->converted[index]={{static_cast<float>(paletteColor&255)/255.0f,
                    static_cast<float>((paletteColor>>8)&255)/255.0f,static_cast<float>((paletteColor>>16)&255)/255.0f,
                    static_cast<float>(paletteColor>>24)/128.0f}};
            }
        }
        if(entry->gathered!=gatherSerial || !matched) {
            Require(gatheredGlyphBounds.size()<=UINT32_MAX-256u,"Native palette table exceeds32-bit addressing");
            entry->offset=static_cast<std::uint32_t>(gatheredGlyphBounds.size());
            gatheredGlyphBounds.insert(gatheredGlyphBounds.end(),entry->converted.begin(),entry->converted.end());
        }
        entry->source=&palette;entry->gathered=gatherSerial;
        return entry->offset;
    }
    void BeginGather()
    {
        if(++gatherSerial==0) {
            for(auto& texture:textures)texture.validatedGather=0;
            for(auto& palette:depthPaletteCache)palette.gathered=0;
            ++gatherSerial;
        }
    }
    bool RetireUnusedTextures(const VkNativeFrameTextureReferences& frameTextures)
    {
        const auto& referenced=frameTextures.Get();
        const auto isReferenced=[&](const VkDrawTexture& texture) {
            return NativeTextureReferenced(referenced,{texture.source,texture.repeatedPadding});
        };
        bool changed=false;
        for(auto at=textures.begin();at!=textures.end();) {
            if(isReferenced(*at)) {
                at->lastReferencedFrame=completedFrameSerial;++at;
            } else if(!at->OwnersAlive()) {Destroy(*at);at=textures.erase(at);changed=true;}
            else ++at;
        }
        if(changed) {
            textureIndices.clear();textureIndices.reserve(textures.size());
            for(std::size_t index=0;index<textures.size();++index)
                textureIndices.emplace(VkNativeTextureKey{textures[index].source,textures[index].repeatedPadding},index);
        }
        return changed;
    }
    void EnsureTarget(std::int32_t id,std::uint32_t width,std::uint32_t height)
    {
        auto& target=renderedTargets[id];
        // Gather only checks that a sampled image exists; GL binds by name.
        target.descriptor=reinterpret_cast<VkDescriptorSet>(std::uintptr_t{1});
        target.previousDescriptor=target.descriptor;
        if(!id) {target.width=width;target.height=height;return;}
        width=std::max(1u,width);height=std::max(1u,height);
        building->targets[static_cast<std::size_t>(id)]={{width,height}};
        building->targetUsed[static_cast<std::size_t>(id)]=true;
        if(target.width==width && target.height==height)return;
        target.width=width;target.height=height;
        char detail[96];std::snprintf(detail,sizeof(detail),"id=%d extent=%ux%u",id,width,height);
        platform::RecordSessionEvent("renderer.vitagl.target",detail);
    }
    // GL thread: (re)create a target's texture and framebuffer.
    void EnsureGlTarget(std::size_t id,std::uint32_t width,std::uint32_t height)
    {
        auto& target=glTargets[id];
        if(target.texture && target.width==width && target.height==height)return;
        ++glTargetBuilds;
        target.width=width;target.height=height;
        GLuint texture=target.texture;
        if(!texture)glGenTextures(1,&texture);
        target.texture=texture;
        glBindTexture(GL_TEXTURE_2D,texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,static_cast<GLsizei>(width),static_cast<GLsizei>(height),0,
            GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        GLuint fbo=target.framebuffer;
        if(!fbo)glGenFramebuffers(1,&fbo);
        target.framebuffer=fbo;
        // vitaGL attaches a hidden depth buffer to a colour-only FBO on demand.
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
        glBindFramebuffer(GL_FRAMEBUFFER,0);
    }
    EcstasyNativeMirrorPresentation MirrorPresentation(const EcstasyFrame& frame) const
    {
        const bool needed=std::any_of(frame.Commands().begin(),frame.Commands().end(),
            [](const EcstasyDrawCommand& draw) {
                return (draw.target && draw.target->id==7) ||
                    (draw.sampledTarget && draw.sampledTarget->id==7);
            });
        if(!needed)return {};
        const auto sourceExtent=frame.SourceDisplayExtent();
        return ecstasyNativeMirrorPresentation(options.width,options.height,
            static_cast<std::uint32_t>(sourceExtent[0]),static_cast<std::uint32_t>(sourceExtent[1]),
            options.width,options.height,4096u);
    }
    void UploadTexture(const TextureInfo& texture,std::size_t index,bool repeatedPadding);
    void LogReupload(const VkDrawTexture& old,const TextureInfo& texture);
    std::uint64_t reuploads{};
    SceUInt64 lastReuploadLog{};
    void Gather(const EcstasyFrame& frame,
        std::vector<VkVertex>& output,std::vector<std::uint32_t>& elementIndices,
        std::vector<VkDrawBatch>& batches);
    GlProgram& Program(std::uint32_t key);
    bool Render(EcstasyFrame* frame);
    // Pipelined host: Complete() is called later on the application thread.
    bool deferRetire{};
};

// Diagnostic: why a cached texture failed validation and is decoded again.
void EcstasyVulkanDevice::Storage::LogReupload(const VkDrawTexture& old,const TextureInfo& texture)
{
    ++reuploads;
    const auto now=sceKernelGetSystemTimeWide();
    if(now-lastReuploadLog<2000000u)return;
    lastReuploadLog=now;
    const auto* wire=texture.descriptor ? texture.descriptor->authoredRecord:nullptr;
    const bool descriptor=wire && std::memcmp(wire,old.sourceDescriptor.data(),old.sourceDescriptor.size())==0;
    const bool pixels=texture.pixels.Data() && texture.pixels.size>=old.sourcePixels.size() &&
        std::memcmp(texture.pixels.Data(),old.sourcePixels.data(),old.sourcePixels.size())==0;
    const bool palette=old.sourcePalette.empty() || (texture.palette.Data() && texture.palette.size>=old.sourcePalette.size() &&
        std::memcmp(texture.palette.Data(),old.sourcePalette.data(),old.sourcePalette.size())==0);
    char detail[256];
    std::snprintf(detail,sizeof(detail),
        "total=%llu texture=%p size=%ux%u psm=%02x same_descriptor=%d same_pixels=%d same_palette=%d palette_bytes=%d/%d "
        "linear=%d/%d borrowed=%d owner_alive=%d upload_completed=%d",
        static_cast<unsigned long long>(reuploads),static_cast<const void*>(&texture),old.width,old.height,
        wire ? wire[0x4a]:0xffu,descriptor,pixels,palette,static_cast<int>(texture.paletteBytes),
        static_cast<int>(old.sourcePaletteBytes),texture.palette.nativeLinearPalette ? 1:0,old.sourcePaletteLinear ? 1:0,
        texture.pixels.borrowed ? 1:0,old.OwnersAlive() ? 1:0,old.uploadCompleted ? 1:0);
    platform::RecordSessionEvent("renderer.vitagl.texture_reupload",detail);
}

void EcstasyVulkanDevice::Storage::UploadTexture(const TextureInfo& texture,std::size_t index,bool repeatedPadding)
{
    const auto* wire=texture.descriptor ? texture.descriptor->authoredRecord:nullptr;
    Require(wire && texture.descriptor->authoredRecordSize>=164,"vitaGL mip upload lost its TextureInfo");
    const auto levels=static_cast<std::uint32_t>(wire[0x6e])+1u;
    Require(levels<=7 && wire[0x6f]<=1 && wire[0x70]<=5 && wire[0x71]<=3,
        "TextureInfo contains unsupported original mip/filter fields");
    const auto baseBytes=static_cast<std::size_t>(ReadU32(wire+0x44));
    Require(baseBytes,"Original texture downloader has an empty base level");
    {
        auto& current=textures[index];
        for(std::size_t at=0;at<current.variants.size();++at) {
            if(!current.variants[at].Matches(texture))continue;
            // Exact earlier image of this animation frame: swap it in.
            auto variants=std::move(current.variants);current.variants.clear();
            auto previous=std::move(current);
            current=std::move(variants[at]);
            variants[at]=std::move(previous);
            current.variants=std::move(variants);
            current.source=&texture;current.lastReferencedFrame=completedFrameSerial;
            Require(nextTextureGeneration!=UINT64_MAX,"Native texture generation exhausted");
            current.generation=++nextTextureGeneration;
            ++statistics.textureVariantHits;
            return;
        }
        // Stash the outgoing image only when the new pixels come from a
        // different immutable owner/offset (a swapped frame, not an in-place
        // rewrite or a borrowed movie plane, which can never match again).
        const bool swappedFrame=current.generation && current.uploadCompleted && current.OwnersAlive() &&
            !texture.pixels.borrowed && texture.pixels.owner &&
            (current.pixelOwner.lock()!=texture.pixels.owner || current.sourcePixelOffset!=texture.pixels.offset);
        if(swappedFrame && current.variants.size()<16) {
            auto variants=std::move(current.variants);current.variants.clear();
            variants.push_back(std::move(current));
            current=VkDrawTexture{};current.variants=std::move(variants);
        }
    }
    std::uint32_t width{},height{};std::size_t sourceBytes{};
    std::vector<std::uint8_t> base;
    // Only level 0 is uploaded and decoded. The retained source range (and
    // so the cache comparison) still spans every authored level.
    base=DecodeTextureMipRgba(texture,0,0,baseBytes,width,height,repeatedPadding);
    for(std::uint32_t level=0;level<levels;++level)sourceBytes+=baseBytes>>(2u*level);
    Require(sourceBytes<=texture.pixels.size,"vitaGL texture lost part of its authored mip chain");
    if(textures[index].generation)LogReupload(textures[index],texture);
    auto& target=textures[index];
    auto* handle=glTextureOf(target);
    if(!handle) {handle=new GlTexture;target.image.image=reinterpret_cast<VkImage>(handle);}
    target.width=width;target.height=height;target.levels=levels;
    const auto minification=wire[0x70];
    const bool linear=options.textureFilter!=EcstasyTextureFilter::Authored ||
        minification==1 || minification>=4 || wire[0x6f];
    ++statistics.textureUploads;statistics.textureUploadBytes+=base.size();
    building->uploads.push_back({handle,std::move(base),width,height,linear ? GL_LINEAR:GL_NEAREST,
        (wire[0x4c]&1u) ? GL_REPEAT:GL_CLAMP_TO_EDGE,(wire[0x4c]&2u) ? GL_REPEAT:GL_CLAMP_TO_EDGE});
    target.source=&texture;target.repeatedPadding=repeatedPadding;target.uploadCompleted=false;
    const auto psm=wire[0x4a];
    target.sourcePixels.assign(texture.pixels.Data(),texture.pixels.Data()+sourceBytes);
    target.sourcePaletteBytes=texture.paletteBytes;
    target.sourcePalette.clear();
    if(psm==0x13 || psm==0x14)
        target.sourcePalette.assign(texture.palette.Data(),texture.palette.Data()+static_cast<std::size_t>(texture.paletteBytes));
    std::copy_n(wire,target.sourceDescriptor.size(),target.sourceDescriptor.begin());
    target.scenery={};
    target.CaptureOwners(texture);
    target.lastReferencedFrame=completedFrameSerial;
    Require(nextTextureGeneration!=UINT64_MAX,"Native texture generation exhausted");
    target.generation=++nextTextureGeneration;
}

#include "src/ecstasy/ecstasy_gather.inc"

namespace {
// Compiled program cache (vitaGL caches only its own FFP shaders). Keyed by
// variant and a hash of both shader sources; any load failure recompiles.
constexpr std::uint32_t kGlProgramCacheVersion=1;
std::uint64_t GlShaderSourceHash()
{
    std::uint64_t hash=1469598103934665603ull^kGlProgramCacheVersion;
    for(const char* text:{kGlVertexShader,kGlFragmentShader})
        for(const char* c=text;*c;++c) {hash^=static_cast<unsigned char>(*c);hash*=1099511628211ull;}
    return hash;
}
std::string GlProgramCachePath(std::uint32_t key)
{
    static const std::uint64_t hash=GlShaderSourceHash();
    char path[160];
    std::snprintf(path,sizeof(path),"ux0:data/NFSHP2N/shader_cache/%03x-%016llx.bin",key,
        static_cast<unsigned long long>(hash));
    return path;
}
std::vector<std::uint8_t> GlReadProgramCache(std::uint32_t key)
{
    std::vector<std::uint8_t> data;
    if(auto* file=std::fopen(GlProgramCachePath(key).c_str(),"rb")) {
        std::fseek(file,0,SEEK_END);
        const long size=std::ftell(file);
        std::fseek(file,0,SEEK_SET);
        if(size>0 && size<(1<<20)) {
            data.resize(static_cast<std::size_t>(size));
            if(std::fread(data.data(),1,data.size(),file)!=data.size())data.clear();
        }
        std::fclose(file);
    }
    return data;
}
void GlWriteProgramCache(std::uint32_t key,GLuint program)
{
    // vitaGL's glGetProgramBinary takes no size query; its output is the
    // attribute table plus two serialized GXP shaders (a few KB).
    std::vector<std::uint8_t> buffer(512*1024);
    GLsizei length=0;GLenum format=0;
    glGetProgramBinary(program,static_cast<GLsizei>(buffer.size()),&length,&format,buffer.data());
    if(length<=0 || static_cast<std::size_t>(length)>buffer.size())return;
    sceIoMkdir("ux0:data/NFSHP2N/shader_cache",0777);
    const auto path=GlProgramCachePath(key);
    const auto temporary=path+".tmp";
    if(auto* file=std::fopen(temporary.c_str(),"wb")) {
        const bool written=std::fwrite(buffer.data(),1,static_cast<std::size_t>(length),file)==static_cast<std::size_t>(length);
        std::fclose(file);
        if(written) {sceIoRemove(path.c_str());sceIoRename(temporary.c_str(),path.c_str());}
        else sceIoRemove(temporary.c_str());
    }
}
}

int ecstasyAffineUV=1; // Vita affine_off.txt restores the projective divide everywhere
int ecstasyFragmentMediump=0; // Vita frag_mediump.txt (A/B: GPU fragment cost)
GlProgram& EcstasyVulkanDevice::Storage::Program(std::uint32_t key)
{
    auto& program=programs[key];
    if(program.program || program.failed)return program;
    const unsigned function=key&3u,texturing=(key>>2)&1u,alphaTest=(key>>3)&3u,showcase=(key>>5)&1u,
        fixedAlpha=(key>>6)&1u,clamp=(key>>7)&1u,gpuPosition=(key>>8)&1u,shade=(key>>9)&1u,affine=(key>>10)&1u,
        depthTexture=(key>>11)&1u;
    char defines[320];
    std::snprintf(defines,sizeof(defines),
        "#define FUNC %u\n#define TEXTURING %u\n#define ATEST %u\n#define SHOWCASE %u\n#define AFIX %u\n"
        "#define CLAMPTL %u\n#define GPUPOS %u\n#define SHADE %u\n#define AFFINE %u\n#define DEPTHTEX %u\n",
        function,texturing,alphaTest,showcase,fixedAlpha,clamp,gpuPosition,shade,affine,depthTexture);
    const auto compile=[&](GLenum kind,const char* body)->GLuint {
        const char* prelude=kind==GL_VERTEX_SHADER ? "":
            ecstasyFragmentMediump ? "precision mediump float;\n":"precision highp float;\n";
        const char* sources[3]={prelude,defines,body};
        const GLuint shader=glCreateShader(kind);
        glShaderSource(shader,3,sources,nullptr);
        glCompileShader(shader);
        GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
        if(!ok) {
            char log[1024]{};glGetShaderInfoLog(shader,sizeof(log),nullptr,log);
            char detail[1200];
            std::snprintf(detail,sizeof(detail),"key=%03x %s %s",key,kind==GL_VERTEX_SHADER ? "vertex":"fragment",log);
            platform::RecordSessionEvent("renderer.vitagl.shader_failed",detail);
            glDeleteShader(shader);return 0;
        }
        return shader;
    };
    const auto compileStarted=std::chrono::steady_clock::now();
    bool cached=false;
    if(const auto binary=GlReadProgramCache(key|(ecstasyFragmentMediump ? 0x80000000u:0u));!binary.empty()) {
        program.program=glCreateProgram();
        glProgramBinary(program.program,0,binary.data(),static_cast<GLsizei>(binary.size()));
        GLint linked=0;glGetProgramiv(program.program,GL_LINK_STATUS,&linked);
        if(linked)cached=true;
        else {glDeleteProgram(program.program);program.program=0;}
    }
    if(!cached) {
    const auto vertex=compile(GL_VERTEX_SHADER,kGlVertexShader);
    const auto fragment=compile(GL_FRAGMENT_SHADER,kGlFragmentShader);
    if(!vertex || !fragment) {
        if(vertex)glDeleteShader(vertex);
        if(fragment)glDeleteShader(fragment);
        program.failed=true;return program;
    }
    program.program=glCreateProgram();
    glAttachShader(program.program,vertex);glAttachShader(program.program,fragment);
    glBindAttribLocation(program.program,0,"a_pos");
    glBindAttribLocation(program.program,1,"a_uvq");
    glBindAttribLocation(program.program,2,"a_col");
    glBindAttribLocation(program.program,3,"a_nrm");
    glLinkProgram(program.program);
    GLint linked=0;glGetProgramiv(program.program,GL_LINK_STATUS,&linked);
    if(!linked) {
        char detail[64];std::snprintf(detail,sizeof(detail),"key=%03x",key);
        platform::RecordSessionEvent("renderer.vitagl.link_failed",detail);
        glDeleteProgram(program.program);program.program=0;program.failed=true;return program;
    }
    GlWriteProgramCache(key|(ecstasyFragmentMediump ? 0x80000000u:0u),program.program);
    }
    const auto location=[&](const char* name) {return glGetUniformLocation(program.program,name);};
    program.m0=location("u_m0");program.m1=location("u_m1");program.m2=location("u_m2");program.m3=location("u_m3");
    program.clipmap=location("u_clipmap");program.shade=location("u_shade");program.material=location("u_mat");
    program.flags=location("u_flags");program.flags2=location("u_flags2");
    program.fparam=location("u_fparam");program.yflip=location("u_yflip");program.texture=location("u_tex");
    program.depth=location("u_depth");program.depthmap=location("u_depthmap");
    glUseProgram(program.program);
    if(program.texture>=0)glUniform1i(program.texture,0);
    if(program.depth>=0)glUniform1i(program.depth,1);
    char detail[96];std::snprintf(detail,sizeof(detail),"key=%03x programs=%" HP2_ZU " cached=%d ms=%.1f",key,programs.size(),
        cached ? 1:0,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-compileStarted).count());
    platform::RecordSessionEvent("renderer.vitagl.program",detail);
    // Build every variant the game uses once, at boot (cached after the first
    // run), so the first race does not stall seconds on shader compiles.
    static bool warmed=false;
    if(!warmed) {
        warmed=true;
        for(const std::uint32_t known:{0x004u,0x314u,0x304u,0x00cu,0x014u})if(known!=key)Program(known);
        if(program.program)glUseProgram(program.program);
    }
    return program;
}

std::atomic<int> glRenderStage{0};
bool EcstasyVulkanDevice::Storage::Render(EcstasyFrame* frame)
{
    glRenderStage=1;
    if(!frame)return false;
    Require(frame->State()==EcstasyFrame::Phase::Ready,"vitaGL accepts only a sealed original frame");
    using Clock=std::chrono::steady_clock;
    const auto milliseconds=[](Clock::duration duration) {return std::chrono::duration<double,std::milli>(duration).count();};
    // Claim a free packet: at most one frame is gathered while one is drawn.
    const auto claimBegin=Clock::now();
    if(glThreadStarted_)sceKernelWaitSema(freeSema_,1,nullptr);
    const auto claimMs=milliseconds(Clock::now()-claimBegin);
    struct ReleaseOnThrow {
        Storage& storage;bool armed{true};
        ~ReleaseOnThrow() {if(armed && storage.glThreadStarted_)sceKernelSignalSema(storage.freeSema_,1);}
    } release{*this};
    {
        std::lock_guard<std::mutex> lock(glStatsMutex);
        if(glError_) {auto error=glError_;glError_=nullptr;std::rethrow_exception(error);}
    }
    building=&packets[nextPacket];nextPacket=(nextPacket+1)%packets.size();
    building->Clear();
    const auto renderBegin=Clock::now();
    statistics={};
    statistics.presentationTarget=true;
    statistics.availableCpuThreads=NativeSceneEffectiveHardwareConcurrency();
    ++nextSubmission;
    // vitaGL defers resource frees until the GPU is done with them; the CPU
    // side treats every Render as following the previous frame's completion.
    ++completedFrameSerial;
    BeginGather();
    VkNativeFrameTextureReferences frameTextures(frame->Commands(),referencedTextures);
    RetireUnusedTextures(frameTextures);
    preparedFrame.historyReaders=EcstasyFindNativeHistoryReaders(frame->Commands());
    const auto mirror=MirrorPresentation(*frame);
    const auto commandCount=frame->Commands().size();
    EnsureTarget(0,options.width,options.height);
    for(const auto& draw:frame->Commands()) {
        Require(draw.target,"vitaGL draw has no retained destination");
        const auto destination=draw.target->id>=7 ? draw.target->id:0;
        if(destination)EnsureTarget(destination,
            destination==7 ? mirror.rasterWidth:static_cast<std::uint32_t>(draw.target->width),
            destination==7 ? mirror.rasterHeight:static_cast<std::uint32_t>(draw.target->height));
        if(draw.sampledTarget && draw.sampledTarget->id>=7) {
            const auto source=draw.sampledTarget->id;
            EnsureTarget(source,
                source==7 ? mirror.rasterWidth:static_cast<std::uint32_t>(draw.sampledTarget->width),
                source==7 ? mirror.rasterHeight:static_cast<std::uint32_t>(draw.sampledTarget->height));
        }
    }
    auto& data=gatheredVertices;auto& elementIndices=gatheredIndices;auto& batches=gatheredBatches;
    data.clear();elementIndices.clear();batches.clear();gatheredGlyphBounds.clear();gatheredGlyphBounds.push_back({});
    const auto gatherBegin=Clock::now();
    glRenderStage=2;
    Gather(*frame,data,elementIndices,batches);
    glRenderStage=3;
    statistics.gatherMilliseconds=milliseconds(Clock::now()-gatherBegin);

    // Resident geometry/index slots: copy dirty entries into the packet; the
    // GL thread writes them in place before drawing it.
    residentPendingUploads.clear();residentIndexPendingUploads.clear();
    auto& packet=*building;
    if(!residentGeometry.Active().empty()) {
        packet.residentVertexCapacity=residentGeometry.Capacity()*VkResidentGeometryCache::SlotBytes;
        if(!residentVerticesAllocated) {residentVerticesAllocated=true;residentGeometry.InvalidateUploads();}
        for(const auto slot:residentGeometry.Active()) {
            const auto& entry=residentGeometry.Entry(slot);
            Require(entry.prepared,"Resident geometry upload has unfinished vertex preparation");
            if(entry.uploaded)continue;
            const auto offset=static_cast<std::size_t>(slot)*VkResidentGeometryCache::SlotBytes;
            const auto bytes=static_cast<std::size_t>(entry.count)*sizeof(VkVertex);
            const auto* source=reinterpret_cast<const std::uint8_t*>(entry.shadow.data());
            packet.residentVertexBytes.insert(packet.residentVertexBytes.end(),source,source+bytes);
            packet.residentVertexRanges.push_back({{offset,bytes}});
            residentPendingUploads.push_back(slot);
        }
    }
    if(!residentIndexCache.Active().empty()) {
        packet.residentIndexCapacity=static_cast<std::size_t>(residentIndexCache.Capacity())*sizeof(std::uint32_t);
        if(!residentIndicesAllocated) {residentIndicesAllocated=true;residentIndexCache.InvalidateUploads();}
        for(auto* entry:residentIndexCache.Active()) {
            if(entry->uploaded)continue;
            const auto offset=static_cast<std::size_t>(entry->first)*sizeof(std::uint32_t);
            const auto bytes=entry->shadow.size()*sizeof(std::uint32_t);
            const auto* source=reinterpret_cast<const std::uint8_t*>(entry->shadow.data());
            packet.residentIndexBytes.insert(packet.residentIndexBytes.end(),source,source+bytes);
            packet.residentIndexRanges.push_back({{offset,bytes}});
            residentIndexPendingUploads.push_back(entry);
        }
    }
    // Packets are drawn in order, so these contents are in place for every
    // later frame that resolves to the same slots.
    residentGeometry.PublishUploads(residentPendingUploads);
    residentIndexCache.PublishUploads(residentIndexPendingUploads);
    packet.textures.resize(textures.size());
    for(std::size_t index=0;index<textures.size();++index)
        packet.textures[index]={glTextureOf(textures[index]),textures[index].width,textures[index].height};
    packet.width=options.width;packet.height=options.height;
    std::swap(packet.vertices,data);std::swap(packet.indices,elementIndices);
    std::swap(packet.batches,batches);std::swap(packet.glyphBounds,gatheredGlyphBounds);
    packet.gatherMilliseconds=statistics.gatherMilliseconds;
    {
        static double lastCoverage=0.0;
        const double now=nativeParticleCoverage[0];
        packet.particleScreens=now>=lastCoverage ? now-lastCoverage:now;
        lastCoverage=now;
    }
    // The packet holds no reference to the frame: release it now.
    frame->MarkSubmitted();
    if(!deferRetire)frame->Complete();
    for(auto& texture:textures)texture.uploadCompleted=true;
    for(auto& entry:renderedTargets)entry.second.initialized=true;
    {
        std::lock_guard<std::mutex> lock(glStatsMutex);
        statistics.drawBatches=lastDrawn;
        statistics.recordSubmitMilliseconds=lastSubmitMs;
        statistics.presentMilliseconds=lastPresentMs;
    }
    statistics.presentAccepted=true;
    if((++gatherJournalFrames%300u)==0) {
        auto& steps=nativeGatherStepMilliseconds;
        if(steps[6]>0.0) {
            char detail[600];
            const double n=steps[6];
            std::snprintf(detail,sizeof(detail),
                "sampled=%.0f x8 lead_ms=%.2f batch_setup_ms=%.2f solid_preamble_ms=%.2f shading_ms=%.2f acquire_ms=%.2f solid_rest_ms=%.2f append_ms=%.2f other_draws_ms=%.2f segments=%.0f prepare_ms=%.2f shading_calls=%llu last_hits=%llu slot_hits=%llu lit_builds=%llu unlit_builds=%llu dedupe=%llu",
                n,8*steps[8]/n,8*steps[9]/n,8*steps[0]/n,8*steps[1]/n,8*steps[2]/n,8*steps[3]/n,8*steps[4]/n,8*steps[5]/n,8*steps[7]/n,milliseconds(Clock::now()-renderBegin),
                (unsigned long long)nativeShadingCounters[0],(unsigned long long)nativeShadingCounters[1],(unsigned long long)nativeShadingCounters[2],
                (unsigned long long)nativeShadingCounters[3],(unsigned long long)nativeShadingCounters[4],(unsigned long long)nativeShadingCounters[5]);
            nativeShadingCounters={};
            platform::RecordSessionEvent("renderer.vitagl.gather_steps",detail);
            std::snprintf(detail,sizeof(detail),"per_frame main_screens=%.2f other_screens=%.3f quads=%.0f",
                nativeParticleCoverage[0]/300,nativeParticleCoverage[1]/300,nativeParticleCoverage[2]/300);
            nativeParticleCoverage={};
            platform::RecordSessionEvent("renderer.vitagl.particle_coverage",detail);
            steps={};
            auto& breaks=nativeBatchBreaks;
            std::snprintf(detail,sizeof(detail),"per_frame solids texture=%.0f state=%.0f index_gap=%.0f transform=%.0f shading=%.0f other=%.0f scenery texture=%.0f state=%.0f index_gap=%.0f transform=%.0f shading=%.0f other=%.0f",
                breaks[0]/300,breaks[1]/300,breaks[2]/300,breaks[3]/300,breaks[4]/300,breaks[5]/300,
                breaks[6]/300,breaks[7]/300,breaks[8]/300,breaks[9]/300,breaks[10]/300,breaks[11]/300);
            platform::RecordSessionEvent("renderer.vitagl.batch_breaks",detail);
            breaks={};
            std::snprintf(detail,sizeof(detail),"per_frame variant_hits_total=%llu compares=%.1f bytes=%.0f solids plain=%.0f reflection=%.0f specular=%.0f shadow=%.0f miss=%.0f reprepare=%.0f",
                static_cast<unsigned long long>(statistics.textureVariantHits),
                nativeTextureCompares/300.0,nativeTextureCompareBytes/300.0,
                nativeSolidKinds[0]/300.0,nativeSolidKinds[1]/300.0,nativeSolidKinds[2]/300.0,
                nativeSolidKinds[3]/300.0,nativeSolidKinds[4]/300.0,nativeSolidKinds[5]/300.0);
            nativeSolidKinds={};
            platform::RecordSessionEvent("renderer.vitagl.texture_compare",detail);
            nativeTextureCompares=0;nativeTextureCompareBytes=0;
        }
    }
    {
        const auto prepareMs=milliseconds(Clock::now()-renderBegin);
        const auto now=sceKernelGetSystemTimeWide();
        if((prepareMs>40.0 || claimMs>15.0) && now-lastSlowPrepareLog>500000u) {
            lastSlowPrepareLog=now;
            char detail[320];
            std::snprintf(detail,sizeof(detail),
                "claim_wait_ms=%.2f prepare_ms=%.2f gather_ms=%.2f texture_uploads=%u texture_bytes=%llu resident_bytes=%" HP2_ZU " "
                "commands=%" HP2_ZU " batches=%" HP2_ZU " vertices=%" HP2_ZU " last_gl_submit_ms=%.2f last_gl_present_ms=%.2f",
                claimMs,prepareMs,statistics.gatherMilliseconds,static_cast<unsigned>(statistics.textureUploads),
                static_cast<unsigned long long>(statistics.textureUploadBytes),
                packet.residentVertexBytes.size()+packet.residentIndexBytes.size(),commandCount,
                packet.batches.size(),packet.vertices.size(),statistics.recordSubmitMilliseconds,statistics.presentMilliseconds);
            platform::RecordSessionEvent("renderer.vitagl.slow_prepare",detail);
        }
    }
    glRenderStage=0;
    release.armed=false;
    if(!glThreadStarted_) {Submit(packet);return true;}
    sceKernelSignalSema(readySema_,1);
    // Unpipelined callers expect the frame on screen when Render returns.
    if(!deferRetire)WaitIdle();
    return true;
}

void EcstasyVulkanDevice::Storage::GlThread()
{
    // Core 2: the application thread owns core 0, the gathering render thread core 1.
    sceKernelChangeThreadCpuAffinityMask(0,SCE_KERNEL_CPU_MASK_USER_2);
    std::size_t index=0;
    for(;;) {
        sceKernelWaitSema(readySema_,1,nullptr);
        if(glStopping_)return;
        auto& packet=packets[index];index=(index+1)%packets.size();
        try {Submit(packet);}
        catch(...) {
            std::lock_guard<std::mutex> lock(glStatsMutex);
            if(!glError_)glError_=std::current_exception();
        }
        sceKernelSignalSema(freeSema_,1);
    }
}

void EcstasyVulkanDevice::Storage::Submit(GlFramePacket& packet)
{
    using Clock=std::chrono::steady_clock;
    const auto milliseconds=[](Clock::duration duration) {return std::chrono::duration<double,std::milli>(duration).count();};
    const auto renderBegin=Clock::now();
    glRenderStage=5;
    for(auto& upload:packet.uploads) {
        auto* texture=upload.texture;
        if(!texture->name) {glGenTextures(1,&texture->name);++glLiveTextureCount;}
        glLiveTextureBytes-=texture->bytes;texture->bytes=upload.width*upload.height*4u;glLiveTextureBytes+=texture->bytes;
        glBindTexture(GL_TEXTURE_2D,texture->name);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,static_cast<GLsizei>(upload.width),static_cast<GLsizei>(upload.height),0,
            GL_RGBA,GL_UNSIGNED_BYTE,upload.rgba.data());
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,upload.filter);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,upload.filter);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,upload.wrapS);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,upload.wrapT);
    }
    packet.uploads.clear();
    for(std::size_t id=1;id<packet.targets.size();++id)
        if(packet.targetUsed[id])EnsureGlTarget(id,packet.targets[id][0],packet.targets[id][1]);
    if(packet.residentVertexCapacity) {
        residentVertices.Ensure(GL_ARRAY_BUFFER,packet.residentVertexCapacity);
        std::size_t at=0;
        for(const auto& range:packet.residentVertexRanges) {
            std::memcpy(residentVertices.mapped+range[0],packet.residentVertexBytes.data()+at,range[1]);at+=range[1];
        }
    }
    if(packet.residentIndexCapacity) {
        residentIndices.Ensure(GL_ELEMENT_ARRAY_BUFFER,packet.residentIndexCapacity);
        std::size_t at=0;
        for(const auto& range:packet.residentIndexRanges) {
            std::memcpy(residentIndices.mapped+range[0],packet.residentIndexBytes.data()+at,range[1]);at+=range[1];
        }
    }
    const auto residentUploadBytes=packet.residentVertexBytes.size()+packet.residentIndexBytes.size();
    const auto& data=packet.vertices;const auto& elementIndices=packet.indices;const auto& batches=packet.batches;
    const auto& gatheredGlyphBounds=packet.glyphBounds;
    // Per-frame streams rotate over a small ring so the GPU's previous frames
    // are never overwritten while still queued.
    streamSlot=(streamSlot+1)%StreamRing;
    auto& vertexStream=streamVertices[streamSlot];
    auto& indexStream=streamIndices[streamSlot];
    if(!data.empty()) {
        vertexStream.Ensure(GL_ARRAY_BUFFER,data.size()*sizeof(VkVertex));
        std::memcpy(vertexStream.mapped,data.data(),data.size()*sizeof(VkVertex));
    }
    if(!elementIndices.empty()) {
        indexStream.Ensure(GL_ELEMENT_ARRAY_BUFFER,elementIndices.size()*sizeof(std::uint32_t));
        std::memcpy(indexStream.mapped,elementIndices.data(),elementIndices.size()*sizeof(std::uint32_t));
    }
    const auto uploadDone=Clock::now();

    // Passes: each colour target owns its batches; a target sampled in this
    // frame renders first (Vulkan's dependency order). Offscreen targets keep
    // Vulkan's top-left row order, so they are drawn without the Y flip.
    constexpr std::size_t kTargets=16;
    std::array<std::vector<std::uint32_t>,kTargets> targetBatches;
    std::array<std::vector<std::int32_t>,kTargets> targetDependencies;
    const auto drawable=[&](const VkDrawBatch& batch) {
        if(batch.textureFunction<0 || batch.textureFunction>3 || batch.postProcess || batch.feedbackGroup || !batch.count)
            return false;
        if(batch.target==14 || batch.sampledTarget==14 || batch.sampledTarget==0)return false;
        if(batch.target<0 || batch.target>=static_cast<std::int32_t>(kTargets))return false;
        if(batch.sampledTarget>0 && (batch.sampledTarget>=static_cast<std::int32_t>(kTargets) ||
            !glTargets[static_cast<std::size_t>(batch.sampledTarget)].texture))return false;
        return true;
    };
    std::uint64_t drawn=0,skipped=0;
    for(std::size_t index=0;index<batches.size();++index) {
        const auto& batch=batches[index];
        if(!drawable(batch)) {++skipped;continue;}
        const auto target=static_cast<std::size_t>(batch.target);
        targetBatches[target].push_back(static_cast<std::uint32_t>(index));
        if(batch.sampledTarget>0) {
            auto& dependencies=targetDependencies[target];
            if(std::find(dependencies.begin(),dependencies.end(),batch.sampledTarget)==dependencies.end())
                dependencies.push_back(batch.sampledTarget);
        }
    }
    for(GLuint attribute=0;attribute<4;++attribute)glEnableVertexAttribArray(attribute);
    GLuint activeVertexBuffer=0,activeIndexBuffer=0,activeTexture=~0u;
    // Shading indices address this frame's table only.
    for(auto& entry:programs)entry.second.shading=UINT32_MAX;
    const auto halfEligible=[&](const VkDrawBatch& batch) {
        if(!batch.particle || batch.target!=0 || batch.sampledTarget>0 || batch.showcaseEnvironmentBlend)return false;
        const auto blend=glBlendFor(batch);
        return blend.enable && blend.source==GL_SRC_ALPHA && blend.equation==GL_FUNC_ADD &&
            (blend.destination==GL_ONE_MINUS_SRC_ALPHA || blend.destination==GL_ONE);
    };
    lastFrameHalf=false;
    const auto drawPass=[&](std::int32_t id) {
        glRenderStage=100+id;
        const bool offscreen=id!=0;
        bool halfFrame=false;
        if(!offscreen && ecstasyHalfResParticles && !halfCompositeFailed && packet.particleScreens>=ecstasyHalfResMinScreens)
            for(const auto index:targetBatches[0])if(halfEligible(batches[index])) {halfFrame=true;break;}
        // Screen-filling smoke (user-approved 2026-11-20): quarter resolution.
        halfDivisor=ecstasyQuarterResMinScreens>0.0 && packet.particleScreens>=ecstasyQuarterResMinScreens ? 4u:2u;
        if(halfFrame && halfDivisor==4u)++quarterFrames;
        if(!offscreen) {
            // The display scene must store its depth (sampled by the half pass)
            // and reload it after the composite for the draws that follow.
            const auto mode=halfFrame ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED:SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED;
            sceGxmDepthStencilSurfaceSetForceLoadMode(&gxm_depth_stencil_surface,mode);
            sceGxmDepthStencilSurfaceSetForceStoreMode(&gxm_depth_stencil_surface,halfFrame ?
                SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED:SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
        }
        bool halfMode=false;
        GLint height=static_cast<GLint>(packet.height);
        glDisable(GL_SCISSOR_TEST);
        if(offscreen) {
            const auto& target=glTargets[static_cast<std::size_t>(id)];
            height=static_cast<GLint>(target.height);
            glBindFramebuffer(GL_FRAMEBUFFER,target.framebuffer);
            glViewport(0,0,static_cast<GLsizei>(target.width),static_cast<GLsizei>(target.height));
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER,0);
            glViewport(0,0,static_cast<GLsizei>(packet.width),static_cast<GLsizei>(packet.height));
        }
        glClearColor(0.0f,0.0f,0.0f,1.0f);glClearDepthf(1.0f);
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        const float yflip=offscreen ? 1.0f:-1.0f;
        const GlProgram* activeProgram{};
        const VkDrawBatch* previous{};
        GlBlendState activeBlend;bool blendKnown=false;
        int activeDepth=-1,activeDepthWrite=-1;
        activeTexture=~0u;
        const auto drawOne=[&](std::uint32_t batchIndex) {
            const auto& batch=batches[batchIndex];
            const bool sampled=batch.sampledTarget>0;
            if(!batch.scissor.extent.width || !batch.scissor.extent.height) {++skipped;return;}
            const auto blend=glBlendFor(batch);
            const bool shade=batch.nativeGpuPosition && batch.nativeShadingIndex!=0;
            const bool texturing=!batch.untextured;
            std::uint32_t key=static_cast<std::uint32_t>(batch.textureFunction)&3u;
            key|=(texturing ? 1u:0u)<<2;
            // Half pass: "a==0 discard" is redundant (exact) when the colour
            // factor is SRC_ALPHA (a=0 adds nothing) and the alpha channel
            // keeps dest (ZERO/ONE or ONE/ONE_MINUS_SRC_ALPHA), with no
            // depth write and no fixed alpha replacing a after the test.
            // Fewer discards: cheaper fill for the heavy smoke pass.
            const bool redundantTest=halfMode && batch.alphaTest==1 && !batch.depthWrite && !blend.fixedAlpha &&
                ecstasyHalfResSkipAlphaTest;
            key|=(static_cast<std::uint32_t>(redundantTest ? 0:std::clamp(batch.alphaTest,0,2)))<<3;
            key|=(batch.showcaseEnvironmentBlend ? 1u:0u)<<5;
            key|=(blend.fixedAlpha ? 1u:0u)<<6;
            key|=(batch.clampTopLeft && texturing ? 1u:0u)<<7;
            key|=(batch.nativeGpuPosition ? 1u:0u)<<8;
            key|=(shade ? 1u:0u)<<9;
            key|=(batch.affineUV && texturing && ecstasyAffineUV ? 1u:0u)<<10;
            key|=(halfMode && !halfHardwareDepth ? 1u:0u)<<11;
            auto& program=Program(key);
            if(!program.program) {++skipped;return;}
            const float drawFlip=halfMode ? 1.0f:yflip;
            if(&program!=activeProgram) {
                glUseProgram(program.program);activeProgram=&program;
                if(program.yflip>=0 && (!program.flipValid || program.flip!=drawFlip)) {
                    glUniform1f(program.yflip,drawFlip);program.flip=drawFlip;program.flipValid=true;
                }
                if(halfMode && program.depthmap>=0 && (!program.depthmapValid || program.depthmapDivisor!=halfDivisor)) {
                    program.depthmapDivisor=halfDivisor;
                    const float w=static_cast<float>(packet.width/halfDivisor),h=static_cast<float>(packet.height/halfDivisor);
                    if(ecstasyHalfResDepthFlip)glUniform4f(program.depthmap,1.0f/w,-1.0f/h,0.0f,1.0f);
                    else glUniform4f(program.depthmap,1.0f/w,1.0f/h,0.0f,0.0f);
                    program.depthmapValid=true;
                }
            }
            if(halfMode) {++halfBatches;if(batch.depthWrite)++halfDepthWrites;}
            const auto vertexBuffer=batch.residentGeometry ? residentVertices.name:vertexStream.name;
            if(!vertexBuffer) {++skipped;return;}
            if(vertexBuffer!=activeVertexBuffer) {
                glBindBuffer(GL_ARRAY_BUFFER,vertexBuffer);
                constexpr GLsizei stride=sizeof(VkVertex);
                glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,stride,reinterpret_cast<const void*>(offsetof(VkVertex,position)));
                glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,stride,reinterpret_cast<const void*>(offsetof(VkVertex,uvq)));
                glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,stride,reinterpret_cast<const void*>(offsetof(VkVertex,color)));
                glVertexAttribPointer(3,3,GL_FLOAT,GL_FALSE,stride,reinterpret_cast<const void*>(offsetof(VkVertex,normal)));
                activeVertexBuffer=vertexBuffer;
            }
            if(halfMode && (!previous || std::memcmp(&previous->viewport,&batch.viewport,sizeof(batch.viewport)))) {
                const auto& viewport=batch.viewport;
                const float scale=1.0f/static_cast<float>(halfDivisor);
                glViewport(static_cast<GLint>(viewport.x*scale),static_cast<GLint>(viewport.y*scale),
                    static_cast<GLsizei>(viewport.width*scale),static_cast<GLsizei>(viewport.height*scale));
                glDepthRangef(viewport.minDepth,viewport.maxDepth);
            } else if(!previous || std::memcmp(&previous->viewport,&batch.viewport,sizeof(batch.viewport))) {
                const auto& viewport=batch.viewport;
                glViewport(static_cast<GLint>(viewport.x),
                    offscreen ? static_cast<GLint>(viewport.y):
                    height-static_cast<GLint>(viewport.y)-static_cast<GLint>(viewport.height),
                    static_cast<GLsizei>(viewport.width),static_cast<GLsizei>(viewport.height));
                glDepthRangef(viewport.minDepth,viewport.maxDepth);
            }
            if(halfMode && (!previous || std::memcmp(&previous->scissor,&batch.scissor,sizeof(batch.scissor)))) {
                const auto& scissor=batch.scissor;
                const GLint d=static_cast<GLint>(halfDivisor);
                const GLint x0=scissor.offset.x/d,y0=scissor.offset.y/d;
                const GLint x1=(scissor.offset.x+static_cast<GLint>(scissor.extent.width)+d-1)/d;
                const GLint y1=(scissor.offset.y+static_cast<GLint>(scissor.extent.height)+d-1)/d;
                glScissor(x0,y0,x1-x0,y1-y0);
            } else if(!previous || std::memcmp(&previous->scissor,&batch.scissor,sizeof(batch.scissor))) {
                const auto& scissor=batch.scissor;
                glScissor(scissor.offset.x,offscreen ? scissor.offset.y:
                    height-scissor.offset.y-static_cast<GLint>(scissor.extent.height),
                    static_cast<GLsizei>(scissor.extent.width),static_cast<GLsizei>(scissor.extent.height));
            }
            if(!blendKnown || !(blend==activeBlend)) {
                if(blend.enable) {
                    glEnable(GL_BLEND);
                    glBlendEquationSeparate(blend.equation,GL_FUNC_ADD);
                    // FIX replaces the written source alpha; keep destination alpha.
                    // Half pass: alpha accumulates coverage for a premultiplied
                    // "over" composite (additive layers add no coverage).
                    if(halfMode)glBlendFuncSeparate(blend.source,blend.destination,
                        blend.destination==GL_ONE ? GL_ZERO:GL_ONE,blend.destination==GL_ONE ? GL_ONE:GL_ONE_MINUS_SRC_ALPHA);
                    else glBlendFuncSeparate(blend.source,blend.destination,
                        blend.fixedAlpha ? GL_ZERO:GL_ONE,blend.fixedAlpha ? GL_ONE:GL_ZERO);
                } else glDisable(GL_BLEND);
                activeBlend=blend;blendKnown=true;
            }
            const int depth=batch.depth && (!halfMode || halfHardwareDepth) ? 1:0,depthWrite=batch.depthWrite && !halfMode ? 1:0;
            if(depth!=activeDepth) {
                if(depth)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
                activeDepth=depth;
            }
            if(depthWrite!=activeDepthWrite) {glDepthMask(depthWrite ? GL_TRUE:GL_FALSE);activeDepthWrite=depthWrite;}
            std::uint32_t texWidth{},texHeight{};
            if(texturing) {
                GLuint name{};
                if(sampled) {
                    const auto& source=glTargets[static_cast<std::size_t>(batch.sampledTarget)];
                    name=source.texture;texWidth=source.width;texHeight=source.height;
                } else {
                    Require(batch.texture<packet.textures.size(),"vitaGL draw lost its cached texture");
                    const auto& texture=packet.textures[batch.texture];
                    name=texture.texture ? texture.texture->name:0;texWidth=texture.width;texHeight=texture.height;
                }
                if(name!=activeTexture) {glBindTexture(GL_TEXTURE_2D,name);activeTexture=name;}
            }
            if(program.fparam>=0) {
                const float texelX=texWidth ? 1.0f/static_cast<float>(texWidth):0.0f;
                const float texelY=texHeight ? 1.0f/static_cast<float>(texHeight):0.0f;
                const std::array<float,4> parameters{{batch.blendConstant,texelX,texelY,0.0f}};
                if(!program.parametersValid || parameters!=program.parameters) {
                    glUniform4fv(program.fparam,1,parameters.data());
                    program.parameters=parameters;program.parametersValid=true;
                }
            }
            if(batch.nativeGpuPosition) {
                std::array<float,20> transform;
                std::copy_n(batch.nativeLocalToClip.begin(),16,transform.begin());
                std::copy_n(batch.nativeClipMapping.begin(),4,transform.begin()+16);
                if(!program.transformValid || std::memcmp(transform.data(),program.transform.data(),sizeof(transform))) {
                    const auto* m=transform.data();
                    glUniform4fv(program.m0,1,m);glUniform4fv(program.m1,1,m+4);
                    glUniform4fv(program.m2,1,m+8);glUniform4fv(program.m3,1,m+12);
                    glUniform4fv(program.clipmap,1,m+16);
                    program.transform=transform;program.transformValid=true;
                }
            }
            if(shade && batch.nativeShadingIndex!=program.shading) {
                const auto first=batch.nativeShadingIndex;
                Require(first+19u<=gatheredGlyphBounds.size(),"vitaGL shading record exceeds the frame table");
                const auto& record=gatheredGlyphBounds;
                std::uint32_t flags{},mode{},material{};
                std::memcpy(&flags,&record[first][0],4);std::memcpy(&mode,&record[first][1],4);
                std::memcpy(&material,&record[first][2],4);
                glUniform4fv(program.shade,19,record[first].data());
                if(material && material+4u<=gatheredGlyphBounds.size())
                    glUniform4fv(program.material,4,record[material].data());
                else {
                    static const float zero[16]{};
                    glUniform4fv(program.material,4,zero);
                }
                glUniform4f(program.flags,(flags&128u) ? 1.0f:0.0f,(flags&64u) ? 1.0f:0.0f,
                    (flags&4u) ? 1.0f:0.0f,(mode&1u) ? 1.0f:0.0f);
                glUniform4f(program.flags2,(mode&2u) ? 1.0f:0.0f,0.0f,0.0f,0.0f);
                program.shading=first;
            }
            const GLenum topology=batch.wireframe ? GL_LINES:GL_TRIANGLES;
            if(batch.indexed) {
                const auto indexBuffer=batch.residentIndices ? residentIndices.name:indexStream.name;
                if(!indexBuffer) {++skipped;previous=&batch;return;}
                if(indexBuffer!=activeIndexBuffer) {glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,indexBuffer);activeIndexBuffer=indexBuffer;}
                glDrawElements(topology,static_cast<GLsizei>(batch.count),GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<std::uintptr_t>(batch.first)*sizeof(std::uint32_t)));
            } else {
                // vitaGL's linear index table is 16-bit: split very long arrays.
                constexpr std::uint32_t chunk=60000u;
                for(std::uint32_t done=0;done<batch.count;done+=chunk)
                    glDrawArrays(topology,static_cast<GLint>(batch.first+done),
                        static_cast<GLsizei>(std::min(chunk,batch.count-done)));
            }
            ++drawn;previous=&batch;
        };
        const auto resetState=[&] {
            activeProgram=nullptr;previous=nullptr;blendKnown=false;activeDepth=activeDepthWrite=-1;
            activeTexture=~0u;activeVertexBuffer=0;
        };
        const auto& list=targetBatches[static_cast<std::size_t>(id)];
        for(std::size_t at=0;at<list.size();) {
            if(halfFrame && halfEligible(batches[list[at]])) {
                halfFrame=false; // later runs this frame draw at full resolution
                std::size_t end=at;
                while(end<list.size() && halfEligible(batches[list[end]]))++end;
                if(!lastFrameHalf) {lastFrameHalf=true;++halfFramesTotal;}
                BeginHalfParticles(packet.width,packet.height);
                halfMode=true;resetState();
                for(;at<end;++at)drawOne(list[at]);
                halfMode=false;
                EndHalfParticles(packet.width,packet.height);
                resetState();
                continue;
            }
            drawOne(list[at++]);
        }
        glDisable(GL_SCISSOR_TEST);
    };
    std::array<std::uint8_t,kTargets> visited{};
    const auto visit=[&](const auto& self,std::int32_t id)->void {
        auto& state=visited[static_cast<std::size_t>(id)];
        if(state)return;
        state=1;
        for(const auto dependency:targetDependencies[static_cast<std::size_t>(id)])self(self,dependency);
        if(id==0 || !targetBatches[static_cast<std::size_t>(id)].empty())drawPass(id);
        state=2;
    };
    visit(visit,0);
    // Offscreen targets nobody samples this frame (env-map faces) are not drawn.
    for(std::size_t id=1;id<kTargets;++id)if(!visited[id])skipped+=targetBatches[id].size();
    glDisable(GL_SCISSOR_TEST);
    drawnBatches+=drawn;skippedBatches+=skipped;
    // Textures retired by this frame's gather: no later packet refers to them.
    for(auto* texture:packet.deletes) {
        if(texture->name) {glDeleteTextures(1,&texture->name);glLiveTextureBytes-=texture->bytes;--glLiveTextureCount;}
        delete texture;
    }
    packet.deletes.clear();
    if(nextScreenshot<ecstasyScreenshotFrames.size() && journalFrames+1>=ecstasyScreenshotFrames[nextScreenshot]) {
        SaveScreenshot(packet.width,packet.height,journalFrames+1);++nextScreenshot;
    }
    const auto submitDone=Clock::now();
    const auto vsync=vsyncRequest.exchange(-1);
    if(vsync>=0)vglWaitVblankStart(vsync ? GL_TRUE:GL_FALSE);
    glRenderStage=4;
    vglSwapBuffers(GL_FALSE);
    // dshots.txt "N": after N frames that used the half pass, copy the next
    // 8 displayed frames (each written as soon as it is taken).
    if(!ecstasyDisplayShotFrames.empty() && nextDisplayShot<8 && halfFramesTotal>=ecstasyDisplayShotFrames[0] &&
        (displayShotPhase++%8u)==0) {
        // Diagnostic: copy what is on screen now (no GPU sync), half size.
        SceDisplayFrameBuf frameBuffer{};frameBuffer.size=sizeof(frameBuffer);
        if(sceDisplayGetFrameBuf(&frameBuffer,SCE_DISPLAY_SETBUF_NEXTFRAME)>=0 && frameBuffer.base) {
            const auto* source=static_cast<const std::uint32_t*>(frameBuffer.base);
            const std::uint32_t w=frameBuffer.width/2,h=frameBuffer.height/2;
            std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w)*h);
            for(std::uint32_t y=0;y<h;++y) // stored bottom-up like glReadPixels
                for(std::uint32_t x=0;x<w;++x)pixels[(h-1-y)*w+x]=source[(y*2)*frameBuffer.pitch+x*2];
            char path[96];std::snprintf(path,sizeof(path),"ux0:data/NFSHP2N/dshot_%llu_h%d_%ux%u.rgba",
                static_cast<unsigned long long>(journalFrames+1),lastFrameHalf ? 1:0,w,h);
            if(FILE* file=std::fopen(path,"wb")) {std::fwrite(pixels.data(),4,pixels.size(),file);std::fclose(file);}
        }
        ++nextDisplayShot;
    }
    if(ecstasyHalfResParticles && !displayTargetScenes) {
        // vitaGL builds the display render target for one scene per frame;
        // the half pass splits the display frame, and a second BeginScene on
        // it would wait for the GPU. Rebuild it once, between frames (the
        // swap ended the scene; the next draw begins a new one), GPU idle.
        displayTargetScenes=1;
        sceGxmFinish(gxm_context);
        SceGxmRenderTargetParams params{};
        params.width=packet.width;params.height=packet.height;params.scenesPerFrame=2;
        params.multisampleMode=SCE_GXM_MULTISAMPLE_NONE;params.driverMemBlock=-1;
        SceGxmRenderTarget* target=nullptr;
        const int created=sceGxmCreateRenderTarget(&params,&target);
        if(created>=0) {sceGxmDestroyRenderTarget(gxm_render_target);gxm_render_target=target;displayTargetScenes=2;}
        char detail[64];std::snprintf(detail,sizeof(detail),"scenes_per_frame=%d result=%d",displayTargetScenes,created);
        platform::RecordSessionEvent("renderer.vitagl.display_target",detail);
    }
    glRenderStage=0;
    const auto presentMs=milliseconds(Clock::now()-submitDone);
    const auto totalMs=milliseconds(Clock::now()-renderBegin);
    if(totalMs>25.0) {
        const auto now=sceKernelGetSystemTimeWide();
        if(now-lastSlowSubmitLog>500000u) {
            lastSlowSubmitLog=now;
            char detail[224];
            std::snprintf(detail,sizeof(detail),"total_ms=%.2f upload_ms=%.2f draw_ms=%.2f swap_ms=%.2f drawn=%llu",
                totalMs,milliseconds(uploadDone-renderBegin),milliseconds(submitDone-uploadDone),presentMs,
                static_cast<unsigned long long>(drawn));
            platform::RecordSessionEvent("renderer.vitagl.slow_submit",detail);
            if(milliseconds(uploadDone-renderBegin)>500.0) {
                std::snprintf(detail,sizeof(detail),"free_kb vram=%u ram=%u phycont=%u resident_kb v=%u i=%u stream_kb v=%u i=%u",
                    static_cast<unsigned>(vglMemFree(VGL_MEM_VRAM)/1024),static_cast<unsigned>(vglMemFree(VGL_MEM_RAM)/1024),
                    static_cast<unsigned>(vglMemFree(VGL_MEM_PHYCONT)/1024),
                    static_cast<unsigned>(residentVertices.size/1024),static_cast<unsigned>(residentIndices.size/1024),
                    static_cast<unsigned>(streamVertices[streamSlot].size/1024),static_cast<unsigned>(streamIndices[streamSlot].size/1024));
                platform::RecordSessionEvent("renderer.vitagl.upload_stall",detail);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(glStatsMutex);
        lastDrawn=drawn;lastSubmitMs=milliseconds(submitDone-uploadDone);lastPresentMs=presentMs;
        lastUploadMs=milliseconds(uploadDone-renderBegin);
    }
    if((++journalFrames%300u)==0) {
        char detail[320];
        std::snprintf(detail,sizeof(detail),
            "frames=%llu batches=%" HP2_ZU " drawn=%llu skipped=%llu vertices=%" HP2_ZU " indices=%" HP2_ZU " resident_upload=%" HP2_ZU " "
            "textures=%" HP2_ZU " programs=%" HP2_ZU " gather_ms=%.2f upload_ms=%.2f draw_ms=%.2f swap_ms=%.2f",
            static_cast<unsigned long long>(journalFrames),batches.size(),static_cast<unsigned long long>(drawn),
            static_cast<unsigned long long>(skipped),data.size(),elementIndices.size(),residentUploadBytes,
            packet.textures.size(),programs.size(),packet.gatherMilliseconds,
            milliseconds(uploadDone-renderBegin),
            milliseconds(submitDone-uploadDone),presentMs);
        platform::RecordSessionEvent("renderer.vitagl.frame",detail);
        std::snprintf(detail,sizeof(detail),"per_frame runs=%.2f batches=%.1f depth_writes=%.1f enabled=%d",
            halfRuns/300.0,halfBatches/300.0,halfDepthWrites/300.0,ecstasyHalfResParticles && !halfCompositeFailed ? 1:0);
        {
            const auto length=std::strlen(detail);
            std::snprintf(detail+length,sizeof(detail)-length," quarter=%.2f",quarterFrames/300.0);
            quarterFrames=0;
        }
        platform::RecordSessionEvent("renderer.vitagl.halfres",detail);
        halfRuns=halfBatches=halfDepthWrites=0;
        {
            const auto info=mallinfo();
            std::size_t streamKb=0;
            for(std::size_t at=0;at<StreamRing;++at)streamKb+=(streamVertices[at].size+streamIndices[at].size)/1024;
            std::snprintf(detail,sizeof(detail),"textures=%llu texture_kb=%llu heap_arena_kb=%u heap_used_kb=%u heap_free_kb=%u "
                "resident_kb v=%u i=%u stream_kb=%u target_builds=%llu buffer_builds=%llu",
                (unsigned long long)glLiveTextureCount.load(),(unsigned long long)(glLiveTextureBytes.load()/1024),
                static_cast<unsigned>(info.arena/1024),static_cast<unsigned>(info.uordblks/1024),static_cast<unsigned>(info.fordblks/1024),
                static_cast<unsigned>(residentVertices.size/1024),static_cast<unsigned>(residentIndices.size/1024),
                static_cast<unsigned>(streamKb),(unsigned long long)glTargetBuilds.load(),(unsigned long long)glBufferBuilds.load());
            platform::RecordSessionEvent("renderer.vitagl.memory_use",detail);
        }
        std::snprintf(detail,sizeof(detail),"free_kb vram=%u ram=%u phycont=%u budget=%u total_kb vram=%u ram=%u phycont=%u",
            static_cast<unsigned>(vglMemFree(VGL_MEM_VRAM)/1024),static_cast<unsigned>(vglMemFree(VGL_MEM_RAM)/1024),
            static_cast<unsigned>(vglMemFree(VGL_MEM_PHYCONT)/1024),static_cast<unsigned>(vglMemFree(VGL_MEM_BUDGET)/1024),
            static_cast<unsigned>(vglMemTotal(VGL_MEM_VRAM)/1024),static_cast<unsigned>(vglMemTotal(VGL_MEM_RAM)/1024),
            static_cast<unsigned>(vglMemTotal(VGL_MEM_PHYCONT)/1024));
        platform::RecordSessionEvent("renderer.vitagl.memory",detail);
    }
}

void EcstasyVulkanDevice::Storage::BeginHalfParticles(std::uint32_t width,std::uint32_t height)
{
    // Diagnostic (Vita halfres_sync.txt): end the display scene and wait for
    // the GPU so its stored depth is complete before the half pass samples it.
    if(ecstasyHalfResSync)glFinish();
    // The display depth (stored at the end of this scene) as a texture on unit 1.
    if(!halfDepthTexture)glGenTextures(1,&halfDepthTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D,halfDepthTexture);
    vglTexImageDepthBuffer(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glActiveTexture(GL_TEXTURE0);
    const auto halfWidth=width/halfDivisor,halfHeight=height/halfDivisor;
    halfTargetCurrent=halfDivisor>2 ? 1:0;
    auto& target=halfTargets[halfTargetCurrent];
    if(!target.texture || target.width!=halfWidth || target.height!=halfHeight) {
        target.width=halfWidth;target.height=halfHeight;
        if(!target.texture)glGenTextures(1,&target.texture);
        glBindTexture(GL_TEXTURE_2D,target.texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,static_cast<GLsizei>(halfWidth),static_cast<GLsizei>(halfHeight),0,
            GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        if(!target.framebuffer)glGenFramebuffers(1,&target.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER,target.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target.texture,0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER,target.framebuffer);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0,0,static_cast<GLsizei>(halfWidth),static_cast<GLsizei>(halfHeight));
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glClearColor(0.0f,0.0f,0.0f,0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0.0f,0.0f,0.0f,1.0f);
    halfHardwareDepth=false;
    if(ecstasyHalfResHardwareDepth && !halfDepthCopyFailed) {
        if(!halfDepthCopyProgram) {
            static const char* vertexSource=
                "attribute vec2 a_pos;\n"
                "void main(){gl_Position=vec4(a_pos,0.0,1.0);}\n";
            static const char* fragmentSource=
                "precision highp float;\nuniform sampler2D u_depth;\nuniform vec4 u_depthmap;\n"
                "void main(){gl_FragDepth=texture2D(u_depth,gl_FragCoord.xy*u_depthmap.xy+u_depthmap.zw).r;"
                "gl_FragColor=vec4(0.0);}\n";
            const auto compile=[&](GLenum kind,const char* source)->GLuint {
                const GLuint shader=glCreateShader(kind);
                glShaderSource(shader,1,&source,nullptr);glCompileShader(shader);
                GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
                if(!ok) {
                    char log[512]{};glGetShaderInfoLog(shader,sizeof(log),nullptr,log);
                    platform::RecordSessionEvent("renderer.vitagl.halfres_depth_shader_failed",log);
                    glDeleteShader(shader);return 0;
                }
                return shader;
            };
            const GLuint vertex=compile(GL_VERTEX_SHADER,vertexSource),fragment=compile(GL_FRAGMENT_SHADER,fragmentSource);
            if(vertex && fragment) {
                halfDepthCopyProgram=glCreateProgram();
                glAttachShader(halfDepthCopyProgram,vertex);glAttachShader(halfDepthCopyProgram,fragment);
                glBindAttribLocation(halfDepthCopyProgram,0,"a_pos");
                glLinkProgram(halfDepthCopyProgram);
                GLint linked=0;glGetProgramiv(halfDepthCopyProgram,GL_LINK_STATUS,&linked);
                if(!linked) {glDeleteProgram(halfDepthCopyProgram);halfDepthCopyProgram=0;}
            }
            if(!halfDepthCopyProgram) {
                halfDepthCopyFailed=true;
                platform::RecordSessionEvent("renderer.vitagl.halfres","depth copy program failed; discard depth test");
            } else {
                glUseProgram(halfDepthCopyProgram);
                glUniform1i(glGetUniformLocation(halfDepthCopyProgram,"u_depth"),1);
                halfDepthCopyMap=glGetUniformLocation(halfDepthCopyProgram,"u_depthmap");
            }
        }
        if(halfDepthCopyProgram && halfQuad) {
            glUseProgram(halfDepthCopyProgram);
            const float w=static_cast<float>(halfWidth),h=static_cast<float>(halfHeight);
            if(halfDepthCopyMap>=0) {
                if(ecstasyHalfResDepthFlip)glUniform4f(halfDepthCopyMap,1.0f/w,-1.0f/h,0.0f,1.0f);
                else glUniform4f(halfDepthCopyMap,1.0f/w,1.0f/h,0.0f,0.0f);
            }
            glDisable(GL_BLEND);
            glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
            glEnable(GL_DEPTH_TEST);glDepthFunc(GL_ALWAYS);glDepthMask(GL_TRUE);
            glBindBuffer(GL_ARRAY_BUFFER,halfQuad);
            for(GLuint attribute=1;attribute<4;++attribute)glDisableVertexAttribArray(attribute);
            glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
            glDrawArrays(GL_TRIANGLE_STRIP,0,4);
            for(GLuint attribute=1;attribute<4;++attribute)glEnableVertexAttribArray(attribute);
            glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
            glDepthFunc(GL_LEQUAL);glDepthMask(GL_FALSE);
            halfHardwareDepth=true;
        }
    }
    glEnable(GL_SCISSOR_TEST);
    ++halfRuns;
}

void EcstasyVulkanDevice::Storage::EndHalfParticles(std::uint32_t width,std::uint32_t height)
{
    if(!halfCompositeProgram && !halfCompositeFailed) {
        static const char* vertexSource=
            "attribute vec2 a_pos;\n"
            "void main(){gl_Position=vec4(a_pos,0.0,1.0);}\n";
        // Sample by window position, like the half pass's depth lookup: each
        // display pixel reads the half pixel drawn at the same memory row,
        // independent of vitaGL's per-scene viewport orientation.
        static const char* fragmentSource=
            "precision highp float;\nuniform sampler2D u_tex;\nuniform vec4 u_map;\n"
            "void main(){gl_FragColor=texture2D(u_tex,gl_FragCoord.xy*u_map.xy+u_map.zw);}\n";
        const auto compile=[&](GLenum kind,const char* source)->GLuint {
            const GLuint shader=glCreateShader(kind);
            glShaderSource(shader,1,&source,nullptr);glCompileShader(shader);
            GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
            if(!ok) {
                char log[512]{};glGetShaderInfoLog(shader,sizeof(log),nullptr,log);
                platform::RecordSessionEvent("renderer.vitagl.halfres_shader_failed",log);
                glDeleteShader(shader);return 0;
            }
            return shader;
        };
        const GLuint vertex=compile(GL_VERTEX_SHADER,vertexSource),fragment=compile(GL_FRAGMENT_SHADER,fragmentSource);
        if(vertex && fragment) {
            halfCompositeProgram=glCreateProgram();
            glAttachShader(halfCompositeProgram,vertex);glAttachShader(halfCompositeProgram,fragment);
            glBindAttribLocation(halfCompositeProgram,0,"a_pos");
            glLinkProgram(halfCompositeProgram);
            GLint linked=0;glGetProgramiv(halfCompositeProgram,GL_LINK_STATUS,&linked);
            if(!linked) {glDeleteProgram(halfCompositeProgram);halfCompositeProgram=0;}
        }
        if(!halfCompositeProgram) {
            halfCompositeFailed=true;
            platform::RecordSessionEvent("renderer.vitagl.halfres","composite program failed; full resolution");
        } else {
            glUseProgram(halfCompositeProgram);
            glUniform1i(glGetUniformLocation(halfCompositeProgram,"u_tex"),0);
            halfCompositeFlip=glGetUniformLocation(halfCompositeProgram,"u_map");
            static const float quad[8]={-1.0f,-1.0f,1.0f,-1.0f,-1.0f,1.0f,1.0f,1.0f};
            glGenBuffers(1,&halfQuad);glBindBuffer(GL_ARRAY_BUFFER,halfQuad);
            glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0,0,static_cast<GLsizei>(width),static_cast<GLsizei>(height));
    if(!halfCompositeProgram) {glEnable(GL_SCISSOR_TEST);return;}
    glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD,GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE,GL_ONE_MINUS_SRC_ALPHA,GL_ZERO,GL_ONE);
    glUseProgram(halfCompositeProgram);
    if(halfCompositeFlip>=0) {
        const float w=1.0f/static_cast<float>(width),h=1.0f/static_cast<float>(height);
        if(ecstasyHalfResCompositeFlip)glUniform4f(halfCompositeFlip,w,-h,0.0f,1.0f);
        else glUniform4f(halfCompositeFlip,w,h,0.0f,0.0f);
    }
    glBindTexture(GL_TEXTURE_2D,halfTargets[halfTargetCurrent].texture);
    glBindBuffer(GL_ARRAY_BUFFER,halfQuad);
    for(GLuint attribute=1;attribute<4;++attribute)glDisableVertexAttribArray(attribute);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,2*sizeof(float),nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    for(GLuint attribute=1;attribute<4;++attribute)glEnableVertexAttribArray(attribute);
    glEnable(GL_SCISSOR_TEST);
}

void EcstasyVulkanDevice::Storage::SaveScreenshot(std::uint32_t width,std::uint32_t height,std::uint64_t frame)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width)*height*4u);
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    glReadPixels(0,0,static_cast<GLsizei>(width),static_cast<GLsizei>(height),GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    char path[96];std::snprintf(path,sizeof(path),"ux0:data/NFSHP2N/shot_%llu_%ux%u.rgba",
        static_cast<unsigned long long>(frame),width,height);
    if(FILE* file=std::fopen(path,"wb")) {std::fwrite(pixels.data(),1,pixels.size(),file);std::fclose(file);}
    platform::RecordSessionEvent("renderer.vitagl.screenshot",path);
}

EcstasyVulkanDevice::EcstasyVulkanDevice(const EcstasyVulkanOptions& options):storage_(std::make_unique<Storage>(options)){}
EcstasyVulkanDevice::~EcstasyVulkanDevice()=default;
void EcstasyVulkanDevice::SetDeferredRetire(bool defer) noexcept {storage_->deferRetire=defer;}
int EcstasyVulkanDevice::RenderStage() noexcept {return glRenderStage.load();}
void EcstasyVulkanDevice::Render(EcstasyFrame& frame)
{
    try {(void)storage_->Render(&frame);}
    catch(const std::exception& error) {
        platform::RecordSessionEvent("renderer.frame.failed",error.what());throw;
    }
}
bool EcstasyVulkanDevice::CanRenderPrepared() const noexcept {return false;}
bool EcstasyVulkanDevice::RenderPrepared() {return false;}
void EcstasyVulkanDevice::InvalidatePrepared() noexcept {}
void EcstasyVulkanDevice::Resize(std::uint32_t width,std::uint32_t height)
{
    Require(width && height,"Cannot resize a vitaGL target to zero");
    storage_->options.width=width;storage_->options.height=height;
}
void EcstasyVulkanDevice::ConfigurePresentation(bool vsync)
{
    storage_->options.vsync=vsync;
    // Applied by the GL thread before its next swap.
    if(storage_->glThreadStarted_)storage_->vsyncRequest=vsync ? 1:0;
    else vglWaitVblankStart(vsync ? GL_TRUE:GL_FALSE);
}
void EcstasyVulkanDevice::SetLogicalSize(std::uint32_t width,std::uint32_t height)
{
    Require(width && height,"Logical vitaGL viewport dimensions are zero");
    storage_->options.logicalWidth=width;storage_->options.logicalHeight=height;
}
std::uint32_t EcstasyVulkanDevice::Width() const noexcept{return storage_->options.width;}
std::uint32_t EcstasyVulkanDevice::Height() const noexcept{return storage_->options.height;}
const EcstasyVulkanFrameStatistics& EcstasyVulkanDevice::LastFrameStatistics() const noexcept{return storage_->statistics;}
EcstasyVulkanColorReadback EcstasyVulkanDevice::ReadColorTarget(std::int32_t)
{
    return {};
}
}
