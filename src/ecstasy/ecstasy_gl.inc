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
    // usage GL_STATIC_DRAW: vitaGL places it in CDRAM first (GPU-fast; CPU
    // writes only, never read back), GL_DYNAMIC_DRAW: main RAM first.
    void Ensure(GLenum bufferTarget,std::size_t required,GLenum usage=GL_DYNAMIC_DRAW) {
        if(name && size>=required)return;
        // Grow by half again: each regrowth leaves the old block allocated
        // until vitaGL's deferred free, so small steps churn GPU memory.
        // (Capacity is read before Destroy, which zeroes it.)
        const auto previous=size;
        const auto grown=previous ? previous+previous/2:std::size_t(0);
        Destroy();++glBufferBuilds;
        target=bufferTarget;
        size=std::max<std::size_t>((std::max(required,grown)+65535u)&~std::size_t(65535u),65536u);
        glGenBuffers(1,&name);
        glBindBuffer(target,name);
        glBufferData(target,static_cast<GLsizeiptr>(size),nullptr,usage);
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
    // Resident geometry of each span (entry + its index token at gather
    // time). Owned by the packet: the next Gather refills the storage copy
    // while the GL thread reads this one (instancing groups by it).
    std::vector<const VkResidentGeometryEntry*> residentSpans;
    std::vector<std::uint64_t> residentSpanTokens;
    std::vector<GlPendingTexture> uploads;
    std::vector<GlTexture*> deletes;
    struct TextureRef {GlTexture* texture{};std::uint32_t width{},height{};std::int32_t minAlpha{-1};};
    // Deferred resident index resolve (ecstasyDeferResolve): the GL thread
    // resolves this packet's resident batches against its own index cache.
    std::uint64_t indexGather{};
    bool deferredResolve{};
    std::vector<TextureRef> textures;
    std::array<std::array<std::uint32_t,2>,16> targets{};
    std::array<bool,16> targetUsed{};
    std::vector<std::uint8_t> residentVertexBytes,residentIndexBytes;
    std::vector<std::array<std::size_t,2>> residentVertexRanges,residentIndexRanges;
    std::size_t residentVertexCapacity{},residentIndexCapacity{};
    std::uint32_t width{},height{};
    double gatherMilliseconds{};
    // gpu_timeline: gather-thread stamps (Render entry, packet claimed, ready).
    std::uint64_t timelineEntry{},timelineClaimed{},timelineReady{};
    std::array<std::uint32_t,4> timelineResident{};
    double particleScreens{}; // main-target particle coverage this frame
    std::vector<EcstasyDepthTransfer> depthTransfers;
    void Clear() {
        depthTransfers.clear();
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
#if INST
// Per-instance model transform (one per-frame table row per draw instance).
attribute vec4 a_i0;
attribute vec4 a_i1;
attribute vec4 a_i2;
attribute vec4 a_i3;
attribute vec4 a_i4;
#define u_m0 a_i0
#define u_m1 a_i1
#define u_m2 a_i2
#define u_m3 a_i3
#define u_clipmap a_i4
#else
uniform vec4 u_m0;
uniform vec4 u_m1;
uniform vec4 u_m2;
uniform vec4 u_m3;
uniform vec4 u_clipmap;
#endif
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
#if SHADE && TRIVIALVS
    v_uvq=a_uvq;v_col=a_col+u_shade[1]*0.0+u_mat[0]*0.0+u_flags*0.0+u_flags2*0.0;
#elif SHADE
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
#elif ATEST==3
    // alpha_check.txt diagnostic: an elided threshold test draws only the
    // fragments it would have discarded, in pure magenta (expected: none).
    if(r.a>=0.3828125)discard;
    r=vec4(1.0,0.0,1.0,1.0);
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
    // Direct GXM path (Vita gxm_off.txt off): the same compiled GXP pair,
    // patched and bound by the port. Uniform values live in CPU shadows of
    // each default uniform buffer, copied to vitaGL's uniform pool on change.
    int directState{}; // 0 untried, 1 ready, -1 unavailable (GL path)
    const SceGxmProgram* vertexGxp{};
    const SceGxmProgram* fragmentGxp{};
    SceGxmShaderPatcherId fragmentId{};
    SceGxmVertexProgram* vertexProgram{};
    std::array<std::pair<std::uint32_t,SceGxmFragmentProgram*>,8> fragmentPrograms{};
    std::size_t fragmentProgramCount{};
    int uM0{-1},uM1{-1},uM2{-1},uM3{-1},uClipmap{-1},uYflip{-1},uShade{-1},uMat{-1},uFlags{-1},uFlags2{-1};
    int uFparam{-1},uDepthmap{-1},textureUnit{-1},depthUnit{-1};
    std::vector<float> vertexUniforms,fragmentUniforms;
    void* vertexBlock{};void* fragmentBlock{};
    bool vertexDirty{true},fragmentDirty{true};
    std::array<float,20> dTransform{};std::array<float,4> dParameters{};float dFlip{};
    bool dTransformValid{},dParametersValid{},dFlipValid{},dDepthmapValid{};
    std::uint32_t dShading{UINT32_MAX},dDepthmapDivisor{};
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
    std::uint64_t residentResetSeen{};
    std::unordered_map<const EditorMarkerSolidGeometry::Strip*,VkModelRunRecord> modelRunRecords;
    std::unordered_map<const EditorMarkerSolidGeometry::Strip*,VkModelRunRecord> sceneryRunRecords;
    std::vector<VkSceneryFirstRecord> sceneryFirstRecords;
    std::vector<const EditorMarkerSolidGeometry::Strip*> modelRunScratchStrips;
    std::vector<const EditorMarkerSolidGeometry::Strip*> sceneryRunScratchStrips;
    std::vector<VkResidentGeometryEntry*> sceneryRunScratchEntries;
    std::vector<VkResidentGeometryEntry*> modelRunScratchEntries,modelRunScratchReplay;
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
    std::array<GlMappedBuffer,StreamRing> streamVertices,streamIndices,streamInstances;
    // Instanced GPU-position draws (Vita instance_off.txt off): every
    // native-transform draw reads its matrix from a per-frame table row, and
    // consecutive draws of the same resident geometry in the same state
    // (only the matrix differs) become one instanced draw. Same shader
    // arithmetic for every such draw, the same triangles in the same order.
    std::vector<float> instanceTable;
    std::vector<std::uint32_t> instanceRow,instanceRun;
    double instanceDraws{},instanceMembers{};
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
    bool halfPrewarmed{};GLuint gpuDiagWhite{};GLuint halfDepthCopyProgram{};GLint halfDepthCopyMap{-1};bool halfDepthCopyFailed{},halfHardwareDepth{};
    GLint halfCompositeFlip{-1};
    bool halfCompositeFailed{};
    std::uint64_t halfRuns{},halfBatches{},halfDepthWrites{},halfMissedBatches{},halfMissedIndices{},halfRunIndices{},halfBreaks[4]{};
    // Composite bounds: [x0,y0,x1,y1] window pixels for the next EndHalfParticles.
    std::array<float,4> halfBounds{{0.0f,0.0f,-1.0f,-1.0f}};GLuint halfBoundsQuad{};
    std::uint64_t halfCompositeBounded{},halfCompositeSkipped{};double halfCompositeArea{};
    std::size_t nextScreenshot{};
    int displayTargetScenes{};
    std::size_t nextDisplayShot{};
    std::uint64_t halfFramesTotal{};
    bool lastFrameHalf{};
    std::uint32_t displayShotPhase{};
    std::vector<std::pair<std::uint64_t,std::vector<std::uint32_t>>> displayShots;
    std::array<std::uint32_t,2> displayShotSize{};
    // Sun depth tiles (eTransferZBufferRect): vitaGL cannot read depth, so
    // each requested sample is drawn as one pixel just in front of the clear
    // depth inside a GL_SAMPLES_PASSED query; the passing count is the clear
    // (sky) sample count StuffSun consumes, published when the GPU is done.
    struct SunQuery {GLuint query{};EcstasyDepthTransfer request;bool pending{};};
    std::array<SunQuery,8> sunQueries{};
    std::size_t nextSunQuery{};
    GLuint sunQueryProgram{},sunQueryBuffer{};GLint sunQueryDepth{-1};bool sunQueryFailed{};
    std::vector<float> sunQueryVertices;
    void IssueSunQueries(const GlFramePacket& packet);
    bool EnsureSunQueryProgram();
    void PublishSunQueries();
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
    // Direct GXM path ownership: GXM state issued by the port since vitaGL
    // last drew (vitaGL's eager state must be re-applied before it draws
    // again), and GL activity since the port last drew (the port's GXM state
    // cache is stale and vitaGL may owe a scene begin or scissor mask).
    bool gxmDirectTouched{},glTouchedSinceDirect{true};
    GlMappedBuffer linearIndices;
    std::uint64_t directDraws{},directFallbacks{},directScissorSyncs{},directSetupFailures{};
    std::array<std::uint64_t,10> directCalls{}; // vprog fprog vunif funif tex depthtex stream0 stream1 instanced_draws index_draws
    bool GxmSetup(GlProgram& program,std::uint32_t key,bool instanced);
    SceGxmFragmentProgram* GxmFragment(GlProgram& program,const SceGxmBlendInfo& blend);
    void EnterGl() {if(gxmDirectTouched) {vglHp2Invalidate();gxmDirectTouched=false;}glTouchedSinceDirect=true;}

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
        for(auto& buffer:streamInstances)buffer.Destroy();
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
        pendingDecodes.clear();
        if(++gatherSerial==0) {
            for(auto& texture:textures)texture.validatedGather=0;
            for(auto& palette:depthPaletteCache)palette.gathered=0;
            ++gatherSerial;
        }
    }
    // Stamp retire (ecstasyRetireStamp): one pass over the commands with the
    // same skips/Requires/keys as CollectNativeTextureReferences stamps each
    // cached entry a command references (found through a self-validating
    // direct-mapped slot cache, else textureIndices); the scan then reads the
    // stamp instead of a sorted set + binary search. Same decisions: a
    // reference to an uncached key never matched an entry anyway.
    struct RetireSlot {const TextureInfo* source{};bool padding{};std::size_t index{};};
    std::array<RetireSlot,1024> retireSlots{};
    std::uint64_t retireSerial{};
    std::uint64_t retireVerifyFrames{},retireVerifyMismatches{};
    // Scan skip: with no owner destruction and no CaptureOwners since a scan
    // that left every entry alive, no entry can fail OwnersAlive (weak_ptrs
    // expire only when an owner is destroyed), so the scan would erase nothing.
    std::uint32_t retireDeathsSeen{};std::uint64_t retireCapturesSeen{};bool retireAllAlive{};
    std::uint64_t retireSkips{},retireScans{};
    void StampReferencedTextures(const std::vector<EcstasyDrawCommand>& commands)
    {
        const auto serial=++retireSerial;
        const TextureInfo* lastTexture=nullptr;bool lastClamp=false;
        for(const auto& draw:commands) {
            if(draw.kind==EcstasyDrawKind::FogPass || draw.kind==EcstasyDrawKind::FocusMaskPass ||
                draw.kind==EcstasyDrawKind::DepthOfFieldPass || draw.kind==EcstasyDrawKind::BlackAndWhitePass ||
                draw.kind==EcstasyDrawKind::SceneHistoryBoundary || draw.kind==EcstasyDrawKind::SceneHistorySource)continue;
            if(draw.sampledTarget) {
                Require(!draw.texture,"Target-sampled draw has conflicting image owners");continue;
            }
            Require(draw.texture,"Vulkan draw lost its actual texture");
            const bool forcedClamp=NativeDrawClampsTexture(draw);
            if(draw.texture==lastTexture && forcedClamp==lastClamp)continue;
            lastTexture=draw.texture;lastClamp=forcedClamp;
            const auto key=NativeTextureKey(*draw.texture,forcedClamp);
            const auto identity=reinterpret_cast<std::uintptr_t>(key.source);
            auto& slot=retireSlots[((identity>>4)^(identity>>14)^(key.repeatedPadding ? 0x155u:0u))&(retireSlots.size()-1)];
            std::size_t index;
            if(slot.source==key.source && slot.padding==key.repeatedPadding && slot.index<textures.size() &&
                textures[slot.index].source==key.source && textures[slot.index].repeatedPadding==key.repeatedPadding)
                index=slot.index;
            else {
                const auto found=textureIndices.find(key);
                if(found==textureIndices.end())continue;
                index=found->second;slot={key.source,key.repeatedPadding,index};
            }
            textures[index].retireStamp=serial;textures[index].lastReferencedFrame=completedFrameSerial;
        }
    }
    bool RetireUnusedTextures(const VkNativeFrameTextureReferences& frameTextures)
    {
        if(ecstasyRetireStamp) {
            // Skip frames need no stamps: every entry is alive, so none is
            // erasable whatever this frame references (lastReferencedFrame
            // is not read by this backend). Verify mode stamps every frame.
            const auto deaths=textureOwnerDeaths.load(std::memory_order_acquire);
            const bool skip=ecstasyRetireStamp!=3 && retireAllAlive && deaths==retireDeathsSeen &&
                textureOwnerCaptures==retireCapturesSeen;
            if(!skip || ecstasyRetireStamp==2)StampReferencedTextures(frameTextures.Commands());
            if(ecstasyRetireStamp==2) {
                const auto& referenced=frameTextures.Get();
                ++retireVerifyFrames;
                for(const auto& texture:textures)
                    if((texture.retireStamp==retireSerial)!=NativeTextureReferenced(referenced,{texture.source,texture.repeatedPadding}))
                        ++retireVerifyMismatches;
                if(retireVerifyFrames%300==0) {
                    char detail[160];std::snprintf(detail,sizeof(detail),"frames=%llu mismatches=%llu textures=%u skips=%llu scans=%llu",
                        static_cast<unsigned long long>(retireVerifyFrames),static_cast<unsigned long long>(retireVerifyMismatches),
                        static_cast<unsigned>(textures.size()),static_cast<unsigned long long>(retireSkips),static_cast<unsigned long long>(retireScans));
                    platform::RecordSessionEvent("renderer.vitagl.retire_verify",detail);
                }
            }
            if(skip) {
                ++retireSkips;
                if(ecstasyRetireStamp==2)for(const auto& texture:textures)
                    if(texture.retireStamp!=retireSerial && !texture.OwnersAlive())++retireVerifyMismatches;
                return false;
            }
            ++retireScans;
            bool changed=false,allAlive=true;
            for(auto at=textures.begin();at!=textures.end();) {
                if(at->retireStamp==retireSerial) {if(allAlive && !at->OwnersAlive())allAlive=false;++at;}
                else if(!at->OwnersAlive()) {Destroy(*at);at=textures.erase(at);changed=true;}
                else ++at;
            }
            retireAllAlive=allAlive;retireDeathsSeen=deaths;retireCapturesSeen=textureOwnerCaptures;
            if(changed) {
                textureIndices.clear();textureIndices.reserve(textures.size());
                for(std::size_t index=0;index<textures.size();++index)
                    textureIndices.emplace(VkNativeTextureKey{textures[index].source,textures[index].repeatedPadding},index);
            }
            return changed;
        }
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
    bool PrewarmAllowed() const noexcept {return true;}
    // Base-level decodes recorded by UploadTexture during Gather and run
    // after its command loop (in parallel when several): the same decode of
    // the same sealed-frame bytes into the same pending upload.
    struct PendingDecode {const TextureInfo* texture{};std::size_t upload{},baseBytes{};bool repeatedPadding{};std::size_t index{};};
    void MarkRedundantAlphaTests(const std::vector<VkVertex>& vertices,const std::vector<std::uint32_t>& indices,
        std::vector<VkDrawBatch>& batches,const std::vector<std::array<float,4>>& record,
        const std::vector<const VkResidentGeometryEntry*>& spans,const std::vector<GlFramePacket::TextureRef>* packetTextures);
    // Latched at the first Render (flag files are read before it).
    bool deferResolve{},deferResolveLatched{},glResidentIndicesAllocated{};
    std::vector<VkResidentIndexEntry*> glIndexPendingUploads;
    // Waits until the GL thread has drawn every packet but the one this
    // Render claimed (it then touches no resident cache state).
    void WaitOtherPacketsIdle()
    {
        if(!glThreadStarted_)return;
        for(std::size_t i=0;i+1<packets.size();++i)sceKernelWaitSema(freeSema_,1,nullptr);
        sceKernelSignalSema(freeSema_,static_cast<int>(packets.size()-1));
        RethrowGlError();
    }
    void ResolveDeferredIndices(GlFramePacket& packet);
    std::vector<PendingDecode> pendingDecodes;
    void RunPendingDecodes();
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
    std::size_t sourceBytes{};
    // Only level 0 is uploaded and decoded. The retained source range (and
    // so the cache comparison) still spans every authored level. The decode
    // itself is deferred (RunPendingDecodes); its extent is TEX0's level 0.
    Require(wire[0x48]<=14 && wire[0x49]<=14,"Invalid authored texture log dimensions");
    const std::uint32_t width=1u<<wire[0x48],height=1u<<wire[0x49];
    for(std::uint32_t level=0;level<levels;++level)sourceBytes+=baseBytes>>(2u*level);
    Require(sourceBytes<=texture.pixels.size,"vitaGL texture lost part of its authored mip chain");
    if(textures[index].generation)LogReupload(textures[index],texture);
    else EcstasyRecordTextureFirstUse(texture,repeatedPadding);
    auto& target=textures[index];
    auto* handle=glTextureOf(target);
    if(!handle) {handle=new GlTexture;target.image.image=reinterpret_cast<VkImage>(handle);}
    target.width=width;target.height=height;target.levels=levels;
    const auto minification=wire[0x70];
    const bool linear=options.textureFilter!=EcstasyTextureFilter::Authored ||
        minification==1 || minification>=4 || wire[0x6f];
    ++statistics.textureUploads;statistics.textureUploadBytes+=static_cast<std::size_t>(width)*height*4u;
    if(platform::nativeDiagnosticsEnabled) {
        // Diagnostic: which images are decoded when (first-use hitches).
        static unsigned logged=0;
        if(logged<800) {
            ++logged;
            char detail[200];
            std::snprintf(detail,sizeof(detail),"frame=%llu key=%08x ptr=%p pack=%s size=%ux%u psm=%02x levels=%u reupload=%d",
                static_cast<unsigned long long>(completedFrameSerial),texture.descriptor->key,static_cast<const void*>(&texture),
                texture.pack ? texture.pack->Name().c_str():"none",width,height,wire[0x4a],levels,textures[index].generation ? 1:0);
            platform::RecordSessionEvent("renderer.vitagl.texture_upload",detail);
        }
    }
    pendingDecodes.push_back({&texture,building->uploads.size(),baseBytes,repeatedPadding,index});
    textures[index].minAlpha=-1;
    building->uploads.push_back({handle,{},width,height,linear ? GL_LINEAR:GL_NEAREST,
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

// Alpha-test elision (Vita alpha_elide_off.txt off). The fragment alpha
// before the test is a product/sum of the texel alpha (byte/128) and the
// interpolated vertex alpha (FUNC 0..3, see kGlFragmentShader). Filtering
// and interpolation stay within the operands' minimum, so when that lower
// bound clears the threshold by 1/64 no fragment can be discarded and the
// discard is dropped: identical pixels, and the GPU keeps its hidden-
// surface removal. Lit (SHADE) and render-target samples keep the test.
int ecstasyAlphaElide=1;
int ecstasyAlphaCheck=0;
// defer_resolve_off.txt clears it: resident index resolve + alpha-test
// marking run on the render thread (core 1) instead of the GL thread.
int ecstasyDeferResolve=1;
void EcstasyVulkanDevice::Storage::MarkRedundantAlphaTests(const std::vector<VkVertex>& vertices,
    const std::vector<std::uint32_t>& indices,std::vector<VkDrawBatch>& batches,const std::vector<std::array<float,4>>& record,
    const std::vector<const VkResidentGeometryEntry*>& spans,const std::vector<GlFramePacket::TextureRef>* packetTextures)
{
    if(!ecstasyAlphaElide)return;
    // Diagnostic reason counts: tested, shade, sampled, no texture alpha,
    // texture alpha low, vertex alpha low, elided, threshold-2 count.
    static std::array<std::uint64_t,8> why{};static unsigned calls=0;
    const auto report=[&] {
        if(++calls%300u || !platform::nativeDiagnosticsEnabled)return;
        char text[200];
        std::snprintf(text,sizeof(text),"tested=%llu shade=%llu sampled=%llu tex_unknown=%llu tex_low=%llu vertex_low=%llu elided=%llu atest2=%llu",
            (unsigned long long)why[0],(unsigned long long)why[1],(unsigned long long)why[2],(unsigned long long)why[3],
            (unsigned long long)why[4],(unsigned long long)why[5],(unsigned long long)why[6],(unsigned long long)why[7]);
        platform::RecordSessionEvent("renderer.vitagl.alpha_elide",text);why={};
    };
    struct Reporter {decltype(report)& r;~Reporter() {r();}} reporter{report};
    for(auto& batch:batches) {
        batch.alphaTestRedundant=false;
        if(batch.alphaTest<1 || batch.alphaTest>2)continue;
        ++why[0];if(batch.alphaTest==2)++why[7];
        if(batch.sampledTarget>0) {++why[2];continue;}
        // SHADE (kGlVertexShader): the vertex alpha is uniform-derived. Its
        // lower bound follows from the record when no term can lower it
        // (diffuse and pow() terms are >= 0, so non-negative coefficients
        // only raise it; floor/clamp are monotonic). Reflection-mapped
        // records wrap alpha modulo 256 and keep the test.
        const bool shaded=batch.nativeGpuPosition && batch.nativeShadingIndex!=0;
        float shadedAlpha=0.0f;
        if(shaded) {
            const auto first=batch.nativeShadingIndex;
            if(first+19u>record.size()) {++why[1];continue;}
            std::uint32_t flags{},mode{},material{};
            std::memcpy(&flags,&record[first][0],4);std::memcpy(&mode,&record[first][1],4);
            std::memcpy(&material,&record[first][2],4);
            static const std::array<float,4> zero{};
            const auto mat=[&](std::uint32_t row)->const std::array<float,4>& {
                return material && material+4u<=record.size() ? record[material+row]:zero;
            };
            const auto steps=[](float value) {return std::floor(std::clamp(value,0.0f,255.0f))/128.0f;};
            const bool lit=(flags&128u)!=0,alphaLit=(flags&64u)!=0;
            if(mode&1u) {++why[1];continue;}
            const float parameter=record[first+1][0];
            if(!(parameter>=0.0f)) {++why[1];continue;}
            float lower=1.0f;bool bounded=true;
            if(lit) {
                for(std::uint32_t row=13;row<=18;++row)bounded=bounded && record[first+row][3]>=0.0f;
                bounded=bounded && mat(0)[1]>=0.0f && mat(3)[1]>=0.0f;
                lower=steps(record[first+12][3]);
            }
            if(alphaLit) {
                bounded=bounded && mat(3)[1]>=0.0f && mat(3)[3]>=0.0f;
                lower=steps(mat(3)[2]);
            }
            if(!bounded || !std::isfinite(lower)) {++why[1];continue;}
            shadedAlpha=(lit || alphaLit) ? lower*parameter:parameter;
        }
        float texel=1.0f;
        if(!batch.untextured) {
            const int textureMin=packetTextures ?
                (batch.texture<packetTextures->size() ? (*packetTextures)[batch.texture].minAlpha:-1):
                (batch.texture<textures.size() ? textures[batch.texture].minAlpha:-1);
            if(textureMin<0) {++why[3];continue;}
            texel=static_cast<float>(textureMin)/128.0f;
        }
        float vertex=std::numeric_limits<float>::infinity();
        if(shaded)vertex=shadedAlpha;
        else if(batch.residentGeometry) {
            if(static_cast<std::size_t>(batch.nativeSpanFirst)+batch.nativeSpanCount>spans.size())continue;
            for(std::uint32_t at=0;at<batch.nativeSpanCount;++at)
                vertex=std::min(vertex,spans[batch.nativeSpanFirst+at]->minAlpha);
        } else if(batch.indexed) {
            if(batch.residentIndices || static_cast<std::size_t>(batch.first)+batch.count>indices.size())continue;
            bool valid=true;
            for(std::uint32_t at=0;at<batch.count;++at) {
                const auto index=indices[batch.first+at];
                if(index>=vertices.size()) {valid=false;break;}
                vertex=std::min(vertex,vertices[index].color[3]);
            }
            if(!valid)continue;
        } else {
            if(static_cast<std::size_t>(batch.first)+batch.count>vertices.size())continue;
            for(std::uint32_t at=0;at<batch.count;++at)vertex=std::min(vertex,vertices[batch.first+at].color[3]);
        }
        if(!(vertex>=0.0f) || !std::isfinite(vertex))continue;
        float least;
        if(batch.untextured)least=vertex;
        else switch(batch.textureFunction&3) {
            case 0:least=texel*vertex;break;
            case 2:least=texel+vertex;break;
            default:least=texel;break;
        }
        const float threshold=batch.alphaTest==2 ? 0.3828125f:0.0f;
        batch.alphaTestRedundant=least>=threshold+1.0f/64.0f;
        if(batch.alphaTestRedundant)++why[6];
        else if(!batch.untextured && texel<threshold+1.0f/64.0f)++why[4];
        else ++why[5];
    }
}

void EcstasyVulkanDevice::Storage::RunPendingDecodes()
{
    if(pendingDecodes.empty())return;
    auto& uploads=building->uploads;
    auto decode=[&](std::size_t index) {
        const auto& pending=pendingDecodes[index];
        auto& upload=uploads[pending.upload];
        std::uint32_t width{},height{};
        upload.rgba=DecodeTextureMipRgba(*pending.texture,0,0,pending.baseBytes,width,height,pending.repeatedPadding);
        Require(width==upload.width && height==upload.height,"vitaGL deferred texture decode changed its extent");
        int least=255;
        for(std::size_t at=3;at<upload.rgba.size();at+=4)least=std::min<int>(least,upload.rgba[at]);
        textures[pending.index].minAlpha=least;
    };
    // Gather-thread pool (cores 1/2): largest decodes are seconds-scale
    // rare first sights, so even two decodes are worth splitting.
    try {RunNativeSceneJobs(pendingDecodes.size(),decode,2,2,true);}
    catch(...) {pendingDecodes.clear();throw;}
    pendingDecodes.clear();
}

#define HP2_GATHER_GL 1
#include "src/ecstasy/ecstasy_gather.inc"
#undef HP2_GATHER_GL

void EcstasyVulkanDevice::Storage::ResolveDeferredIndices(GlFramePacket& packet)
{
    // GL thread, before the packet's uploads. The geometry entries its spans
    // name were referenced by that Gather and stay unevicted/unreassigned
    // through the next one (SetProtectPrevious); the 2-packet ring keeps a
    // third Gather from starting before this packet is drawn.
    residentIndexCache.Begin(packet.indexGather);
    ResolveNativeResidentIndices(residentIndexCache,packet.residentSpans,packet.residentSpanTokens,packet.indices,packet.batches);
    MarkRedundantAlphaTests(packet.vertices,packet.indices,packet.batches,packet.glyphBounds,packet.residentSpans,&packet.textures);
    glIndexPendingUploads.clear();
    if(!residentIndexCache.Active().empty()) {
        packet.residentIndexCapacity=static_cast<std::size_t>(residentIndexCache.Capacity())*sizeof(std::uint32_t);
        if(!glResidentIndicesAllocated) {glResidentIndicesAllocated=true;residentIndexCache.InvalidateUploads();}
        for(auto* entry:residentIndexCache.Active()) {
            if(entry->uploaded)continue;
            const auto offset=static_cast<std::size_t>(entry->first)*sizeof(std::uint32_t);
            const auto bytes=entry->shadow.size()*sizeof(std::uint32_t);
            const auto* source=reinterpret_cast<const std::uint8_t*>(entry->shadow.data());
            packet.residentIndexBytes.insert(packet.residentIndexBytes.end(),source,source+bytes);
            packet.residentIndexRanges.push_back({{offset,bytes}});
            glIndexPendingUploads.push_back(entry);
        }
    }
    residentIndexCache.PublishUploads(glIndexPendingUploads);
}

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
    std::snprintf(path,sizeof(path),"ux0:data/NFSHP2/shader_cache/%03x-%016llx.bin",key,
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
    sceIoMkdir("ux0:data/NFSHP2/shader_cache",0777);
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

extern "C" std::uint32_t vsync_interval; // vitaGL: vblanks waited per swap (0 = no vsync)
int ecstasyInstancing=1; // Vita instance_off.txt: per-draw uniform matrices
int ecstasyAffineUV=1; // Vita affine_off.txt restores the projective divide everywhere
int ecstasyForceNoVsync=0;
// Vita default 2: present every 2nd vblank (30 fps lock); 0 (fps_uncapped.txt) leaves vitaGL's
// interval alone. Applied by the GL thread before each swap.
int ecstasyVsyncInterval=0;
// Vita gpu_diag.txt N (diagnostic only, wrong images on purpose): 1 = every
// texture a 1x1 white, 2 = lit vertex shading replaced by the plain path,
// 3 = every draw scissored to 8x8 pixels, 4 = particle (half-eligible) draws skipped.
int ecstasyGpuDiag=0;
// Vita scissor_elide_off.txt: keep the scissor test on for whole-target rectangles.
int ecstasyScissorElide=1;
int ecstasyFragmentMediump=0; // Vita frag_mediump.txt (A/B: GPU fragment cost)
GlProgram& EcstasyVulkanDevice::Storage::Program(std::uint32_t key)
{
    auto& program=programs[key];
    if(program.program || program.failed)return program;
    const unsigned function=key&3u,texturing=(key>>2)&1u,alphaTest=(key>>3)&3u,showcase=(key>>5)&1u,
        fixedAlpha=(key>>6)&1u,clamp=(key>>7)&1u,gpuPosition=(key>>8)&1u,shade=(key>>9)&1u,affine=(key>>10)&1u,
        depthTexture=(key>>11)&1u,instanced=(key>>12)&1u;
    char defines[340];
    std::snprintf(defines,sizeof(defines),
        "#define FUNC %u\n#define TEXTURING %u\n#define ATEST %u\n#define SHOWCASE %u\n#define AFIX %u\n"
        "#define CLAMPTL %u\n#define GPUPOS %u\n#define SHADE %u\n#define AFFINE %u\n#define DEPTHTEX %u\n#define TRIVIALVS %u\n"
        "#define INST %u\n",
        function,texturing,alphaTest,showcase,fixedAlpha,clamp,gpuPosition,shade,affine,depthTexture,(ecstasyGpuDiag==2||ecstasyGpuDiag==7) ? 1u:0u,instanced);
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
    if(const auto binary=(ecstasyGpuDiag==2||ecstasyGpuDiag==7) ? std::vector<std::uint8_t>{}:GlReadProgramCache(key|(ecstasyFragmentMediump ? 0x80000000u:0u));!binary.empty()) {
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
    if(instanced) {
        glBindAttribLocation(program.program,4,"a_i0");glBindAttribLocation(program.program,5,"a_i1");
        glBindAttribLocation(program.program,6,"a_i2");glBindAttribLocation(program.program,7,"a_i3");
        glBindAttribLocation(program.program,8,"a_i4");
    }
    glLinkProgram(program.program);
    GLint linked=0;glGetProgramiv(program.program,GL_LINK_STATUS,&linked);
    if(!linked) {
        char detail[64];std::snprintf(detail,sizeof(detail),"key=%03x",key);
        platform::RecordSessionEvent("renderer.vitagl.link_failed",detail);
        glDeleteProgram(program.program);program.program=0;program.failed=true;return program;
    }
    if(ecstasyGpuDiag!=2 && ecstasyGpuDiag!=7)GlWriteProgramCache(key|(ecstasyFragmentMediump ? 0x80000000u:0u),program.program);
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
        for(std::uint32_t known:{0x004u,0x314u,0x304u,0x00cu,0x014u}) {
            if(ecstasyInstancing && (known&0x100u))known|=0x1000u;
            if(known!=key)Program(known);
        }
        if(program.program)glUseProgram(program.program);
    }
    return program;
}

int ecstasyGxmDirect=1; // Vita gxm_off.txt: every draw through vitaGL
// Diagnostic diag_nodraw.txt: 1 skips every direct draw, 2 only particle batches.
int ecstasyDiagNoDraw=0;
int ecstasyGxmFlushDraws=0; // Vita gxm_flush.txt N: sceGxmMidSceneFlush every N direct draws (vertex work starts early)
int ecstasyGxmParity=0; // Vita gxm_parity.txt: shot frames drawn twice (vitaGL shot +100000000, then direct)
int ecstasyGxmProf=0; // Vita gxm_prof.txt: per-draw pass profile (adds ~2 us clock calls per draw)
bool EcstasyVulkanDevice::Storage::GxmSetup(GlProgram& program,std::uint32_t key,bool instanced)
{
    if(program.directState)return program.directState>0;
    program.directState=-1;
    const auto fail=[&](const char* why) {
        ++directSetupFailures;
        char detail[160];std::snprintf(detail,sizeof(detail),"key=%03x reason=%s",key,why);
        platform::RecordSessionEvent("renderer.gxm.setup_failed",detail);
        // Name dump of the vertex program's parameters (first failure only).
        static bool dumped=false;
        if(!dumped && program.vertexGxp) {
            dumped=true;
            const auto count=sceGxmProgramGetParameterCount(program.vertexGxp);
            for(unsigned at=0;at<count;++at) {
                const auto* parameter=sceGxmProgramGetParameter(program.vertexGxp,at);
                std::snprintf(detail,sizeof(detail),"vertex param=%s category=%d type=%d index=%u container=%u",
                    sceGxmProgramParameterGetName(parameter),static_cast<int>(sceGxmProgramParameterGetCategory(parameter)),
                    static_cast<int>(sceGxmProgramParameterGetType(parameter)),sceGxmProgramParameterGetResourceIndex(parameter),
                    sceGxmProgramParameterGetContainerIndex(parameter));
                platform::RecordSessionEvent("renderer.gxm.param",detail);
            }
        }
        return false;
    };
    SceGxmShaderPatcherId vertexId{};
    if(!vglHp2ProgramInfo(program.program,&program.vertexGxp,&program.fragmentGxp,&vertexId,&program.fragmentId))
        return fail("program_info");
    // Vertex attributes: VkVertex (68-byte stream 0) and, for INST, the
    // 80-byte instance table rows (stream 1), the same layout the GL path binds.
    struct AttributeSpec {const char* name;unsigned stream,offset,components;};
    const AttributeSpec specs[]={
        {"a_pos",0,static_cast<unsigned>(offsetof(VkVertex,position)),4},{"a_uvq",0,static_cast<unsigned>(offsetof(VkVertex,uvq)),3},
        {"a_col",0,static_cast<unsigned>(offsetof(VkVertex,color)),4},{"a_nrm",0,static_cast<unsigned>(offsetof(VkVertex,normal)),3},
        {"a_i0",1,0,4},{"a_i1",1,16,4},{"a_i2",1,32,4},{"a_i3",1,48,4},{"a_i4",1,64,4}};
    std::array<SceGxmVertexAttribute,9> attributes{};unsigned attributeCount=0;
    for(const auto& spec:specs) {
        if(spec.stream==1 && !instanced)continue;
        const auto* parameter=sceGxmProgramFindParameterByName(program.vertexGxp,spec.name);
        if(!parameter) {
            if(!std::strcmp(spec.name,"a_pos"))return fail("no_a_pos");
            continue; // optimised out
        }
        if(sceGxmProgramParameterGetCategory(parameter)!=SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)return fail("attribute_category");
        auto& attribute=attributes[attributeCount++];
        attribute.streamIndex=static_cast<unsigned short>(spec.stream);
        attribute.offset=static_cast<unsigned short>(spec.offset);
        attribute.format=SCE_GXM_ATTRIBUTE_FORMAT_F32;
        attribute.componentCount=static_cast<unsigned char>(spec.components);
        attribute.regIndex=static_cast<unsigned short>(sceGxmProgramParameterGetResourceIndex(parameter));
    }
    std::array<SceGxmVertexStream,2> streams{};
    streams[0].stride=static_cast<unsigned short>(sizeof(VkVertex));streams[0].indexSource=SCE_GXM_INDEX_SOURCE_INDEX_32BIT;
    streams[1].stride=80;streams[1].indexSource=SCE_GXM_INDEX_SOURCE_INSTANCE_32BIT;
    if(sceGxmShaderPatcherCreateVertexProgram(gxm_shader_patcher,vertexId,attributes.data(),attributeCount,
        streams.data(),instanced ? 2u:1u,&program.vertexProgram)<0 || !program.vertexProgram)return fail("vertex_patch");
    // Default-buffer float uniforms (offsets in 32-bit words, as vitaGL).
    const auto uniform=[&](const SceGxmProgram* gxp,const char* name,int& offset)->bool {
        offset=-1;
        const auto* parameter=sceGxmProgramFindParameterByName(gxp,name);
        if(!parameter)return true;
        if(sceGxmProgramParameterGetCategory(parameter)!=SCE_GXM_PARAMETER_CATEGORY_UNIFORM ||
            sceGxmProgramParameterGetType(parameter)!=SCE_GXM_PARAMETER_TYPE_F32 ||
            sceGxmProgramParameterGetContainerIndex(parameter)!=SCE_GXM_MAX_UNIFORM_BUFFERS)return false;
        offset=static_cast<int>(sceGxmProgramParameterGetResourceIndex(parameter));
        return true;
    };
    bool uniformsOk=uniform(program.vertexGxp,"u_m0",program.uM0) && uniform(program.vertexGxp,"u_m1",program.uM1) &&
        uniform(program.vertexGxp,"u_m2",program.uM2) && uniform(program.vertexGxp,"u_m3",program.uM3) &&
        uniform(program.vertexGxp,"u_clipmap",program.uClipmap) && uniform(program.vertexGxp,"u_yflip",program.uYflip) &&
        uniform(program.vertexGxp,"u_shade",program.uShade) && uniform(program.vertexGxp,"u_mat",program.uMat) &&
        uniform(program.vertexGxp,"u_flags",program.uFlags) && uniform(program.vertexGxp,"u_flags2",program.uFlags2) &&
        uniform(program.fragmentGxp,"u_fparam",program.uFparam) && uniform(program.fragmentGxp,"u_depthmap",program.uDepthmap);
    if(!uniformsOk)return fail("uniform_type");
    const auto sampler=[&](const char* name)->int {
        const auto* parameter=sceGxmProgramFindParameterByName(program.fragmentGxp,name);
        if(!parameter || sceGxmProgramParameterGetCategory(parameter)!=SCE_GXM_PARAMETER_CATEGORY_SAMPLER)return -1;
        return static_cast<int>(sceGxmProgramParameterGetResourceIndex(parameter));
    };
    program.textureUnit=sampler("u_tex");program.depthUnit=sampler("u_depth");
    if(((key>>2)&1u) && program.textureUnit<0)return fail("no_texture_sampler");
    if(((key>>11)&1u) && program.depthUnit<0)return fail("no_depth_sampler");
    program.vertexUniforms.assign(sceGxmProgramGetDefaultUniformBufferSize(program.vertexGxp)/4u,0.0f);
    program.fragmentUniforms.assign(sceGxmProgramGetDefaultUniformBufferSize(program.fragmentGxp)/4u,0.0f);
    const auto fits=[](const std::vector<float>& buffer,int offset,std::size_t floats) {
        return offset<0 || static_cast<std::size_t>(offset)+floats<=buffer.size();
    };
    if(!fits(program.vertexUniforms,program.uM0,4) || !fits(program.vertexUniforms,program.uM1,4) ||
        !fits(program.vertexUniforms,program.uM2,4) || !fits(program.vertexUniforms,program.uM3,4) ||
        !fits(program.vertexUniforms,program.uClipmap,4) || !fits(program.vertexUniforms,program.uYflip,1) ||
        !fits(program.vertexUniforms,program.uShade,19*4) || !fits(program.vertexUniforms,program.uMat,16) ||
        !fits(program.vertexUniforms,program.uFlags,4) || !fits(program.vertexUniforms,program.uFlags2,4) ||
        !fits(program.fragmentUniforms,program.uFparam,4) || !fits(program.fragmentUniforms,program.uDepthmap,4))
        return fail("uniform_bounds");
    program.directState=1;
    char detail[200];
    std::snprintf(detail,sizeof(detail),"key=%03x attributes=%u vertex_uniform_words=%u fragment_uniform_words=%u tex_unit=%d depth_unit=%d",
        key,attributeCount,static_cast<unsigned>(program.vertexUniforms.size()),static_cast<unsigned>(program.fragmentUniforms.size()),
        program.textureUnit,program.depthUnit);
    platform::RecordSessionEvent("renderer.gxm.program",detail);
    return true;
}

SceGxmFragmentProgram* EcstasyVulkanDevice::Storage::GxmFragment(GlProgram& program,const SceGxmBlendInfo& blend)
{
    static_assert(sizeof(SceGxmBlendInfo)==4,"SceGxmBlendInfo is one word");
    std::uint32_t code{};std::memcpy(&code,&blend,sizeof(code));
    for(std::size_t at=0;at<program.fragmentProgramCount;++at)
        if(program.fragmentPrograms[at].first==code)return program.fragmentPrograms[at].second;
    SceGxmFragmentProgram* fragment{};
    if(sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher,program.fragmentId,SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
        SCE_GXM_MULTISAMPLE_NONE,&blend,program.vertexGxp,&fragment)<0)return nullptr;
    if(program.fragmentProgramCount<program.fragmentPrograms.size())
        program.fragmentPrograms[program.fragmentProgramCount++]={code,fragment};
    else program.fragmentPrograms[(code>>3)%program.fragmentPrograms.size()]={code,fragment}; // patcher keeps the old one alive
    return fragment;
}

namespace {
SceGxmBlendFactor GxmFactor(GLenum factor) noexcept
{
    switch(factor) {
        case GL_ZERO:return SCE_GXM_BLEND_FACTOR_ZERO;
        case GL_SRC_ALPHA:return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
        case GL_ONE_MINUS_SRC_ALPHA:return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case GL_DST_ALPHA:return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
        case GL_ONE_MINUS_DST_ALPHA:return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default:return SCE_GXM_BLEND_FACTOR_ONE;
    }
}
SceGxmBlendFunc GxmEquation(GLenum equation) noexcept
{
    return equation==GL_FUNC_SUBTRACT ? SCE_GXM_BLEND_FUNC_SUBTRACT:
        equation==GL_FUNC_REVERSE_SUBTRACT ? SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT:SCE_GXM_BLEND_FUNC_ADD;
}
// The blend word vitaGL builds for the same GL calls the GL path issues
// (glBlendEquationSeparate(eq,ADD) + glBlendFuncSeparate, or glDisable(BLEND)
// -> change_blend_mask), so the patcher returns the same fragment program.
SceGxmBlendInfo GxmBlendFor(const GlBlendState& blend,bool halfMode) noexcept
{
    SceGxmBlendInfo info{};
    info.colorMask=SCE_GXM_COLOR_MASK_ALL;
    if(!blend.enable) {
        info.colorFunc=SCE_GXM_BLEND_FUNC_NONE;info.alphaFunc=SCE_GXM_BLEND_FUNC_NONE;
        info.colorSrc=SCE_GXM_BLEND_FACTOR_SRC_ALPHA;info.colorDst=SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        info.alphaSrc=SCE_GXM_BLEND_FACTOR_ONE;info.alphaDst=SCE_GXM_BLEND_FACTOR_ZERO;
        return info;
    }
    info.colorFunc=GxmEquation(blend.equation);info.alphaFunc=SCE_GXM_BLEND_FUNC_ADD;
    info.colorSrc=GxmFactor(blend.source);info.colorDst=GxmFactor(blend.destination);
    if(halfMode) {
        info.alphaSrc=blend.destination==GL_ONE ? SCE_GXM_BLEND_FACTOR_ZERO:SCE_GXM_BLEND_FACTOR_ONE;
        info.alphaDst=blend.destination==GL_ONE ? SCE_GXM_BLEND_FACTOR_ONE:SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    } else {
        info.alphaSrc=blend.fixedAlpha ? SCE_GXM_BLEND_FACTOR_ZERO:SCE_GXM_BLEND_FACTOR_ONE;
        info.alphaDst=blend.fixedAlpha ? SCE_GXM_BLEND_FACTOR_ONE:SCE_GXM_BLEND_FACTOR_ZERO;
    }
    return info;
}
}

// Batches were written by the gather thread and are mostly cold here; each
// is ~8 cache lines. Prefetch a few batches ahead in the GL thread's loops.
inline void PrefetchBatch(const VkDrawBatch& batch) noexcept
{
    const auto* bytes=reinterpret_cast<const char*>(&batch);
    for(std::size_t at=0;at<sizeof(VkDrawBatch);at+=32)__builtin_prefetch(bytes+at);
}

std::atomic<int> glRenderStage{0};
extern int ecstasyGpuTimeline;
bool EcstasyVulkanDevice::Storage::Render(EcstasyFrame* frame)
{
    glRenderStage=1;
    if(!frame)return false;
    Require(frame->State()==EcstasyFrame::Phase::Ready,"vitaGL accepts only a sealed original frame");
    using Clock=std::chrono::steady_clock;
    const auto milliseconds=[](Clock::duration duration) {return std::chrono::duration<double,std::milli>(duration).count();};
    // Claim a free packet: at most one frame is gathered while one is drawn.
    const auto claimBegin=Clock::now();
    const std::uint64_t timelineEntry=ecstasyGpuTimeline ? sceKernelGetSystemTimeWide():0;
    HP2_GATHER_PHASE(0u);
    if(glThreadStarted_)sceKernelWaitSema(freeSema_,1,nullptr);
    HP2_GATHER_PHASE(1u<<8);
    const auto claimMs=milliseconds(Clock::now()-claimBegin);
    struct ReleaseOnThrow {
        Storage& storage;bool armed{true};
        ~ReleaseOnThrow() {if(armed && storage.glThreadStarted_)sceKernelSignalSema(storage.freeSema_,1);}
    } release{*this};
    {
        std::lock_guard<std::mutex> lock(glStatsMutex);
        if(glError_) {auto error=glError_;glError_=nullptr;std::rethrow_exception(error);}
    }
    if(!deferResolveLatched) {
        deferResolveLatched=true;deferResolve=ecstasyDeferResolve!=0;
        residentGeometry.SetProtectPrevious(deferResolve);
    }
    building=&packets[nextPacket];nextPacket=(nextPacket+1)%packets.size();
    building->Clear();
    building->timelineEntry=timelineEntry;
    building->timelineClaimed=ecstasyGpuTimeline ? sceKernelGetSystemTimeWide():0;
    const auto renderBegin=Clock::now();
    statistics={};
    statistics.presentationTarget=true;
    statistics.availableCpuThreads=NativeSceneEffectiveHardwareConcurrency();
    ++nextSubmission;
    // vitaGL defers resource frees until the GPU is done with them; the CPU
    // side treats every Render as following the previous frame's completion.
    ++completedFrameSerial;
    BeginGather();HP2_GATHER_PHASE((1u<<8)|(1u<<5));
    VkNativeFrameTextureReferences frameTextures(frame->Commands(),referencedTextures);HP2_GATHER_PHASE((1u<<8)|(2u<<5));
    RetireUnusedTextures(frameTextures);HP2_GATHER_PHASE((1u<<8)|(3u<<5));
    preparedFrame.historyReaders=EcstasyFindNativeHistoryReaders(frame->Commands());HP2_GATHER_PHASE((1u<<8)|(4u<<5));
    const auto mirror=MirrorPresentation(*frame);HP2_GATHER_PHASE((1u<<8)|(5u<<5));
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
    Gather(*frame,data,elementIndices,batches);HP2_GATHER_PHASE((5u<<8)|(1u<<5));
    RunPendingDecodes();HP2_GATHER_PHASE((5u<<8)|(2u<<5));
    if(!deferResolve)MarkRedundantAlphaTests(data,elementIndices,batches,gatheredGlyphBounds,gatheredResidentIndexSpans,nullptr);
    glRenderStage=3;HP2_GATHER_PHASE(6u<<8);
    statistics.gatherMilliseconds=milliseconds(Clock::now()-gatherBegin);

    // Resident geometry/index slots: copy dirty entries into the packet; the
    // GL thread writes them in place before drawing it.
    residentPendingUploads.clear();residentIndexPendingUploads.clear();
    auto& packet=*building;
    if(!residentGeometry.Active().empty()) {
        packet.residentVertexCapacity=residentGeometry.Capacity()*VkResidentGeometryCache::SlotBytes;
        if(!residentVerticesAllocated) {residentVerticesAllocated=true;residentGeometry.InvalidateUploads();}
        for(const auto slot:residentGeometry.Active()) {
            // Compact slot state: an uploaded slot reads no entry.
            const auto slotState=residentGeometry.SlotState(slot);
            Require(slotState&1u,"Resident geometry upload has unfinished vertex preparation");
            if(slotState&2u)continue;
            const auto& entry=residentGeometry.Entry(slot);
            const auto offset=static_cast<std::size_t>(slot)*VkResidentGeometryCache::SlotBytes;
            const auto bytes=static_cast<std::size_t>(entry.count)*sizeof(VkVertex);
            const auto* source=reinterpret_cast<const std::uint8_t*>(entry.shadow.data());
            packet.residentVertexBytes.insert(packet.residentVertexBytes.end(),source,source+bytes);
            packet.residentVertexRanges.push_back({{offset,bytes}});
            residentPendingUploads.push_back(slot);
        }
    }
    packet.deferredResolve=deferResolve;packet.indexGather=gatherSerial;
    if(!deferResolve && !residentIndexCache.Active().empty()) {
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
    if(!deferResolve)residentIndexCache.PublishUploads(residentIndexPendingUploads);
    packet.textures.resize(textures.size());
    for(std::size_t index=0;index<textures.size();++index)
        packet.textures[index]={glTextureOf(textures[index]),textures[index].width,textures[index].height,textures[index].minAlpha};
    packet.width=options.width;packet.height=options.height;
    packet.depthTransfers=frame->DepthTransfers();
    std::swap(packet.vertices,data);std::swap(packet.indices,elementIndices);
    std::swap(packet.batches,batches);std::swap(packet.glyphBounds,gatheredGlyphBounds);
    // Gather clears both before refilling them.
    std::swap(packet.residentSpans,gatheredResidentIndexSpans);
    std::swap(packet.residentSpanTokens,gatheredResidentSpanTokens);
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
            // (render.shading_cache in the window log resets these)
            platform::RecordSessionEvent("renderer.vitagl.gather_steps",detail);
            std::snprintf(detail,sizeof(detail),"per_sampled_frame template_ms=%.2f template_acquire_ms=%.2f template_timed=%.0f runs=%.1f run_strips=%.1f run_template_strips=%.1f",
                8*steps[10]/n,8*steps[11]/n,steps[12]/n,steps[13]/n,steps[14]/n,steps[15]/n);
            platform::RecordSessionEvent("renderer.vitagl.gather_templates",detail);
            std::snprintf(detail,sizeof(detail),"per_frame template_hits=%.1f solid_commands=%.1f shadow_commands=%.1f sorted_commands=%.1f",
                nativeGatherCounters[0]/300.0,nativeGatherCounters[1]/300.0,nativeGatherCounters[2]/300.0,nativeGatherCounters[4]/300.0);
            nativeGatherCounters={};
            platform::RecordSessionEvent("renderer.vitagl.gather_solids",detail);
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
            char detail[512];
            std::snprintf(detail,sizeof(detail),
                "claim_wait_ms=%.2f prepare_ms=%.2f gather_ms=%.2f texture_uploads=%u texture_bytes=%llu resident_bytes=%" HP2_ZU " "
                "commands=%" HP2_ZU " batches=%" HP2_ZU " vertices=%" HP2_ZU " last_gl_submit_ms=%.2f last_gl_present_ms=%.2f "
                "validate_ms=%.2f loop_ms=%.2f vjobs_ms=%.2f indices_ms=%.2f resident_new=%u resident_evict=%u resident_capture=%u",
                claimMs,prepareMs,statistics.gatherMilliseconds,static_cast<unsigned>(statistics.textureUploads),
                static_cast<unsigned long long>(statistics.textureUploadBytes),
                packet.residentVertexBytes.size()+packet.residentIndexBytes.size(),commandCount,
                packet.batches.size(),packet.vertices.size(),statistics.recordSubmitMilliseconds,statistics.presentMilliseconds,
                nativeGatherFrameMs[0],nativeGatherFrameMs[1],nativeGatherFrameMs[2],nativeGatherFrameMs[3],
                nativeGatherResidentEvents[0],nativeGatherResidentEvents[1],nativeGatherResidentEvents[2]);
            platform::RecordSessionEvent("renderer.vitagl.slow_prepare",detail);
        }
        if(ecstasyGatherDetail && prepareMs>ecstasyGatherDetailMs) {
            const auto& ms=nativeGatherDetail.gatherCategoryMilliseconds;const auto& n=nativeGatherDetail.gatherCategoryCommands;
            char detail[512];
            std::snprintf(detail,sizeof(detail),
                "prepare_ms=%.2f gather_ms=%.2f validate_ms=%.2f loop_ms=%.2f vjobs_ms=%.2f indices_ms=%.2f "
                "scenery=%.2f/%llu solid=%.2f/%llu particle=%.2f/%llu world=%.2f/%llu ui=%.2f/%llu other=%.2f/%llu "
                "commands=%" HP2_ZU " batches=%" HP2_ZU " resident_new=%u resident_capture=%u texture_uploads=%u",
                prepareMs,statistics.gatherMilliseconds,nativeGatherFrameMs[0],nativeGatherFrameMs[1],nativeGatherFrameMs[2],nativeGatherFrameMs[3],
                ms[0],(unsigned long long)n[0],ms[1],(unsigned long long)n[1],ms[2],(unsigned long long)n[2],
                ms[3],(unsigned long long)n[3],ms[4],(unsigned long long)n[4],ms[5],(unsigned long long)n[5],
                commandCount,packet.batches.size(),nativeGatherResidentEvents[0],nativeGatherResidentEvents[2],
                static_cast<unsigned>(statistics.textureUploads));
            platform::RecordSessionEvent("renderer.vitagl.gather_detail",detail);
            std::array<double,24> d{};
            for(std::size_t at=0;at<d.size();++at)d[at]=8.0*(nativeGatherStepMilliseconds[at]-nativeGatherStepBefore[at]);
            std::snprintf(detail,sizeof(detail),
                "solid: lead=%.2f setup=%.2f preamble=%.2f shading=%.2f acquire=%.2f rest=%.2f append=%.2f other_draws=%.2f "
                "scenery_first: n=%.0f texture_raster=%.2f acquire=%.2f packet=%.2f shading=%.2f rest=%.2f",
                d[8],d[9],d[0],d[1],d[2],d[3],d[4],d[5],d[21]/8.0*8.0,d[16],d[17],d[18],d[19],d[20]);
            platform::RecordSessionEvent("renderer.vitagl.gather_detail_steps",detail);
        }
    }
    glRenderStage=0;
    release.armed=false;
    packet.timelineReady=ecstasyGpuTimeline ? sceKernelGetSystemTimeWide():0;
    packet.timelineResident=nativeGatherResidentEvents;
    HP2_GATHER_PHASE(0u);
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
    ::vitaWatchedThreads[2]=sceKernelGetThreadId();
    {char detail[40];std::snprintf(detail,sizeof(detail),"priority=%d",sceKernelGetThreadCurrentPriority());
        platform::RecordSessionEvent("renderer.vitagl.gl_thread_priority",detail);}
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

// Phase 0 GPU timeline (Vita gpu_timeline.txt, diagnostic only): every
// frame records when the GL thread began/ended its submit and returned from
// the swap, plus the display scene's fence (vitaGL ends every scene with
// query_fence.value++). A watcher thread polls the fence region and stamps
// when the GPU finished each frame, so steady-state GPU throughput and GPU
// starvation (waiting for the submit) can be separated without draining.
int ecstasyGpuTimeline=0;
int ecstasyHalfFirstRun=0;
int ecstasyGpuProbe=0;
// resident_vram.txt: resident geometry in CDRAM (tested 2026-11-21: no GPU gain, -30 MB VRAM).
int ecstasyResidentVram=0;
// gpu_timeline.txt "1 N": screenshot (id 200000000+n) the frame after a GPU
// tail over 38 ms, at most N shots and one per 2 s (what is on screen).
int ecstasyGpuHeavyShots=0;
static bool gpuHeavyShotPending=false;
// Per-frame draw mix for the timeline (alpha-tested, half-res, offscreen).
static std::uint32_t gpuFrameAlphaTested=0,gpuFrameHalfDraws=0,gpuFrameOffscreen=0,gpuFrameIndices=0,gpuFrameOffIndices=0,gpuFrameAtIndices=0;
static std::uint32_t gpuHeavyShotFrame=0;
struct GpuTimelineRecord {
    std::uint64_t submitBegin{},submitEnd{},swapEnd{},uploadEnd{},passBegin{},passEnd{},entry{},claimed{},ready{};
    std::uint32_t fence{},draws{},alphaTested{},halfDraws{},offscreen{},particleMilli{},half{},indices{},offIndices{},atIndices{};
    std::array<std::uint32_t,4> resident{};
    std::atomic<std::uint64_t> done{0};
};
static GpuTimelineRecord gpuTimeline[128];
static std::atomic<std::uint32_t> gpuTimelineHead{0};
static void* GpuTimelineWatcher(void*)
{
    std::uint32_t next=0;
    for(;;) {
        const auto head=gpuTimelineHead.load(std::memory_order_acquire);
        const std::uint32_t observed=*static_cast<volatile std::uint32_t*>(query_fence.address);
        const auto now=sceKernelGetSystemTimeWide();
        while(next!=head) {
            auto& record=gpuTimeline[next%128u];
            if(static_cast<std::int32_t>(observed-record.fence)<0)break;
            record.done.store(now,std::memory_order_release);++next;
        }
        sceKernelDelayThread(200);
    }
    return nullptr;
}

void EcstasyVulkanDevice::Storage::Submit(GlFramePacket& packet)
{
    using Clock=std::chrono::steady_clock;
    const auto timelineBegin=ecstasyGpuTimeline ? sceKernelGetSystemTimeWide():0;
    const auto timelineDrawnBefore=drawnBatches;
    static bool clockCalibrated=false;
    if(!clockCalibrated) {
        // One-time: cost of the clocks used by the engine's timers.
        clockCalibrated=true;
        volatile std::uint64_t sink=0;
        const auto t0=sceKernelGetSystemTimeWide();
        for(int i=0;i<1000;++i)sink+=sceKernelGetProcessTimeLow();
        const auto t1=sceKernelGetSystemTimeWide();
        for(int i=0;i<1000;++i)sink+=sceKernelGetSystemTimeWide();
        const auto t2=sceKernelGetSystemTimeWide();
        for(int i=0;i<1000;++i)sink+=static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto t3=sceKernelGetSystemTimeWide();
        char detail[128];
        std::snprintf(detail,sizeof(detail),"ns_per_call process_time_low=%.0f system_time_wide=%.0f steady_clock=%.0f",
            (t1-t0)*1.0,(t2-t1)*1.0,(t3-t2)*1.0);
        platform::RecordSessionEvent("renderer.clock_cost",detail);
    }
    static bool timelineStarted=false;
    if(ecstasyGpuTimeline && !timelineStarted) {
        timelineStarted=true;
        pthread_t watcher;pthread_attr_t attributes;pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes,16*1024);
        if(pthread_create(&watcher,&attributes,GpuTimelineWatcher,nullptr)==0)pthread_detach(watcher);
        else ecstasyGpuTimeline=0;
        pthread_attr_destroy(&attributes);
        platform::RecordSessionEvent("renderer.gpu_timeline",ecstasyGpuTimeline ? "watcher started":"watcher failed");
    }
    const auto milliseconds=[](Clock::duration duration) {return std::chrono::duration<double,std::milli>(duration).count();};
    // G0 GPU probe: one frame in 16 drains the GPU first, so the finish
    // after its swap measures that frame's GPU work alone. That drain breaks
    // CPU/GPU overlap and cost a missed frame every ~0.5 s under the 30 fps
    // lock (2026-11-24), so it is opt-in (Vita gpu_probe.txt).
    static unsigned gpuProbePhase=0;
    const bool gpuProbeFrame=ecstasyGpuProbe && !ecstasyGpuTimeline && platform::nativeDiagnosticsEnabled && (gpuProbePhase++%16u)==0;
    if(gpuProbeFrame)sceGxmFinish(gxm_context);
    const auto renderBegin=Clock::now();
    struct GpuFrameCounts {unsigned draws,blended,passes,scissorUpdates,alphaTested;double textureBytes;std::vector<GLuint> seen;};
    static GpuFrameCounts gpuCounts{};
    if(gpuProbeFrame) {gpuCounts.draws=gpuCounts.blended=gpuCounts.passes=gpuCounts.scissorUpdates=gpuCounts.alphaTested=0;gpuCounts.textureBytes=0;gpuCounts.seen.clear();}
    if(packet.deferredResolve)ResolveDeferredIndices(packet);
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
        residentVertices.Ensure(GL_ARRAY_BUFFER,packet.residentVertexCapacity,ecstasyResidentVram ? GL_STATIC_DRAW:GL_DYNAMIC_DRAW);
        std::size_t at=0;
        for(const auto& range:packet.residentVertexRanges) {
            std::memcpy(residentVertices.mapped+range[0],packet.residentVertexBytes.data()+at,range[1]);at+=range[1];
        }
    }
    if(packet.residentIndexCapacity) {
        residentIndices.Ensure(GL_ELEMENT_ARRAY_BUFFER,packet.residentIndexCapacity,ecstasyResidentVram ? GL_STATIC_DRAW:GL_DYNAMIC_DRAW);
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
    const auto timelineUploadEnd=ecstasyGpuTimeline ? sceKernelGetSystemTimeWide():0;
    static double preLists=0,preInstance=0,preCopy=0,preRest=0,preBatches=0,preRows=0;
    const bool profPrepass=ecstasyGxmProf!=0;
    const std::uint32_t preStart=profPrepass ? static_cast<std::uint32_t>(sceKernelGetSystemTimeWide()):0u;

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
        if(index+6<batches.size())PrefetchBatch(batches[index+6]);
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
    const std::uint32_t preListsEnd=profPrepass ? static_cast<std::uint32_t>(sceKernelGetSystemTimeWide()):0u;
    for(GLuint attribute=0;attribute<4;++attribute)glEnableVertexAttribArray(attribute);
    // Instance table: rows for every GPU-position draw, runs of identical
    // resident geometry grouped per pass in draw order (particles excluded,
    // so a run never crosses a half-resolution range).
    instanceRow.assign(batches.size(),UINT32_MAX);instanceRun.assign(batches.size(),1u);
    instanceTable.clear();
    // Rows go straight into this frame's mapped instance stream (sized for
    // every batch up front): no staging vector and second copy.
    auto& instanceStream=streamInstances[streamSlot];
    std::uint32_t instanceRows=0;
    if(ecstasyInstancing && !batches.empty())instanceStream.Ensure(GL_ARRAY_BUFFER,batches.size()*20u*sizeof(float));
    if(ecstasyInstancing) {
        const auto sameGeometry=[&](const VkDrawBatch& a,const VkDrawBatch& b) {
            if(a.count!=b.count || a.texture!=b.texture || a.nativeShadingIndex!=b.nativeShadingIndex)return false;
            if(!a.residentGeometry || !b.residentGeometry || !a.indexed || !b.indexed || a.particle || b.particle)return false;
            if(a.nativeIndirectTransform || b.nativeIndirectTransform || !b.nativeGpuPosition)return false;
            if(a.nativeSpanCount!=b.nativeSpanCount || a.residentIndices!=b.residentIndices)return false;
            if(a.alphaTestRedundant!=b.alphaTestRedundant)return false;
            if(!b.SameDrawState(a))return false;
            // The packet's own span copy (never the storage one Gather refills).
            const auto& spans=packet.residentSpans;const auto& tokens=packet.residentSpanTokens;
            if(static_cast<std::size_t>(a.nativeSpanFirst)+a.nativeSpanCount>spans.size() ||
                static_cast<std::size_t>(b.nativeSpanFirst)+b.nativeSpanCount>spans.size() ||
                tokens.size()!=spans.size())return false;
            for(std::uint32_t at=0;at<a.nativeSpanCount;++at)
                if(spans[a.nativeSpanFirst+at]!=spans[b.nativeSpanFirst+at] ||
                    tokens[a.nativeSpanFirst+at]!=tokens[b.nativeSpanFirst+at])return false;
            return true;
        };
        auto* rows=reinterpret_cast<float*>(instanceStream.mapped);
        const auto addRow=[&](const VkDrawBatch& batch) {
            const auto row=instanceRows++;
            float* out=rows+static_cast<std::size_t>(row)*20u;
            std::memcpy(out,batch.nativeLocalToClip.data(),16*sizeof(float));
            std::memcpy(out+16,batch.nativeClipMapping.data(),4*sizeof(float));
            return row;
        };
        for(const auto& list:targetBatches) {
            for(std::size_t at=0;at<list.size();) {
                const auto head=list[at];
                if(at+4<list.size())PrefetchBatch(batches[list[at+4]]);
                const auto& batch=batches[head];
                if(!batch.nativeGpuPosition || batch.nativeIndirectTransform) {++at;continue;}
                instanceRow[head]=addRow(batch);
                std::size_t end=at+1;
                while(end<list.size() && end-at<4096 && sameGeometry(batch,batches[list[end]])) {
                    if(end+4<list.size())PrefetchBatch(batches[list[end+4]]);
                    addRow(batches[list[end]]);instanceRun[list[end]]=0;++end;
                }
                instanceRun[head]=static_cast<std::uint32_t>(end-at);
                if(end-at>1) {instanceDraws+=1.0;instanceMembers+=static_cast<double>(end-at);}
                at=end;
            }
        }
        static unsigned instanceFrames=0;
        if(++instanceFrames==300) {
            char detail[96];std::snprintf(detail,sizeof(detail),"per_frame runs=%.0f draws_merged=%.0f rows=%u",
                instanceDraws/300,(instanceMembers-instanceDraws)/300,instanceRows);
            platform::RecordSessionEvent("renderer.vitagl.instancing",detail);
            instanceFrames=0;instanceDraws=instanceMembers=0;
        }
    }
    const std::uint32_t preInstanceEnd=profPrepass ? static_cast<std::uint32_t>(sceKernelGetSystemTimeWide()):0u;
    if(instanceRows) {
        for(GLuint attribute=4;attribute<9;++attribute) {glEnableVertexAttribArray(attribute);glVertexAttribDivisor(attribute,1);}
    }
    const std::uint32_t preCopyEnd=profPrepass ? static_cast<std::uint32_t>(sceKernelGetSystemTimeWide()):0u;
    if(profPrepass) {
        preLists+=preListsEnd-preStart;preInstance+=preInstanceEnd-preListsEnd;preCopy+=preCopyEnd-preInstanceEnd;
        preBatches+=static_cast<double>(batches.size());preRows+=static_cast<double>(instanceRows);
    }
    GLuint activeVertexBuffer=0,activeIndexBuffer=0,activeTexture=~0u;
    // Shading indices address this frame's table only; direct uniform
    // blocks live in vitaGL's circular pool and are reused within a frame only.
    for(auto& entry:programs) {
        entry.second.shading=UINT32_MAX;entry.second.dShading=UINT32_MAX;
        entry.second.vertexBlock=nullptr;entry.second.fragmentBlock=nullptr;
    }
    if(ecstasyGxmDirect && !linearIndices.name) {
        constexpr std::uint32_t linearCount=60000u;
        linearIndices.Ensure(GL_ELEMENT_ARRAY_BUFFER,linearCount*sizeof(std::uint32_t));
        auto* linear=reinterpret_cast<std::uint32_t*>(linearIndices.mapped);
        for(std::uint32_t at=0;at<linearCount;++at)linear[at]=at;
    }
    {
        static unsigned directFrames=0;
        if(ecstasyGxmDirect && ++directFrames==300) {
            char detail[300];std::snprintf(detail,sizeof(detail),"per_frame direct=%.1f gl=%.1f syncs=%.1f setup_failures=%llu "
                "vprog=%.1f fprog=%.1f vunif=%.1f funif=%.1f tex=%.1f depthtex=%.1f stream0=%.1f stream1=%.1f instanced=%.1f indexed=%.1f",
                directDraws/300.0,directFallbacks/300.0,directScissorSyncs/300.0,static_cast<unsigned long long>(directSetupFailures),
                directCalls[0]/300.0,directCalls[1]/300.0,directCalls[2]/300.0,directCalls[3]/300.0,directCalls[4]/300.0,
                directCalls[5]/300.0,directCalls[6]/300.0,directCalls[7]/300.0,directCalls[8]/300.0,directCalls[9]/300.0);
            platform::RecordSessionEvent("renderer.gxm.direct",detail);
            directFrames=0;directDraws=directFallbacks=directScissorSyncs=0;directCalls={};
        }
    }
    const auto halfEligible=[&](const VkDrawBatch& batch) {
        if(!batch.particle || batch.target!=0 || batch.sampledTarget>0 || batch.showcaseEnvironmentBlend)return false;
        const auto blend=glBlendFor(batch);
        return blend.enable && blend.source==GL_SRC_ALPHA && blend.equation==GL_FUNC_ADD &&
            (blend.destination==GL_ONE_MINUS_SRC_ALPHA || blend.destination==GL_ONE);
    };
    lastFrameHalf=false;
    // Pass profile (with gpu_timeline.txt): where the GL thread's pass time goes.
    static double profBind=0,profHalf=0,profPre=0,profIssue=0,profSync=0,profPass=0,profDraws=0;static unsigned profFrames=0;
    const bool prof=ecstasyGxmProf!=0;
    const auto profNow=[&] {return prof ? static_cast<std::uint32_t>(sceKernelGetSystemTimeWide()):0u;};
    const auto drawPass=[&](std::int32_t id) {
        const auto passStart=profNow();
        glRenderStage=100+id;
        const bool offscreen=id!=0;
        bool halfFrame=false;
        if(!offscreen && ecstasyHalfResParticles && !halfCompositeFailed && packet.particleScreens>=ecstasyHalfResMinScreens)
            for(const auto index:targetBatches[0])if(halfEligible(batches[index])) {halfFrame=true;break;}
        // Screen-filling smoke (user-approved 2026-11-20): quarter resolution.
        halfDivisor=ecstasyQuarterResMinScreens>0.0 && packet.particleScreens>=ecstasyQuarterResMinScreens ? 4u:2u;
        if(halfFrame && halfDivisor==4u)++quarterFrames;
        // One split per frame: give it the run with the most particle
        // indices. A blended non-particle draw (2026-11-24 replay: every
        // frame) often splits a small first run from the bulk of the smoke,
        // which then drew at full resolution. half_firstrun.txt: first run.
        std::size_t halfRunStart=0;
        if(halfFrame && !ecstasyHalfFirstRun) {
            const auto& candidates=targetBatches[0];
            std::uint64_t best=0;
            for(std::size_t scan=0;scan<candidates.size();) {
                if(!halfEligible(batches[candidates[scan]])) {++scan;continue;}
                const auto start=scan;std::uint64_t size=0;
                while(scan<candidates.size() && halfEligible(batches[candidates[scan]]))size+=batches[candidates[scan++]].count;
                if(size>best) {best=size;halfRunStart=start;}
            }
        }
        if(!offscreen) {
            // The display scene must store its depth (sampled by the half pass)
            // and reload it after the composite for the draws that follow.
            // ecstasyHalfDepthPolicy >=1: the first display scene starts with
            // a full depth clear, so it needs no load (as in every frame
            // without the split); the resumed scene loads it (set before
            // EndHalfParticles rebinds the display). >=2: that resumed scene
            // stores nothing either, again as every unsplit frame ends.
            const auto mode=halfFrame && ecstasyHalfDepthPolicy<1 ?
                SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED:SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED;
            sceGxmDepthStencilSurfaceSetForceLoadMode(&gxm_depth_stencil_surface,mode);
            sceGxmDepthStencilSurfaceSetForceStoreMode(&gxm_depth_stencil_surface,halfFrame ?
                SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED:SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
        }
        bool halfMode=false;
        // Diagnostic: GL calls between draws that take over 15 ms.
        const auto slowGl=[&](const char* what,std::uint32_t started) {
            if(!collisionWitnessClockMicros)return;
            const std::uint32_t elapsed=collisionWitnessClockMicros()-started;
            static int logged=0;
            if(elapsed<15000u || logged>=40)return;
            ++logged;
            char detail[128];
            std::snprintf(detail,sizeof(detail),"what=%s ms=%.2f target=%d offscreen=%d half_divisor=%u frame=%llu",
                what,elapsed*0.001,static_cast<int>(id),offscreen ? 1:0,halfDivisor,static_cast<unsigned long long>(completedFrameSerial));
            platform::RecordSessionEvent("renderer.vitagl.slow_gl",detail);
        };
        const std::uint32_t bindStarted=collisionWitnessClockMicros ? collisionWitnessClockMicros():0u;
        GLint height=static_cast<GLint>(packet.height);
        EnterGl();
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
        // vitaGL implements the scissor with stencil-mask quads drawn over
        // the whole target on every scissor rectangle/enable change. A
        // rectangle covering the whole target clips nothing, so it is drawn
        // with the test off (exact); real rectangles keep the test.
        int scissorOn=-1;std::array<GLint,4> scissorRect{{-1,-1,-1,-1}};
        bool scissorPending=true; // vitaGL applies GL scissor changes lazily (scene_reset)
        const GLint passWidth=offscreen ? static_cast<GLint>(glTargets[static_cast<std::size_t>(id)].width):static_cast<GLint>(packet.width);
        const auto applyScissor=[&](GLint x,GLint y,GLint w,GLint h,GLint fullWidth,GLint fullHeight) {
            if(!ecstasyScissorElide) {
                if(scissorOn!=1) {glEnable(GL_SCISSOR_TEST);scissorOn=1;scissorPending=true;}
                const std::array<GLint,4> rect{{x,y,w,h}};
                if(rect!=scissorRect) {glScissor(x,y,w,h);scissorRect=rect;scissorPending=true;if(gpuProbeFrame)++gpuCounts.scissorUpdates;}
                return;
            }
            const bool full=x<=0 && y<=0 && x+w>=fullWidth && y+h>=fullHeight;
            if(full) {
                if(scissorOn!=0) {glDisable(GL_SCISSOR_TEST);scissorOn=0;scissorPending=true;if(gpuProbeFrame)++gpuCounts.scissorUpdates;}
                return;
            }
            const std::array<GLint,4> rect{{x,y,w,h}};
            const bool changed=rect!=scissorRect || scissorOn!=1;
            if(changed)scissorPending=true;
            if(rect!=scissorRect) {glScissor(x,y,w,h);scissorRect=rect;}
            if(scissorOn!=1) {glEnable(GL_SCISSOR_TEST);scissorOn=1;}
            if(changed && gpuProbeFrame)++gpuCounts.scissorUpdates;
        };
        slowGl("bind_clear",bindStarted);
        if(prof)profBind+=static_cast<std::uint32_t>(sceKernelGetSystemTimeWide())-passStart;
        const float yflip=offscreen ? 1.0f:-1.0f;
        const GlProgram* activeProgram{};
        const VkDrawBatch* previous{};
        GlBlendState activeBlend;bool blendKnown=false;
        int activeDepth=-1,activeDepthWrite=-1;
        activeTexture=~0u;
        // Direct GXM bindings issued since the last vitaGL activity.
        const GlProgram* dProgram{};const SceGxmFragmentProgram* dFragment{};
        const SceGxmTexture* dTexture{};const SceGxmTexture* dDepthTexture{};
        const void* dStream0{};const void* dStream1{};
        int dTextureUnit=-1,dDepthUnit=-1;
        // Consecutive draws mostly share a key and a blend state.
        std::uint32_t lastKey=UINT32_MAX;GlProgram* lastProgram{};
        const GlProgram* lastFragmentProgram{};GlBlendState lastFragmentBlend;bool lastFragmentHalf{};
        SceGxmFragmentProgram* lastFragment{};
        const auto directReset=[&] {dProgram=nullptr;dFragment=nullptr;dTexture=dDepthTexture=nullptr;dStream0=dStream1=nullptr;dTextureUnit=dDepthUnit=-1;};
        int sinceFlush=0;
        const auto drawOneBody=[&](std::uint32_t batchIndex) {
            const auto drawStart=profNow();
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
            const bool checkElided=ecstasyAlphaCheck && (batch.alphaTestRedundant || ecstasyAlphaCheck==2) && batch.alphaTest==2 && !redundantTest;
            const bool noTest=redundantTest || (batch.alphaTestRedundant && !ecstasyAlphaCheck) || ecstasyGpuDiag==5 || ecstasyGpuDiag==7;
            key|=(static_cast<std::uint32_t>(checkElided ? 3:noTest ? 0:std::clamp(batch.alphaTest,0,2)))<<3;
            if(gpuProbeFrame && batch.alphaTest && !noTest)++gpuCounts.alphaTested;
            if(ecstasyGpuTimeline) {
                const std::uint32_t members=std::max<std::uint32_t>(instanceRun[batchIndex],1u);
                if(batch.alphaTest && !noTest)gpuFrameAlphaTested+=members;
                if(halfMode)gpuFrameHalfDraws+=members;
                if(batch.target!=0)gpuFrameOffscreen+=members;
                gpuFrameIndices+=batch.count*members;
                if(batch.target!=0)gpuFrameOffIndices+=batch.count*members;
                if(batch.alphaTest && !noTest)gpuFrameAtIndices+=batch.count*members;
            }
            key|=(batch.showcaseEnvironmentBlend ? 1u:0u)<<5;
            key|=(blend.fixedAlpha ? 1u:0u)<<6;
            key|=(batch.clampTopLeft && texturing ? 1u:0u)<<7;
            key|=(batch.nativeGpuPosition ? 1u:0u)<<8;
            key|=(shade ? 1u:0u)<<9;
            key|=(batch.affineUV && texturing && ecstasyAffineUV ? 1u:0u)<<10;
            key|=(halfMode && !halfHardwareDepth ? 1u:0u)<<11;
            const bool instanced=batch.nativeGpuPosition && instanceRow[batchIndex]!=UINT32_MAX;
            key|=(instanced ? 1u:0u)<<12;
            if(key!=lastKey) {lastProgram=&Program(key);lastKey=key;}
            auto& program=*lastProgram;
            if(!program.program) {++skipped;return;}
            const float drawFlip=halfMode ? 1.0f:yflip;
            // Direct GXM draw: same programs, state and buffers as the GL
            // calls below would hand vitaGL, issued without vitaGL's per-draw
            // work. Eager GL state (viewport, depth, blend/scissor values)
            // is still set through GL so both paths see one state.
            SceGxmFragmentProgram* directFragment{};
            if(ecstasyGxmDirect && !batch.wireframe && !ecstasyGpuDiag && GxmSetup(program,key,instanced) &&
                (texturing || program.textureUnit<0) && (halfMode || program.depthUnit<0)) {
                if(&program==lastFragmentProgram && blend==lastFragmentBlend && halfMode==lastFragmentHalf)directFragment=lastFragment;
                else {
                    directFragment=GxmFragment(program,GxmBlendFor(blend,halfMode));
                    lastFragmentProgram=&program;lastFragmentBlend=blend;lastFragmentHalf=halfMode;lastFragment=directFragment;
                }
            }
            const bool direct=directFragment!=nullptr;
            if(direct) {
                if(program.uYflip>=0 && (!program.dFlipValid || program.dFlip!=drawFlip)) {
                    program.vertexUniforms[static_cast<std::size_t>(program.uYflip)]=drawFlip;
                    program.dFlip=drawFlip;program.dFlipValid=true;program.vertexDirty=true;
                }
                if(halfMode && program.uDepthmap>=0 && (!program.dDepthmapValid || program.dDepthmapDivisor!=halfDivisor)) {
                    program.dDepthmapDivisor=halfDivisor;
                    const float w=static_cast<float>(packet.width/halfDivisor),h=static_cast<float>(packet.height/halfDivisor);
                    float* map=program.fragmentUniforms.data()+program.uDepthmap;
                    if(ecstasyHalfResDepthFlip) {map[0]=1.0f/w;map[1]=-1.0f/h;map[2]=0.0f;map[3]=1.0f;}
                    else {map[0]=1.0f/w;map[1]=1.0f/h;map[2]=0.0f;map[3]=0.0f;}
                    program.dDepthmapValid=true;program.fragmentDirty=true;
                }
            } else {
                EnterGl();++directFallbacks;
            }
            if(!direct && &program!=activeProgram) {
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
            if(!direct && vertexBuffer!=activeVertexBuffer) {
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
            if(halfMode) {
                const auto& scissor=batch.scissor;
                const GLint d=static_cast<GLint>(halfDivisor);
                const GLint x0=scissor.offset.x/d,y0=scissor.offset.y/d;
                const GLint x1=(scissor.offset.x+static_cast<GLint>(scissor.extent.width)+d-1)/d;
                const GLint y1=(scissor.offset.y+static_cast<GLint>(scissor.extent.height)+d-1)/d;
                applyScissor(x0,y0,x1-x0,y1-y0,static_cast<GLint>(packet.width)/d,static_cast<GLint>(packet.height)/d);
            } else {
                const auto& scissor=batch.scissor;
                applyScissor(scissor.offset.x,offscreen ? scissor.offset.y:
                    height-scissor.offset.y-static_cast<GLint>(scissor.extent.height),
                    static_cast<GLint>(scissor.extent.width),static_cast<GLint>(scissor.extent.height),passWidth,height);
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
            GLuint directTextureName{};
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
                if((ecstasyGpuDiag==1||ecstasyGpuDiag==7) && !sampled) {
                    if(!gpuDiagWhite) {
                        static const std::uint8_t white[4]={255,255,255,128};
                        glGenTextures(1,&gpuDiagWhite);glBindTexture(GL_TEXTURE_2D,gpuDiagWhite);
                        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,white);
                        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                        activeTexture=~0u;
                    }
                    name=gpuDiagWhite;
                }
                if(gpuProbeFrame && std::find(gpuCounts.seen.begin(),gpuCounts.seen.end(),name)==gpuCounts.seen.end()) {
                    gpuCounts.seen.push_back(name);gpuCounts.textureBytes+=4.0*texWidth*texHeight;
                }
                if(direct)directTextureName=name;
                else if(name!=activeTexture) {glBindTexture(GL_TEXTURE_2D,name);activeTexture=name;}
            }
            if(gpuProbeFrame) {++gpuCounts.draws;if(blend.enable)++gpuCounts.blended;}
            if(ecstasyGpuDiag==3) {glEnable(GL_SCISSOR_TEST);glScissor(0,0,8,8);scissorOn=-1;scissorRect={{-1,-1,-1,-1}};}
            if(direct ? program.uFparam>=0:program.fparam>=0) {
                const float texelX=texWidth ? 1.0f/static_cast<float>(texWidth):0.0f;
                const float texelY=texHeight ? 1.0f/static_cast<float>(texHeight):0.0f;
                const std::array<float,4> parameters{{batch.blendConstant,texelX,texelY,0.0f}};
                if(direct) {
                    if(!program.dParametersValid || parameters!=program.dParameters) {
                        std::memcpy(program.fragmentUniforms.data()+program.uFparam,parameters.data(),sizeof(parameters));
                        program.dParameters=parameters;program.dParametersValid=true;program.fragmentDirty=true;
                    }
                } else if(!program.parametersValid || parameters!=program.parameters) {
                    glUniform4fv(program.fparam,1,parameters.data());
                    program.parameters=parameters;program.parametersValid=true;
                }
            }
            GLsizei instances=1;
            if(direct) {
                if(instanced)instances=static_cast<GLsizei>(instanceRun[batchIndex]);
                else if(batch.nativeGpuPosition) {
                    std::array<float,20> transform;
                    std::copy_n(batch.nativeLocalToClip.begin(),16,transform.begin());
                    std::copy_n(batch.nativeClipMapping.begin(),4,transform.begin()+16);
                    if(!program.dTransformValid || std::memcmp(transform.data(),program.dTransform.data(),sizeof(transform))) {
                        float* u=program.vertexUniforms.data();
                        const int offsets[5]={program.uM0,program.uM1,program.uM2,program.uM3,program.uClipmap};
                        for(int row=0;row<5;++row)if(offsets[row]>=0)std::memcpy(u+offsets[row],transform.data()+row*4,4*sizeof(float));
                        program.dTransform=transform;program.dTransformValid=true;program.vertexDirty=true;
                    }
                }
            } else if(instanced) {
                instances=static_cast<GLsizei>(instanceRun[batchIndex]);
                glBindBuffer(GL_ARRAY_BUFFER,instanceStream.name);activeVertexBuffer=0;
                const auto base=static_cast<std::uintptr_t>(instanceRow[batchIndex])*20u*sizeof(float);
                for(GLuint row=0;row<5;++row)
                    glVertexAttribPointer(4+row,4,GL_FLOAT,GL_FALSE,20*sizeof(float),reinterpret_cast<const void*>(base+row*4u*sizeof(float)));
            } else if(batch.nativeGpuPosition) {
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
            if(direct && shade && batch.nativeShadingIndex!=program.dShading) {
                const auto first=batch.nativeShadingIndex;
                Require(first+19u<=gatheredGlyphBounds.size(),"vitaGL shading record exceeds the frame table");
                const auto& record=gatheredGlyphBounds;
                std::uint32_t flags{},mode{},material{};
                std::memcpy(&flags,&record[first][0],4);std::memcpy(&mode,&record[first][1],4);
                std::memcpy(&material,&record[first][2],4);
                float* u=program.vertexUniforms.data();
                if(program.uShade>=0)std::memcpy(u+program.uShade,record[first].data(),19*4*sizeof(float));
                if(program.uMat>=0) {
                    if(material && material+4u<=gatheredGlyphBounds.size())
                        for(int row=0;row<4;++row)std::memcpy(u+program.uMat+row*4,record[material+row].data(),4*sizeof(float));
                    else std::fill_n(u+program.uMat,16,0.0f);
                }
                if(program.uFlags>=0) {
                    u[program.uFlags]=(flags&128u) ? 1.0f:0.0f;u[program.uFlags+1]=(flags&64u) ? 1.0f:0.0f;
                    u[program.uFlags+2]=(flags&4u) ? 1.0f:0.0f;u[program.uFlags+3]=(mode&1u) ? 1.0f:0.0f;
                }
                if(program.uFlags2>=0) {
                    u[program.uFlags2]=(mode&2u) ? 1.0f:0.0f;u[program.uFlags2+1]=0.0f;u[program.uFlags2+2]=0.0f;u[program.uFlags2+3]=0.0f;
                }
                program.dShading=first;program.vertexDirty=true;
            }
            if(!direct && shade && batch.nativeShadingIndex!=program.shading) {
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
            if(ecstasyGpuDiag==6) {++skipped;previous=&batch;return;}
            if(direct) {
                const auto issueStart=profNow();
                if(prof) {profPre+=issueStart-drawStart;profDraws+=1;}
                void* indexBase{};
                if(batch.indexed) {
                    const auto indexBuffer=batch.residentIndices ? residentIndices.name:indexStream.name;
                    if(!indexBuffer) {++skipped;previous=&batch;return;}
                    indexBase=vglHp2UseBuffer(indexBuffer);
                } else indexBase=vglHp2UseBuffer(linearIndices.name);
                auto* vertexBase=static_cast<std::uint8_t*>(vglHp2UseBuffer(vertexBuffer));
                if(!indexBase || !vertexBase) {++skipped;previous=&batch;return;}
                // vitaGL activity since the last direct draw: let it begin the
                // scene and draw its scissor mask first, then rebind everything.
                if(glTouchedSinceDirect || scissorPending) {
                    const auto syncStart=profNow();
                    if(gxmDirectTouched) {vglHp2Invalidate();gxmDirectTouched=false;}
                    vglHp2Sync();
                    directReset();glTouchedSinceDirect=false;scissorPending=false;++directScissorSyncs;
                    if(prof)profSync+=static_cast<std::uint32_t>(sceKernelGetSystemTimeWide())-syncStart;
                }
                gxmDirectTouched=true;
                const bool programChanged=&program!=dProgram;
                if(programChanged) {sceGxmSetVertexProgram(gxm_context,program.vertexProgram);dProgram=&program;++directCalls[0];}
                if(directFragment!=dFragment) {sceGxmSetFragmentProgram(gxm_context,directFragment);dFragment=directFragment;++directCalls[1];}
                if(!program.vertexUniforms.empty() && (program.vertexDirty || !program.vertexBlock || programChanged)) {
                    if(program.vertexDirty || !program.vertexBlock) {
                        program.vertexBlock=vglHp2ReserveUniforms(static_cast<std::uint32_t>(program.vertexUniforms.size()*sizeof(float)));
                        std::memcpy(program.vertexBlock,program.vertexUniforms.data(),program.vertexUniforms.size()*sizeof(float));
                        program.vertexDirty=false;
                    }
                    sceGxmSetVertexDefaultUniformBuffer(gxm_context,program.vertexBlock);++directCalls[2];
                }
                if(!program.fragmentUniforms.empty() && (program.fragmentDirty || !program.fragmentBlock || programChanged)) {
                    if(program.fragmentDirty || !program.fragmentBlock) {
                        program.fragmentBlock=vglHp2ReserveUniforms(static_cast<std::uint32_t>(program.fragmentUniforms.size()*sizeof(float)));
                        std::memcpy(program.fragmentBlock,program.fragmentUniforms.data(),program.fragmentUniforms.size()*sizeof(float));
                        program.fragmentDirty=false;
                    }
                    sceGxmSetFragmentDefaultUniformBuffer(gxm_context,program.fragmentBlock);++directCalls[3];
                }
                if(program.textureUnit>=0) {
                    const auto* texture=vglHp2UseTexture(directTextureName);
                    if(texture && (texture!=dTexture || program.textureUnit!=dTextureUnit)) {
                        sceGxmSetFragmentTexture(gxm_context,static_cast<unsigned>(program.textureUnit),texture);
                        dTexture=texture;dTextureUnit=program.textureUnit;++directCalls[4];
                    }
                }
                if(program.depthUnit>=0) {
                    const auto* texture=vglHp2UseTexture(halfDepthTexture);
                    if(texture && (texture!=dDepthTexture || program.depthUnit!=dDepthUnit)) {
                        sceGxmSetFragmentTexture(gxm_context,static_cast<unsigned>(program.depthUnit),texture);
                        dDepthTexture=texture;dDepthUnit=program.depthUnit;++directCalls[5];
                    }
                }
                const void* instanceBase{};
                if(instanced) {
                    auto* rows=static_cast<std::uint8_t*>(vglHp2UseBuffer(instanceStream.name));
                    if(!rows) {++skipped;previous=&batch;return;}
                    instanceBase=rows+static_cast<std::size_t>(instanceRow[batchIndex])*80u;
                    if(instanceBase!=dStream1) {sceGxmSetVertexStream(gxm_context,1,instanceBase);dStream1=instanceBase;++directCalls[7];}
                    ++directCalls[8];
                }
                if(ecstasyDiagNoDraw==1 || (ecstasyDiagNoDraw==2 && batch.particle)) {}
                else if(batch.indexed) {
                    if(vertexBase!=dStream0) {sceGxmSetVertexStream(gxm_context,0,vertexBase);dStream0=vertexBase;++directCalls[6];}
                    ++directCalls[9];
                    const auto* indices=static_cast<const std::uint8_t*>(indexBase)+static_cast<std::size_t>(batch.first)*sizeof(std::uint32_t);
                    if(instanced)sceGxmDrawInstanced(gxm_context,SCE_GXM_PRIMITIVE_TRIANGLES,SCE_GXM_INDEX_FORMAT_U32,indices,
                        batch.count*static_cast<std::uint32_t>(instances),batch.count);
                    else sceGxmDraw(gxm_context,SCE_GXM_PRIMITIVE_TRIANGLES,SCE_GXM_INDEX_FORMAT_U32,indices,batch.count);
                } else {
                    constexpr std::uint32_t chunk=60000u;
                    for(std::uint32_t done=0;done<batch.count;done+=chunk) {
                        const auto n=std::min(chunk,batch.count-done);
                        const void* stream=vertexBase+static_cast<std::size_t>(batch.first+done)*sizeof(VkVertex);
                        if(stream!=dStream0) {sceGxmSetVertexStream(gxm_context,0,stream);dStream0=stream;++directCalls[6];}
                        if(instanced)sceGxmDrawInstanced(gxm_context,SCE_GXM_PRIMITIVE_TRIANGLES,SCE_GXM_INDEX_FORMAT_U32,indexBase,n,n);
                        else sceGxmDraw(gxm_context,SCE_GXM_PRIMITIVE_TRIANGLES,SCE_GXM_INDEX_FORMAT_U32,indexBase,n);
                    }
                }
                ++directDraws;++drawn;previous=&batch;
                if(ecstasyGxmFlushDraws>0 && ++sinceFlush>=ecstasyGxmFlushDraws) {
                    sinceFlush=0;
                    const int result=sceGxmMidSceneFlush(gxm_context,0,nullptr,nullptr);
                    static int flushErrors=0;
                    if(result<0 && flushErrors<4) {
                        ++flushErrors;char detail[48];std::snprintf(detail,sizeof(detail),"error=0x%08x",static_cast<unsigned>(result));
                        platform::RecordSessionEvent("renderer.gxm.midscene_flush",detail);
                    }
                }
                if(prof)profIssue+=static_cast<std::uint32_t>(sceKernelGetSystemTimeWide())-issueStart;
                return;
            }
            if(batch.indexed) {
                const auto indexBuffer=batch.residentIndices ? residentIndices.name:indexStream.name;
                if(!indexBuffer) {++skipped;previous=&batch;return;}
                if(indexBuffer!=activeIndexBuffer) {glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,indexBuffer);activeIndexBuffer=indexBuffer;}
                if(instanced)glDrawElementsInstanced(topology,static_cast<GLsizei>(batch.count),GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<std::uintptr_t>(batch.first)*sizeof(std::uint32_t)),instances);
                else glDrawElements(topology,static_cast<GLsizei>(batch.count),GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<std::uintptr_t>(batch.first)*sizeof(std::uint32_t)));
            } else if(instanced) {
                constexpr std::uint32_t chunk=60000u;
                for(std::uint32_t done=0;done<batch.count;done+=chunk)
                    glDrawArraysInstanced(topology,static_cast<GLint>(batch.first+done),
                        static_cast<GLsizei>(std::min(chunk,batch.count-done)),1);
            } else {
                // vitaGL's linear index table is 16-bit: split very long arrays.
                constexpr std::uint32_t chunk=60000u;
                for(std::uint32_t done=0;done<batch.count;done+=chunk)
                    glDrawArrays(topology,static_cast<GLint>(batch.first+done),
                        static_cast<GLsizei>(std::min(chunk,batch.count-done)));
            }
            ++drawn;previous=&batch;
        };
        // Slow-draw probe (cheap clock only): a single draw over 15 ms is
        // first-use work inside vitaGL; log its state (first 40 per run).
        // (Direct GXM: no per-draw probe; two clock syscalls per draw cost
        // more than the first-use work it looks for, which is vitaGL's.)
        const auto drawOne=[&](std::uint32_t batchIndex) {
            if(!collisionWitnessClockMicros || ecstasyGxmDirect) {drawOneBody(batchIndex);return;}
            const std::uint32_t started=collisionWitnessClockMicros();
            drawOneBody(batchIndex);
            const std::uint32_t elapsed=collisionWitnessClockMicros()-started;
            static int logged=0;
            if(elapsed<15000u || logged>=40)return;
            ++logged;
            const auto& batch=batches[batchIndex];
            const auto blend=glBlendFor(batch);
            std::uint32_t tw=0,th=0;int fmt=-1;
            if(batch.sampledTarget<=0 && !batch.untextured && batch.texture<packet.textures.size()) {
                tw=packet.textures[batch.texture].width;th=packet.textures[batch.texture].height;fmt=(int)batch.texture;
            }
            char detail[320];
            std::snprintf(detail,sizeof(detail),"ms=%.2f half=%d offscreen=%d sampled=%d texture=%d tex=%ux%u untextured=%d tfunc=%d alpha_test=%d "
                "blend=%d src=%04x dst=%04x eq=%04x fixed=%d depth=%d depth_write=%d gpu_pos=%d shade=%d affine=%d indexed=%d resident=%d count=%u",
                elapsed*0.001,halfMode ? 1:0,offscreen ? 1:0,(int)batch.sampledTarget,fmt,tw,th,batch.untextured ? 1:0,(int)batch.textureFunction,
                (int)batch.alphaTest,blend.enable ? 1:0,(unsigned)blend.source,(unsigned)blend.destination,(unsigned)blend.equation,blend.fixedAlpha ? 1:0,
                batch.depth ? 1:0,batch.depthWrite ? 1:0,batch.nativeGpuPosition ? 1:0,batch.nativeShadingIndex!=0 ? 1:0,batch.affineUV ? 1:0,
                batch.indexed ? 1:0,batch.residentGeometry ? 1:0,(unsigned)batch.count);
            platform::RecordSessionEvent("renderer.vitagl.slow_draw",detail);
        };
        const auto resetState=[&] {
            activeProgram=nullptr;previous=nullptr;blendKnown=false;activeDepth=activeDepthWrite=-1;
            activeTexture=~0u;activeVertexBuffer=0;
            scissorOn=-1;scissorRect={{-1,-1,-1,-1}}; // half begin/end leave the test on
        };
        const auto& list=targetBatches[static_cast<std::size_t>(id)];
        for(std::size_t at=0;at<list.size();) {
            if(halfFrame && at>=halfRunStart && halfEligible(batches[list[at]])) {
                halfFrame=false; // later runs this frame draw at full resolution
                std::size_t end=at;
                while(end<list.size() && halfEligible(batches[list[end]])) {halfRunIndices+=batches[list[end]].count;++end;}
                if(end<list.size()) {
                    // What ended the run: 0 particle (other blend), 1 non-particle blended, 2 opaque, 3 other.
                    const auto& breaker=batches[list[end]];
                    const auto blend=glBlendFor(breaker);
                    ++halfBreaks[breaker.particle ? 0:blend.enable ? 1:breaker.target==0 ? 2:3];
                }
                if(!lastFrameHalf) {lastFrameHalf=true;++halfFramesTotal;}
                std::uint32_t halfStarted=collisionWitnessClockMicros ? collisionWitnessClockMicros():0u;
                EnterGl();
                auto halfProf=profNow();
                BeginHalfParticles(packet.width,packet.height);
                if(prof)profHalf+=static_cast<std::uint32_t>(sceKernelGetSystemTimeWide())-halfProf;
                slowGl("half_begin",halfStarted);
                halfMode=true;resetState();
                // gpu_diag 8: keep the split, draw nothing in it (split cost).
                if(ecstasyGpuDiag==8)at=end;
                // Composite bounds (half_bounds_off.txt): the half target is
                // cleared to transparent black and composited with ONE /
                // ONE_MINUS_SRC_ALPHA, so a display pixel whose bilinear
                // footprint holds only cleared texels is left exactly as it
                // was. The window rectangle of this run's particle vertices,
                // widened by 3 half texels and mirrored vertically (either
                // window orientation), bounds every other pixel.
                halfBounds={{0.0f,0.0f,-1.0f,-1.0f}};
                if(ecstasyHalfCompositeBounds) {
                    const float W=static_cast<float>(packet.width),H=static_cast<float>(packet.height);
                    float x0=W,x1=0.0f,y0=H,y1=0.0f;bool valid=true,any=false;
                    for(auto scan=at;valid && scan<end;++scan) {
                        const auto& batch=batches[list[scan]];
                        const auto& vp=batch.viewport;
                        if(batch.residentGeometry || !batch.indexed || vp.x!=0.0f || vp.y!=0.0f || vp.width!=W || vp.height!=H ||
                            static_cast<std::size_t>(batch.first)+batch.count>packet.indices.size()) {valid=false;break;}
                        for(std::uint32_t index=0;index<batch.count;++index) {
                            const auto vertex=packet.indices[batch.first+index];
                            if(vertex>=packet.vertices.size()) {valid=false;break;}
                            const auto& p=packet.vertices[vertex].position;
                            if(!(p[3]>0.0f) || !std::isfinite(p[0]) || !std::isfinite(p[1])) {valid=false;break;}
                            const float x=(p[0]/p[3]+1.0f)*0.5f*W,y=(p[1]/p[3]+1.0f)*0.5f*H;
                            x0=std::min(x0,x);x1=std::max(x1,x);
                            y0=std::min({y0,y,H-y});y1=std::max({y1,y,H-y});any=true;
                        }
                    }
                    if(valid) {
                        const float margin=3.0f*static_cast<float>(halfDivisor);
                        if(!any)halfBounds={{0.0f,0.0f,0.0f,0.0f}}; // nothing drawn: no composite
                        else halfBounds={{std::max(0.0f,std::floor(x0-margin)),std::max(0.0f,std::floor(y0-margin)),
                            std::min(W,std::ceil(x1+margin)),std::min(H,std::ceil(y1+margin))}};
                    }
                }
                for(;at<end;++at)drawOne(list[at]);
                halfMode=false;
                halfStarted=collisionWitnessClockMicros ? collisionWitnessClockMicros():0u;
                EnterGl();
                if(ecstasyHalfDepthPolicy>=1) {
                    // The resumed display scene (begun by EndHalfParticles).
                    sceGxmDepthStencilSurfaceSetForceLoadMode(&gxm_depth_stencil_surface,SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
                    if(ecstasyHalfDepthPolicy>=2)
                        sceGxmDepthStencilSurfaceSetForceStoreMode(&gxm_depth_stencil_surface,SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
                }
                halfProf=profNow();
                EndHalfParticles(packet.width,packet.height);
                if(prof)profHalf+=static_cast<std::uint32_t>(sceKernelGetSystemTimeWide())-halfProf;
                slowGl("half_end",halfStarted);
                resetState();
                continue;
            }
            if(ecstasyGpuDiag==4 && halfEligible(batches[list[at]])) {++at;continue;}
            if(!offscreen && halfEligible(batches[list[at]])) {++halfMissedBatches;halfMissedIndices+=batches[list[at]].count;}
            const auto head=list[at];
            for(std::size_t ahead=at+1;ahead<list.size() && ahead<=at+8;++ahead)
                if(instanceRun[list[ahead]]) {PrefetchBatch(batches[list[ahead]]);break;}
            drawOne(head);
            at+=std::max<std::uint32_t>(instanceRun[head],1u);
        }
        if(!offscreen && !halfPrewarmed && ecstasyHalfResParticles && !halfCompositeFailed) {
            // (Also compiles the sun occlusion query program, see EnsureSunQueryProgram.)
            // One-time setup of the half and quarter passes (programs, quad,
            // targets) costs ~340 ms on first use; pay it on the first
            // displayed frame instead of mid-race. Each target is cleared to
            // transparent black and composited with ONE/ONE_MINUS_SRC_ALPHA,
            // which leaves the display unchanged.
            halfPrewarmed=true;
            const std::uint32_t prewarmStarted=collisionWitnessClockMicros ? collisionWitnessClockMicros():0u;
            const auto savedDivisor=halfDivisor;
            EnterGl();
            for(const std::uint32_t divisor:{2u,4u}) {
                halfDivisor=divisor;
                BeginHalfParticles(packet.width,packet.height);
                EndHalfParticles(packet.width,packet.height);
            }
            halfDivisor=savedDivisor;resetState();
            (void)EnsureSunQueryProgram();
            if(collisionWitnessClockMicros) {
                char detail[64];std::snprintf(detail,sizeof(detail),"ms=%.2f",(collisionWitnessClockMicros()-prewarmStarted)*0.001);
                platform::RecordSessionEvent("renderer.vitagl.halfres_prewarm",detail);
            }
        }
        EnterGl();
        glDisable(GL_SCISSOR_TEST);
        if(prof)profPass+=static_cast<std::uint32_t>(sceKernelGetSystemTimeWide())-passStart;
    };
    std::array<std::uint8_t,kTargets> visited{};
    const auto visit=[&](const auto& self,std::int32_t id)->void {
        auto& state=visited[static_cast<std::size_t>(id)];
        if(state)return;
        state=1;
        for(const auto dependency:targetDependencies[static_cast<std::size_t>(id)])self(self,dependency);
        if(id==0 || !targetBatches[static_cast<std::size_t>(id)].empty()) {
            const std::uint32_t passStarted=collisionWitnessClockMicros ? collisionWitnessClockMicros():0u;
            if(gpuProbeFrame)++gpuCounts.passes;
            drawPass(id);
            if(collisionWitnessClockMicros) {
                const std::uint32_t elapsed=collisionWitnessClockMicros()-passStarted;
                static int logged=0;
                if(elapsed>=40000u && logged<40) {
                    ++logged;char detail[96];
                    std::snprintf(detail,sizeof(detail),"target=%d ms=%.2f batches=%u frame=%llu",static_cast<int>(id),elapsed*0.001,
                        static_cast<unsigned>(targetBatches[static_cast<std::size_t>(id)].size()),static_cast<unsigned long long>(completedFrameSerial));
                    platform::RecordSessionEvent("renderer.vitagl.slow_pass",detail);
                }
            }
        }
        state=2;
    };
    if(profPrepass)preRest+=static_cast<std::uint32_t>(sceKernelGetSystemTimeWide())-preCopyEnd;
    const auto timelinePassBegin=ecstasyGpuTimeline ? sceKernelGetSystemTimeWide():0;
    if(ecstasyGxmParity && ecstasyGxmDirect && nextScreenshot<ecstasyScreenshotFrames.size() &&
        journalFrames+1>=ecstasyScreenshotFrames[nextScreenshot]) {
        // Parity check: the same packet through vitaGL first, then direct.
        ecstasyGxmDirect=0;
        visit(visit,0);
        SaveScreenshot(packet.width,packet.height,journalFrames+1+100000000ull);
        visited={};
        ecstasyGxmDirect=1;
    }
    visit(visit,0);
    const auto timelinePassEnd=ecstasyGpuTimeline ? sceKernelGetSystemTimeWide():0;
    if(prof && ++profFrames==300) {
        char detail[200];
        std::snprintf(detail,sizeof(detail),"per_frame_ms pass=%.2f bind_clear=%.2f half=%.2f draw_pre=%.2f draw_issue=%.2f (sync=%.2f) draws=%.0f",
            profPass/3e5,profBind/3e5,profHalf/3e5,profPre/3e5,profIssue/3e5,profSync/3e5,profDraws/300);
        platform::RecordSessionEvent("renderer.gxm.pass_prof",detail);
        std::snprintf(detail,sizeof(detail),"per_frame_ms lists=%.2f instance=%.2f instance_copy=%.2f rest=%.2f batches=%.0f rows=%.0f",
            preLists/3e5,preInstance/3e5,preCopy/3e5,preRest/3e5,preBatches/300,preRows/300);
        platform::RecordSessionEvent("renderer.gxm.pre_prof",detail);
        preLists=preInstance=preCopy=preRest=preBatches=preRows=0;
        profBind=profHalf=profPre=profIssue=profSync=profPass=profDraws=0;profFrames=0;
    }
    PublishSunQueries();
    if(!packet.depthTransfers.empty())IssueSunQueries(packet);
    // Offscreen targets nobody samples this frame (env-map faces) are not drawn.
    for(std::size_t id=1;id<kTargets;++id)if(!visited[id])skipped+=targetBatches[id].size();
    glDisable(GL_SCISSOR_TEST);
    drawnBatches+=drawn;skippedBatches+=skipped;
    // Textures retired by this frame's gather: no later packet refers to them.
    const std::uint32_t deletesStarted=collisionWitnessClockMicros ? collisionWitnessClockMicros():0u;
    const auto deleteCount=packet.deletes.size();
    for(auto* texture:packet.deletes) {
        if(texture->name) {glDeleteTextures(1,&texture->name);glLiveTextureBytes-=texture->bytes;--glLiveTextureCount;}
        delete texture;
    }
    packet.deletes.clear();
    if(collisionWitnessClockMicros && deleteCount) {
        const std::uint32_t elapsed=collisionWitnessClockMicros()-deletesStarted;
        if(elapsed>=15000u) {
            char detail[64];std::snprintf(detail,sizeof(detail),"ms=%.2f count=%u",elapsed*0.001,static_cast<unsigned>(deleteCount));
            platform::RecordSessionEvent("renderer.vitagl.slow_deletes",detail);
        }
    }
    if(nextScreenshot<ecstasyScreenshotFrames.size() && journalFrames+1>=ecstasyScreenshotFrames[nextScreenshot]) {
        SaveScreenshot(packet.width,packet.height,journalFrames+1);++nextScreenshot;
    }
    if(gpuHeavyShotPending) {
        gpuHeavyShotPending=false;
        EnterGl();
        SaveScreenshot(packet.width,packet.height,200000000u+gpuHeavyShotFrame);
    }
    const auto submitDone=Clock::now();
    const auto vsync=vsyncRequest.exchange(-1);
    if(vsync>=0)vglWaitVblankStart(vsync ? GL_TRUE:GL_FALSE);
    if(ecstasyVsyncInterval>0 && vsync_interval!=0u)vsync_interval=static_cast<std::uint32_t>(ecstasyVsyncInterval);
    glRenderStage=4;
    const auto swapStarted=Clock::now();
    const auto timelineSubmitEnd=ecstasyGpuTimeline ? sceKernelGetSystemTimeWide():0;
    vglSwapBuffers(GL_FALSE);
    if(ecstasyGpuTimeline) {
        const auto head=gpuTimelineHead.load(std::memory_order_relaxed);
        auto& record=gpuTimeline[head%128u];
        record.submitBegin=timelineBegin;record.submitEnd=timelineSubmitEnd;record.swapEnd=sceKernelGetSystemTimeWide();
        record.fence=query_fence.value;record.draws=static_cast<std::uint32_t>(drawn);(void)timelineDrawnBefore;
        record.uploadEnd=timelineUploadEnd;record.passBegin=timelinePassBegin;record.passEnd=timelinePassEnd;
        record.entry=packet.timelineEntry;record.claimed=packet.timelineClaimed;record.ready=packet.timelineReady;
        record.alphaTested=gpuFrameAlphaTested;record.halfDraws=gpuFrameHalfDraws;record.offscreen=gpuFrameOffscreen;
        record.particleMilli=static_cast<std::uint32_t>(packet.particleScreens*1000.0);record.half=lastFrameHalf ? 1u:0u;
        record.resident=packet.timelineResident;
        record.indices=gpuFrameIndices;record.offIndices=gpuFrameOffIndices;record.atIndices=gpuFrameAtIndices;
        gpuFrameAlphaTested=gpuFrameHalfDraws=gpuFrameOffscreen=gpuFrameIndices=gpuFrameOffIndices=gpuFrameAtIndices=0;
        record.done.store(0,std::memory_order_relaxed);
        gpuTimelineHead.store(head+1,std::memory_order_release);
        // Report the frame 16 back (long done): one line per frame, times
        // relative to the previous frame's GPU completion.
        if(head>=17) {
            const auto& frame=gpuTimeline[(head-16)%128u];
            const auto& previous=gpuTimeline[(head-17)%128u];
            const auto done=frame.done.load(std::memory_order_acquire),before=previous.done.load(std::memory_order_acquire);
            if(done && before) {
                char detail[480];
                std::snprintf(detail,sizeof(detail),"n=%u sb=%lld se=%lld sw=%lld done=%lld draws=%u up=%lld pre=%lld pass=%lld post=%lld ent=%lld clm=%lld rdy=%lld at=%u hd=%u off=%u ps=%u half=%u idx=%u oidx=%u aidx=%u rn=%u re=%u rc=%u rv=%u",head-16,
                    static_cast<long long>(frame.submitBegin-before),static_cast<long long>(frame.submitEnd-before),
                    static_cast<long long>(frame.swapEnd-before),static_cast<long long>(done-before),frame.draws,
                    static_cast<long long>(frame.uploadEnd-frame.submitBegin),static_cast<long long>(frame.passBegin-frame.uploadEnd),
                    static_cast<long long>(frame.passEnd-frame.passBegin),static_cast<long long>(frame.submitEnd-frame.passEnd),
                    static_cast<long long>(frame.entry-before),static_cast<long long>(frame.claimed-before),static_cast<long long>(frame.ready-before),
                    frame.alphaTested,frame.halfDraws,frame.offscreen,frame.particleMilli,frame.half,frame.indices,frame.offIndices,frame.atIndices,
                    frame.resident[0],frame.resident[1],frame.resident[2],frame.resident[3]);
                platform::RecordSessionEvent("renderer.gpu_tl",detail);
                static std::uint64_t lastHeavyShot=0;
                const auto tail=static_cast<long long>(done-std::max(frame.submitEnd,before));
                if(ecstasyGpuHeavyShots>0 && tail>38000 && frame.swapEnd-lastHeavyShot>2000000u) {
                    --ecstasyGpuHeavyShots;lastHeavyShot=frame.swapEnd;
                    gpuHeavyShotPending=true;gpuHeavyShotFrame=head;
                }
            }
        }
    }
    {
        // G0: swap time every frame; on probe frames (GPU drained at frame
        // start) the time to finish after the swap, and the whole frame.
        static double alphaSum=0,scissorSum=0,swapSum=0,finishSum=0,frameSum=0,drawSum=0,blendSum=0,passSum=0,textureSum=0,vertexSum=0;
        static unsigned swapCount=0,finishCount=0;
        const auto swapped=Clock::now();
        swapSum+=milliseconds(swapped-swapStarted);++swapCount;
        if(gpuProbeFrame) {
            sceGxmFinish(gxm_context);
            const auto finished=Clock::now();
            finishSum+=milliseconds(finished-swapped);frameSum+=milliseconds(finished-renderBegin);++finishCount;
            drawSum+=gpuCounts.draws;scissorSum+=gpuCounts.scissorUpdates;alphaSum+=gpuCounts.alphaTested;blendSum+=gpuCounts.blended;passSum+=gpuCounts.passes;
            textureSum+=gpuCounts.textureBytes;vertexSum+=static_cast<double>(packet.vertices.size());
        }
        if(swapCount>=300) {
            const double n=finishCount ? finishCount:1.0;
            char detail[320];
            std::snprintf(detail,sizeof(detail),"swap_ms=%.2f gpu_after_swap_ms=%.2f gpu_frame_ms=%.2f finish_samples=%u submit_ms=%.2f vsync=%d diag=%d "
                "draws=%.0f blended=%.0f passes=%.1f texture_kb=%.0f stream_vertices=%.0f scissor_updates=%.1f elide=%d alpha_tested=%.0f",
                swapSum/swapCount,finishSum/n,frameSum/n,finishCount,
                milliseconds(submitDone-renderBegin),options.vsync ? 1:0,ecstasyGpuDiag,
                drawSum/n,blendSum/n,passSum/n,textureSum/n/1024.0,vertexSum/n,scissorSum/n,ecstasyScissorElide,alphaSum/n);
            platform::RecordSessionEvent("renderer.vitagl.gpu_probe",detail);
            alphaSum=scissorSum=swapSum=finishSum=frameSum=drawSum=blendSum=passSum=textureSum=vertexSum=0;swapCount=finishCount=0;
        }
    }
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
            char path[96];std::snprintf(path,sizeof(path),"ux0:data/NFSHP2/dshot_%llu_h%d_%ux%u.rgba",
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
        char detail[512];
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
            std::snprintf(detail+length,sizeof(detail)-length," quarter=%.2f bounded=%.2f skipped=%.2f bounded_area=%.3f",quarterFrames/300.0,
                halfCompositeBounded/300.0,halfCompositeSkipped/300.0,halfCompositeBounded ? halfCompositeArea/halfCompositeBounded:0.0);
            const auto length2=std::strlen(detail);
            std::snprintf(detail+length2,sizeof(detail)-length2," run_idx=%.0f missed=%.1f missed_idx=%.0f breaks=%llu/%llu/%llu/%llu",
                halfRunIndices/300.0,halfMissedBatches/300.0,halfMissedIndices/300.0,(unsigned long long)halfBreaks[0],(unsigned long long)halfBreaks[1],
                (unsigned long long)halfBreaks[2],(unsigned long long)halfBreaks[3]);
            halfRunIndices=halfMissedBatches=halfMissedIndices=0;for(auto& b:halfBreaks)b=0;
            halfCompositeBounded=halfCompositeSkipped=0;halfCompositeArea=0.0;
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

int ecstasySunQuery=1;
// The query program is compiled by vitaGL's runtime GLSL compiler (~340 ms on
// Vita). First use is the race-start camera cut (sun first in view), so the
// boot prewarm calls this on the first displayed frame instead.
bool EcstasyVulkanDevice::Storage::EnsureSunQueryProgram()
{
    if(!ecstasySunQuery || sunQueryFailed)return false;
    if(!sunQueryProgram) {
        static const char* vertexSource=
            "attribute vec2 a_pos;\nuniform float u_z;\n"
            "void main(){gl_Position=vec4(a_pos,u_z,1.0);}\n";
        static const char* fragmentSource="void main(){gl_FragColor=vec4(0.0);}\n";
        const auto compile=[&](GLenum kind,const char* source)->GLuint {
            const GLuint shader=glCreateShader(kind);
            glShaderSource(shader,1,&source,nullptr);glCompileShader(shader);
            GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
            if(!ok) {glDeleteShader(shader);return 0;}
            return shader;
        };
        const GLuint vertex=compile(GL_VERTEX_SHADER,vertexSource),fragment=compile(GL_FRAGMENT_SHADER,fragmentSource);
        GLint linked=0;
        if(vertex && fragment) {
            sunQueryProgram=glCreateProgram();
            glAttachShader(sunQueryProgram,vertex);glAttachShader(sunQueryProgram,fragment);
            glBindAttribLocation(sunQueryProgram,0,"a_pos");
            glLinkProgram(sunQueryProgram);
            glGetProgramiv(sunQueryProgram,GL_LINK_STATUS,&linked);
        }
        if(!linked) {
            sunQueryFailed=true;
            platform::RecordSessionEvent("renderer.vitagl.sun_query","program failed; sun stays unoccluded");
            return false;
        }
        sunQueryDepth=glGetUniformLocation(sunQueryProgram,"u_z");
        glGenBuffers(1,&sunQueryBuffer);
        for(auto& slot:sunQueries)glGenQueries(1,&slot.query);
    }
    return true;
}

void EcstasyVulkanDevice::Storage::IssueSunQueries(const GlFramePacket& packet)
{
    if(!EnsureSunQueryProgram())return;
    const auto width=static_cast<std::int32_t>(packet.width),height=static_cast<std::int32_t>(packet.height);
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    glViewport(0,0,width,height);glDepthRangef(0.0f,1.0f);
    glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);
    glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
    glEnable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glDepthFunc(GL_LESS);
    glUseProgram(sunQueryProgram);
    glBindBuffer(GL_ARRAY_BUFFER,sunQueryBuffer);
    for(GLuint attribute=1;attribute<4;++attribute)glDisableVertexAttribArray(attribute);
    // vitaGL applies scissor/state changes lazily at the next draw, the
    // scissor as a full-screen stencil quad. Flush them with one clipped
    // triangle outside the view so nothing but the samples is counted.
    {
        const float offscreen[6]={2.0f,2.0f,3.0f,2.0f,2.0f,3.0f};
        glUniform1f(sunQueryDepth,0.0f);
        glBufferData(GL_ARRAY_BUFFER,sizeof(offscreen),offscreen,GL_STREAM_DRAW);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
        glDrawArrays(GL_TRIANGLES,0,3);
    }
    const float sx=static_cast<float>(options.width)/static_cast<float>(options.logicalWidth);
    const float sy=static_cast<float>(options.height)/static_cast<float>(options.logicalHeight);
    const auto ndcX=[&](std::int32_t x) {return 2.0f*static_cast<float>(x)/static_cast<float>(width)-1.0f;};
    const auto ndcY=[&](std::int32_t y) {return 2.0f*static_cast<float>(height-y)/static_cast<float>(height)-1.0f;};
    for(const auto& request:packet.depthTransfers) {
        auto& slot=sunQueries[nextSunQuery];
        if(slot.pending)break; // GPU still owes this slot's count; skip a frame
        nextSunQuery=(nextSunQuery+1)%sunQueries.size();
        // Same sample positions as the Vulkan readback; a sample outside the
        // attachment never counts as clear.
        const auto& bounds=request.sampleBounds;
        sunQueryVertices.clear();
        for(std::int32_t y=std::max(bounds[2],0);y<std::min(bounds[3],request.height);++y) {
            const float sourceY=static_cast<float>(static_cast<std::int64_t>(request.y)+y)+0.5f;
            const auto py=static_cast<std::int32_t>(std::floor((static_cast<float>(request.targetTop)+sourceY*request.nativeSampleScale)*sy));
            if(py<0 || py>=height)continue;
            for(std::int32_t x=std::max(bounds[0],0);x<std::min(bounds[1],request.width);++x) {
                const float sourceX=static_cast<float>(static_cast<std::int64_t>(request.x)+x)+0.5f;
                const auto px=static_cast<std::int32_t>(std::floor((static_cast<float>(request.targetLeft)+sourceX*request.nativeSampleScale)*sx));
                if(px<0 || px>=width)continue;
                const float x0=ndcX(px),x1=ndcX(px+1),y0=ndcY(py),y1=ndcY(py+1);
                sunQueryVertices.insert(sunQueryVertices.end(),{x0,y0,x1,y0,x0,y1,x1,y0,x1,y1,x0,y1});
            }
        }
        // Clear (sky) when the stored depth is within 1.5 units of the clear
        // value: the Vulkan readback's llround((1-d)*max)<2 test.
        const double limit=1.0-1.5/static_cast<double>(request.maximumDepth);
        glUniform1f(sunQueryDepth,static_cast<float>(2.0*limit-1.0));
        slot.request=request;slot.pending=true;
        glBeginQuery(GL_SAMPLES_PASSED,slot.query);
        if(!sunQueryVertices.empty()) {
            glBufferData(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(sunQueryVertices.size()*sizeof(float)),
                sunQueryVertices.data(),GL_STREAM_DRAW);
            glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
            glDrawArrays(GL_TRIANGLES,0,static_cast<GLsizei>(sunQueryVertices.size()/2));
        }
        glEndQuery(GL_SAMPLES_PASSED);
    }
    for(GLuint attribute=1;attribute<4;++attribute)glEnableVertexAttribArray(attribute);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glDepthFunc(GL_LEQUAL);
}

void EcstasyVulkanDevice::Storage::PublishSunQueries()
{
    for(auto& slot:sunQueries) {
        if(!slot.pending)continue;
        GLint available=0;glGetQueryObjectiv(slot.query,GL_QUERY_RESULT_AVAILABLE,&available);
        if(!available)continue;
        GLuint passed=0;glGetQueryObjectuiv(slot.query,GL_QUERY_RESULT_NO_WAIT,&passed);
        slot.pending=false;
        // StuffSun only counts clear samples inside its bounds: mark that
        // many of them clear, every other sample at the maximum depth.
        const auto& request=slot.request;const auto& bounds=request.sampleBounds;
        auto& values=request.destination->values;
        std::uint32_t clear=passed;
        for(std::int32_t y=0;y<request.height;++y)for(std::int32_t x=0;x<request.width;++x) {
            const bool inside=x>=bounds[0] && x<bounds[1] && y>=bounds[2] && y<bounds[3];
            auto& value=values[static_cast<std::size_t>(y)*static_cast<std::size_t>(request.width)+static_cast<std::size_t>(x)];
            if(inside && clear) {value=0;--clear;} else value=request.maximumDepth;
        }
        if(platform::nativeDiagnosticsEnabled) {
            static unsigned logged=0;
            if((logged++%60)==0) {
                char text[96];std::snprintf(text,sizeof(text),"passed=%u bounds=%d,%d,%d,%d",passed,
                    bounds[0],bounds[1],bounds[2],bounds[3]);
                platform::RecordSessionEvent("renderer.vitagl.sun_query",text);
            }
        }
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
    // halfBounds (window pixels, set by drawPass): x1<0 full screen, an
    // empty rectangle draws nothing (nothing was drawn into the half target).
    const auto bounds=halfBounds;halfBounds={{0.0f,0.0f,-1.0f,-1.0f}};
    const bool full=bounds[2]<0.0f;
    if(!full && (bounds[2]<=bounds[0] || bounds[3]<=bounds[1])) {++halfCompositeSkipped;glEnable(GL_SCISSOR_TEST);return;}
    if(full)glBindBuffer(GL_ARRAY_BUFFER,halfQuad);
    else {
        const float W=static_cast<float>(width),H=static_cast<float>(height);
        const float l=bounds[0]/W*2.0f-1.0f,r=bounds[2]/W*2.0f-1.0f,b=bounds[1]/H*2.0f-1.0f,t=bounds[3]/H*2.0f-1.0f;
        const float quad[8]={l,b,r,b,l,t,r,t};
        if(!halfBoundsQuad)glGenBuffers(1,&halfBoundsQuad);
        glBindBuffer(GL_ARRAY_BUFFER,halfBoundsQuad);
        glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_DYNAMIC_DRAW);
        halfCompositeArea+=static_cast<double>((bounds[2]-bounds[0])*(bounds[3]-bounds[1]))/(W*H);
        ++halfCompositeBounded;
    }
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
    char path[96];std::snprintf(path,sizeof(path),"ux0:data/NFSHP2/shot_%llu_%ux%u.rgba",
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
    frame.FinalizeOrder();
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
    if(ecstasyForceNoVsync)vsync=false; // Vita vsync_off.txt (diagnostic: display pacing vs GPU)
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
