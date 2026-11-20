#include "indep/inc/AICarController.hpp"
#include "indep/inc/Ecstasy.hpp"
#include "indep/inc/Flags.hpp"
#include "indep/inc/NativeRenderSettings.hpp"
#include "indep/inc/NativePipelineCache.hpp"
#if defined(_M_X64) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

#include "CameraMover.hpp"
#include "indep/inc/RaceCoordinator.hpp"
#include "bware/inc/bWare.hpp"
#include "indep/inc/eLight.hpp"
#include "indep/inc/eEnvMap.hpp"
#include "indep/inc/AcidFX.hpp"
#include "indep/inc/FlailerDB.hpp"
#include "indep/inc/Texture.hpp"
#include "indep/inc/eMath.hpp"
#include "indep/inc/Scenery.hpp"
#include "indep/inc/WorldObject.hpp"
#include "indep/inc/CarInfo.hpp"
#include "indep/inc/Player.hpp"
#include "indep/inc/GameFlow.hpp"
#include "indep/inc/NIS.hpp"
#include "indep/inc/Clans.hpp"
#include "indep/inc/World.hpp"
#include "indep/inc/WeatherMan.hpp"
#include "indep/inc/FMenus.hpp"
#include "indep/inc/Fred.hpp"
#include "indep/inc/Timer.hpp"
#include "bware/inc/bChunk.hpp"
#include "bware/inc/bWare.hpp"
#include "indep/inc/Ecstasy.hpp"
#include "bware/inc/bWare.hpp"
#include "bware/inc/bMath.hpp"
#include "bware/inc/bMath.hpp"
#include "bware/inc/bMath.hpp"
#include <cstring>
#include <algorithm>
#ifdef __SWITCH__
#include <switch.h>
#include "src/nx/controller_art.hpp"
#include "src/nx/display.hpp"
#endif
#include <cmath>
#include <vector>
#include <atomic>
#include <exception>
#include <cstdio>
#include <sstream>
#include <iterator>
#include <list>
#include <memory_resource>
#include <type_traits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace {
const EditorMarkerSolid* findModelSolid(const EditorMarkerResources* root,uint32_t key)
{
    for(const EditorMarkerResources* resource=root;resource;resource=resource->nextCatalogue)
        for(size_t index=0;index<resource->solidCount;++index)
            if(resource->solids[index].key==key)return resource->solids+index;
    return nullptr;
}

EditorMarkerTexture* findModelTexture(EditorMarkerResources* root,uint32_t key)
{
    for(EditorMarkerResources* resource=root;resource;resource=resource->nextCatalogue)
        for(size_t index=0;index<resource->textureCount;++index)
            if(resource->textures[index].key==key)return resource->textures+index;
    return nullptr;
}
}

namespace hp2 {
unsigned NativeSceneEffectiveHardwareConcurrency() noexcept
{
#if defined(_WIN32)
    DWORD_PTR processMask=0,systemMask=0;
    const bool processKnown=GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask)!=FALSE;
    GROUP_AFFINITY currentThread{};
    const bool threadKnown=GetThreadGroupAffinity(GetCurrentThread(),&currentThread)!=FALSE;
    std::uint64_t affinityMask=processKnown ? static_cast<std::uint64_t>(processMask):0;
    if(threadKnown) {
        const auto threadMask=static_cast<std::uint64_t>(currentThread.Mask);
        affinityMask=affinityMask ? affinityMask&threadMask:threadMask;
    }
    // Windows 11 may expose more than one processor group to a process.
    // GetThreadGroupAffinity describes only its primary group, so an
    // unrestricted primary-group mask must not become an artificial cap.
    // Explicit narrower affinity remains authoritative. Workers are left to
    // the OS scheduler; we never overwrite the user's affinity assignment.
    const bool narrowerProcess=processKnown && processMask && processMask!=systemMask;
    const bool narrowerThread=threadKnown && processKnown && systemMask && currentThread.Mask!=systemMask;
    if(!narrowerProcess && !narrowerThread && GetActiveProcessorGroupCount()>1) {
        try {
            USHORT groupCount=0;
            GetProcessGroupAffinity(GetCurrentProcess(),&groupCount,nullptr);
            if(groupCount>1) {
                std::vector<USHORT> groups(groupCount);
                if(GetProcessGroupAffinity(GetCurrentProcess(),&groupCount,groups.data())) {
                    unsigned available=0;
                    for(USHORT index=0;index<groupCount;++index)
                        available+=GetActiveProcessorCount(groups[index]);
                    if(available)return available;
                }
            }
        } catch(...) {
            // An optional topology allocation cannot break a render frame.
            // The primary affinity mask below remains a valid fallback.
        }
    }
    if(affinityMask) {
        unsigned count=0;
        while(affinityMask) {affinityMask&=affinityMask-1;++count;}
        return count;
    }
#elif defined(__SWITCH__)
    // Visibility checks ask for the worker budget many times per frame.
    static const unsigned cores=[] {
        u64 allowed{};
        if(R_SUCCEEDED(svcGetInfo(&allowed,InfoType_CoreMask,CUR_PROCESS_HANDLE,0)) && allowed)
            return static_cast<unsigned>(__builtin_popcountll(allowed));
        return std::max(1u,std::thread::hardware_concurrency());
    }();
    return cores;
#elif defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if(sched_getaffinity(0,sizeof(allowed),&allowed)==0) {
        const auto count=static_cast<unsigned>(CPU_COUNT(&allowed));
        if(count)return count;
    }
#endif
    // A failed optional affinity query does not make a multicore CPU a
    // single-core machine. The OS still enforces its actual affinity.
    return std::max(1u,std::thread::hardware_concurrency());
}

#ifdef __SWITCH__
void ConfigureNativeSceneWorker() noexcept
{
    u64 mask{};
    if(R_SUCCEEDED(svcGetInfo(&mask,InfoType_CoreMask,CUR_PROCESS_HANDLE,0)) && mask)
        (void)svcSetThreadCoreMask(CUR_THREAD_HANDLE,-1,static_cast<u32>(mask));
}
#endif

std::int32_t environmentMappingEnabled=1; // Released writable EnableEnvMap32F720.
std::array<float,2> textureAnimationScrollSpeed{};
std::int32_t textureAnimationScrollMode=1;
float textureAnimationScrollInterval=1.0f;
EcstasyNativeRenderPolicy nativeRenderPolicy;
std::uint32_t ecstasyCaptureNativeRenderEggState() noexcept
{
    return (nativeRenderPolicy.texturing ? EcstasyRenderEggTexturing:0u) |
        (nativeRenderPolicy.wireframe ? EcstasyRenderEggWireframe:0u) |
        (nativeRenderPolicy.pixelClampOverride ? EcstasyRenderEggClampTopLeft:0u);
}
struct ResourceRegistry::Storage {
    EditorMarkerResources& models;
    CarReplayAnimationRegistry& animations;
    ChopperEffectData& effects;
    frontend::FlailerResources& flailer;
    LightTextureResources light;
    SunResources sun;
    LightSceneRegistry lights;
    QueuedFileService* queuedFiles{};
    std::array<EditorMarkerTexture*,9> specialTextures{};
    Storage(EditorMarkerResources& bank,CarReplayAnimationRegistry& animationList,
        ChopperEffectData& effectData,frontend::FlailerResources& ui)
        :models(bank),animations(animationList),effects(effectData),flailer(ui)
    {}
};

ResourceRegistry::ResourceRegistry(EditorMarkerResources& models,CarReplayAnimationRegistry& animations,
    ChopperEffectData& effects,frontend::FlailerResources& flailer)
    :storage_(std::make_unique<Storage>(models,animations,effects,flailer))
{
    Require(!models.catalogueRoot,"Resource registry requires the canonical model bank");
    Require(!models.resourceRegistry,"Model bank already has its actual resource registry");
    models.resourceRegistry=this;
}
ResourceRegistry::~ResourceRegistry()
{
    ecstasyDeleteAllDuplicatedSolids(&storage_->models);
    if (storage_->models.resourceRegistry==this)storage_->models.resourceRegistry=nullptr;
}
EditorMarkerResources& ResourceRegistry::Models() noexcept { return storage_->models; }
CarReplayAnimationRegistry& ResourceRegistry::Animations() noexcept { return storage_->animations; }
ChopperEffectData& ResourceRegistry::Effects() noexcept { return storage_->effects; }
frontend::FlailerResources& ResourceRegistry::Flailer() noexcept { return storage_->flailer; }
SunResources& ResourceRegistry::Sun() noexcept {return storage_->sun;}
LightSceneRegistry& ResourceRegistry::Lights() noexcept {return storage_->lights;}
void ResourceRegistry::BindQueuedFiles(QueuedFileService& files)
{
    Require(!storage_->queuedFiles || storage_->queuedFiles==&files,
        "Resource registry cannot borrow a second queued-file service");
    storage_->queuedFiles=&files;
}
QueuedFileService* ResourceRegistry::QueuedFiles() const noexcept {return storage_->queuedFiles;}
const EditorMarkerTexture* ResourceRegistry::FindTexture(std::uint32_t key) const noexcept
{ return findModelTexture(&storage_->models,key); }
EditorMarkerTexture* ResourceRegistry::ResolveTexture(std::uint32_t key,bool fallback)
{
    if(auto* texture=texturePackRegistry.ResolveLoadedTexture(storage_->models,key))return texture->descriptor;
    // PAL1ABF48(a2=0) uses1AC3F0: pixels and any required palette must
    // already belong to the same record. Metadata alone is not drawable.
    // The separate a2=1 metadata lookup is not requested by these callers.
    return fallback && key!=0x19006835u ? ResolveTexture(0x19006835u,false) : nullptr;
}
const EditorMarkerLightMaterial* ResourceRegistry::FindLightMaterial(std::uint32_t key) const noexcept
{
    for (auto* catalogue=storage_->models.lightMaterialCatalogues;catalogue;catalogue=catalogue->next)
        for (const auto& material:catalogue->materials)if (material.key==key)return &material;
    return storage_->models.defaultLightMaterial;
}
bool ResourceRegistry::IsTextureAlive(const EditorMarkerTexture* texture) const noexcept
{
    if (!texture)return false;
    for (auto* resource=&storage_->models;resource;resource=resource->nextCatalogue)
        for (std::size_t index=0;index<resource->textureCount;++index)
            if (resource->textures+index==texture)return true;
    return false;
}
void ResourceRegistry::ResourceRefresh()
{
    auto& models=storage_->models;
    if (models.fixupsBlocked)return;
    if (models.modelsChanged || models.texturesChanged) {
        texturePackRegistry.InvalidateDescriptorIndex();
        if (models.texturesChanged)
            for (auto* catalogue=&models;catalogue;catalogue=catalogue->nextCatalogue)
                for (std::size_t index=0;index<catalogue->solidCount;++index)
                    if (auto* geometry=catalogue->solids[index].geometry) {
                        geometry->FixTextureTable(*this);
                        geometry->FixStripSTs();
                        geometry->CalcAutoK();
                    }
        editorMarkerModelsReconnectAll(&models);
        carReplayAnimationReconnectAll(&storage_->animations);
        RefreshSpecialTextures();
        models.modelsChanged=0;models.texturesChanged=0;
        storage_->animations.reconnectRequired=0;
    }
    carReplayAnimationResourceEndFixup(&storage_->animations);
}
void ResourceRegistry::UpdateLightTextures() { storage_->light.UpdateTextures(*this); }
void ResourceRegistry::RefreshSpecialTextures()
{
    constexpr std::array<std::uint32_t,9> keys{0xdbd4u,0xcf29d0dfu,0x32283a11u,
        0x02c8711cu,0xbad886d4u,0xab9a6e21u,0x0184d1f8u,0xb9ab56e4u,0x4ecc769cu};
    for (std::size_t index=0;index<keys.size();++index)
        storage_->specialTextures[index]=ResolveTexture(keys[index],false);
    auto* descriptor=storage_->specialTextures[7];
    if (!descriptor)return;
    auto* texture=textureInfoForDescriptor(descriptor);
    Require(texture!=nullptr,"Special texture has no canonical pixel/palette owner");
    auto* palette=texture->palette.MutableData();
    if (!palette)return;
    Require(texture->paletteBytes>=0 && texture->palette.size>=static_cast<std::size_t>(texture->paletteBytes),
        "Special texture palette extent is truncated");
    CreateSpecialTexturePalette(palette,static_cast<std::size_t>(texture->paletteBytes)/4u);
}
void ResourceRegistry::CreateSpecialTexturePalette(std::uint8_t* palette,std::size_t count)
{
    Require(palette || !count,"Special palette has no output storage");
    for(std::size_t index=0;index<count;++index) {
        const float angle=static_cast<float>(index)/static_cast<float>(count)*1.5707499980926514f+
            1.5707964897155762f;
        const float intensity=std::max(rotationSineRadians(angle),0.009999999776482582f);
        // PAL103328..103360's float-word conversion preserves an intensity
        // near its input, not int(intensity) reinterpreted as a denormal.
        // Native PC evaluates the curve directly and retains the final byte
        // truncation. In particular intensity1 must produce128, never0.
        const auto lane=static_cast<std::uint8_t>(convertFloatToInt32(std::clamp(intensity*128.0f,0.0f,255.0f)));
        std::fill_n(palette+index*4u,4,lane);
    }
}

const EditorMarkerTexture* ResourceRegistry::LightTexture(std::size_t index) const
{ return storage_->light.At(index); }
void ResourceRegistry::RetireTextureCatalogue(const EditorMarkerResources& catalogue) noexcept
{
    // RefreshSpecialTextures borrows descriptors from their actual pack.
    // During original track/common retirement fixups remain blocked, but the
    // native RAII catalogue is freed immediately. Revoke only its aliases
    // before detaching; no lookup, rebinding, or fallback is performed here.
    for(auto*& texture:storage_->specialTextures)
        for(std::size_t index=0;texture && index<catalogue.textureCount;++index)
            if(texture==catalogue.textures+index)texture=nullptr;
}
const EditorMarkerTexture* ResourceRegistry::RearViewTexture(std::size_t index) const
{
    Require(index<2,"Rear-view special texture index is outside its source pair");
    return storage_->specialTextures[index];
}
const EditorMarkerTexture* ResourceRegistry::SpecialTexture(std::size_t index) const
{
    Require(index<storage_->specialTextures.size(),"Special texture index is outside the original table");
    return storage_->specialTextures[index];
}
}

namespace {
const EditorMarkerTexture& stripTexture(const EditorMarkerSolidGeometry& geometry,
    const EditorMarkerSolidGeometry::Strip& strip)
{
    hp2::Require(strip.textureIndex>=0 && static_cast<std::size_t>(strip.textureIndex)<geometry.textures.size(),
        "Solid strip texture index is outside its retained table");
    const auto* texture=geometry.textures[static_cast<std::size_t>(strip.textureIndex)].texture;
    hp2::Require(texture && texture->authoredRecord && texture->authoredRecordSize>=0x74,
        "Solid texture binding has no actual texture descriptor");
    return *texture;
}

float solidApproximateLog2(float value)
{
    std::uint32_t bits;std::memcpy(&bits,&value,4);
    const int exponent=static_cast<int>((bits>>23)&255u);
    const int power=exponent-127<1 ? exponent-128 : exponent-126;
    const float iterations=std::fabs(finiteRatio(127.0f,static_cast<float>(power)));
    const float factor=value;
    for (int i=1;i<convertFloatToInt32(iterations);++i)value*=factor;
    std::memcpy(&bits,&value,4);
    return finiteRatio(static_cast<float>(static_cast<int>((bits>>23)&255u)-127),iterations);
}

std::int16_t solidTextureK(const EditorMarkerTexture& texture,float ratio)
{
    const auto* record=texture.authoredRecord;
    int bias=hp2::ReadU16(record+0x72)&0xfff;
    if (bias&0x800)bias-=0x1000;
    if (ratio==0.0f)return 0;
    std::int8_t level;std::memcpy(&level,record+0x71,1);
    const std::uint32_t scaleBits=1u<<(static_cast<std::uint32_t>(level)&31u);
    std::int32_t scale;std::memcpy(&scale,&scaleBits,4);
    const float logarithm=solidApproximateLog2(finiteRatio(1.0f,std::sqrt(std::fabs(ratio))));
    const float difference=static_cast<float>(bias)*0.0625f-logarithm*static_cast<float>(scale);
    const auto packed=static_cast<std::uint16_t>(convertFloatToInt32(difference*16.0f));
    std::int16_t result;std::memcpy(&result,&packed,2);return result;
}
}

void EditorMarkerSolidGeometry::FixTextureTable(hp2::ResourceRegistry& resources)
{
    for (auto& binding:textures)binding.texture=resources.ResolveTexture(binding.key,true);
}
void EditorMarkerSolidGeometry::FixLightMaterialTable(hp2::ResourceRegistry& resources)
{
    lightMaterials.resize(lightMaterialKeys.size());
    for (std::size_t index=0;index<lightMaterialKeys.size();++index)
        lightMaterials[index]=resources.FindLightMaterial(lightMaterialKeys[index]);
}

void EditorMarkerSolidGeometry::Strip::PrepareNativeTopology()
{
    if(nativeVertexRevision)StampNativeVertexRevision();
    const std::size_t count=positions.size();
    hp2::Require(count<=48,"Native solid strip topology exceeds source vertex limit");
    nativeUniqueOrder.clear();nativeTriangleOrder.clear();
    nativeUniqueOrder.reserve(count);
    nativeTriangleOrder.reserve(count>2 ? (count-2)*3 : 0);
    std::uint64_t visited=0;
    for(std::size_t triangle=2;triangle<count;++triangle) {
        if(drawDisableMask&(std::uint64_t{1}<<triangle))continue;
        const std::size_t corners[]={triangle-2+(triangle&1u),
            triangle-1-(triangle&1u),triangle};
        for(const auto vertex:corners) {
            const auto bit=std::uint64_t{1}<<vertex;
            if(!(visited&bit)) {
                nativeUniqueOrder.push_back(static_cast<std::uint8_t>(vertex));
                visited|=bit;
            }
            nativeTriangleOrder.push_back(static_cast<std::uint8_t>(vertex));
        }
    }
    nativeTopologyCount=count;
    nativeTopologyMask=drawDisableMask;
}

void EditorMarkerSolidGeometry::Strip::StampNativeVertexRevision() noexcept
{
    static std::atomic<std::uint64_t> nextRevision{};
    nativeVertexRevision=nextRevision.fetch_add(1,std::memory_order_relaxed)+1;
}

void EditorMarkerSolidGeometry::FixStripSTs()
{
    for (auto& strip:strips) {
        const auto& texture=stripTexture(*this,strip);
        if (texture.key==0x19006835u)continue;
        const std::uint16_t dimensions[2]={hp2::ReadU16(texture.authoredRecord+0x24),
            hp2::ReadU16(texture.authoredRecord+0x26)};
        float ratios[2],inverses[2];bool changed[2];
        for (int axis=0;axis<2;++axis) {
            hp2::Require(dimensions[axis]>0 && dimensions[axis]<=32767,"Solid texture dimension is invalid");
            std::uint32_t power=1;while (power<dimensions[axis])power<<=1;
            ratios[axis]=static_cast<float>(dimensions[axis])/static_cast<float>(power);
            const auto scale=static_cast<std::uint16_t>(convertFloatToInt32(ratios[axis]*32768.0f));
            changed[axis]=scale!=strip.textureScales[axis];
            inverses[axis]=finiteRatio(1.0f,static_cast<float>(strip.textureScales[axis])*0.000030517578125f);
        }
        if (!changed[0] && !changed[1])continue;
        if(strip.nativeVertexRevision)strip.StampNativeVertexRevision();
        for (auto& st:strip.textureCoordinates)
            for (int axis=0;axis<2;++axis)if (changed[axis]) {
                st[axis]*=inverses[axis];st[axis]*=ratios[axis];
            }
        for (int axis=0;axis<2;++axis)if (changed[axis])
            strip.textureScales[axis]=static_cast<std::uint16_t>(convertFloatToInt32(ratios[axis]*32768.0f));
    }
}

void EditorMarkerSolidGeometry::CalcAutoK()
{
    for (auto& strip:strips) {
        const auto& texture=stripTexture(*this,strip);
        const float projection=static_cast<float>(strip.autoKProjection)*0.00006103515625f;
        const float perpendicular=std::sqrt(std::fabs(1.0f-projection*projection));
        const float cosine=maximumFinite(0.0f,rotationSineAngle(static_cast<std::uint16_t>(strip.autoKAngle+0x4000u)));
        const float coefficient=(cosine*0.4f+0.6f)*0.8f;
        const float denominator=maximumFinite((coefficient*perpendicular+1.0f)-coefficient,0.001f);
        const int width=static_cast<int>(hp2::ReadU16(texture.authoredRecord+0x24));
        const int height=static_cast<int>(hp2::ReadU16(texture.authoredRecord+0x26));
        strip.autoK=solidTextureK(texture,finiteRatio(strip.autoKScale*static_cast<float>(width*height),denominator));
        strip.mipLevelScale=texture.authoredRecord[0x71];
        // Source SetMipMapK writes the low byte to the strip attribute.
        // Vulkan will consume this CPU value, not an invented GS packet.
    }
}

int32_t editorMarkerResourcesAttach(EditorMarkerResources* root,EditorMarkerResources* catalogue)
{
    if(!root || !catalogue || catalogue->catalogueRoot ||
       catalogue->nextCatalogue || catalogue->first)return 0;
    if(root->catalogueRoot)root=root->catalogueRoot;
    if(root==catalogue)return 0;
    EditorMarkerResources* tail=root;
    while(tail->nextCatalogue)tail=tail->nextCatalogue;
    tail->nextCatalogue=catalogue;
    catalogue->catalogueRoot=root;
    if(catalogue->solidCount)root->modelsChanged=1;
    if(catalogue->textureCount)root->texturesChanged=1;
    ++root->bindingRevision;
    return 1;
}

void editorMarkerResourcesDetach(EditorMarkerResources* catalogue)
{
    if(!catalogue || !catalogue->catalogueRoot)return;
    EditorMarkerResources* root=catalogue->catalogueRoot;
    EditorMarkerResources* previous=root;
    while(previous->nextCatalogue && previous->nextCatalogue!=catalogue)previous=previous->nextCatalogue;
    if(previous->nextCatalogue!=catalogue)return;
    if(root->resourceRegistry)root->resourceRegistry->RetireTextureCatalogue(*catalogue);
    previous->nextCatalogue=catalogue->nextCatalogue;
    catalogue->nextCatalogue=nullptr;
    catalogue->catalogueRoot=nullptr;
    if(catalogue->solidCount)root->modelsChanged=1;
    if(catalogue->textureCount)root->texturesChanged=1;
    ++root->bindingRevision;
}

void editorMarkerModelInitialize(EditorMarkerModel* model,EditorMarkerResources* resources,uint32_t key)
{
    std::memset(model,0,sizeof(*model));
    if(resources->catalogueRoot)resources=resources->catalogueRoot;
    model->resources=resources;
    model->key=key;
    if(!key)return;
    editorMarkerModelReconnectSolid(model);
    model->previous=resources->last;
    if(resources->last)resources->last->next=model;
    else resources->first=model;
    resources->last=model;
}

void editorMarkerModelSetReplacement(EditorMarkerModel* model,EditorMarkerReplacement* replacement)
{
    // The two-argument native caller still requires the complete original
    // attach contract: publish replacements, apply bindings and repair STs.
    // Reconnecting the table alone leaves source geometry using old UVs.
    ++model->resources->bindingRevision;
    editorMarkerModelAttachReplacementTextures(model,replacement,replacement ? 1:0,
        model->detailLevel);
}

void editorMarkerModelAttachReplacementTextures(EditorMarkerModel* model,
    EditorMarkerReplacement* replacements,int32_t count,int32_t detailLevel)
{
    // PAL10C798. These are the original shared replacement cells. Each model
    // records its own detail index into them; geometry remains in its eSolid.
    model->replacement=replacements;
    const auto countByte=static_cast<std::uint8_t>(count);
    const auto detailByte=static_cast<std::uint8_t>(detailLevel);
    std::memcpy(&model->replacementCount,&countByte,1);
    std::memcpy(&model->detailLevel,&detailByte,1);
    editorMarkerModelReconnectTextureTable(model);
    if (model->solid) {
        editorMarkerModelApplyReplacementTextureTable(model);
        hp2::Require(model->solid->geometry!=nullptr,"Model has no decoded geometry owner");
        model->solid->geometry->FixStripSTs();
    }
}

int32_t editorMarkerModelRenameTexture(EditorMarkerModel* model,uint32_t originalKey,uint32_t replacementKey)
{
    // PAL10C7F0 changes the first matching key in the live solid table.
    if (!model->solid)return 0;
    hp2::Require(model->solid->geometry && model->resources && model->resources->resourceRegistry,
        "Model texture rename has no geometry/resource owner");
    for (auto& texture:model->solid->geometry->textures)if (texture.key==originalKey) {
        texture.key=replacementKey;
        texture.texture=model->resources->resourceRegistry->ResolveTexture(replacementKey,true);
        return 1;
    }
    return 0;
}

void editorMarkerSolidReplaceLightMaterial(const EditorMarkerSolid* solid,uint32_t originalKey,
    const EditorMarkerLightMaterial* replacement)
{
    // PAL10C4B0 replaces every matching material pointer, retaining the keys.
    hp2::Require(solid && solid->geometry,"Light material replacement has no solid geometry");
    auto& geometry=*solid->geometry;
    hp2::Require(geometry.lightMaterials.size()==geometry.lightMaterialKeys.size(),
        "Light material keys and references differ");
    for (std::size_t index=0;index<geometry.lightMaterialKeys.size();++index)
        if (geometry.lightMaterialKeys[index]==originalKey)geometry.lightMaterials[index]=replacement;
}

void editorMarkerSolidReplaceLightMaterialKey(const EditorMarkerSolid* solid,EditorMarkerResources* resources,
    uint32_t originalKey,uint32_t replacementKey)
{
    // PAL10C4E0 resolves the same global material catalogue used by the solid.
    hp2::Require(resources && resources->resourceRegistry,"Light material replacement has no registry");
    editorMarkerSolidReplaceLightMaterial(solid,originalKey,
        resources->resourceRegistry->FindLightMaterial(replacementKey));
}

void editorMarkerModelReplaceLightMaterial(EditorMarkerModel* model,uint32_t originalKey,uint32_t replacementKey)
{
    // PAL10CAC0 is the model-to-solid tail call, with the source null test.
    if (model->solid)editorMarkerSolidReplaceLightMaterialKey(
        model->solid,model->resources,originalKey,replacementKey);
}

const EditorMarkerAnimationTree* editorMarkerModelAnimationTree(const EditorMarkerModel* model)
{
    // Complete PAL10CA80: an existing solid owns a tree even when count is0.
    return model->solid ? &model->solid->animationTree : nullptr;
}

void editorMarkerModelReconnectSolid(EditorMarkerModel* model)
{
    if (!model->key)return;
    const auto* solid=findModelSolid(model->resources,model->key);
    if (solid==model->solid)return;
    model->solid=solid;
    model->rigidAnimationStart=0;
    if (!solid || !solid->rigidAnimation || solid->rigidAnimation->playbackType!=0)return;
    model->rigidAnimationStart=0xc000u;
    if (solid->resourceFlags&0x20u)
        model->rigidAnimationStart=static_cast<std::uint16_t>(model->rigidAnimationStart|vectorDefaultRandomBounded(256));
}

void editorMarkerModelReconnectTextureTable(EditorMarkerModel* model)
{
    if (model->replacementCount<=0)return;
    hp2::Require(model->replacement && model->detailLevel>=0 && model->detailLevel<16,
        "Model replacement array or detail level is invalid");
    auto* registry=model->resources->resourceRegistry;
    hp2::Require(registry!=nullptr,"Model replacement requires its actual resource registry");
    for (std::int32_t index=0;index<model->replacementCount;++index) {
        auto& replacement=model->replacement[index];
        replacement.texture=registry->ResolveTexture(replacement.replacementKey,true);
        auto& mapped=replacement.textureIndices[model->detailLevel];mapped=-1;
        if (model->solid && model->solid->geometry) {
            const auto& textures=model->solid->geometry->textures;
            for (std::size_t texture=0;texture<textures.size();++texture)
                if (textures[texture].key==replacement.originalKey) {
                    const auto textureByte=static_cast<std::uint8_t>(texture);
                    std::memcpy(&mapped,&textureByte,1);break;
                }
        }
    }
    editorMarkerModelApplyReplacementTextureTable(model);
}

void editorMarkerModelApplyReplacementTextureTable(EditorMarkerModel* model)
{
    // Actual10C878 is also called by RenderStrips. It changes the shared
    // solid texture table, rather than constructing a per-model copy.
    if (!model->solid || model->replacementCount<=0)return;
    hp2::Require(model->solid->geometry && model->replacement &&
        model->detailLevel>=0 && model->detailLevel<16,
        "Replacement application has an invalid geometry, array or detail binding");
    auto& bindings=model->solid->geometry->textures;
    for (std::int32_t index=0;index<model->replacementCount;++index) {
        const auto& replacement=model->replacement[index];
        const auto mapped=replacement.textureIndices[model->detailLevel];
        if (mapped>=0) {
            hp2::Require(static_cast<std::size_t>(mapped)<bindings.size(),
                "Model replacement binding is outside the solid");
            bindings[static_cast<std::size_t>(mapped)].texture=replacement.texture;
        }
    }
}

void editorMarkerModelsReconnectAll(EditorMarkerResources* resources)
{
    for (auto* model=resources->first;model;model=model->next) {
        editorMarkerModelReconnectSolid(model);
        editorMarkerModelReconnectTextureTable(model);
        if (model->solid && model->solid->geometry)model->solid->geometry->FixStripSTs();
    }
}

void editorMarkerModelDispose(EditorMarkerModel* model)
{
    if(!model->key)return;
    EditorMarkerResources* resources=model->resources;
    if(model->previous)model->previous->next=model->next;
    else resources->first=model->next;
    if(model->next)model->next->previous=model->previous;
    else resources->last=model->previous;
    model->solid=nullptr;
    model->key=0;
    model->replacement=nullptr;
    model->replacementCount=0;
    model->rigidAnimationStart=0;
    model->next=model->previous=nullptr;
}

struct EditorMarkerDuplicatedSolid {
    EditorMarkerSolid solid{};
    std::unique_ptr<EditorMarkerSolidGeometry> geometry;
    EditorMarkerResources catalogue{};
    EditorMarkerDuplicatedSolid* next{};
};

void ecstasySolidChangeName(EditorMarkerSolid* solid,const char* name)
{
    const auto size=std::strlen(name);
    hp2::Require(size<sizeof(solid->name),"Duplicated solid name exceeds its original field");
    std::memcpy(solid->name,name,size+1);
    solid->key=hashString(name);
}

void ecstasyDuplicateSolid(EditorMarkerResources* resources,const char* sourceName,const char* newName)
{
    if (resources->catalogueRoot)resources=resources->catalogueRoot;
    const auto* source=findModelSolid(resources,hashString(sourceName));
    if (!source)return;
    auto copy=std::make_unique<EditorMarkerDuplicatedSolid>();
    copy->solid=*source;
    hp2::Require(source->geometry!=nullptr,"Duplicated solid has no decoded vertex owner");
    copy->geometry=std::make_unique<EditorMarkerSolidGeometry>(*source->geometry);
    copy->solid.geometry=copy->geometry.get();
    copy->solid.resourceFlags=static_cast<std::uint16_t>(copy->solid.resourceFlags|4u);
    ecstasySolidChangeName(&copy->solid,newName);
    copy->catalogue.solids=&copy->solid;
    copy->catalogue.solidCount=1;
    hp2::Require(editorMarkerResourcesAttach(resources,&copy->catalogue)!=0,
        "Duplicated solid could not join its existing resource bank");
    auto** last=&resources->duplicatedSolids;
    while (*last)last=&(*last)->next;
    *last=copy.release();
}

void ecstasyDeleteAllDuplicatedSolids(EditorMarkerResources* resources)
{
    if (resources->catalogueRoot)resources=resources->catalogueRoot;
    auto* copy=resources->duplicatedSolids;
    while (copy) {
        auto* next=copy->next;
        editorMarkerResourcesDetach(&copy->catalogue);
        // Native references must not retain a freed solid. The same live
        // eModel list is used; reconnection selects the surviving catalogue.
        for (auto* model=resources->first;model;model=model->next)
            if (model->solid==&copy->solid)editorMarkerModelReconnectSolid(model);
        delete copy;
        copy=next;
    }
    resources->duplicatedSolids=nullptr;
}

int32_t ecstasyModelNumVertices(const EditorMarkerModel* model)
{
    if (!model->solid)return 0;
    hp2::Require(model->solid->geometry!=nullptr,"Model has no decoded solid geometry");
    return model->solid->geometry->vertexCount;
}

void ecstasySolidSetEnvironmentMapping(const EditorMarkerSolid* solid,uint32_t textureKey,int32_t enabled)
{
    hp2::Require(solid->geometry!=nullptr,"Environment mapping has no decoded solid");
    auto& geometry=*solid->geometry;
    for (auto& strip:geometry.strips) {
        hp2::Require(strip.textureIndex>=0 && static_cast<std::size_t>(strip.textureIndex)<geometry.textures.size(),
            "Environment-mapped strip has an invalid texture selector");
        if (geometry.textures[static_cast<std::size_t>(strip.textureIndex)].key!=textureKey)continue;
        if (enabled)strip.flags=static_cast<std::uint16_t>(strip.flags|0x0100u);
        else strip.flags=static_cast<std::uint16_t>(strip.flags&0xfeffu);
    }
}

void ecstasyModelSetEnvironmentMapping(EditorMarkerModel* model,uint32_t textureKey,int32_t enabled)
{
    if (model->solid)ecstasySolidSetEnvironmentMapping(model->solid,textureKey,enabled);
}

void ecstasyCallDamageVertexFunction(EditorMarkerModel* model,BodyDamageModel* damage,
    EcstasyDamageVertexFunction function)
{
    if (!model->solid)return;
    hp2::Require(model->solid->geometry!=nullptr,"Damage traversal has no live solid geometry");
    std::int32_t vertexIndex=0;
    for (auto& strip:model->solid->geometry->strips) {
        const auto damageFlag=static_cast<std::int32_t>(strip.flags&0x0800u);
        hp2::Require(strip.colors.size()==strip.positions.size(),
            "Damage traversal requires the original RGB555 channel for every vertex");
        if(strip.nativeVertexRevision)strip.StampNativeVertexRevision();
        std::int32_t damaged=0;
        for (std::size_t vertex=0;vertex<strip.positions.size();++vertex) {
            // PAL unpacks XYZ, calls the real function, then packs XYZ back.
            // The native decoded arrays are already its canonical storage.
            auto position=strip.positions[vertex];
            damaged|=function(damage,vertexIndex++,position.data(),&strip.colors[vertex],nullptr,damageFlag);
            strip.positions[vertex]=position;
        }
        if (damageFlag) {
            if (damaged)strip.flags=static_cast<std::uint16_t>(strip.flags&0xfbffu);
            else strip.flags=static_cast<std::uint16_t>(strip.flags|0x0400u);
        }
    }
}

int32_t carCinematicCurrentViewMode(const CarCinematicServices* services)
{
    return services->presentationLayout;
}

void ecstasyViewInitialize(CarCinematicScreen* screen)
{
    std::memset(screen,0,sizeof(*screen));
    screen->viewScale=1.0f;
}

void ecstasyViewsBind(CarCinematicServices* services,CarCinematicView cameras[4],
    EcstasyRenderTarget targets[8])
{
    auto& first=services->screens[0];
    first.id=0;first.target=&targets[0];first.auxiliaryTarget=&targets[3];
    first.enabled=1;first.view=&cameras[0];
    auto& playerOne=services->screens[1];
    playerOne.id=1;playerOne.target=&targets[1];playerOne.auxiliaryTarget=&targets[4];
    playerOne.view=&cameras[1];
    auto& playerTwo=services->screens[2];
    playerTwo.id=2;playerTwo.target=&targets[2];playerTwo.auxiliaryTarget=&targets[5];
    playerTwo.view=&cameras[2];
    auto& mirror=services->screens[3];
    mirror.id=3;mirror.target=&targets[7];mirror.view=&cameras[3];
}

CarCinematicScreen* ecstasyViewAt(CarCinematicServices* services,int32_t index)
{
    // The original bank has ten records. Reject an invalid native binding
    // instead of forming an out-of-range host pointer.
    return index>=0 && index<10 ? services->screens+index : nullptr;
}

CarCinematicScreen* ecstasyViewCurrent(const CarCinematicServices* services)
{
    return services->currentScreen;
}

void ecstasyViewSetCurrent(CarCinematicServices* services,CarCinematicScreen* screen)
{
    services->currentScreen=screen;
}

CarCinematicView* ecstasyViewCurrentCamera(const CarCinematicServices* services)
{
    return services->currentScreen ? services->currentScreen->view : nullptr;
}

void ecstasyViewsUpdateMode(CarCinematicServices* services)
{
    const bool one=services->screens[1].last!=nullptr;
    const bool two=services->screens[2].last!=nullptr;
    const bool mirror=services->screens[3].last!=nullptr;
    services->presentationLayout=one && two ? 3 : one && mirror ? 2 : one ? 1 : 0;
}

void ecstasyViewsApplyMode(CarCinematicServices* services)
{
    // Complete layout switch of PAL107DB0..108048, reached by the actual
    // eDisplayFrame1128F4. GPU retirement is guaranteed by the synchronous
    // native presenter before this next-frame viewport mutation.
    hp2::Require(services && services->screens[0].target,
        "View layout has no actual presentation extent");
    services->appliedPresentationLayout=services->presentationLayoutOverride ?
        services->presentationLayoutOverride:services->presentationLayout;
    auto& one=services->screens[1];auto& two=services->screens[2];auto& mirror=services->screens[3];
    hp2::Require(one.target && two.target && mirror.target,"Original view target bank is incomplete");
    auto& first=*one.target;auto& second=*two.target;auto& rear=*mirror.target;
    auto& firstAux=one.auxiliaryTarget ? *one.auxiliaryTarget:first;
    auto& secondAux=two.auxiliaryTarget ? *two.auxiliaryTarget:second;
    const auto width=services->screens[0].target->width,height=services->screens[0].target->height;
    const auto rectangle=[](EcstasyRenderTarget& target,int32_t left,int32_t top,int32_t w,int32_t h) {
        target.left=left;target.top=top;target.width=w;target.height=h;
    };
    switch(services->appliedPresentationLayout) {
    case 3:
        rectangle(first,0,0,width,height/2);rectangle(second,0,height/2,width,height/2);
        rectangle(firstAux,0,0,width,height/2);rectangle(secondAux,0,height/2,width,height/2);
        rectangle(rear,0,0,0,0);first.enabled=second.enabled=1;one.enabled=two.enabled=1;
        rear.enabled=0;mirror.enabled=0;break;
    case 4:
        rectangle(first,0,0,width/2,height);rectangle(second,width/2,0,width/2,height);
        rectangle(firstAux,0,0,width/2,height);rectangle(secondAux,width/2,0,width/2,height);
        rectangle(rear,0,0,0,0);first.enabled=second.enabled=1;one.enabled=two.enabled=1;
        rear.enabled=0;mirror.enabled=0;break;
    case 2:
        rectangle(first,0,0,width,height);rectangle(second,0,0,0,0);
        rectangle(firstAux,0,0,width,height);rectangle(secondAux,0,0,0,0);
        rectangle(rear,0,0,256,64);first.enabled=1;one.enabled=1;
        second.enabled=0;two.enabled=0;rear.enabled=1;mirror.enabled=1;break;
    case 0:case 1:
        rectangle(first,0,0,width,height);rectangle(second,0,0,0,0);
        rectangle(firstAux,0,0,width,height);rectangle(secondAux,0,0,0,0);
        rectangle(rear,0,0,0,0);first.enabled=1;one.enabled=1;
        second.enabled=0;two.enabled=0;rear.enabled=0;mirror.enabled=0;break;
    default:break; // Source unsigned mode>=5 returns without layout writes.
    }
}

void ecstasyViewAttachCameraMover(CarCinematicServices* services,
    CarCinematicScreen* screen,CarCinematicBase* camera)
{
    camera->previous=screen->last;camera->next=nullptr;
    if (screen->last)screen->last->next=camera;else screen->first=camera;
    screen->last=camera;
    ecstasyViewsUpdateMode(services);
}

void ecstasyViewDetachCameraMover(CarCinematicServices* services,
    CarCinematicScreen* screen,CarCinematicBase* camera)
{
    if (camera->previous)camera->previous->next=camera->next;else screen->first=camera->next;
    if (camera->next)camera->next->previous=camera->previous;else screen->last=camera->previous;
    // Source unlink does not clear the removed node's own links.
    ecstasyViewsUpdateMode(services);
}

void ecstasyViewLocalToScreenClip(const CarCinematicScreen* screen,float output[16],
    const float localToWorld[16])
{
    eMulMatrix(output,localToWorld,screen->matrices.worldToScreenClip);
}

void ecstasyViewLocalToClip(const CarCinematicScreen* screen,float output[16],
    const float localToWorld[16])
{
    eMulMatrix(output,localToWorld,screen->matrices.worldToClip);
}

void ecstasyViewBiasMatrixForZSorting(const CarCinematicScreen* screen,
    float matrix[16],float scale)
{
    if (!screen->view)return;
    float adjustment[16];eIdentity(adjustment);
    const auto& eye=screen->view->current.eye;
    for (int axis=0;axis<4;++axis)
        adjustment[12+axis]=(matrix[12+axis]-eye[axis])*scale+eye[axis];
    adjustment[0]=adjustment[5]=adjustment[10]=scale;adjustment[15]=1.0f;
    matrix[12]=matrix[13]=matrix[14]=0.0f;matrix[15]=1.0f;
    eMulMatrix(matrix,matrix,adjustment);
}

int32_t ecstasyViewPixelSize(const CarCinematicScreen* screen,float size,float distance)
{
    const float result=size*0.5f<distance ? size*screen->pixelScale/distance :
        screen->pixelScale+screen->pixelScale;
    return convertFloatToInt32(result);
}

namespace {
int32_t viewPositionPixelsAtScale(const CarCinematicScreen* screen,
    const float position[3],float radius,float pixelScale)
{
    const auto& camera=screen->view->current;
    const float x=position[0]-camera.eye[0],y=position[1]-camera.eye[1],z=position[2]-camera.eye[2];
    float facing=x*camera.forward[0]+y*camera.forward[1];
    facing=facing+z*camera.forward[2];
    if (facing < -radius)return 0;
    const float distance=std::sqrt((x*x+y*y)+z*z)-radius;
    const float result=radius*0.5f<distance ? radius*pixelScale/distance : pixelScale+pixelScale;
    return convertFloatToInt32(result);
}
}

int32_t ecstasyViewPositionPixelSize(const CarCinematicScreen* screen,
    const float position[3],float radius)
{
    // A discarded source ticker read has no native rendering side effect.
    // Do not issue QueryPerformanceCounter once per object per camera.
    return viewPositionPixelsAtScale(screen,position,radius,screen->pixelScale);
}

int32_t ecstasyViewSourcePositionPixelSize(const CarCinematicScreen* screen,
    const float position[3],float radius)
{
    hp2::Require(screen && screen->view && screen->matricesCalculated &&
        std::isfinite(screen->sourcePixelScale) && screen->sourcePixelScale>0.0f,
        "Authored projected size requires the calculated source focal scale");
    return viewPositionPixelsAtScale(screen,position,radius,screen->sourcePixelScale);
}

int32_t ecstasyViewNativePositionPixelSize(const CarCinematicScreen* screen,
    const float position[3],float radius,int32_t sourceFocal)
{
    hp2::Require(screen && screen->view && screen->matricesCalculated,
        "Native visibility requires a calculated view");
    const float focal=sourceFocal ? screen->sourcePixelScale:screen->pixelScale;
    return viewPositionPixelsAtScale(screen,position,radius,
        focal*hp2::nativeViewDistanceScale(screen->id));
}

int32_t ecstasyViewBoundsPixelSize(const CarCinematicScreen* screen,
    const float first[3],const float second[3])
{
    const float dx=first[0]-second[0],dy=first[1]-second[1],dz=first[2]-second[2];
    const float radius=std::sqrt((dx*dx+dy*dy)+dz*dz)*0.5f;
    const auto& camera=screen->view->current;
    const float x=(first[0]+second[0])*0.5f-camera.eye[0];
    const float y=(first[1]+second[1])*0.5f-camera.eye[1];
    const float z=(first[2]+second[2])*0.5f-camera.eye[2];
    float facing=x*camera.forward[0]+y*camera.forward[1];facing=facing+z*camera.forward[2];
    if (facing < -radius)return 0;
    const float distance=std::sqrt((x*x+y*y)+z*z)-radius;
    // This overload uses radius, rather than radius/2, in the near test.
    float result=screen->pixelScale;
    if (radius<distance)result=radius*result/distance;
    return convertFloatToInt32(result);
}

void ecstasyViewScreenPosition(const CarCinematicScreen* screen,float output[3],
    const float position[3])
{
    const float point[4]={position[0],position[1],position[2],1.0f};
    float transformed[4];eRotTransPers(transformed,screen->matrices.worldToScreen,point);
    const auto* target=screen->target;
    output[2]=transformed[2];
    output[0]=transformed[0]+static_cast<float>(target->width/2);
    output[1]=transformed[1]+static_cast<float>(target->height/2);
}

float ecstasyViewScreenWidth(const CarCinematicScreen* screen)
{
    return static_cast<float>(screen->target->width);
}

float ecstasyViewScreenHeight(const CarCinematicScreen* screen)
{
    return static_cast<float>(screen->target->height);
}

const char* ecstasyViewIdName(int32_t id)
{
    static const char* const names[]={"EVIEW_FLAILER","EVIEW_PLAYER1","EVIEW_PLAYER2",
        "EVIEW_PLAYER1_RVM","EVIEW_ENVMAP0F","EVIEW_ENVMAP0R","EVIEW_ENVMAP0B",
        "EVIEW_ENVMAP0L","EVIEW_ENVMAP0U","EVIEW_ENVMAP0D"};
    if (id==-1)return "EVIEW_NONE";
    return id>=0 && id<10 ? names[id] : "EVIEW_UNKNOWN(?)";
}

void ecstasyViewProjectionInitialize(EcstasyViewProjectionSettings* settings,
    int32_t horizontalResolution)
{
    settings->referenceFieldOfView=16384;settings->splitFieldOfView=9102;
    settings->horizontalResolution=horizontalResolution;
    settings->horizontalClipScale=0.5f;settings->verticalClipScale=0.5f;
    // Native projection uses centre-relative pixels; the GS raster-origin
    // bias is neither part of an asset nor needed by a PC rasterizer.
    settings->horizontalClipOffset=0.0f;settings->verticalClipOffset=0.0f;
    settings->nativeAspectCorrection=0;settings->nativeSourceVideoMode=0;
    // Released projection endpoint values, converted once from their source
    // representation into ordinary numeric depth units. PC rendering uses
    // these values directly and retains D32 interpolation precision.
    const float ranges[2][6]={{1.0f,65280.0f,4.0f,65535.0f,255.0f,65535.0f},
        {1.0f,524288.0f,4.0f,524800.0f,589824.0f,983040.0f}};
    for(std::size_t row=0;row<2;++row)std::copy_n(ranges[row],6,settings->depthRanges[row]);
}

namespace {
float halfAngleTangent(uint16_t fieldOfView)
{
    const auto half=static_cast<uint16_t>(fieldOfView>>1);
    return rotationSineAngle(half)/vectorCosineAngle(half);
}

void packProjection(EcstasyProjectionPair& output,const float matrix[16])
{
    float inverse[16];afxMatrixInverse(inverse,matrix);
    for (int axis=0;axis<3;++axis) {
        output.diagonal[axis]=matrix[axis*5];
        output.inverseDiagonal[axis]=inverse[axis*5];
    }
    for (int axis=0;axis<4;++axis) {
        output.translation[axis]=matrix[12+axis];
        output.inverseTranslation[axis]=inverse[12+axis];
    }
}

void updateTargetContexts(EcstasyRenderTarget& target,const EcstasyViewMatrices& matrices)
{
    const EcstasyProjectionPair* projections[]={&matrices.screenProjection,&matrices.nearProjection,
        &matrices.frustumProjection};
    target.viewMatrices=&matrices;
    for (int index=0;index<3;++index) {
        auto& context=target.contexts[index];
        std::memcpy(context.worldToClip,matrices.worldToClip,sizeof(context.worldToClip));
        context.projection=*projections[index];
        eTransposeMatrix(context.transposedWorldToView,matrices.worldToView);
    }
}
}

CarCinematicResult ecstasyViewCalculateMatrices(CarCinematicServices* services,
    CarCinematicScreen* screen,float nearOverride,float farOverride,float clipFarOverride)
{
    if (!services || !screen || !screen->view || !screen->target ||
        services->projection.horizontalResolution<=0)return CAR_CINEMATIC_INVALID_BINDING;
    const auto& settings=services->projection;
    const auto& camera=screen->view->current;
    auto& target=*screen->target;
    float nearPlane=camera.nearClip,farPlane=camera.farClip;
    float ratio=halfAngleTangent(settings.referenceFieldOfView)/halfAngleTangent(camera.fieldOfView);
    target.fieldOfViewScale=ratio;
    if (screen->auxiliaryTarget)screen->auxiliaryTarget->fieldOfViewScale=ratio;
    // Source GetFirst is the tail of the native oldest-to-newest stack.
    const auto* first=screen->last;
    const bool exempt=first && (first->kind==1 || first->kind==5);
    if ((screen->id==1 || screen->id==2) && !exempt) {
        const float span=farPlane-nearPlane;
        ratio=halfAngleTangent(settings.splitFieldOfView)/halfAngleTangent(camera.fieldOfView);
        nearPlane=nearPlane*ratio;farPlane=nearPlane+span;
    }
    if (nearOverride!=0.0f)nearPlane=nearOverride;
    if (farOverride!=0.0f)farPlane=farOverride;
    // Extend the actual world/mirror frustum, not the physical Camera or its
    // simulation state. Sky overrides and cubemap cameras keep their ranges.
    if(nearOverride==0.0f && farOverride==0.0f)
        farPlane*=hp2::nativeViewDistanceScale(screen->id);
    screen->nearClip=nearPlane;screen->farClip=farPlane;
    const float tangent=halfAngleTangent(camera.fieldOfView);
    screen->pixelScale=tangent!=0.0f ? static_cast<float>(settings.horizontalResolution/2)/tangent : 10.0f;
    screen->sourcePixelScale=screen->pixelScale;
    screen->fieldOfViewDegrees=static_cast<float>(camera.fieldOfView)*0.0054931640625f;
    float rasterScaleX=target.horizontalScale,rasterScaleY=target.verticalScale;
    const bool nativeProjection=settings.nativeAspectCorrection!=0 && screen->id>0;
    if(nativeProjection && tangent!=0.0f) {
        screen->sourcePixelScale=256.0f/tangent;
        // PAL103124..10316C publishes a512-wide source display. Its
        // SetScreenBuffers1064C8 uses a distinct TV-pixel vertical scale:
        // PAL1.285/512 high, NTSC1.0714286566/448 high. Preserve that
        // vertical framing on square PC pixels and reveal additional world
        // horizontally, instead of cropping it as the window gets wider.
        // Camera eye, authored FOV, chase distance and gameplay stay intact.
        if(screen->id==1 || screen->id==2) {
            const bool ntsc=settings.nativeSourceVideoMode==1;
            const float sourceHeight=ntsc?448.0f:512.0f;
            const float sourceScale=ntsc?1.071428656578064f:1.284999966621399f;
            screen->pixelScale=((256.0f/tangent)*sourceScale)*static_cast<float>(target.height)/sourceHeight;
            rasterScaleX=rasterScaleY=1.0f;
            // The source directional-strip shortcut must not reject the
            // additional horizontal field exposed by a widescreen display.
            const float effective=2.0f*std::atan(static_cast<float>(target.width)/(2.0f*screen->pixelScale))*57.29577951308232f;
            screen->fieldOfViewDegrees=std::max(screen->fieldOfViewDegrees,effective);
        } else {
            // Original256x64 mirror and64-square cubemap targets do not
            // grow with the PC window. Their source focal input stays512.
            screen->pixelScale=256.0f/tangent;
            rasterScaleX=screen->id==3?1.5f:1.0f;
            rasterScaleY=screen->id==3?1.0800000429153442f:1.0f;
        }
    }
    std::memcpy(screen->forward,camera.forward,sizeof(screen->forward));
    const float inversePixel=1.0f/screen->pixelScale;
    const auto* range=settings.depthRanges[target.format==0x31 ? 1 : 0];
    const float span=farPlane-nearPlane,product=farPlane*nearPlane;
    const float depthSlope=((farPlane*range[0]+(-range[1])*nearPlane)/span)/screen->pixelScale;
    const float depthOffset=(product*(range[1]-range[0])/span)/screen->pixelScale;
    const float horizontalSlope=((farPlane*range[2]+(-range[3])*nearPlane)/span)/screen->pixelScale;
    const float horizontalOffset=(product*(range[3]-range[2])/span)/screen->pixelScale;
    const float verticalSlope=((farPlane*range[4]+(-range[5])*nearPlane)/span)/screen->pixelScale;
    const float verticalOffset=(product*(range[5]-range[4])/span)/screen->pixelScale;
    const float xOffset=settings.horizontalClipOffset/screen->pixelScale;
    const float yOffset=settings.verticalClipOffset/screen->pixelScale;
    float screenProjection[16]={rasterScaleX,0,0,0,0,rasterScaleY,0,0,
        xOffset,yOffset,depthSlope,inversePixel,0,0,depthOffset,0};
    float nearProjection[16];std::memcpy(nearProjection,screenProjection,sizeof(nearProjection));
    nearProjection[10]=horizontalSlope;nearProjection[14]=horizontalOffset;
    float frustumProjection[16];std::memcpy(frustumProjection,screenProjection,sizeof(frustumProjection));
    frustumProjection[10]=verticalSlope;frustumProjection[14]=verticalOffset;
    const float width=static_cast<float>(target.width),height=static_cast<float>(target.height);
    const float xClip=width*settings.horizontalClipScale,yClip=height*settings.verticalClipScale;
    float xExtent=width*0.5f,yExtent=height*0.5f;
    if (xClip<xExtent)xExtent=xClip;
    if (yClip<yExtent)yExtent=yClip;
    const float nativeScaleX=nativeProjection?rasterScaleX:1.0f;
    const float nativeScaleY=nativeProjection?rasterScaleY:1.0f;
    const float nearX=(nearPlane*xClip)/(screen->pixelScale*nativeScaleX);
    const float nearY=(nearPlane*yClip)/(screen->pixelScale*nativeScaleY);
    float clip[16];eIdentity(clip);
    clip[0]=(nearPlane+nearPlane)/(nearX+nearX);
    clip[5]=(nearPlane+nearPlane)/(nearY+nearY);
    clip[10]=(farPlane+nearPlane)/span;clip[11]=1.0f;
    clip[14]=(product*-2.0f)/span;clip[15]=0.0f;
    float inverse[16];eIdentity(inverse);
    inverse[0]=1.0f/clip[0];inverse[5]=1.0f/clip[5];
    inverse[10]=0.0f;inverse[11]=1.0f/clip[14];
    inverse[14]=1.0f;inverse[15]=(-clip[10])/clip[14];
    float clipToScreen[16],clipToNear[16],clipToFrustum[16];
    eMulMatrix(clipToScreen,inverse,screenProjection);
    eMulMatrix(clipToNear,inverse,nearProjection);
    eMulMatrix(clipToFrustum,inverse,frustumProjection);
    float screenClip[16];eIdentity(screenClip);
    const float clipFar=clipFarOverride!=0.0f ? clipFarOverride : farPlane;
    const float clipX=nearPlane*xExtent/(screen->pixelScale*nativeScaleX);
    const float clipY=nearPlane*yExtent/(screen->pixelScale*nativeScaleY);
    screenClip[0]=(nearPlane+nearPlane)/(clipX+clipX);
    screenClip[5]=(nearPlane+nearPlane)/(clipY+clipY);
    screenClip[10]=(clipFar+nearPlane)/(clipFar-nearPlane);screenClip[11]=1.0f;
    screenClip[14]=((clipFar*nearPlane)*-2.0f)/(clipFar-nearPlane);screenClip[15]=0.0f;
    // Final10EBF4 multiplication is observable in its float rounding.
    eMulMatrix(screenProjection,clip,clipToScreen);
    auto& matrices=screen->matrices;
    std::memcpy(matrices.worldToView,camera.matrix,sizeof(matrices.worldToView));
    std::memcpy(matrices.clipToScreen,clipToScreen,sizeof(matrices.clipToScreen));
    eMulMatrix(matrices.worldToClip,matrices.worldToView,clip);
    eMulMatrix(matrices.worldToScreen,matrices.worldToView,screenProjection);
    eMulMatrix(matrices.worldToScreenClip,matrices.worldToView,screenClip);
    packProjection(matrices.screenProjection,clipToScreen);
    packProjection(matrices.nearProjection,clipToNear);
    packProjection(matrices.frustumProjection,clipToFrustum);
    updateTargetContexts(target,matrices);
    if (screen->auxiliaryTarget)updateTargetContexts(*screen->auxiliaryTarget,matrices);
    screen->matricesCalculated=1;
    return CAR_CINEMATIC_OK;
}

uint32_t hashBytes(const void* bytes, size_t length)
{
    const uint8_t* source = (const uint8_t*)bytes;
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < length; ++i)
        value = value * 33u + source[i];
    return value;
}

CarCinematicScreen* screenRadialBlurScreen(ScreenRadialBlur* radialBlur, int32_t seat)
{
    if (!radialBlur || !radialBlur->cameras || seat < 0 || seat >= 3) return NULL;
    const GameSession* session = radialBlur->cameras->session;
    if (session && session->state == 4 && session->viewSelection) {
        if (seat != session->viewSelection->seat) return NULL;
        return &radialBlur->cameras->screens[1];
    }
    return &radialBlur->cameras->screens[seat + 1];
}

void screenRadialBlurStartScreen(ScreenRadialBlur* radialBlur, CarCinematicScreen* screen)
{
    if (!radialBlur || !radialBlur->cameras || !screen) return;
    const GameSession* session = radialBlur->cameras->session;
    if (session && session->state == 7) return;
    const ptrdiff_t index = screen - radialBlur->cameras->screens - 1;
    if (index < 0 || index >= 4) return;
    screen->radialBlurTexture = radialBlur->texture;
    radialBlur->firstAmount[index] = 0;
    radialBlur->secondAmount[index] = 160;
}

void screenRadialBlurStart(ScreenRadialBlur* radialBlur, int32_t seat)
{
    CarCinematicScreen* screen = screenRadialBlurScreen(radialBlur, seat);
    if (screen) screenRadialBlurStartScreen(radialBlur, screen);
}

void screenRadialBlurStop(ScreenRadialBlur* radialBlur, int32_t seat)
{
    CarCinematicScreen* screen = screenRadialBlurScreen(radialBlur, seat);
    if (screen) screenRadialBlurStopScreen(screen);
}

void screenRadialBlurSetAmounts(ScreenRadialBlur* radialBlur, int32_t seat, float first, float second)
{
    if (!radialBlur || seat < 0 || seat >= 4) return;
    radialBlur->firstAmount[seat] = (uint8_t)(int32_t)(first * 255.0f);
    radialBlur->secondAmount[seat] = (uint8_t)(int32_t)(second * 255.0f);
}

// PAL10F1A8..10F1B0: revoke the borrowed source texture when blur stops.
void screenRadialBlurStopScreen(CarCinematicScreen* screen)
{
    screen->radialBlurTexture = nullptr;
}

void editorMarkerSolidBounds(const EditorMarkerSolid* solid,float minimum[3],float maximum[3])
{
    const float minimumX=solid->minimum[0];
    const float minimumY=solid->minimum[1];
    const float minimumZ=solid->minimum[2];
    const float maximumZ=solid->maximum[2];
    const float maximumX=solid->maximum[0];
    const float maximumY=solid->maximum[1];
    minimum[2]=minimumZ;
    minimum[0]=minimumX;
    minimum[1]=minimumY;
    maximum[2]=maximumZ;
    maximum[0]=maximumX;
    maximum[1]=maximumY;
}

int32_t ecstasyClassifyBounds(const float minimum[3],const float maximum[3],const float matrix[16])
{
#if defined(_M_X64) || defined(__SSE2__)
    // Native x64 SIMD evaluates the same four component lanes together.
    // Keep the original base + X/Y/Z edge order (including negative W and
    // boundary rounding); replacing it with extracted planes is not equivalent.
    const auto mx=_mm_loadu_ps(matrix),my=_mm_loadu_ps(matrix+4),mz=_mm_loadu_ps(matrix+8);
    const auto first=_mm_add_ps(_mm_mul_ps(mx,_mm_set1_ps(minimum[0])),
        _mm_mul_ps(my,_mm_set1_ps(minimum[1])));
    const auto base=_mm_add_ps(_mm_add_ps(first,_mm_mul_ps(mz,_mm_set1_ps(minimum[2]))),
        _mm_loadu_ps(matrix+12));
    const auto xStep=_mm_mul_ps(mx,_mm_set1_ps(maximum[0]-minimum[0]));
    const auto yStep=_mm_mul_ps(my,_mm_set1_ps(maximum[1]-minimum[1]));
    const auto zStep=_mm_mul_ps(mz,_mm_set1_ps(maximum[2]-minimum[2]));
    const __m128 corners[4]={base,_mm_add_ps(base,xStep),
        _mm_add_ps(_mm_add_ps(base,xStep),yStep),_mm_add_ps(base,yStep)};
    const auto sign=_mm_set1_ps(-0.0f);
    std::uint32_t any=0,common=63;
    for(int layer=0;layer<2;++layer)for(int corner=0;corner<4;++corner) {
        const auto point=layer ? _mm_add_ps(corners[corner],zStep):corners[corner];
        const auto limit=_mm_andnot_ps(sign,_mm_shuffle_ps(point,point,_MM_SHUFFLE(3,3,3,3)));
        const auto positive=static_cast<std::uint32_t>(_mm_movemask_ps(_mm_cmpgt_ps(point,limit)))&7u;
        const auto negative=static_cast<std::uint32_t>(_mm_movemask_ps(_mm_cmplt_ps(point,_mm_xor_ps(limit,sign))))&7u;
        const auto flags=positive|(negative<<3u);
        any|=flags;common&=flags;
        // Once two corners disagree, later corners cannot restore a common
        // outside plane or remove an already-observed outside component.
        if(any && !common)return 1;
    }
    return any==0 ? 2:common==0 ? 1:0;
#else
    float base[4],xStep[4],yStep[4],zStep[4];
    const float x=maximum[0]-minimum[0],y=maximum[1]-minimum[1],z=maximum[2]-minimum[2];
    for (int axis=0;axis<4;++axis) {
        const float first=matrix[axis]*minimum[0]+matrix[4+axis]*minimum[1];
        base[axis]=(first+matrix[8+axis]*minimum[2])+matrix[12+axis];
        xStep[axis]=matrix[axis]*x;yStep[axis]=matrix[4+axis]*y;zStep[axis]=matrix[8+axis]*z;
    }
    // Source constructs a base corner and three edge deltas, then clips
    // the eight additions. Re-transforming eight positions changes rounding.
    float corners[4][4];
    for (int axis=0;axis<4;++axis) {
        corners[0][axis]=base[axis];
        corners[1][axis]=base[axis]+xStep[axis];
        corners[2][axis]=corners[1][axis]+yStep[axis];
        corners[3][axis]=base[axis]+yStep[axis];
    }
    uint32_t any=0,common=63;
    for (int layer=0;layer<2;++layer)for (int corner=0;corner<4;++corner) {
        float point[4];
        for (int axis=0;axis<4;++axis)point[axis]=layer ? corners[corner][axis]+zStep[axis] : corners[corner][axis];
        const float limit=std::fabs(point[3]);uint32_t flags=0;
        for (int axis=0;axis<3;++axis) {
            if (point[axis]>limit)flags|=1u<<(axis*2);
            if (point[axis]<-limit)flags|=2u<<(axis*2);
        }
        any|=flags;common&=flags;
    }
    return any==0 ? 2 : common==0 ? 1 : 0;
#endif
}

namespace {
bool stripFacingVisible(const EditorMarkerSolidGeometry::Strip& strip,const float facing[4],
    float halfFieldOfView,bool forceMode)
{
    const float x=static_cast<float>(strip.cullDirectionXY[0])*0.00006103515625f;
    const float y=static_cast<float>(strip.cullDirectionXY[1])*0.00006103515625f;
    const float z=static_cast<float>(strip.autoKProjection)*0.00006103515625f;
    float dot=x*facing[0]+y*facing[1];dot=dot+z*facing[2];
    const auto sineAngle=vectorArcSineAngle(dot);
    const auto angle=static_cast<uint16_t>(16384u-sineAngle);
    const float degrees=static_cast<float>(angle)*0.0054931640625f;
    const int32_t authoredAngle=strip.autoKAngle<32768u ? strip.autoKAngle :
        static_cast<int32_t>(strip.autoKAngle)-65536;
    // Actual10D1C8 scales the unsigned angle in mode2.10D2D8 instead
    // passes the signed lane without this scale in mode1/3.
    const float cone=forceMode ? static_cast<float>(strip.autoKAngle)*0.0054931640625f :
        static_cast<float>(authoredAngle);
    const float threshold=(90.0f-halfFieldOfView)-cone;
    return !(degrees<threshold);
}

void classifyStrips(std::vector<EcstasyVisibleStrip>& output,const EditorMarkerModel& model,
    const CarCinematicScreen& screen,const float localToClip[16],const float localToWorld[16],
    int32_t mode,bool angular)
{
    const auto& strips=model.solid->geometry->strips;
    float facing[4]{};
    if (angular) {
        // PAL10D0AC..10D0F0 transforms the world direction by the general
        // inverse. Scaled and quantized scenery matrices need not be orthogonal.
        float inverse[16];afxMatrixInverse(inverse,localToWorld);
        const float negativeForward[4]={-screen.forward[0],-screen.forward[1],-screen.forward[2],0};
        eMulVector4(facing,inverse,negativeForward);
    }
    if (mode!=1 && mode!=2 && mode!=3)return;
    const int32_t intersectClass=mode==3 ? 2 : mode;
    const auto classify=[&](std::size_t index) {
        const auto& strip=strips[index];
        int32_t classification=2;
        if (mode!=2) {
            classification=ecstasyClassifyBounds(strip.minimum.data(),strip.maximum.data(),localToClip);
            if (classification==1)classification=intersectClass;
        }
        if (!classification)return int32_t{0};
        if (angular && (strip.flags&0x1000u) &&
            !stripFacingVisible(strip,facing,screen.fieldOfViewDegrees*0.5f,mode==2))return int32_t{0};
        return classification;
    };
    // The final loop never assigns its previous-class register after
    // resetting it to-1. Preserve every original one-strip run and its
    // index order; only the independent bound/cone calculations run apart.
    if(strips.size()<512 || hp2::NativeSceneEffectiveHardwareConcurrency()<2) {
        for(std::size_t index=0;index<strips.size();++index) {
            const auto classification=classify(index);
            if(classification)output.push_back({index,1,classification});
        }
        return;
    }
    output.resize(strips.size());
    hp2::RunNativeSceneJobs(strips.size(),[&](std::size_t index) {
        const auto classification=classify(index);
        output[index]={index,static_cast<std::uint16_t>(classification ? 1:0),classification};
    },512);
    output.erase(std::remove_if(output.begin(),output.end(),
        [](const EcstasyVisibleStrip& run){return run.count==0;}),output.end());
}
}

bool ecstasyDetermineVisibleStrips(std::vector<EcstasyVisibleStrip>& output,
    const EditorMarkerModel& model,const CarCinematicScreen& screen,
    const float localToWorld[16],bool forceVisible,int32_t mode)
{
    output.clear();
    if (!model.solid)return false;
    hp2::Require(model.solid->geometry!=nullptr,"Visible model has no same-owner decoded geometry");
    const auto count=model.solid->geometry->strips.size();
    const auto all=[&](int32_t classification) {
        if (count)output.push_back({0,static_cast<uint16_t>(count),classification});
    };
    float screenClip[16],clip[16];
    if (mode==2) {
        if (model.solid->renderFlags&0x1000u) {
            ecstasyViewLocalToClip(&screen,clip,localToWorld);
            classifyStrips(output,model,screen,clip,localToWorld,2,true);
        } else all(2);
    } else {
        ecstasyViewLocalToScreenClip(&screen,screenClip,localToWorld);
        int32_t classification=ecstasyClassifyBounds(model.solid->minimum,model.solid->maximum,screenClip);
        if (!classification)return false;
        if (classification==1) {
            ecstasyViewLocalToClip(&screen,clip,localToWorld);
            classification=ecstasyClassifyBounds(model.solid->minimum,model.solid->maximum,clip);
        }
        if (forceVisible || classification==2)all(classification);
        else if (classification==1) {
            const bool angular=(model.solid->renderFlags&0x1000u)!=0;
            classifyStrips(output,model,screen,angular ? clip : screenClip,localToWorld,1,angular);
        }
    }
    return !output.empty();
}

void editorMarkerModelBounds(const EditorMarkerModel* model,float minimum[3],float maximum[3])
{
    if (model->solid) editorMarkerSolidBounds(model->solid,minimum,maximum);
}

const EditorMarkerRigidAnimation* editorMarkerModelRigidAnimation(const EditorMarkerModel* model)
{
    return model->solid ? model->solid->rigidAnimation : nullptr;
}

void editorMarkerModelSetRigidAnimationFrame(EditorMarkerModel* model,int32_t frame,int32_t worldFrames)
{
    const uint32_t start = (static_cast<uint32_t>(worldFrames) -
        static_cast<uint32_t>(frame) * 2u) & 0x3fffu;
    model->rigidAnimationStart = static_cast<uint16_t>(start | 0x8000u);
}

void editorMarkerModelSetupVertexAnimation(EditorMarkerModel* model,int32_t worldFrames)
{
    if(!model->solid || !model->solid->vertexAnimation)return;
    const auto& animation=*model->solid->vertexAnimation;
    hp2::Require(animation.count>0 && animation.count<=64 && animation.framesPerSecond>0,
        "Vertex animation has no authored frame sequence");
    // PAL10DB70..10DBCC selects discrete geometry at30fps from the60Hz
    // world clock. The low14-bit start is shared with rigid animation. Use
    // native64-bit time arithmetic rather than a console multiply/divider;
    // preserve the source unsigned remainder choice for a negative start.
    const auto ticks=static_cast<int64_t>(worldFrames)-static_cast<int64_t>(model->rigidAnimationStart&0x3fffu);
    const auto sample=(ticks*static_cast<int64_t>(animation.framesPerSecond))/60;
    const auto remainder=sample%static_cast<int64_t>(animation.count);
    const auto index=static_cast<uint32_t>(remainder)%animation.count;
    hp2::Require(animation.frames[index]!=nullptr,"Vertex animation frame is unowned");
    model->solid=animation.frames[index];
}

int32_t editorMarkerModelRigidAnimationTime(const EditorMarkerModel* model,int32_t worldFrames)
{
    const uint16_t start = model->rigidAnimationStart;
    if (!start) return -1;
    const EditorMarkerRigidAnimation* animation = editorMarkerModelRigidAnimation(model);
    if (!animation || animation->frameCount <= 0 || animation->frameDuration <= 0) return -1;
    const int32_t time = static_cast<int32_t>((static_cast<uint32_t>(worldFrames) - start) & 0x3fffu);
    const int32_t duration = animation->frameCount * animation->frameDuration;
    if (start & 0x4000u) return time % duration;
    return time < duration ? time : -1;
}

#include "indep/inc/Ecstasy.hpp"
#include "bware/inc/bWare.hpp"
#include "bware/inc/bWare.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <map>
#include <string>

namespace hp2 {
namespace {
std::string text(const std::uint8_t* bytes,std::size_t capacity)
{
    const auto* end=std::find(bytes,bytes+capacity,std::uint8_t(0));
    Require(end!=bytes+capacity,"Editor asset name is not terminated");
    return {reinterpret_cast<const char*>(bytes),static_cast<std::size_t>(end-bytes)};
}

float real(const std::uint8_t* bytes)
{
    const std::uint32_t bits=ReadU32(bytes);
    float value;
    std::memcpy(&value,&bits,sizeof(value));
    Require(std::isfinite(value),"Editor model has a non-finite bound");
    return value;
}

std::uint64_t decodeStripDrawDisableMask(const std::uint8_t* packet,std::size_t count)
{
    // Final PAL vuiReferenceStripper2A76E0 and its two transform workers
    // transport header X/Z/W through RINIT/RNEXT into XYZF2.W bit15 (ADC).
    // Y belongs to texture state. A reseed follows three already computed
    // position-W lanes; the native format decoder preserves that ordering.
    Require(count>0 && count<=48,"Solid strip exceeds the original 48-vertex packet schedule");
    const auto padded=(count+3u)&~std::size_t(3u);
    Require(packet[5]==padded/4u-1u,"Solid strip packet loop count disagrees with its vertex extent");
    const auto prefix=padded%16u;
    const std::size_t firstLane=(padded<16u || prefix) ? 0u:(padded==32u ? 2u:(padded==48u ? 0u:3u));
    auto random=ReadU32(packet+12+firstLane*4)&0x7fffffu;
    std::uint64_t mask{};
    for (std::size_t vertex=0;vertex<count;++vertex) {
        if (padded>32u && vertex==(prefix ? prefix:16u)+3u)
            random=ReadU32(packet+20)&0x7fffffu;
        if (padded>16u && vertex==padded-16u+3u)
            random=ReadU32(packet+24)&0x7fffffu;
        random=((random<<1)^((random>>4)&1u)^((random>>22)&1u))&0x7fffffu;
        mask|=std::uint64_t((random>>15)&1u)<<vertex;
    }
    return mask;
}

std::unique_ptr<EditorMarkerSolidGeometry> decodeSolidGeometry(const std::vector<std::uint8_t>& bytes,
    const std::uint8_t* header,const Chunk& table,const Chunk& packets,const Chunk& textures,const Chunk& materials)
{
    auto geometry=std::make_unique<EditorMarkerSolidGeometry>();
    geometry->vertexCount=static_cast<std::int16_t>(ReadU16(header+0x38));
    Require(geometry->vertexCount>=0,"Solid vertex count is outside its signed extent");
    if (textures.id) {
        Require(textures.size%8==0,"Solid texture table has a partial binding");
        for (std::size_t offset=0;offset<textures.size;offset+=8)
            geometry->textures.push_back({ReadU32(bytes.data()+textures.payloadOffset+offset),nullptr});
    }
    const auto strips=ReadU16(header+0x34);
    Require(strips<=32767,"Solid strip count is outside its signed extent");
    if (!strips)return geometry;
    Require(table.id && packets.id,"Solid strips are missing their real packet streams");
    const auto tableBegin=(table.payloadOffset+15u)&~std::size_t(15u);
    const auto packetBegin=(packets.payloadOffset+15u)&~std::size_t(15u);
    Require(tableBegin<=table.endOffset && std::size_t(strips)*64<=table.endOffset-tableBegin,
        "Solid strip table is truncated");
    const auto read=[&](std::size_t offset) {
        Require(offset<=packets.endOffset && packets.endOffset-offset>=4,"Solid vertex stream is truncated");
        return real(bytes.data()+offset);
    };
    for (std::size_t index=0;index<strips;++index) {
        const auto* record=bytes.data()+tableBegin+index*64;
        EditorMarkerSolidGeometry::Strip strip{};
        std::memcpy(&strip.textureIndex,record,1);
        for (std::size_t axis=0;axis<3;++axis) {
            strip.minimum[axis]=real(record+0x10+axis*4);
            strip.maximum[axis]=real(record+0x20+axis*4);
        }
        strip.flags=ReadU16(record+0xe);
        for (std::size_t axis=0;axis<2;++axis) {
            const auto word=ReadU16(record+0x30+axis*2);
            std::memcpy(&strip.cullDirectionXY[axis],&word,sizeof(word));
        }
        strip.textureScales={ReadU16(record+4),ReadU16(record+6)};
        const auto projection=ReadU16(record+0x34);std::memcpy(&strip.autoKProjection,&projection,2);
        strip.autoKAngle=ReadU16(record+0x36);strip.autoKScale=real(record+0x38);
        const auto packet=packetBegin+ReadU32(record+8);
        Require(packet>=packetBegin && packet<=packets.endOffset && packets.endOffset-packet>=28,
            "Solid strip points outside its retained packet chunk");
        const std::size_t count=bytes[packet+4];
        Require(count==record[0x1d],"Strip and packet vertex counts disagree");
        strip.drawDisableMask=decodeStripDrawDisableMask(bytes.data()+packet,count);
        std::size_t emittedPolygons{};
        for (std::size_t vertex=2;vertex<count;++vertex)
            if (!(strip.drawDisableMask&(std::uint64_t(1)<<vertex)))++emittedPolygons;
        Require(emittedPolygons==record[0x1e],"Solid strip ADC topology disagrees with its authored polygon count");
        // RenderStrips10E01C reads the signed material selector stored in
        // the strip header's otherwise unused bounds-W lane, not VIFbyte7.
        std::memcpy(&strip.lightMaterialIndex,record+0x1f,1);
        strip.positions.resize(count);strip.textureCoordinates.resize(count);strip.colors.resize(count);
        const auto fullPositions=(count/4)*4;
        const auto stBegin=packet+28+count*12+4+(count>=4 && count%4 ? 4 : 0);
        const auto colorBegin=stBegin+count*8+8+(count>=2 && count%2 ? 4 : 0);
        Require(colorBegin<=packets.endOffset && count*2<=packets.endOffset-colorBegin,
            "Solid RGB555 stream is truncated");
        for (std::size_t vertex=0;vertex<count;++vertex) {
            auto xyz=packet+28;
            const auto lane=vertex%4;
            if (vertex<fullPositions) {
                xyz+=(vertex/4)*48+4;
                for (std::size_t axis=0;axis<3;++axis)
                    strip.positions[vertex][axis]=read(xyz+axis*16+lane*4);
            } else {
                if (count>=4)xyz+=(vertex/4)*48+4;
                xyz+=4;
                for (std::size_t axis=0;axis<3;++axis)
                    strip.positions[vertex][axis]=read(xyz+(axis*(count%4)+lane)*4);
            }
            std::size_t s,t;
            if (vertex<(count/2)*2) {
                const auto at=stBegin+(vertex/2)*16+4;
                s=at+(vertex%2 ? 12 : 0);t=at+(vertex%2 ? 8 : 4);
            } else {
                s=stBegin+(count>=2 ? (vertex/2)*16+4 : 0)+4;t=s+4;
            }
            strip.textureCoordinates[vertex]={read(s),read(t)};
            strip.colors[vertex]=ReadU16(bytes.data()+colorBegin+vertex*2);
        }
        if(strip.flags&0x1c0u) {
            const auto normalCommand=(colorBegin+count*2+3u)&~std::size_t(3u);
            Require(normalCommand<=packets.endOffset && packets.endOffset-normalCommand>=4,
                "Lit solid strip has no authored normal stream");
            const auto command=ReadU32(bytes.data()+normalCommand);
            const auto vectors=3u*((count+3u)/4u);
            Require((command&0xff00ffffu)==0x6e00805cu && ((command>>16)&255u)==vectors,
                "Solid normal stream differs from released signed V4_8 TOPS+5C");
            Require(vectors*4<=packets.endOffset-normalCommand-4,
                "Solid signed normal payload is truncated");
            strip.normals.resize(count);
            constexpr float normalScale=0.007874015718698502f; // PAL VU immediate3C010204
            for(std::size_t vertex=0;vertex<count;++vertex)
                for(std::size_t axis=0;axis<3;++axis) {
                    std::int8_t value;
                    std::memcpy(&value,bytes.data()+normalCommand+4+(vertex/4)*12+axis*4+vertex%4,1);
                    strip.normals[vertex][axis]=static_cast<float>(value)*normalScale;
                }
        }
        // Keep the original decoded strip immutable and prepare its native
        // index topology once, not once per camera, lighting or alpha pass.
        strip.PrepareNativeTopology();strip.StampNativeVertexRevision();
        geometry->strips.push_back(std::move(strip));
    }
    std::size_t decodedVertices=0;
    for (const auto& strip:geometry->strips)decodedVertices+=strip.positions.size();
    Require(decodedVertices==static_cast<std::size_t>(geometry->vertexCount),
        "Solid vertex header does not match its decoded strips");
    const auto count=ReadU16(header+0xae);
    if (count) {
        Require(materials.id!=0,"Solid light-material table is missing");
        const auto start=(materials.payloadOffset+15u)&~std::size_t(15u);
        Require(start<=materials.endOffset && std::size_t(count)*8<=materials.endOffset-start,
            "Solid light-material bindings are truncated");
        for (std::size_t index=0;index<count;++index)
            geometry->lightMaterialKeys.push_back(ReadU32(bytes.data()+start+index*8));
    }
    return geometry;
}
} // namespace

TrackRouteEditorResources::TrackRouteEditorResources(const std::vector<std::uint8_t>& ingame,
    EditorMarkerResources* registry)
    : authored_(ingame)
{
    std::map<std::uint32_t,std::uint32_t> inheritedAnimations;
    std::vector<std::pair<std::uint32_t,std::size_t>> textureChunks;
    const auto visit=[&](const auto& self,std::size_t begin,std::size_t size,unsigned depth)->void {
        Require(depth<=32,"Editor asset nesting exceeds 32 levels");
        for(const auto& chunk:ReadChunks(authored_,begin,size)){
            if(chunk.id==0x80034002u){
                Chunk header{},rigid{},labels{},stripTable{},stripPackets{},textureTable{},materials{},tree{};
                for(const auto& child:ReadChunks(authored_,chunk.payloadOffset,chunk.size)) {
                    if(child.id==0x34003u){Require(!header.id,"Editor model repeats its header");header=child;}
                    else if(child.id==0x34012u){Require(!rigid.id,"Model repeats its rigid animation");rigid=child;}
                    else if(child.id==0x34011u){Require(!labels.id,"Model repeats its frame labels");labels=child;}
                    else if(child.id==0x34004u){Require(!stripTable.id,"Model repeats its strip table");stripTable=child;}
                    else if(child.id==0x34005u){Require(!stripPackets.id,"Model repeats its strip packets");stripPackets=child;}
                    else if(child.id==0x34006u){Require(!textureTable.id,"Model repeats its texture table");textureTable=child;}
                    else if(child.id==0x3401du){Require(!materials.id,"Model repeats its light-material table");materials=child;}
                    else if(child.id==0x34007u){Require(!tree.id,"Model repeats its animation tree");tree=child;}
                }
                Require(header.id!=0,"Editor model has no header");
                const std::size_t start=(header.payloadOffset+15u)&~std::size_t(15u);
                Require(start<=header.endOffset&&header.endOffset-start>=192,"Editor model header is truncated");
                const auto* record=authored_.data()+start;
                const auto name=text(record+16,24);
                const auto key=ReadU32(record+8);
                // Shipped scenery stores truncated display names. The
                // authored key, rather than a hash of that name, is the
                // lookup identity. Repeated keys retain source load order.
                (void)name;
                EditorMarkerSolid model{};
                model.key=key;
                std::memcpy(model.name,record+16,sizeof(model.name));
                model.resourceFlags=ReadU16(record+0x30);
                model.renderFlags=ReadU16(record+0x32);
                model.authoredGeometry=authored_.data()+chunk.payloadOffset;
                model.authoredGeometrySize=chunk.size;
                for(int axis=0;axis<3;++axis){model.minimum[axis]=real(record+64+axis*4);model.maximum[axis]=real(record+80+axis*4);}
                for(int axis=0;axis<3;++axis)model.origin[axis]=real(record+0x90+axis*4);
                for(int lane=0;lane<16;++lane)model.positionMatrix[lane]=real(record+0x60+lane*4);
                auto geometry=decodeSolidGeometry(authored_,record,stripTable,stripPackets,textureTable,materials);
                model.geometry=geometry.get();geometry_.push_back(std::move(geometry));
                // Actual LoaderSolidList10389C derives the embedded tree from
                // its aligned payload/96. The leaf10CA80 exposes this owner.
                // A static model without34007 retains its authored zero count.
                const auto authoredNodeCount=ReadU32(record+0xa0);
                Require(authoredNodeCount<=static_cast<std::uint32_t>(INT32_MAX),"Animation tree count exceeds signed range");
                model.animationTree.nodeCount=static_cast<std::int32_t>(authoredNodeCount);
                if (tree.id) {
                    const auto nodes=(tree.payloadOffset+15u)&~std::size_t(15u);
                    Require(nodes<=tree.endOffset,"Animation tree alignment exceeds payload");
                    const auto bytes=tree.endOffset-nodes;
                    Require(bytes/96<=static_cast<std::size_t>(INT32_MAX),"Animation tree node count exceeds signed range");
                    model.animationTree.nodeCount=static_cast<std::int32_t>(bytes/96);
                    model.animationTree.authoredNodes=authored_.data()+nodes;
                    model.animationTree.authoredBytes=bytes;
                } else Require(!authoredNodeCount,"Animation tree has no retained node payload");
                if (rigid.id) {
                    const std::size_t animationStart=(rigid.payloadOffset+15u)&~std::size_t(15u);
                    Require(animationStart<=rigid.endOffset && rigid.endOffset-animationStart>=48,"Rigid animation header is truncated");
                    const auto* data=authored_.data()+animationStart;
                    auto animation=std::make_unique<RigidAnimationStorage>();
                    const uint16_t count=ReadU16(data);
                    std::memcpy(&animation->native.frameCount,&count,sizeof(count));
                    std::memcpy(&animation->native.frameDuration,data+3,1);
                    std::memcpy(&animation->native.playbackType,data+2,1);
                    animation->native.key=ReadU32(data+4);
                    Require(animation->native.frameCount>0 && animation->native.frameDuration>0,
                        "Rigid animation dimensions must be positive");
                    const std::size_t lanes=std::size_t(animation->native.frameCount)*16u;
                    Require(lanes<=(rigid.endOffset-animationStart-48)/4,"Rigid animation matrix stream is truncated");
                    animation->matrices.resize(lanes);
                    for(std::size_t lane=0;lane<lanes;++lane)animation->matrices[lane]=real(data+48+lane*4);
                    if(labels.id){
                        Require(labels.size%2==0,"Rigid animation label table has a partial element");
                        animation->labels.resize(labels.size/2);
                        for(std::size_t i=0;i<animation->labels.size();++i)
                            animation->labels[i]=ReadU16(authored_.data()+labels.payloadOffset+i*2);
                    }
                    animation->native.matrices=animation->matrices.data();
                    animation->native.labels=animation->labels.data();
                    animation->native.labelCount=animation->labels.size();
                    model.rigidAnimation=&animation->native;
                    rigidAnimations_.push_back(std::move(animation));
                }
                if (ReadU32(record+40)) inheritedAnimations.emplace(key,ReadU32(record+40));
                solids_.push_back(model);
            }else if(chunk.id==0xb0300001u || chunk.id==0xb0300100u){
                textureChunks.emplace_back(chunk.id,chunk.headerOffset);
            }else if(chunk.IsContainer())self(self,chunk.payloadOffset,chunk.size,depth+1);
        }
    };
    visit(visit,0,authored_.size(),0);
    native_.solids=solids_.data();native_.solidCount=solids_.size();
    if(registry)Require(editorMarkerResourcesAttach(registry,&native_),"Model catalogue is already registered");
    try {
    // The real texture class owns each retained descriptor and palette.
    // Do not decode another flat descriptor copy in this model resource.
    for (const auto& [kind,offset]:textureChunks) {
        if(kind==0xb0300001u)
            texturePacks_.push_back(std::make_unique<TexturePack>(texturePackRegistry,
                registry?*registry:native_,authored_,offset));
        else {
            // LoaderTexturePack100910 accepts version1 only; other versions
            // remain an explicitly uninstantiated authored resource.
            Require(offset<=authored_.size() && authored_.size()-offset>=20,
                "Texture animation pack header is truncated");
            if(ReadU32(authored_.data()+offset+16)==1)
                textureAnimationPacks_.push_back(std::make_unique<TextureAnimPack>(texturePackRegistry,
                    registry?*registry:native_,authored_,offset));
        }
    }
    lightMaterials_=std::make_unique<LightMaterialResources>(authored_,registry?*registry:native_);
    auto* root=registry?registry:&native_;
    if (root->catalogueRoot)root=root->catalogueRoot;
    for (auto& solid:solids_) {
        auto& geometry=*solid.geometry;
        if (root->resourceRegistry)geometry.FixTextureTable(*root->resourceRegistry);
        geometry.lightMaterials.resize(geometry.lightMaterialKeys.size());
        for (std::size_t index=0;index<geometry.lightMaterialKeys.size();++index) {
            auto* found=root->defaultLightMaterial;
            for (auto* catalogue=root->lightMaterialCatalogues;catalogue;catalogue=catalogue->next) {
                bool matched=false;
                for (const auto& material:catalogue->materials)
                    if (material.key==geometry.lightMaterialKeys[index]) {found=&material;matched=true;break;}
                if (matched)break;
            }
            geometry.lightMaterials[index]=found;
        }
    }
    for (auto& solid:solids_) {
        const auto inherited=inheritedAnimations.find(solid.key);
        if(inherited==inheritedAnimations.end())continue;
        const auto* source=findModelSolid(registry ? registry:&native_,inherited->second);
        if(source)solid.rigidAnimation=source->rigidAnimation;
    }
    // PAL103950..1039EC: identical0x10 solid keys form one ordered sequence,
    // at30fps with at most64 members; later members share the first rigid
    // animation. Native groups stay within their actual resource lifetime.
    // Concurrent/reloaded catalogues must never retain each other's solids.
    std::map<std::uint32_t,EditorMarkerVertexAnimation*> variants;
    for(auto& solid:solids_)if(solid.resourceFlags&0x10u) {
        auto& group=variants[solid.key];
        if(!group) {
            auto sequence=std::make_unique<EditorMarkerVertexAnimation>();
            sequence->framesPerSecond=30;group=sequence.get();
            vertexAnimations_.push_back(std::move(sequence));
        }else solid.rigidAnimation=group->frames[0]->rigidAnimation;
        if(group->count<64)group->frames[group->count++]=&solid;
        solid.vertexAnimation=group;
    }
    native_.solids=solids_.data();native_.solidCount=solids_.size();
    } catch (...) {
        editorMarkerResourcesDetach(&native_);
        throw;
    }
}

TrackRouteEditorResources::~TrackRouteEditorResources()
{
    if (!native_.catalogueRoot)ecstasyDeleteAllDuplicatedSolids(&native_);
    editorMarkerResourcesDetach(&native_);
}

} // namespace hp2

// Native counterpart of the counter producer in PAL eVblCallback111F58.
// GS FIELD, DMA page swaps and debug timing are platform-specific and are not
// claimed as a completed native eVblCallback. The Vulkan producer is still a
// required integration gate; this counter advances only on an explicit event.
namespace { std::atomic<std::uint32_t> graphicsVideoBlankCounter{0}; }
std::uint32_t graphicsVBlankCount() noexcept {
    return graphicsVideoBlankCounter.load(std::memory_order_acquire);
}

namespace hp2 {
namespace {
// Final2C79C0, indexed by the same GameTime mode as source GetVideoMode.
constexpr std::array<std::array<std::int32_t,2>,2> defaultDisplayOffsets{{{{56,27}},{{0,0}}}};
std::array<std::array<std::int32_t,2>,2> displayOffsets=defaultDisplayOffsets;
std::size_t DisplayModeIndex(const GameTime& clock)
{
    const auto mode=gameTimeGetVideoMode(&clock);
    Require(mode==HP2_VIDEO_PAL || mode==HP2_VIDEO_NTSC,"Display calibration has an invalid actual video mode");
    return static_cast<std::size_t>(mode);
}
}
void eSetScreenDisplayOffsets(const GameTime& clock,std::int32_t x,std::int32_t y)
{
    // Original setter stores signed words without a menu clamp.
    displayOffsets[DisplayModeIndex(clock)]={{x,y}};
}
void eGetScreenDisplayOffsets(const GameTime& clock,std::int32_t& x,std::int32_t& y)
{
    const auto& source=displayOffsets[DisplayModeIndex(clock)];x=source[0];y=source[1];
}
void EcstasyFrame::CaptureDisplayOffsets(const GameTime& clock)
{
    Require(phase_==Phase::Building,"Cannot change display offsets in a sealed graphics frame");
    const auto mode=DisplayModeIndex(clock);std::int32_t x{},y{};eGetScreenDisplayOffsets(clock,x,y);
    sourceDisplayExtent_={{512u,mode==HP2_VIDEO_PAL ? 512u:448u}};
    // CreateTextureContext108D80..108D9C uses WorldTimer, in4000Hz ticks.
    textureTime_=static_cast<float>(clock.worldTimer)*0.0002500000118743628f;
    // Native full-raster fit: the centered client area corresponds to the
    // source default display origin. PAL eSetDisplaySystem1030E8 chooses
    // 512x512 or512x448; released DISPLAY MAGH4 is five VCK per source
    // pixel, MAGV0 is one raster per pixel. We do not emulate CRT blanking,
    // its extra DW+1 clock, or the original four-line overscan crop.
    // Convert before subtraction so any persisted signed word is safe.
    displayTranslation_[0]=(static_cast<double>(x)-defaultDisplayOffsets[mode][0])/(512.0*5.0);
    displayTranslation_[1]=(static_cast<double>(y)-defaultDisplayOffsets[mode][1])/
        (mode==HP2_VIDEO_PAL ? 512.0:448.0);
}
void EcstasyFrame::AdvanceTextureAnimationsAfterCompletion(ResourceRegistry& resources,const GameTime& clock)
{
    Require(phase_==Phase::Building && !textureAnimationResources_,
        "Texture animation update was scheduled outside its source frame boundary");
    textureAnimationResources_=&resources;
    textureAnimationWorldFrames_=clock.worldTimeFrames;textureAnimationRealFrames_=clock.realTimeFrames;
}
EcstasyFrame::EcstasyFrame()=default;

const EcstasyDrawInstance& EcstasyFrame::CaptureInstance(const float localToWorld[16],
    const CarCinematicScreen& view,const SceneLightContext* lightContext)
{
    Require(phase_==Phase::Building && localToWorld && view.matricesCalculated,"Draw instance lost its active capture, transform or projection");
    if(liveInstances_==capturedInstances_.size())capturedInstances_.emplace_back();
    auto& instance=capturedInstances_[liveInstances_];
    eMulMatrix(instance.localToClip.data(),localToWorld,view.matrices.worldToClip);
    if(lightContext)instance.lightContext=*lightContext;
    else instance.lightContext.reset();
    ++liveInstances_;
    return instance;
}

const EcstasyDrawProjection& EcstasyFrame::CaptureProjection(const CarCinematicScreen& view)
{
    Require(phase_==Phase::Building,"Projection capture requires a building frame");
    const auto& source=view.matrices;
    if(lastProjection_ &&
        std::memcmp(lastProjection_->matrices[0].data(),source.clipToScreen,sizeof(source.clipToScreen))==0 &&
        std::memcmp(&lastProjection_->nearSource,&source.nearProjection,sizeof(source.nearProjection))==0 &&
        std::memcmp(&lastProjection_->frustumSource,&source.frustumProjection,sizeof(source.frustumProjection))==0)
        return *lastProjection_;
    if(liveProjections_==capturedProjections_.size())capturedProjections_.emplace_back();
    auto& snapshot=capturedProjections_[liveProjections_];
    std::copy_n(source.clipToScreen,16,snapshot.matrices[0].begin());
    snapshot.nearSource=source.nearProjection;
    snapshot.frustumSource=source.frustumProjection;
    const auto capture=[](std::array<float,16>& matrix,const EcstasyProjectionPair& projection) {
        matrix.fill(0.0f);
        for(std::size_t axis=0;axis<3;++axis)matrix[axis*5]=projection.diagonal[axis];
        std::copy_n(projection.translation,4,matrix.begin()+12);
    };
    capture(snapshot.matrices[1],source.nearProjection);
    capture(snapshot.matrices[2],source.frustumProjection);
    ++liveProjections_;
    lastProjection_=&snapshot;
    return snapshot;
}

void EcstasyFrame::CaptureEnvironment(EnvironmentMap* environment,ResourceRegistry& resources,
    std::int32_t applicationState,std::uint32_t renderFrameCounter,bool inGame)
{
    Require(phase_==Phase::Building,"Cannot replace environment owners in a sealed frame");
    inGameMaterials_=inGame;
    environment_=environment;environmentApplicationState_=applicationState;renderFrameCounter_=renderFrameCounter;
    environmentUpdated_=false;
    environmentCameraFaces_=environment ? environment->PendingCameraFaces():0u;
    // PAL112B28 downloads the actual CarSelectEnvMap descriptor into the
    // separate frontend texture slot used by AttachMicroCode10AF50/10B038.
    frontendEnvironmentTexture_=textureInfoForDescriptor(resources.SpecialTexture(8));
    specularFalloffTexture_=textureInfoForDescriptor(resources.SpecialTexture(6));
}
const float* EcstasyFrame::EnvironmentMatrix() const noexcept
{
    return environment_ ? environment_->BodyMatrix().data():nullptr;
}

SceneLightContext EcstasyFrame::SetupWorldLightContext(ResourceRegistry& resources,const CarCinematicScreen& view) const
{
    // PAL107900..107AB0. Source+0x70 is a transient context allocation;
    // native values are copied into the same frame's immutable draw packets.
    Require(view.view,"World light context has no original camera");
    float identity[16];eIdentity(identity);
    float eye[3];
    for(std::size_t axis=0;axis<3;++axis)
        eye[axis]=view.view->current.eye[axis]-view.view->current.matrix[axis*4+2]*10.0f;
    eye[2]+=4.0f;
    auto context=SetupLightContext(resources.Lights(),identity,EnvironmentMatrix(),eye,view,environmentApplicationState_);
    const auto& sun=resources.Sun().Vector();
    const float position[4]={sun[0],sun[1],sun[2],0},zero[4]{},up[4]{0,0,1,0};
    float sunMatrix[16];carCinematicLookAt(sunMatrix,position,zero,up);
    // Source107A84..107A90 replaces lightPositions(+80), not the separate
    // environmentMatrix(+E0). The subsequent scale matrix is identity.
    eMulMatrix(context.lightPositions.data(),identity,sunMatrix);
    return context;
}

void EcstasyFrame::RenderTargetStrip(const std::vector<frontend::FlailerPolyVertex>& vertices,
    const EcstasyRenderTarget& source,const EcstasyRenderTarget& destination)
{
    Require(phase_==Phase::Building && vertices.size()>=4 && vertices.size()<=128 && vertices.size()%2==0 && source.enabled && destination.enabled &&
        source.id>=8 && source.id<=13 && destination.id==14,
        "Environment remap strip has no original source/destination target");
    auto owned=std::make_unique<std::vector<frontend::FlailerPolyVertex>>(vertices);
    EcstasyDrawCommand command;command.kind=EcstasyDrawKind::RenderTargetStrip;
    command.worldStrip=owned.get();command.target=&destination;command.sampledTarget=&source;
    command.zWriteEnabled=false;command.rasterState={{0x01000100u,0,0,0}};
    worldStrips_.push_back(std::move(owned));
    try {commands_.push_back(command);}catch(...) {worldStrips_.pop_back();throw;}
    environmentUpdated_=true;
}
EcstasyFrame::~EcstasyFrame()
{
    // A submitted buffer can still be read by the device. Destroying it is
    // a host lifetime error, never an implicit fake GPU completion.
    if (phase_==Phase::Submitted)std::terminate();
    Release();
}

std::int32_t EcstasyFrame::SetZWriteState(std::int32_t enabled)
{
    Require(phase_==Phase::Building,"Cannot change depth writes in a sealed graphics frame");
    // Complete PAL1033C8..1033D8 eSetZWriteState: return the old state and
    // store enabled!=0. Frame-local storage transports the original packet
    // state into deferred Vulkan commands, without a device-global override.
    const auto previous=static_cast<std::int32_t>(zWriteEnabled_);
    zWriteEnabled_=enabled!=0;
    return previous;
}

bool EcstasyFrame::SetDashboardProjection(bool enabled)
{
    Require(phase_==Phase::Building,"Cannot change dashboard projection in a sealed frame");
    const bool previous=dashboardProjection_;dashboardProjection_=enabled;return previous;
}

std::array<float,2> ecstasyNativeDepthRange(const float projection[16],std::int32_t format)
{
    const double maximum=format==0x31 ? 16777215.0:65535.0;
    const auto endpoint=[&](double clip) {
        const double divisor=projection[11]*clip+projection[15];
        Require(std::isfinite(divisor) && divisor!=0.0,"World depth lacks its captured projection");
        const double source=(projection[10]*clip+projection[14])/divisor;
        // Matrix products may round an exact endpoint a few native float
        // ulps outside its interval. Clip the numeric depth at the physical
        // attachment boundary, without quantizing it to a console integer.
        const double tolerance=maximum*std::numeric_limits<float>::epsilon()*8.0;
        Require(std::isfinite(source) && source>=-tolerance && source<=maximum+tolerance,
            "Native depth endpoint exceeds its target interval");
        return static_cast<float>(1.0-std::clamp(source,0.0,maximum)/maximum);
    };
    const std::array<float,2> result{{endpoint(-1.0),endpoint(1.0)}};
    Require(result[0]<=result[1],"Native depth interval is reversed");
    return result;
}

void EcstasyFrame::AttachParticleGroup(const std::vector<EcstasyParticleQuad>& quads,
    const EditorMarkerTexture& descriptor,const CarCinematicScreen& view,float averageDepth)
{
    if(quads.empty())return;
    Require(phase_==Phase::Building && view.target && view.target->enabled &&
        view.target->width>0 && view.target->height>0 && view.matricesCalculated,
        "Particle group lacks its active view/target");
    const auto* texture=textureInfoForDescriptor(&descriptor);
    Require(texture && texture->pixels.Data(),"Particle group has no loaded texture owner");
    Require(std::isfinite(averageDepth),"Particle group depth is not finite");
    if(liveParticleGroups_==particleGroups_.size())
        particleGroups_.push_back(std::make_unique<std::vector<EcstasyParticleQuad>>());
    auto& owned=*particleGroups_[liveParticleGroups_];
    owned.assign(quads.begin(),quads.end());
    EcstasyDrawCommand command;command.kind=EcstasyDrawKind::ParticleGroup;
    command.particleGroup=&owned;command.texture=texture;command.view=&view;command.target=view.target;
    command.zWriteEnabled=zWriteEnabled_;command.depthSorted=texture->requiresDepthSort;
    command.sortDepth=averageDepth;command.alphaGroup=++nextAlphaGroup_;
    try {commands_.push_back(command);}catch(...) {owned.clear();throw;}
    ++liveParticleGroups_;
}

void EcstasyFrame::AddPolygon(const frontend::FlailerPoly& polygon,
    const EditorMarkerTexture& descriptor,const CarCinematicScreen& view,
    std::int32_t layer,float alphaScale,EcstasyUiPlacement placement)
{
    Require(phase_==Phase::Building,"Cannot append to a sealed graphics frame");
    Require(view.target && view.target->enabled && view.target->width>0 && view.target->height>0,
        "Flailer polygon has no active original render target");
    const auto* texture=textureInfoForDescriptor(&descriptor);
    Require(texture,"Flailer polygon texture pixels are not loaded: descriptor has no active texture owner");
    const auto* orderingTexture=texture;
    bool neutralTextureOperand=false;
    if(!texture->pixels.Data() || (texture->paletteBytes && !texture->palette.Data())) {
        // PAL19ECE0 retires the previous slideshow images while its next
        // bitmap remains visible with all four alpha values zero. The
        // retained descriptor can therefore legitimately have no CPU image.
        // Preserve this draw's alpha=0 write and original texture ordering:
        // for MODULATE and (Cs-Cd)*As+Cd with no Z write, sampled texels cannot
        // affect RGBA/depth. Borrow a live neutral operand only in that exact
        // PC transport case; a visible or otherwise different packet still
        // requires its requested image. This is not a source loading fallback.
        const auto* wire=descriptor.authoredRecord;
        if(texture->pack && wire && descriptor.authoredRecordSize>0x50 &&
            !texture->requiresDepthSort && wire[0x4e]==0 && wire[0x50]==0 &&
            polygon.rasterState[0]==0x01000100u && std::isfinite(alphaScale) &&
            std::all_of(polygon.vertices.begin(),polygon.vertices.end(),
                [](const frontend::FlailerPolyVertex& vertex){return (vertex.color>>24)==0;})) {
            auto* registry=texture->pack->CatalogueRoot().resourceRegistry;
            auto* neutral=registry ? textureInfoForDescriptor(registry->ResolveTexture(0x19006835u,false)):nullptr;
            if(neutral && neutral->pixels.Data() && (!neutral->paletteBytes || neutral->palette.Data()) &&
                neutral->descriptor && neutral->descriptor->authoredRecord &&
                neutral->descriptor->authoredRecordSize>0x4e &&
                !neutral->requiresDepthSort && neutral->descriptor->authoredRecord[0x4e]==0) {
                texture=neutral;neutralTextureOperand=true;
            }
        }
    }
    if(!texture->pixels.Data() || (texture->paletteBytes && !texture->palette.Data())) {
        // The registry lookup established the live owner. Do not inspect
        // an unregistered descriptor merely to enrich a failure message.
        std::ostringstream error;
        error<<"Flailer polygon texture pixels are not loaded: key=0x"<<std::hex<<texture->descriptor->key
            <<std::dec<<", pack="<<(texture->pack ? texture->pack->Name():"<none>")
            <<", pixel_bytes="<<texture->pixelBytes<<", palette_bytes="<<texture->paletteBytes
            <<", palette_loaded="<<(texture->palette.Data()!=nullptr)
            <<", pending_reads="<<(texture->pack && texture->pack->HasPendingReads());
        throw std::runtime_error(error.str());
    }
    // The original ePoly::Render10FE5C uses the texture's alpha-list lane
    // even for Flailer polygons. It suppresses that list in opaque-only
    // targets and stores layerDepth-MaxZ (or direct Z) as its sort key.
    if(texture->requiresDepthSort && view.target->opaqueOnly)return;
    EcstasyDrawCommand command;command.zWriteEnabled=neutralTextureOperand ? false:zWriteEnabled_;
    command.uiPlacement=placement;
    command.polygon=&polygon;command.texture=texture;command.view=&view;command.target=view.target;
    if(neutralTextureOperand)command.orderingTexture=orderingTexture;
    command.clipToScreen=&CaptureProjection(view).matrices[0];
    command.rasterState=polygon.rasterState;command.layer=layer;command.alphaScale=alphaScale;
    command.depthSorted=texture->requiresDepthSort;
    if(command.depthSorted) {
        command.sortDepth=polygon.vertices[0].position[2];
        if(polygon.rasterState[2]&255u) {
            static constexpr float depths[29]={10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,
                5,6,7,8,5,4,3,2,1,1,2,3,4};
            const auto index=convertFloatToInt32(command.sortDepth);
            Require(index>=0 && index<29,"Flailer alpha key has no authored layer");
            command.sortDepth=depths[index]-(view.target->format==0x31 ? 16777215.0f:65535.0f);
        }
    }
    commands_.push_back(command);
}

void EcstasyFrame::AttachSolidPolys()
{
    Require(phase_==Phase::Building,"Cannot attach solid lists in a sealed graphics frame");
    // PAL10BB20 walks only the solid texture chains. The alpha arena/list
    // survives this call, including the two display calls1134A4/1135B8
    // between scenery/particles, environment remap and the later cars.
    const auto count=static_cast<std::size_t>(std::count_if(
        commands_.begin()+static_cast<std::ptrdiff_t>(pendingAlphaBegin_),commands_.end(),
        [](const EcstasyDrawCommand& command){return command.depthSorted;}));
    pendingAlphaCommands_.reserve(pendingAlphaCommands_.size()+count);
    auto output=pendingAlphaBegin_;
    for(auto index=pendingAlphaBegin_;index<commands_.size();++index) {
        if(commands_[index].depthSorted)pendingAlphaCommands_.push_back(std::move(commands_[index]));
        else {
            if(output!=index)commands_[output]=std::move(commands_[index]);
            ++output;
        }
    }
    commands_.resize(output);
    if(output==pendingAlphaBegin_)return;
    // AddTexturesSorted PAL109358..109488 groups the first-use texture
    // list into priorities0..7. Linked textures insert immediately after
    // their parent's first anchor when both priorities match; later siblings
    // therefore precede earlier siblings. A stable priority sort alone is
    // different. These local anchors expire with this source flush, exactly
    // as DeleteDownloadedTextures109488 clears TextureInfo+9C.
    // The source uses temporary packet/list storage. Bulk native scratch
    // avoids allocating one heap node for every texture and list link.
    // Overflow still uses the ordinary allocator; no packet is truncated.
    std::array<std::byte,65536> localStorage;
    std::pmr::monotonic_buffer_resource scratch(localStorage.data(),localStorage.size());
    struct TextureChain {
        const TextureInfo* texture{};
        const EcstasyRenderTarget* sampled{};
        std::array<std::size_t,15> first,last;
        TextureChain(const TextureInfo* image,const EcstasyRenderTarget* target)
            :texture(image),sampled(target)
        {
            first.fill(std::numeric_limits<std::size_t>::max());last=first;
        }
    };
    constexpr auto noCommand=std::numeric_limits<std::size_t>::max();
    // Each command has exactly one target. Link its position once rather
    // than rescanning every texture's full command list for all15 targets.
    // These indices are scratch only; original frame/texture owners survive
    // until the ordered command values have been moved back below.
    std::pmr::vector<std::size_t> next(output-pendingAlphaBegin_,noCommand,&scratch);
    std::pmr::vector<TextureChain> chains{&scratch};
    std::pmr::map<std::pair<const TextureInfo*,const EcstasyRenderTarget*>,std::size_t> active{&scratch};
    // Source strips and their material passes frequently keep the same
    // ordering texture. This lookup is local to this exact solid flush;
    // only an already-created chain index is reused, never its draw order.
    const TextureInfo* previousTexture=nullptr;
    const EcstasyRenderTarget* previousSampled=nullptr;
    std::size_t previousChain=noCommand;
    for(auto index=pendingAlphaBegin_;index<commands_.size();++index) {
        const auto& command=commands_[index];
        const auto* texture=command.orderingTexture ? command.orderingTexture:command.texture;
        const auto* sampled=texture ? nullptr:command.sampledTarget;
        Require((texture || sampled) && command.target && command.target->id>=0 && command.target->id<15,
            "Solid texture chain lost its original image or target");
        std::size_t chainIndex=previousChain;
        if(previousChain==noCommand || texture!=previousTexture || sampled!=previousSampled) {
            const auto key=std::make_pair(texture,sampled);
            auto found=active.find(key);
            if(found==active.end()) {
                found=active.emplace(key,chains.size()).first;
                chains.emplace_back(texture,sampled);
            }
            chainIndex=found->second;
            previousTexture=texture;previousSampled=sampled;previousChain=chainIndex;
        }
        auto& chain=chains[chainIndex];
        const auto target=static_cast<std::size_t>(command.target->id);
        if(chain.last[target]==noCommand)chain.first[target]=index;
        else next[chain.last[target]-pendingAlphaBegin_]=index;
        chain.last[target]=index;
    }
    std::array<std::pmr::list<std::size_t>,8> priorities{{
        std::pmr::list<std::size_t>{&scratch},std::pmr::list<std::size_t>{&scratch},
        std::pmr::list<std::size_t>{&scratch},std::pmr::list<std::size_t>{&scratch},
        std::pmr::list<std::size_t>{&scratch},std::pmr::list<std::size_t>{&scratch},
        std::pmr::list<std::size_t>{&scratch},std::pmr::list<std::size_t>{&scratch}}};
    struct Anchor {std::uint8_t bucket{};std::pmr::list<std::size_t>::iterator position;};
    std::pmr::map<const TextureInfo*,Anchor> anchors{&scratch};
    const auto priority=[](const TextureInfo& texture) {
        Require(texture.descriptor && texture.descriptor->authoredRecord &&
            texture.descriptor->authoredRecordSize>=164,"Solid chain lost its TextureInfo priority");
        const auto value=texture.descriptor->authoredRecord[0x29];
        Require(value<8,"Solid texture priority exceeds the original eight lists");
        return value;
    };
    for(std::size_t index=0;index<chains.size();++index) {
        const auto* texture=chains[index].texture;
        if(!texture) {
            // UpdateGifTags105F1C builds generated-target TextureInfos from
            // cleared storage: priority0 and a self-linked download owner.
            priorities[0].push_back(index);continue;
        }
        const auto value=priority(*texture);
        const auto* parent=texture->linkedTexture;
        Require(parent,"Solid texture lacks its canonical linked download owner");
        const auto anchor=anchors.find(parent);
        if(anchor!=anchors.end() && priority(*parent)==value)
            priorities[anchor->second.bucket].insert(std::next(anchor->second.position),index);
        else {
            priorities[value].push_back(index);
            anchors[parent]={value,std::prev(priorities[value].end())};
        }
    }
    // Keep the source chain walk, but carry only source indices through
    // the permutation. Copying every large captured lighting/transform
    // value to a second command buffer and back is unnecessary. A cycle
    // moves each nonidentity command once plus one temporary per cycle.
    // No command or borrower can outlive this exact source flush.
    std::pmr::vector<std::size_t> order{&scratch};
    order.reserve(output-pendingAlphaBegin_);
    for(const auto& bucket:priorities)for(const auto chain:bucket) {
        // AttachSolidPolys10BC40..10BCC0 walks all15 target chains for
        // each texture. Material's base/specular/reflection packets remain
        // together inside their actual parent chain and original target.
        for(const auto first:chains[chain].first)
            for(auto index=first;index!=noCommand;index=next[index-pendingAlphaBegin_])
                order.push_back(index-pendingAlphaBegin_);
    }
    Require(order.size()==output-pendingAlphaBegin_,"Native solid command order lost a source command");
    static_assert(std::is_nothrow_move_constructible<EcstasyDrawCommand>::value &&
        std::is_nothrow_move_assignable<EcstasyDrawCommand>::value,
        "In-place native command ordering requires nonthrowing value moves");
    for(std::size_t start=0;start<order.size();++start) {
        if(order[start]==start)continue;
        auto saved=std::move(commands_[pendingAlphaBegin_+start]);
        auto destination=start;
        while(order[destination]!=start) {
            const auto source=order[destination];
            commands_[pendingAlphaBegin_+destination]=std::move(commands_[pendingAlphaBegin_+source]);
            order[destination]=destination;destination=source;
        }
        commands_[pendingAlphaBegin_+destination]=std::move(saved);
        order[destination]=destination;
    }
    pendingAlphaBegin_=output;
}

void EcstasyFrame::RenderScreenPolygon(const frontend::FlailerPoly& polygon,
    const TextureInfo* texture,const CarCinematicScreen& view)
{
    // PAL10FA68..10FF88. This value transport is the direct-Z ePoly path
    // used by Sun; its texture still owns the original TEST/ZBUF context.
    if(!texture)return;
    Require(phase_==Phase::Building && view.target && view.target->enabled &&
        view.matricesCalculated && texture->pixels.Data(),"Screen polygon lost its loaded texture or view");
    if(texture->requiresDepthSort && view.target->opaqueOnly)return;
    auto owned=std::make_unique<frontend::FlailerPoly>(polygon);
    if(owned->rasterState[1]&0x00ff0000u)owned->rasterState[1]&=~0xff000000u;
    EcstasyDrawCommand command;command.kind=EcstasyDrawKind::ScreenPolygon;
    command.polygon=owned.get();command.texture=texture;command.view=&view;command.target=view.target;
    command.rasterState=owned->rasterState;command.zWriteEnabled=zWriteEnabled_;
    command.depthSorted=texture->requiresDepthSort;command.sortDepth=polygon.vertices[0].position[2];
    command.clipToScreen=&CaptureProjection(view).matrices[0];
    worldPolygons_.push_back(std::move(owned));
    try {commands_.push_back(command);}catch(...) {worldPolygons_.pop_back();throw;}
}

void EcstasyFrame::TransferZBufferRect(std::shared_ptr<SunDepthSamples> destination,
    const CarCinematicScreen& view,std::int32_t x,std::int32_t y,std::int32_t width,std::int32_t height,
    float nativeSampleScale,const std::array<std::int32_t,4>* consumedBounds)
{
    // PAL111C58 creates an active-stage request. The native device retains
    // its original deferred lifetime and reads actual preceding GPU depth.
    Require(phase_==Phase::Building && destination && view.target && view.target->enabled &&
        view.target->id>=0 && view.target->id<7 && view.matricesCalculated &&
        x>=0 && y>=0 && width>0 && height>0 &&
        std::isfinite(nativeSampleScale) && nativeSampleScale>0.0f &&
        (static_cast<double>(x)+width)*nativeSampleScale<=view.target->width &&
        (static_cast<double>(y)+height)*nativeSampleScale<=view.target->height &&
        static_cast<std::size_t>(width)*static_cast<std::size_t>(height)<=destination->values.size(),
        "Source depth transfer has no retained destination or valid main-target rectangle");
    EcstasyDepthTransfer request;request.destination=std::move(destination);
    request.x=x;request.y=y;request.width=width;request.height=height;
    request.targetWidth=view.target->width;request.targetHeight=view.target->height;
    request.targetLeft=view.target->left;request.targetTop=view.target->top;
    request.nativeSampleScale=nativeSampleScale;
    request.consumedBoundsValid=consumedBounds!=nullptr;
    if(consumedBounds)request.consumedBounds=*consumedBounds;
    request.maximumDepth=view.target->format==0x31 ? 16777215u:65535u;
    std::copy(view.matrices.clipToScreen,view.matrices.clipToScreen+16,request.clipToScreen.begin());
    depthTransfers_.push_back(request);
}

void EcstasyFrame::AttachAlphaPolys()
{
    Require(phase_==Phase::Building,"Cannot attach alpha lists in a sealed graphics frame");
    // Finish any still-unattached native solid packets before the original
    // alpha list. Explicit earlier solid boundaries have already consumed
    // their ranges, so this never reorders across a source stage.
    AttachSolidPolys();
    // Earlier solid-only boundaries may have retained alpha packets. Insert
    // them before newer pending packets to preserve source insertion order,
    // including equal-key and grouped material-pass tie behavior below.
    if(!pendingAlphaCommands_.empty()) {
        commands_.insert(commands_.begin()+static_cast<std::ptrdiff_t>(pendingAlphaBegin_),
            std::make_move_iterator(pendingAlphaCommands_.begin()),
            std::make_move_iterator(pendingAlphaCommands_.end()));
        pendingAlphaCommands_.clear();
    }
    // PAL10B838 flushes one list per target after the solid lists. Its
    // SortAlphaKeys1055B0/RadixSortAlphaKeys1051D8 use integer float keys:
    // priority0..7, then far-to-near; the final reverse walk reverses equal
    // positive keys, while the signed radix pass retains negative ties.
    // Work only since the last source flush. Sky, world, mirror and HUD must
    // never enter one frame-wide ordering operation.
    struct Packet {
        std::size_t begin{},end{};
        std::int32_t target{};
        std::uint32_t priority{},key{},bits{};
    };
    std::array<std::byte,16384> localStorage;
    std::pmr::monotonic_buffer_resource scratch(localStorage.data(),localStorage.size());
    std::pmr::vector<Packet> alpha{&scratch};
    alpha.reserve(commands_.size()-pendingAlphaBegin_);
    for(auto index=pendingAlphaBegin_;index<commands_.size();++index) {
        const auto& command=commands_[index];
        if(!command.depthSorted)continue;
        const auto* texture=command.orderingTexture ? command.orderingTexture:command.texture;
        Require(texture && texture->descriptor && command.target &&
            texture->descriptor->authoredRecord && texture->descriptor->authoredRecordSize>0x29,
            "Alpha packet lost its actual texture priority or target");
        Packet packet;packet.begin=index;packet.end=index+1;packet.target=command.target->id;
        const auto priority=texture->descriptor->authoredRecord[0x29];
        packet.priority=priority<8 ? priority:7;
        std::memcpy(&packet.bits,&command.sortDepth,sizeof(packet.bits));
        packet.key=(packet.bits&0x80000000u) ? ~packet.bits:packet.bits^0x80000000u;
        if(command.alphaGroup) {
            while(packet.end<commands_.size() && commands_[packet.end].alphaGroup==command.alphaGroup) {
                const auto& member=commands_[packet.end];
                const auto* memberTexture=member.orderingTexture ? member.orderingTexture:member.texture;
                Require(member.depthSorted && memberTexture==texture && member.target==command.target,
                    "Alpha group no longer represents one original direct packet");
                ++packet.end;
            }
            index=packet.end-1;
        }
        alpha.push_back(packet);
    }
    if(!alpha.empty()) {
        std::sort(alpha.begin(),alpha.end(),[](const Packet& a,const Packet& b) {
            if(a.target!=b.target)return a.target<b.target;
            if(a.priority!=b.priority)return a.priority<b.priority;
            if(a.key!=b.key)return a.key>b.key;
            return (a.bits&0x80000000u) ? a.begin<b.begin:a.begin>b.begin;
        });
        auto& ordered=orderingScratch_;
        struct ClearScratch {std::vector<EcstasyDrawCommand>& value;~ClearScratch(){value.clear();}} clear{ordered};
        ordered.clear();
        ordered.reserve(commands_.size()-pendingAlphaBegin_);
        for(auto index=pendingAlphaBegin_;index<commands_.size();++index)
            if(!commands_[index].depthSorted)ordered.push_back(std::move(commands_[index]));
        for(const auto& packet:alpha)
            for(auto index=packet.begin;index<packet.end;++index)
                ordered.push_back(std::move(commands_[index]));
        std::move(ordered.begin(),ordered.end(),commands_.begin()+static_cast<std::ptrdiff_t>(pendingAlphaBegin_));
    }
    pendingAlphaBegin_=commands_.size();
}

void EcstasyFrame::OverlayRVMImages(const CarCinematicServices& views,ResourceRegistry& resources)
{
    // PAL10EC70..10EDC8 (exclusive). Both textures belong to the canonical special-texture
    // pair refreshed by the resource owner. The source writes mask byte4F
    // before checking view3, then draws the two authored ALPHA materials.
    const auto* info=resources.RearViewTexture(0);
    const auto* mask=resources.RearViewTexture(1);
    if(!info || !mask)return;
    auto* maskTexture=textureInfoForDescriptor(mask);
    Require(maskTexture && maskTexture->pack,"Rear-view mask has no retained texture pack");
    maskTexture->pack->SetDescriptorByte4F(maskTexture->descriptor,1);
    const auto& rear=views.screens[3];
    if(!rear.enabled)return;
    Require(rear.target,"Rear-view overlay lost its actual target");
    if(!rear.target->enabled)return;
    Require(phase_==Phase::Building && rear.matricesCalculated && rear.target->id==7 &&
        rear.target->width==256 && rear.target->height==64,
        "Rear-view overlay lacks its original calculated view and target");
    const float maximum=rear.target->format==0x31 ? 16777215.0f:65535.0f;
    const float positions[4][2]={{1,-14},{257,-14},{257,76},{1,76}};
    const float coordinates[4][2]={{1,0},{0,0},{0,1},{1,1}};
    for(const auto* descriptor:{info,mask}) {
        const auto* texture=textureInfoForDescriptor(descriptor);
        Require(texture && texture->pixels.Data(),"Rear-view overlay texture pixels are not loaded");
        auto polygon=std::make_unique<frontend::FlailerPoly>();
        polygon->rasterState={{0x01000100u,0x80u,0,0}};
        for(std::size_t index=0;index<4;++index) {
            auto& vertex=polygon->vertices[index];
            vertex.position={{positions[index][0],positions[index][1],maximum,0}};
            vertex.textureCoordinate={{coordinates[index][0],coordinates[index][1],1}};
            vertex.color=0x80808080u;
        }
        EcstasyDrawCommand command;command.kind=EcstasyDrawKind::MirrorOverlayPolygon;
        command.polygon=polygon.get();command.texture=texture;command.view=&rear;command.target=rear.target;
        command.clipToScreen=&CaptureProjection(rear).matrices[0];
        command.rasterState=polygon->rasterState;command.zWriteEnabled=zWriteEnabled_;
        command.depthSorted=texture->requiresDepthSort;command.sortDepth=maximum;
        worldPolygons_.push_back(std::move(polygon));
        try {commands_.push_back(command);}catch(...) {worldPolygons_.pop_back();throw;}
    }
}

// PC-only viewport/capture adaptation. PAL's rear projection, source
// polygon, textures and game-state logic remain fixed at their proven values.
// The source (512 x 448 NTSC or 512 x 512 PAL) is fitted into a centered
// uniform-scale canvas. The physical rear color/depth attachments are grown
// to native pixel density; the unchanged source quad is transformed once
// by the renderer's existing logical-to-physical viewport mapping.
EcstasyNativeMirrorPresentation ecstasyNativeMirrorPresentation(
    std::uint32_t targetWidth,std::uint32_t targetHeight,
    std::uint32_t sourceWidth,std::uint32_t sourceHeight,
    std::uint32_t physicalWidth,std::uint32_t physicalHeight,
    std::uint32_t maximumImageDimension)
{
    Require(targetWidth && targetHeight && sourceWidth==512u &&
        (sourceHeight==448u || sourceHeight==512u) && physicalWidth &&
        physicalHeight && maximumImageDimension>=256u,
        "Native mirror presentation lost its valid PAL canvas or physical target");
    // Source DisplayRVMs has the *opposite* U coordinates and 160..352
    // coordinates in a 512-wide logical display. The Vulkan rearSample
    // shader already maps that original canvas to the PC output viewport;
    // scaling these CPU vertices again would clip the mirror at 1080p.
    // Keep the PAL polygon precisely as authored. Increase only target7's
    // physical raster (never the source camera's logical 256x64).
    const auto captureScale=std::max(1.0,std::min(
        std::max(static_cast<double>(physicalWidth)/sourceWidth,
                 static_cast<double>(physicalHeight)/sourceHeight),
        static_cast<double>(maximumImageDimension)/256.0));
    EcstasyNativeMirrorPresentation result{};
    result.left=160.0f;result.top=20.0f;
    result.right=352.0f;result.bottom=84.0f;
    result.rasterWidth=static_cast<std::uint32_t>(std::min<double>(maximumImageDimension,
        std::ceil(256.0*captureScale)));
    result.rasterHeight=static_cast<std::uint32_t>(std::min<double>(maximumImageDimension,
        std::ceil(64.0*captureScale)));
    return result;
}

EcstasyNativeMirrorScissor ecstasyNativeMirrorScissor(
    double viewportX,double viewportY,double viewportWidth,double viewportHeight,
    std::uint32_t sourceWidth,std::uint32_t sourceHeight,
    std::uint32_t outputWidth,std::uint32_t outputHeight,
    double translationX,double translationY)
{
    Require(sourceWidth==512u && (sourceHeight==448u || sourceHeight==512u) &&
        outputWidth && outputHeight && std::isfinite(viewportX) &&
        std::isfinite(viewportY) && std::isfinite(viewportWidth) &&
        std::isfinite(viewportHeight) && viewportWidth>0 && viewportHeight>0 &&
        std::isfinite(translationX) && std::isfinite(translationY),
        "Rear-view native scissor has no valid original canvas or viewport");
    // Vulkan's rectangle is [left,right) / [top,bottom). Round for pixel
    // centres, not downwards across the author's frame/mask boundary.
    const auto integer=[&](double value,std::uint32_t limit) {
        return static_cast<std::int32_t>(std::ceil(std::clamp(value-0.5,0.0,
            static_cast<double>(std::min<std::uint32_t>(limit,0x7fffffffu)))));
    };
    return {
        integer(viewportX+viewportWidth*160.0/sourceWidth+translationX,outputWidth),
        integer(viewportY+viewportHeight*20.0/sourceHeight+translationY,outputHeight),
        integer(viewportX+viewportWidth*352.0/sourceWidth+translationX,outputWidth),
        integer(viewportY+viewportHeight*84.0/sourceHeight+translationY,outputHeight)
    };
}

float ecstasyNativeMirrorMaskedSampleU(float originalU) noexcept
{
    // PAL OverlayRVMImages uses 1..257 on the 256-wide offscreen target.
    // Source x<1 is *camera only*, never overwritten by RVM/RVM_MASK. The
    // later DisplayRVMs reverses U, exposing precisely this column on the
    // right edge. The authentic RVM_MASK has a second dotted/transparent
    // edge column at x=1, but its next column x=2 is opaque in the glass
    // interior. Two source pixels plus a half-texel inset avoids both the
    // uncovered camera-only lane and bilinear bleed of the dotted border.
    constexpr float safeSourceU=2.5f/256.0f;
    return safeSourceU+(1.0f-safeSourceU)*originalU;
}

std::int32_t ecstasyNativeMirrorCompositeAlphaTest() noexcept
{
    // The real RVM_MASK is paletted PSMT8 with transparent outside pixels;
    // the source target7 alpha contains its shape. Drop only low-alpha edge
    // leakage before native bilinear composition. No shader replacement.
    return 2; // Existing GPU TEST alpha >48/128; PAL data remains unchanged.
}

void EcstasyFrame::DisplayRVMs(const CarCinematicServices& views,const CarCinematicScreen& destination)
{
    // Complete released source selection and polygon inputs10EDC8..10EEEC.
    // The original loop visits view3 only and tests both enabled owners.
    const auto& rear=views.screens[3];
    if(!rear.enabled)return;
    Require(rear.target,"Rear-view screen lost its actual render target");
    if(!rear.target->enabled)return;
    Require(phase_==Phase::Building && destination.target && destination.target->enabled &&
        destination.matricesCalculated && rear.target!=destination.target &&
        rear.target->id==7 && rear.target->width==256 && rear.target->height==64 &&
        sourceDisplayExtent_[0]==512 && (sourceDisplayExtent_[1]==448 || sourceDisplayExtent_[1]==512),
        "Rear-view composition lacks its original source/destination target or captured display extent");
    auto polygon=std::make_unique<frontend::FlailerPoly>();
    // Original ePoly constructor: neutral128-unit color and source alpha
    // equation. DisplayRVMs alone clears ScaleCoordinates (byte97).
    polygon->rasterState={{0x01000100u,0x00000080u,0,0}};
    const float maximum=destination.target->format==0x31 ? 16777215.0f:65535.0f;
    // Keep PAL 160..352 / 20..84 exactly. Native target7 scaling is
    // performed once during Vulkan target allocation / viewport projection,
    // never in this original 10EDC8..10EEEC scene composition function.
    const float positions[4][2]={{160,20},{352,20},{352,84},{160,84}};
    const float coordinates[4][2]={{1,0},{0,0},{0,1},{1,1}};
    for(std::size_t index=0;index<4;++index) {
        auto& vertex=polygon->vertices[index];
        vertex.position={{positions[index][0],positions[index][1],maximum,0}};
        vertex.textureCoordinate={{coordinates[index][0],coordinates[index][1],1}};
        vertex.color=0x80808080u;
    }
    EcstasyDrawCommand command;command.kind=EcstasyDrawKind::RenderTargetPolygon;
    command.polygon=polygon.get();command.view=&destination;command.target=destination.target;
    command.clipToScreen=&CaptureProjection(destination).matrices[0];
    command.sampledTarget=rear.target;command.zWriteEnabled=zWriteEnabled_;
    command.rasterState=polygon->rasterState;
    worldPolygons_.push_back(std::move(polygon));
    try {commands_.push_back(command);}catch(...) {worldPolygons_.pop_back();throw;}
}

static std::array<hp2::frontend::FlailerPoly,4> radialBlurPolygons(
    const EcstasyRenderTarget& target,uint8_t inner,uint8_t outer)
{
    const float left=static_cast<float>(target.left),top=static_cast<float>(target.top);
    const float right=static_cast<float>(target.left+target.width);
    const float bottom=static_cast<float>(target.top+target.height);
    // Source signed integer midpoint before conversion to float.
    const float middleX=static_cast<float>(target.left+target.width/2);
    const float middleY=static_cast<float>(target.top+target.height/2);
    const float z=target.format==0x31 ? 16777215.0f : 65535.0f;
    const float coordinates[4][2]={{0,0},{1,0},{1,1},{0,1}};
    std::array<hp2::frontend::FlailerPoly,4> output{};
    hp2::frontend::FlailerPoly p;
    // byte90..93 = 0,0,0,1; byte94..97 = 128,0,1,0.
    // (Cs-Cs)*As + Cd writes mask alpha while preserving RGB.
    p.rasterState={{0x01000000u,0x00010080u,0,0}};
    const float positions[4][2]={{left,top},{middleX,top},{middleX,middleY},{left,middleY}};
    for(size_t i=0;i<4;++i) {
        p.vertices[i].position={{positions[i][0],positions[i][1],z,0}};
        p.vertices[i].textureCoordinate={{coordinates[i][0],coordinates[i][1],1}};
        p.vertices[i].color=0x00808080u|(uint32_t(i==2?inner:outer)<<24);
    }
    output[0]=p;
    p.vertices[0].position[0]=p.vertices[3].position[0]=right;output[1]=p;
    p.vertices[0].position[1]=p.vertices[1].position[1]=bottom;output[2]=p;
    p.vertices[0].position[0]=p.vertices[3].position[0]=left;output[3]=p;
    return output;
}

EcstasyNativeFeedbackRegion ecstasyNativeFeedbackRegion(const EcstasyRenderTarget& target,
    std::uint32_t width,std::uint32_t height,std::uint32_t logicalWidth,std::uint32_t logicalHeight,
    const std::array<double,2>& translation)
{
    Require(width && height && logicalWidth && logicalHeight && target.width>0 && target.height>0 &&
        width<=static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) &&
        height<=static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()),
        "Colour feedback lost its native or logical raster extent");
    const double sx=static_cast<double>(width)/logicalWidth,sy=static_cast<double>(height)/logicalHeight;
    const double spanX=target.width*sx,spanY=target.height*sy;
    const double x=target.left*sx+translation[0]*width,y=target.top*sy+translation[1]*height;
    Require(std::isfinite(x) && std::isfinite(y),"Colour feedback display translation is nonfinite");
    const double left=std::clamp(x,0.0,static_cast<double>(width));
    const double top=std::clamp(y,0.0,static_cast<double>(height));
    const double right=std::clamp(x+spanX,0.0,static_cast<double>(width));
    const double bottom=std::clamp(y+spanY,0.0,static_cast<double>(height));
    EcstasyNativeFeedbackRegion result;
    if(right<=left || bottom<=top)return result;
    result.viewport={{static_cast<float>(left),static_cast<float>(top),
        static_cast<float>(right-left),static_cast<float>(bottom-top)}};
    const auto l=static_cast<std::int32_t>(std::floor(left)),t=static_cast<std::int32_t>(std::floor(top));
    result.scissor={{l,t,static_cast<std::int32_t>(std::ceil(right))-l,static_cast<std::int32_t>(std::ceil(bottom))-t}};
    result.textureRectangle={{static_cast<float>((left-x)/spanX),static_cast<float>((top-y)/spanY),
        static_cast<float>((right-x)/spanX),static_cast<float>((bottom-y)/spanY)}};
    return result;
}

void EcstasyFrame::DisplayFilter(const CarCinematicServices& views,ResourceRegistry& resources)
{
    Require(phase_==Phase::Building,"Screen filter belongs to the current display frame");
    if(!nativeRenderPolicy.screenFilters[0].enabled && !nativeRenderPolicy.screenFilters[1].enabled)return;
    AttachAlphaPolys();
    const auto* descriptor=resources.SpecialTexture(3); // PAL103F1C/24: FilterGrey02C8711C.
    // PAL10F700/704 explicitly permits this optional development resource
    // to be absent. Retain that null gate; never manufacture a substitute.
    if(!descriptor)return;
    const auto* texture=textureInfoForDescriptor(descriptor);
    Require(texture && texture->pixels.Data(),"Screen filter lost its released FilterGrey resource");
    const auto group=++nextFeedbackGroup_;
    Require(group!=0,"Native colour-feedback group count overflowed");
    for(std::size_t index=1;index<3;++index) {
        const auto& view=views.screens[index];const auto& filter=nativeRenderPolicy.screenFilters[index-1];
        if(!filter.enabled || !view.target || !view.target->enabled)continue;
        // The released mode6/7 leaves select this exact destination equation
        // and TFX0. Reject a different descriptor rather than substituting it.
        Require(filter.blend==std::array<std::uint8_t,4>{{1,0,2,1}} && filter.textureFunction==0,
            "Screen filter has an unsupported original destination equation");
        EcstasyDrawCommand command;command.kind=EcstasyDrawKind::DestinationFilterPass;
        command.view=&view;command.target=view.target;command.texture=texture;
        command.screenFilter=filter;command.feedbackGroup=group;command.zWriteEnabled=false;
        commands_.push_back(command);
    }
    pendingAlphaBegin_=commands_.size();
}

void EcstasyFrame::DisplayRadialBlur(const CarCinematicServices& views)
{
    Require(phase_==Phase::Building,"Radial blur belongs to the current display frame");
    if(!views.radialBlur)return;
    for(std::size_t index=1;index<3;++index) {
        const auto& view=views.screens[index];
        if(!view.enabled || !view.radialBlurTexture || !view.target || !view.target->enabled)continue;
        Require(view.matricesCalculated && view.radialBlurTexture->pixels.Data(),
            "Radial blur lost its authored texture or calculated view");
        const auto polygons=radialBlurPolygons(*view.target,views.radialBlur->firstAmount[index-1],
            views.radialBlur->secondAmount[index-1]);
        for(const auto& polygon:polygons) {
            auto owned=std::make_unique<frontend::FlailerPoly>(polygon);
            EcstasyDrawCommand command;command.kind=EcstasyDrawKind::ScreenFilterPolygon;
            command.polygon=owned.get();command.texture=view.radialBlurTexture;
            command.view=&view;command.target=view.target;command.rasterState=polygon.rasterState;
            command.zWriteEnabled=zWriteEnabled_;
            command.clipToScreen=&CaptureProjection(view).matrices[0];
            worldPolygons_.push_back(std::move(owned));
            try {commands_.push_back(command);}catch(...) {worldPolygons_.pop_back();throw;}
        }
        AttachSolidPolys(); // PAL10F4A0 calls10BB20: mask solid chain only.
    }
}

void EcstasyFrame::DisplayBlendPrevious(const CarCinematicServices& views)
{
    Require(phase_==Phase::Building,"History blend belongs to the current display frame");
    const auto* previous=views.screens[0].target;
    if(!previous || !previous->enabled)return;
    for(std::size_t index=1;index<4;++index) {
        const auto& view=views.screens[index];
        if(!view.enabled || !view.radialBlurTexture || !view.target || !view.target->enabled)continue;
        Require(view.matricesCalculated && previous->width>0 && previous->height>0 &&
            sourceDisplayExtent_[0] && sourceDisplayExtent_[1],"History blend lacks its display projection");
        const auto& target=*view.target;
        auto polygon=std::make_unique<frontend::FlailerPoly>();
        polygon->rasterState={{0x01010100u,0x00010080u,0,0}};
        const float left=static_cast<float>(target.left),top=static_cast<float>(target.top);
        const float right=left+static_cast<float>(target.width),bottom=top+static_cast<float>(target.height);
        const float maximum=target.format==0x31 ? 16777215.0f:65535.0f;
        const float positions[4][2]={{left,top},{right,top},{right,bottom},{left,bottom}};
        // PAL10F600..10F64C addresses a512x512 previous buffer, including
        // half a SOURCE pixel. Native history contains only the active raster,
        // so remove console padding and scale the same authored coordinates.
        // Deliberately preserve the source max=width/height, not right/bottom.
        const float u0=left/static_cast<float>(previous->width)+0.5f/static_cast<float>(sourceDisplayExtent_[0]);
        const float v0=top/static_cast<float>(previous->height)+0.5f/static_cast<float>(sourceDisplayExtent_[1]);
        const float u1=static_cast<float>(target.width)/static_cast<float>(previous->width)-0.5f/static_cast<float>(sourceDisplayExtent_[0]);
        const float v1=static_cast<float>(target.height)/static_cast<float>(previous->height)-0.5f/static_cast<float>(sourceDisplayExtent_[1]);
        const float coordinates[4][2]={{u0,v0},{u1,v0},{u1,v1},{u0,v1}};
        for(std::size_t vertex=0;vertex<4;++vertex) {
            polygon->vertices[vertex].position={{positions[vertex][0],positions[vertex][1],maximum,0}};
            polygon->vertices[vertex].textureCoordinate={{coordinates[vertex][0],coordinates[vertex][1],1}};
            polygon->vertices[vertex].color=0x80808080u;
        }
        EcstasyDrawCommand command;command.kind=EcstasyDrawKind::ScreenFilterPolygon;
        command.polygon=polygon.get();command.view=&view;command.target=view.target;
        command.sampledTarget=previous;command.sampledPreviousFrame=true;
        command.zWriteEnabled=zWriteEnabled_;command.rasterState=polygon->rasterState;
        command.clipToScreen=&CaptureProjection(view).matrices[0];
        worldPolygons_.push_back(std::move(polygon));
        try {commands_.push_back(command);}catch(...) {worldPolygons_.pop_back();throw;}
        AttachSolidPolys(); // PAL10F670 calls10BB20: history solid chain only.
    }
}

namespace {
const std::array<float,16>& CaptureMaterialProjection(
    const TextureInfo& texture,const EcstasyDrawProjection& projection)
{
    // PAL AttachSolidPolys10BC0C/10BC54..10BC78 and
    // AttachAlphaPolys10B9D8..10BA04 select the target's +8C/+90/+94
    // projection with the texture's byte75. This is independent of alpha
    // sorting and Z-write. The authored shadow/road/lava overlays use1.
    // Capture it with the issuing view: sky, world and auxiliary targets
    // may recalculate these same matrices before the native draw is gathered.
    if(!texture.descriptor || !texture.descriptor->authoredRecord || texture.descriptor->authoredRecordSize<164)
        throw std::runtime_error("World material lost its original projection selector");
    const auto mode=texture.descriptor->authoredRecord[0x75];
    return projection.matrices[mode==1 || mode==2 ? mode:0];
}
}

void EcstasyFrame::RenderPolygon3D(const frontend::FlailerPoly& polygon,
    const TextureInfo* texture,const float localToWorld[16],const CarCinematicScreen& view)
{
    // PAL10FF88..110238. The final release uses the fixed0x200 draw flag;
    // beta's descriptor4F-dependent flag is absent from the release body.
    if (!texture)return;
    Require(phase_==Phase::Building,"Cannot append to a sealed graphics frame");
    Require(localToWorld && view.target && view.target->enabled &&
        view.target->width>0 && view.target->height>0 && texture->pixels.Data(),
        "World polygon lacks its actual target, matrix or loaded texture");
    auto owned=std::make_unique<frontend::FlailerPoly>(polygon);
    EcstasyDrawCommand command;command.zWriteEnabled=zWriteEnabled_;
    command.kind=EcstasyDrawKind::WorldPolygon;command.polygon=owned.get();
    command.textureQScale=view.pixelScale;
    command.clipToScreen=&CaptureMaterialProjection(*texture,CaptureProjection(view));
    command.texture=texture;command.view=&view;command.target=view.target;
    command.instance=&CaptureInstance(localToWorld,view);
    command.rasterState=polygon.rasterState;command.drawFlags=0x200u;
    command.vertexOrder={0,1,3,2};command.depthSorted=texture->requiresDepthSort;
    if (command.depthSorted) {
        // Exact source alpha-sort key: vertex0 is transformed by worldToView
        // alone. Applying localToWorld here would change the original key.
        float depth[4];eMulVector3(depth,view.matrices.worldToView,polygon.vertices[0].position.data());
        command.sortDepth=depth[2];
    }
    worldPolygons_.push_back(std::move(owned));
    try {commands_.push_back(command);}catch (...) {worldPolygons_.pop_back();throw;}
}

void EcstasyFrame::RenderWorldStrip(const std::vector<frontend::FlailerPolyVertex>& vertices,
    const TextureInfo& texture,const float localToWorld[16],const CarCinematicScreen& view,EcstasyCarShadow shadow)
{
    Require(phase_==Phase::Building,"Cannot append world strip to a sealed graphics frame");
    Require(localToWorld && view.target && view.target->enabled && view.matricesCalculated &&
        texture.pixels.Data(),"World strip lacks its source target, matrix or texture");
    // PAL1222B8 emits up to2*(64+1) vertices for a convex car-shadow border.
    Require(vertices.size()>=3 && vertices.size()<=130,"World strip exceeds its original immediate extent");
    auto owned=std::make_unique<std::vector<frontend::FlailerPolyVertex>>(vertices);
    EcstasyDrawCommand command;command.kind=EcstasyDrawKind::WorldStrip;
    command.textureQScale=view.pixelScale;
    command.clipToScreen=&CaptureMaterialProjection(texture,CaptureProjection(view));
    command.worldStrip=owned.get();command.texture=&texture;command.view=&view;command.target=view.target;
    command.carShadow=shadow;
    command.zWriteEnabled=zWriteEnabled_;command.drawFlags=0x200u;
    command.instance=&CaptureInstance(localToWorld,view);
    command.depthSorted=texture.requiresDepthSort;
    if(command.depthSorted) {
        float depth[4];eMulVector3(depth,view.matrices.worldToView,vertices.front().position.data());
        command.sortDepth=depth[2];
    }
    worldStrips_.push_back(std::move(owned));
    try {commands_.push_back(command);}catch(...) {worldStrips_.pop_back();throw;}
}

struct EcstasyFrame::StripCaptureState {
    // Constructed once on the issuing thread, before workers observe it.
    EcstasyDrawCommand command;
    const EcstasyDrawInstance* instance{};
    const EcstasyDrawProjection* projection{};
    Phase phase{};
    bool zWriteEnabled{},dashboardProjection{},inGameMaterials{},environmentUpdated{};
    const TextureInfo* frontendEnvironmentTexture{};
    const TextureInfo* specularFalloffTexture{};
    const EnvironmentMap* environment{};
    std::int32_t environmentApplicationState{};
    std::array<std::int32_t,5> reflectionBlend{};
};

EcstasyFrame::StripCaptureState EcstasyFrame::CaptureStripState() const
{
    StripCaptureState state;
    state.phase=phase_;state.zWriteEnabled=zWriteEnabled_;
    state.dashboardProjection=dashboardProjection_;state.inGameMaterials=inGameMaterials_;
    state.environmentUpdated=environmentUpdated_;state.environment=environment_;
    state.frontendEnvironmentTexture=frontendEnvironmentTexture_;
    state.specularFalloffTexture=specularFalloffTexture_;
    state.environmentApplicationState=environmentApplicationState_;
    state.reflectionBlend=nativeRenderPolicy.reflectionBlend;
    return state;
}

void EcstasyFrame::RenderStrip(const EditorMarkerModel& model,
    const EditorMarkerSolidGeometry::Strip& strip,const TextureInfo& texture,
    const EditorMarkerLightMaterial* material,const float localToWorld[16],
    const CarCinematicScreen& view,std::int32_t clipClass,std::uint32_t flags,float alphaScale,
    const SceneLightContext* lightContext)
{
    auto state=CaptureStripState();
    state.instance=&CaptureInstance(localToWorld,view,lightContext);
    state.projection=&CaptureProjection(view);
    CaptureStrip(commands_,nextAlphaGroup_,state,model,strip,texture,material,
        localToWorld,view,clipClass,flags,alphaScale,lightContext);
}

void EcstasyFrame::CaptureStrip(std::vector<EcstasyDrawCommand>& output,
    std::uint64_t& alphaGroups,const StripCaptureState& state,const EditorMarkerModel& model,
    const EditorMarkerSolidGeometry::Strip& strip,const TextureInfo& texture,
    const EditorMarkerLightMaterial* material,const float localToWorld[16],
    const CarCinematicScreen& view,std::int32_t clipClass,std::uint32_t flags,float alphaScale,
    const SceneLightContext* lightContext)
{
    Require(state.phase==Phase::Building,"Cannot append model geometry to a sealed frame");
    Require(localToWorld && view.target && view.target->enabled && view.matricesCalculated &&
        texture.pixels.Data(),"Solid strip lacks its actual matrix, target or loaded texture");
    Require(strip.positions.size()==strip.textureCoordinates.size() &&
        strip.positions.size()==strip.colors.size(),"Solid strip channels have different extents");
    if (strip.positions.size()<3)return;
    auto command=state.command;command.zWriteEnabled=state.zWriteEnabled;
    command.kind=EcstasyDrawKind::SolidStrip;command.strip=&strip;command.model=&model;
    command.textureQScale=view.pixelScale;
    command.clipToScreen=&CaptureMaterialProjection(texture,*state.projection);
    if(state.dashboardProjection)command.clipToScreen=&state.projection->matrices[2];
    if(material && state.inGameMaterials && material->inGameVariant)material=material->inGameVariant.get();
    command.texture=&texture;command.lightMaterial=material;command.view=&view;command.target=view.target;
    command.instance=state.instance;
    command.clipClass=clipClass;command.drawFlags=flags;command.alphaScale=alphaScale;
    command.depthSorted=texture.requiresDepthSort;
    if (command.depthSorted) {
        // Complete PAL10CBF8 sort-key calculation: centre of the strip bounds,
        // then the supplied model transform, then the actual world-to-view.
        float center[4],world[4],camera[4];
        for (std::size_t axis=0;axis<3;++axis)center[axis]=(strip.minimum[axis]+strip.maximum[axis])*0.5f;
        center[3]=1.0f;eMulVector4(world,localToWorld,center);
        eMulVector4(camera,view.matrices.worldToView,world);command.sortDepth=camera[2];
    }
    if(flags&0x140u) {
        Require(lightContext && material && strip.normals.size()==strip.positions.size(),
            "Reflected strip lacks its original normal/material/light context");
        command.orderingTexture=&texture;
        if(command.depthSorted)command.alphaGroup=++alphaGroups;
        output.push_back(command);
        // Source VU0618 updates the base alpha, then0348/0350 submits
        // that base packet. The separate0320 context reuses its ST/RGBA
        // with the real SpecularFalloff texture and destination-alpha ALPHA58.
        if(flags&0x40u) {
            Require(state.specularFalloffTexture && state.specularFalloffTexture->pixels.Data(),
                "Specular material has no authored SpecularFalloff texture");
            EcstasyDrawCommand specular=command;specular.kind=EcstasyDrawKind::SolidSpecularStrip;
            specular.texture=state.specularFalloffTexture;specular.rasterState={{0x01010200u,64,0,0}};
            output.push_back(specular);
        }
        if(!(flags&0x100u))return;
        EcstasyDrawCommand reflection=command;reflection.kind=EcstasyDrawKind::SolidReflectionStrip;
        // PAL10B08C..10B0F8: only the second reflection context outside
        // the frontend uses these five writable ALPHA selectors. Material
        // ALPHA and the first reflection context keep their own source state.
        const auto& blend=state.reflectionBlend;
        const bool overrideReflection=(flags&4u) && state.environmentApplicationState!=3;
        const auto reflectionAlpha=static_cast<std::uint32_t>(blend[0]) |
            (static_cast<std::uint32_t>(blend[1])<<8) |
            (static_cast<std::uint32_t>(blend[2])<<16) |
            (static_cast<std::uint32_t>(blend[3])<<24);
        reflection.rasterState={{overrideReflection ? reflectionAlpha:0x01000200u,
            static_cast<std::uint32_t>(state.environmentApplicationState==3 ? 64:overrideReflection ? blend[4]:16),0,0}};
        if(state.environmentApplicationState==3) {
            Require(state.frontendEnvironmentTexture && state.frontendEnvironmentTexture->pixels.Data(),
                "Frontend reflected strip has no loaded CarSelectEnvMap texture");
            reflection.texture=state.frontendEnvironmentTexture;
            reflection.showcaseEnvironmentBlend=true;
        } else {
            Require(state.environment,"Reflected strip has no original environment-map owner");
            reflection.texture=nullptr;reflection.sampledTarget=&state.environment->MappedTarget();
            reflection.sampledPreviousFrame=!state.environmentUpdated;
        }
        output.push_back(reflection);
    } else output.push_back(command);
}

// Evaluated directly from authored exponent/amplitude pairs on the PC CPU.
// The per-material table avoids expensive powf calls for every lit vertex.
// The fallback covers programmatically supplied test/editor materials.
static float NativeSpecularResponse(const EditorMarkerLightMaterial& material,
    float base,bool alphaMaterial)
{
    const auto* parameters=material.NativeCoefficients();
    const float exponent=parameters[alphaMaterial ? 12:0];
    const float amplitude=parameters[alphaMaterial ? 13:1];
    base=std::max(base,parameters[2]);
    if(material.nativeSpecularReady && base>=0.1f && base<=1.0f) {
        constexpr float kScale=(EditorMarkerLightMaterial::kNativeResponseSamples-1)/0.9f;
        const float scaled=(base-0.1f)*kScale;
        const auto index=std::min(static_cast<std::size_t>(scaled),
            EditorMarkerLightMaterial::kNativeResponseSamples-2);
        const float fraction=scaled-static_cast<float>(index);
        const auto& values=material.nativeSpecularResponse[alphaMaterial ? 1:0];
        return values[index]+fraction*(values[index+1]-values[index]);
    }
    return amplitude*std::pow(base,exponent);
}

// A material/light pair is immutable while its sealed frame is gathered.
// Prepare these products once for a strip group; the vertex evaluator keeps
// the original diffuse/specular addition and quantization order unchanged.
struct NativeVertexLighting {
    std::array<float,4> ambient{};
    std::array<std::array<float,3>,4> diffuse{},specular{};
    bool materialAlpha{},litSpecular{},alphaSpecular{},needReflection{};
};
static NativeVertexLighting PrepareNativeVertexLighting(const SceneLightContext& context,
    const EditorMarkerLightMaterial& material,std::uint32_t flags)
{
    const auto* coefficients=material.NativeCoefficients();
    const bool materialAlpha=(flags&4u)!=0;
    const bool rgbSpecular=coefficients[8]!=0.0f || coefficients[9]!=0.0f ||
        coefficients[10]!=0.0f;
    const bool litSpecular=(flags&0x80u) && (rgbSpecular ||
        (materialAlpha && coefficients[15]!=0.0f));
    const bool alphaSpecular=(flags&0x40u) && coefficients[15]!=0.0f;
    const bool needReflection=litSpecular || alphaSpecular;
    NativeVertexLighting result;
    result.materialAlpha=materialAlpha;result.litSpecular=litSpecular;
    result.alphaSpecular=alphaSpecular;result.needReflection=needReflection;
    if(flags&0x80u)for(std::size_t axis=0;axis<4;++axis) {
        result.ambient[axis]=axis==3 && materialAlpha ? coefficients[14]:context.colorMatrix[12+axis];
        for(std::size_t light=0;light<3;++light) {
            result.diffuse[axis][light]=axis<3 ? context.colorMatrix[light*4+axis]*coefficients[4+axis]:
                (materialAlpha ? 0.0f:context.colorMatrix[light*4+axis]);
            result.specular[axis][light]=axis<3 ? context.colorMatrix[light*4+axis]*coefficients[8+axis]:
                (materialAlpha && light==0 ? coefficients[15]:0.0f);
        }
    }
    return result;
}
static std::array<float,4> EvaluateNativeVertexColor(const SceneLightContext& context,
    const EditorMarkerLightMaterial& material,const float position[3],const float normal[3],
    std::uint16_t color,std::uint32_t flags,const NativeVertexLighting& lighting,
    const float* preparedReflection=nullptr)
{
    // Native PC diffuse/specular evaluation. Retain authored lights/material
    // coefficients and the original channel accumulation/blend order.
    const auto* coefficients=material.NativeCoefficients();
    const bool materialAlpha=lighting.materialAlpha;
    const bool litSpecular=lighting.litSpecular;
    const bool alphaSpecular=lighting.alphaSpecular;
    const bool needReflection=lighting.needReflection;
    float reflection[3]{};
    if(needReflection && preparedReflection)std::copy(preparedReflection,preparedReflection+3,reflection);
    else if(needReflection) {
        float relative[3],twiceDot=0.0f;
        for(std::size_t axis=0;axis<3;++axis) {
            relative[axis]=position[axis]-context.localEye[axis];
            twiceDot+=relative[axis]*normal[axis];twiceDot+=relative[axis]*normal[axis];
        }
        float squared=0.0f;
        for(std::size_t axis=0;axis<3;++axis) {
            reflection[axis]=relative[axis]-normal[axis]*twiceDot;
            squared+=reflection[axis]*reflection[axis];
        }
        const float inverseLength=squared>0.0f ? 1.0f/std::sqrt(squared):0.0f;
        for(auto& axis:reflection)axis*=inverseLength;
    }
    std::array<float,4> output{{static_cast<float>(color&31u)/16.0f,
        static_cast<float>((color>>5)&31u)/16.0f,static_cast<float>((color>>10)&31u)/16.0f,1}};
    // Both released strip dispatchers gate VU1998 on bit0x80 (VU02B8
    // and04E0). Bit0x40 independently requests the alpha-only VU0618
    // pass below. Do not calculate a full three-light color only to replace
    // every channel with the unchanged authored color for that path.
    if(flags&0x80u) {
      std::array<float,3> diffuse{},specular{};
      for(std::size_t light=0;light<3;++light) {
        float normalDot=0.0f;
        for(std::size_t axis=0;axis<3;++axis)
            normalDot+=context.directionMatrix[axis*4+light]*normal[axis];
        diffuse[light]=std::max(normalDot,0.0f);
        if(litSpecular) {
            float reflectionDot=0.0f;
            for(std::size_t axis=0;axis<3;++axis)
                reflectionDot+=context.directionMatrix[axis*4+light]*reflection[axis];
            specular[light]=NativeSpecularResponse(material,reflectionDot,materialAlpha);
        }
      }
      for(std::size_t axis=0;axis<4;++axis) {
        float value=lighting.ambient[axis];
        for(std::size_t light=0;light<3;++light) {
            value+=lighting.diffuse[axis][light]*diffuse[light];
        }
        for(std::size_t light=0;light<3;++light) {
            value+=lighting.specular[axis][light]*specular[light];
        }
        if(axis<3) {
            const float authored=static_cast<float>(((color>>(axis*5))&31u)*8u);
            value*=1.0f+(authored-255.0f)*coefficients[3];
        }
        Require(std::isfinite(value),"Released material lighting produced a non-finite color");
        output[axis]=static_cast<float>(static_cast<std::int32_t>(std::clamp(value,0.0f,255.0f)))/128.0f;
      }
    }
    if(flags&0x40u) {
        // VU0618..09C0 is a subsequent alpha-only write: the RGB from
        // VU1998 (when enabled) or the authored V4_5 stream is unchanged.
        float dot=0;
        if(alphaSpecular)
            for(std::size_t axis=0;axis<3;++axis)dot+=context.directionMatrix[axis*4]*reflection[axis];
        const float specularAlpha=alphaSpecular ? NativeSpecularResponse(material,dot,true):0.0f;
        const float alpha=coefficients[14]+coefficients[15]*specularAlpha;
        Require(std::isfinite(alpha),"Released specular alpha is non-finite");
        output[3]=static_cast<float>(static_cast<std::int32_t>(std::clamp(alpha,0.0f,255.0f)))/128.0f;
    }
    return output;
}

std::array<float,4> EcstasyLitVertexColor(const SceneLightContext& context,
    const EditorMarkerLightMaterial& material,const float position[3],const float normal[3],
    std::uint16_t color,std::uint32_t flags)
{
    const auto lighting=PrepareNativeVertexLighting(context,material,flags);
    return EvaluateNativeVertexColor(context,material,position,normal,color,flags,lighting);
}

static EcstasyReflectionVertex ReflectNativeVertex(const SceneLightContext& context,
    const EditorMarkerLightMaterial& material,const float position[3],const float normal[3],
    std::uint16_t color,std::uint32_t flags,const NativeVertexLighting* lighting,
    bool reuseBaseColor=false)
{
    // Native PC sphere-map direction. No console reciprocal-root or VU
    // bit interpretation remains in the vertex hot path.
    const auto inverseLength=[](float squared) {
        return squared>0.0f ? 1.0f/std::sqrt(squared):0.0f;
    };
    float relative[3],reflection[3];float twiceDot=0.0f;
    for(std::size_t axis=0;axis<3;++axis) {
        relative[axis]=position[axis]-context.localEye[axis];
        twiceDot+=relative[axis]*normal[axis];twiceDot+=relative[axis]*normal[axis];
    }
    float squared=0.0f;
    for(std::size_t axis=0;axis<3;++axis) {
        reflection[axis]=relative[axis]-normal[axis]*twiceDot;
        squared+=reflection[axis]*reflection[axis];
    }
    const auto inverse=inverseLength(squared);
    for(auto& axis:reflection)axis*=inverse;
    float mapped[3];
    for(std::size_t axis=0;axis<3;++axis) {
        // The VU negates x,z,w of every matrix column before multiplication.
        const float sign=axis==1 ? 1.0f:-1.0f;
        mapped[axis]=(context.environmentMatrix[axis]*sign)*reflection[0];
        mapped[axis]+=(context.environmentMatrix[4+axis]*sign)*reflection[1];
        mapped[axis]+=(context.environmentMatrix[8+axis]*sign)*reflection[2];
    }
    const float z=mapped[2]+1.0f;
    squared=mapped[0]*mapped[0]+mapped[1]*mapped[1];squared+=z*z;
    const float sphereInverse=inverseLength(squared);
    EcstasyReflectionVertex output;
    output.uv={{0.5f+(mapped[0]*sphereInverse)*0.5f,0.5f+(mapped[1]*sphereInverse)*0.5f}};
    output.color={{1,1,1,1}};
    if((flags&4u) && !reuseBaseColor) {
        if(flags&0x80u) {
            Require(lighting!=nullptr,"Reflected vertex lost its prepared material lighting");
            output.color=EvaluateNativeVertexColor(context,material,position,normal,color,flags,*lighting,reflection);
        }
        else output.color={{static_cast<float>(color&31u)/16.0f,
            static_cast<float>((color>>5)&31u)/16.0f,static_cast<float>((color>>10)&31u)/16.0f,1}};
    }
    const auto* coefficients=material.NativeCoefficients();
    const float alpha=coefficients[7]+coefficients[11]*mapped[2];
    Require(std::isfinite(alpha),"Environment material produced non-finite alpha");
    output.color[3]=static_cast<float>(static_cast<std::uint32_t>(convertFloatToInt32(alpha))&255u)/128.0f;
    return output;
}

EcstasyReflectionVertex EcstasyReflectVertex(const SceneLightContext& context,
    const EditorMarkerLightMaterial& material,const float position[3],const float normal[3],
    std::uint16_t color,std::uint32_t flags)
{
    if((flags&4u) && (flags&0x80u)) {
        const auto lighting=PrepareNativeVertexLighting(context,material,flags);
        return ReflectNativeVertex(context,material,position,normal,color,flags,&lighting);
    }
    return ReflectNativeVertex(context,material,position,normal,color,flags,nullptr);
}

bool EcstasyFrame::RenderOpaqueSceneryBatch(OpaqueSceneryBatch& batch,const EditorMarkerModel& model,
    const float localToWorld[16],const CarCinematicScreen& view,const SceneLightContext* lightContext)
{
    const auto* solid=model.solid;
    if(!solid || !solid->geometry || model.replacement || solid->rigidAnimation ||
        solid->vertexAnimation || (solid->renderFlags&0x1000u) ||
        dashboardProjection_ || !zWriteEnabled_)return false;

    const auto& geometry=*solid->geometry;
    const auto revision=texturePackRegistry.BindingRevision();
    const bool reused=batch.geometry==&geometry && batch.textureRevision==revision &&
        batch.bindings.size()==geometry.textures.size() &&
        std::equal(batch.bindings.begin(),batch.bindings.end(),geometry.textures.begin(),
            [](const auto* saved,const auto& live) {return saved==live.texture;});
    if(!reused) {
        batch.textureRevision=0;
        batch.geometry=&geometry;
        batch.eligible=true;
        batch.bindings.clear();batch.strips.clear();
        for(const auto& binding:geometry.textures)batch.bindings.push_back(binding.texture);
        for(const auto& strip:geometry.strips) {
            if((strip.flags&0x400u) || strip.positions.size()<3)continue;
            const auto index=static_cast<std::size_t>(strip.textureIndex);
            const auto* texture=index<batch.bindings.size() && batch.bindings[index]?
                textureInfoForDescriptor(batch.bindings[index]):nullptr;
            if(!texture || texture->requiresDepthSort || (strip.flags&0x140u)) {
                batch.eligible=false;
                break;
            }
            batch.strips.push_back({&strip,texture,
                static_cast<std::uint32_t>(strip.flags)|(texture->drawFlag4Enabled?4u:0u)});
        }
        batch.textureRevision=revision;
    }
    if(!batch.eligible || std::any_of(batch.strips.begin(),batch.strips.end(),
        [](const auto& strip) {return !strip.texture->pixels.Data();}))return false;

    Require(phase_==Phase::Building && view.target && view.target->enabled && view.matricesCalculated,
        "Opaque scenery batch lost its live view");
    EcstasyDrawCommand command;
    command.kind=EcstasyDrawKind::SolidStrip;command.model=&model;command.cachedOpaqueScenery=true;
    command.view=&view;command.target=view.target;command.clipClass=2;
    command.textureQScale=view.pixelScale;
    command.instance=&CaptureInstance(localToWorld,view,lightContext);
    const auto& projection=CaptureProjection(view);
    const auto* root=model.resources && model.resources->catalogueRoot ?
        model.resources->catalogueRoot:model.resources;
    const auto* fallback=root?root->defaultLightMaterial:nullptr;
    for(const auto& strip:batch.strips) {
        if(command.texture!=strip.texture) {
            command.texture=strip.texture;
            command.clipToScreen=&CaptureMaterialProjection(*strip.texture,projection);
        }
        command.strip=strip.geometry;command.drawFlags=lightContext?strip.flags:strip.flags&~0x1c0u;
        const auto index=static_cast<std::size_t>(strip.geometry->lightMaterialIndex);
        auto* material=index<geometry.lightMaterials.size()?geometry.lightMaterials[index]:nullptr;
        if(!material)material=fallback;
        command.lightMaterial=material && inGameMaterials_ && material->inGameVariant ?
            material->inGameVariant.get():material;
        commands_.push_back(command);
    }
    if(reused)reusedOpaqueSceneryStrips_+=batch.strips.size();
    return true;
}

std::size_t EcstasyFrame::RenderModel(EditorMarkerModel& model,const float localToWorld[16],
    const CarCinematicScreen& view,std::uint32_t flags,float alphaScale,std::int32_t worldFrames,
    const SceneLightContext* lightContext,const std::vector<std::array<float,16>>* boneMatrices)
{
    // PAL10DC80 delegates rigid animation before culling and texture-table
    // replacement before strip dispatch. The source's temporary DMA memory
    // is replaced by frame-owned matrix values; all geometry stays borrowed.
    if (!model.solid)return 0;
    Require(localToWorld && model.solid->geometry,"Rendered solid has no decoded geometry owner");
    float animated[16];
    if (!boneMatrices && model.solid->rigidAnimation && model.rigidAnimationStart) {
        const auto time=editorMarkerModelRigidAnimationTime(&model,worldFrames);
        if (time<0)model.rigidAnimationStart=0;
        else {
            float sample[16];
            Require(editorMarkerRigidAnimationMatrixAtTime(model.solid->rigidAnimation,sample,time)!=0,
                "Rigid model animation has no authored frame");
            eMulMatrix(animated,sample,localToWorld);localToWorld=animated;
        }
    }
    editorMarkerModelSetupVertexAnimation(&model,worldFrames);
    auto& visible=visibleScratch_;
    auto& sources=modelStripScratch_;
    auto& chunks=modelCaptureScratch_;
    auto& textures=modelTextureScratch_;
    struct ClearCapture {
        std::vector<EcstasyVisibleStrip>& visible;
        std::vector<ModelStripSource>& sources;
        std::vector<ModelCaptureChunk>& chunks;
        std::vector<const TextureInfo*>& textures;
        std::size_t chunksUsed{};
        ~ClearCapture() {
            visible.clear();sources.clear();textures.clear();
            for(std::size_t index=0;index<chunksUsed;++index) {
                auto& chunk=chunks[index];
                chunk.commands.clear();chunk.animatedStrips.clear();chunk.alphaGroups=0;
            }
        }
    } clear{visible,sources,chunks,textures};
    if (!ecstasyDetermineVisibleStrips(visible,model,view,localToWorld,boneMatrices!=nullptr,
        boneMatrices ? 1:((flags&4u) ? 2:1)))return 0;
    // These are the original live model/solid mutations, once and in source
    // order. No worker changes model animation, texture tables or registries.
    if (model.replacement)editorMarkerModelApplyReplacementTextureTable(&model);
    const auto& geometry=*model.solid->geometry;
    const auto start=commands_.size();
    auto state=CaptureStripState();
    state.instance=&CaptureInstance(localToWorld,view,lightContext);
    state.projection=&CaptureProjection(view);
    const bool environmentEnabled=environmentMappingEnabled!=0;
    std::size_t count=0;
    for(const auto& run:visible)count+=run.count;
    // Task size amortizes queue publication; CPU count is discovered by the
    // shared scheduler and is never restricted to a fixed application cap.
    constexpr std::size_t minimumParallelStrips=512,stripsPerChunk=64;
    const bool parallel=count>=minimumParallelStrips && NativeSceneEffectiveHardwareConcurrency()>1;
    if(parallel) {
        // LookupDescriptor can rebuild a lazy resource index. Resolve every
        // binding on the owner after the original shared-table replacement;
        // workers consume these actual TextureInfo pointers without any
        // registry access, even if optional cache allocation failed.
        textures.reserve(geometry.textures.size());
        for(const auto& binding:geometry.textures)
            textures.push_back(textureInfoForDescriptor(binding.texture));
    }
    const EditorMarkerLightMaterial* fallbackMaterial=nullptr;
    if(model.resources) {
        const auto* root=model.resources->catalogueRoot ? model.resources->catalogueRoot:model.resources;
        fallbackMaterial=root->defaultLightMaterial;
    }
    // This exact multiplication is shared by the model's ordered strip
    // packets. The owner does not advance the view or any borrowed input
    // before RunNativeSceneJobs has drained, including exceptional exits.
    const auto capture=[&](std::size_t index,std::int32_t clipClass,
        std::vector<EcstasyDrawCommand>& output,
        std::vector<std::unique_ptr<EditorMarkerSolidGeometry::Strip>>& posedStrips,
        std::uint64_t& alphaGroups) {
        Require(index<geometry.strips.size(),"Visible strip run leaves its actual model");
        const auto& strip=geometry.strips[index];
        if (strip.flags&0x400u)return;
        const auto& descriptor=stripTexture(geometry,strip);
        const auto* texture=parallel ? textures[static_cast<std::size_t>(strip.textureIndex)]:
            textureInfoForDescriptor(&descriptor);
        Require(texture,"Model texture is absent from the canonical TexturePack");
        // Final RenderStrips10DF1C..10DF50: target's alpha-only/opaque-only
        // passes are encoded by caller flags8/16; no second strip list.
        if ((flags&8u) && !texture->requiresDepthSort)return;
        if (((flags&16u) || view.target->opaqueOnly) && texture->requiresDepthSort)return;
        const EditorMarkerLightMaterial* material=nullptr;
        if (strip.lightMaterialIndex>=0 &&
            static_cast<std::size_t>(strip.lightMaterialIndex)<geometry.lightMaterials.size())
            material=geometry.lightMaterials[static_cast<std::size_t>(strip.lightMaterialIndex)];
        if (!material)material=fallbackMaterial;
        // Preserve the source strip flags and its texture4F/final74 predicate.
        auto draw=static_cast<std::uint32_t>(strip.flags)|(texture->drawFlag4Enabled ? 4u:0u);
        if (clipClass==1)draw|=0x200u;
        if ((flags&1u) || !environmentEnabled)draw&=~0x100u;
        if (flags&2u)draw&=~0x80u;
        if (!lightContext)draw&=~0x1c0u; // released10DDE0 null-context mask
        const auto* submitted=&strip;
        if(boneMatrices) {
            // PAL10D778 DoBlendingOnStrip copies the actual strip packet and
            // changes XYZ only. Its three embedded low bytes are two bone
            // indices and an unscaled8-bit blend value, not extra channels.
            auto animatedStrip=std::make_unique<EditorMarkerSolidGeometry::Strip>(strip);
            animatedStrip->nativeVertexRevision=0;
            for(auto& position:animatedStrip->positions) {
                std::array<std::uint32_t,3> encoded{};
                std::memcpy(encoded.data(),position.data(),sizeof(encoded));
                const auto first=encoded[0]&255u,second=encoded[1]&255u,weight=encoded[2]&255u;
                const auto transformVertex=[&](std::uint32_t bone,float outputVertex[4]) {
                    Require(bone<boneMatrices->size(),"Animated vertex bone exceeds its retained skeleton");
                    eMulVector3(outputVertex,(*boneMatrices)[bone].data(),position.data());
                };
                float posed[4];
                if(weight==0)transformVertex(first,posed);
                else if(weight==1)transformVertex(second,posed);
                else {
                    float other[4];transformVertex(first,posed);transformVertex(second,other);
                    const float blend=static_cast<float>(weight)*(1.0f/256.0f);
                    for(std::size_t axis=0;axis<3;++axis)
                        posed[axis]=posed[axis]*(1.0f-blend)+other[axis]*blend;
                }
                std::copy(posed,posed+3,position.begin());
            }
            submitted=animatedStrip.get();posedStrips.push_back(std::move(animatedStrip));
        }
        CaptureStrip(output,alphaGroups,state,model,*submitted,*texture,material,localToWorld,
            view,clipClass,draw,alphaScale,lightContext);
    };
    if(!parallel) {
        for(const auto& run:visible)for(std::size_t index=run.first;index<run.first+run.count;++index)
            capture(index,run.clipClass,commands_,animatedStrips_,nextAlphaGroup_);
        return commands_.size()-start;
    }
    sources.reserve(count);
    for(const auto& run:visible)for(std::size_t index=run.first;index<run.first+run.count;++index)
        sources.push_back({index,run.clipClass});
    const auto chunkCount=count/stripsPerChunk+(count%stripsPerChunk!=0);
    if(chunks.size()<chunkCount)chunks.resize(chunkCount);
    clear.chunksUsed=chunkCount;
    RunNativeSceneJobs(chunkCount,[&](std::size_t chunkIndex) {
        auto& chunk=chunks[chunkIndex];
        const auto first=chunkIndex*stripsPerChunk;
        const auto last=first+std::min(stripsPerChunk,count-first);
        chunk.commands.reserve((last-first)*3);
        for(auto index=first;index<last;++index)
            capture(sources[index].index,sources[index].clipClass,
                chunk.commands,chunk.animatedStrips,chunk.alphaGroups);
    },2);
    std::size_t commandCount=0,posedCount=0;
    for(std::size_t index=0;index<chunkCount;++index) {
        commandCount+=chunks[index].commands.size();posedCount+=chunks[index].animatedStrips.size();
    }
    commands_.reserve(commands_.size()+commandCount);
    animatedStrips_.reserve(animatedStrips_.size()+posedCount);
    // Restore source strip order, then each strip's base/specular/reflection
    // order. Alpha IDs are assigned by that same ordered commit; scheduling
    // cannot alter transparent groups or any later stable alpha/texture sort.
    for(std::size_t index=0;index<chunkCount;++index) {
        auto& chunk=chunks[index];
        for(auto& strip:chunk.animatedStrips)animatedStrips_.push_back(std::move(strip));
        for(auto& command:chunk.commands) {
            if(command.alphaGroup)command.alphaGroup+=nextAlphaGroup_;
            commands_.push_back(std::move(command));
        }
        nextAlphaGroup_+=chunk.alphaGroups;
    }
    return commands_.size()-start;
}

void EcstasyDisplayFrameRate(frontend::FlailerResources& resources,
    frontend::FlailerRenderManager& render,const CarCinematicScreen& view,EcstasyFrame& frame)
{
    // Independent PC overlays use the actual global font. The FPS toggle
    // never gates DEBUG, and DEBUG never supplies the displayed frame rate.
    const auto* debugCaption=applicationDebugToolsEnabled?joystickNativeDebugCaption():nullptr;
    if(!applicationShowFrameRate && !debugCaption)return;
    constexpr std::uint32_t fontKey=0x5C10A676u;
    const auto* font=resources.FindFont(fontKey);
    if(!font || !font->GetTextureInfo(resources))return;
    const auto drawCaption=[&](const char* caption,float y) {
        frontend::FlailerTextChunk text;
        text.kind=1;text.visible=1;text.fontKey=fontKey;text.formatOptions=2;
        text.topLeftX=8.0f;text.topLeftY=y;text.scaleX=text.scaleY=0.75f;
        const auto length=std::strlen(caption);
        Require(length<std::numeric_limits<std::uint32_t>::max(),"Native overlay text exceeds its owned capacity");
        text.color=0xffffffffu;text.textCapacity=static_cast<std::uint32_t>(length+1);
        text.ownedText.resize(text.textCapacity+1,0);text.text=text.ownedText.data();text.SetString(caption);
        try {
            render.RenderFlailerText(resources,text,nullptr,nullptr,nullptr,1.0f);
            render.RenderLayer(0,view,frame);
        } catch(...) {text.DeleteFlailerStrip();throw;}
        // The captured frame retains the actual strip until retirement.
        text.DeleteFlailerStrip();
    };
    if(applicationShowFrameRate) {
        char caption[48]{};
        if(applicationFrameTiming.frameRateAvailable)
            std::snprintf(caption,sizeof(caption),"FPS: %.1f",applicationFrameTiming.framesPerSecond);
        else std::snprintf(caption,sizeof(caption),"FPS: --");
        drawCaption(caption,8.0f);
    }
    if(debugCaption)drawCaption(debugCaption,applicationShowFrameRate?24.0f:8.0f);
}

void EcstasyDisplayFrame(const EcstasySceneBindings& scene,EcstasyFrame& frame)
{
    // PAL112858: retain original environment/world/car/filter/HUD phases.
    // Native packets capture the state that the console consumed immediately;
    // the GPU endpoint owns only images, dependencies and completion fences.
    Require(frame.State()==EcstasyFrame::Phase::Building && scene.clock &&
        scene.ui.resources && scene.ui.render,"eDisplayFrame lacks its live frame/UI owners");
    using CaptureClock=std::chrono::steady_clock;
    auto capturePhaseStarted=scene.captureProfile ? CaptureClock::now():CaptureClock::time_point{};
    const auto captureCheckpoint=[&](std::size_t phase) {
        if(!scene.captureProfile)return;
        const auto now=CaptureClock::now();
        scene.captureProfile->milliseconds[phase]+=
            std::chrono::duration<double,std::milli>(now-capturePhaseStarted).count();
        capturePhaseStarted=now;
    };
    if(scene.captureProfile) {
        if(scene.emitRaceGeometry)++scene.captureProfile->emittedCaptures;
        else ++scene.captureProfile->discardedCaptures;
    }
    frame.SetPresentationAdvance(scene.advancePresentation);
    frame.CaptureDisplayOffsets(*scene.clock); // original ePutDisplayEnvironment reads both source offsets
    auto& views=scene.views;auto& render=*scene.ui.render;
    // PAL11293C..1129C8 writes the physical screen Z format before
    // UpdateGifTags105BC8..105C5C propagates it to logical targets0..5.
    // The rear target7 owns a separate PSMZ16S buffer (SetScreenBuffers
    //106B0C..106B78); environment targets8..14 keep their own PSMZ16.
    const auto screenDepthFormat=scene.flow && scene.flow->inGameFlag ? 0x31:0x3a;
    for(std::size_t index=0;index<3;++index) {
        auto& view=views.screens[index];
        if(view.target)view.target->format=screenDepthFormat;
        if(view.auxiliaryTarget)view.auxiliaryTarget->format=screenDepthFormat;
    }
    if(views.screens[3].target)views.screens[3].target->format=0x3a;
    auto* resources=scene.ui.resources->Models().resourceRegistry;
    Require(resources,"Display frame has no canonical resource owner");
    auto* simulation=scene.track ? scene.track->Simulation():nullptr;
    const auto applicationState=scene.applicationState ? *scene.applicationState:0;
    if(scene.advancePresentation && scene.environmentMap && scene.environmentMap->enabled)
        UpdateEnvironmentMapCameras(*scene.environmentMap,views,simulation ? simulation->world:nullptr,
            scene.track ? scene.track->Players():nullptr,applicationState,scene.clock->realTimeElapsed);
    frame.CaptureEnvironment(scene.environmentMap,*resources,applicationState,views.renderFrameCounter,
        scene.flow && scene.flow->inGameFlag!=0);
    ecstasyViewsUpdateMode(&views);
    ecstasyViewsApplyMode(&views);
    const auto active=[](const CarCinematicScreen& view) {
        return view.enabled && view.view && view.target && view.target->enabled;
    };
    for(auto& view:views.screens)if(active(view))
        Require(ecstasyViewCalculateMatrices(&views,&view,0,0,0)==CAR_CINEMATIC_OK,
            "Original view projection could not be calculated");
    if(scene.screens) {
        Require(scene.frontendFrame,"Screen model callbacks have no actual frontend frame owner");
        // Clear the previous frame before BeginFrame enqueues current UI.
        // CompleteSubmissions must not erase newly generated clipped text.
        scene.frontendFrame->Clear();
    }
    render.BeginFrame(*scene.ui.resources,scene.ui.language,
        static_cast<float>(scene.clock->realTimer)*0.0002500000118743628f);
    if(scene.screens)scene.screens->RenderModels(scene.advancePresentation);
    // PAL112C1C: opaque frontend cars precede the sky/background. Their
    // owner targets player view1/2; view0 remains the Flailer camera.
    if(scene.applicationState && *scene.applicationState==3)
        for(std::size_t i=1;i<3;++i)if(active(views.screens[i])) {
            ecstasyViewSetCurrent(&views,&views.screens[i]);
            RenderFrontEndCars(views.screens[i],frame,scene.clock->worldTimeFrames,
                views.presentationLayout,true,false,true,false);
        }
    frame.AttachAlphaPolys();
    auto& frontend=views.screens[0];
    const auto renderFlailerLayer=[&](std::int32_t layer) {
        // PAL112F78/113B58/113B98/113BE0: each nonempty UI layer
        // immediately flushes both source lists before the next layer.
        // Texture priorities apply within that boundary. Deferring all UI
        // until one final flush moves the priority7 preview fade after the
        // nearer logo/help/thumbnail layers and makes the whole shop blink.
        if(render.RenderLayer(layer,frontend,frame)) {
            frame.AttachSolidPolys();frame.AttachAlphaPolys();
        }
    };
    captureCheckpoint(0);
    const auto* sky=scene.track ? scene.track->Sky():nullptr;
    // PAL112E38 disables Z writes for the sky packet group; PAL112F24
    // restores them after its flush. Depth testing remains enabled, so
    // opaque frontend cars already submitted can still occlude the sky.
    frame.SetZWriteState(0);
    if(sky)for(std::size_t i=1;i<10;++i) {
        auto& view=views.screens[i];if(!active(view))continue;
        Require(ecstasyViewCalculateMatrices(&views,&view,100.0f,50000.0f,0)==CAR_CINEMATIC_OK,
            "Original sky projection could not be calculated");
        ecstasyViewSetCurrent(&views,&view);
        sky->StuffSky(view,frame,scene.clock->worldTimeFrames,scene.emitRaceGeometry);
    }
    frame.AttachAlphaPolys();
    frame.SetZWriteState(1);
    if(active(frontend))for(std::int32_t layer=24;layer>=20;--layer)
        renderFlailerLayer(layer);
    frame.AttachAlphaPolys();
    // PAL1130F0 reads the actual InGameFlag330540 before adding any
    // scenery views. A decoded track file alone does not enable drawing:
    // textures/fixups and enabled sound owners are still loading then.
    // EndGameFlowLoadingTrackSpecific owns publication of this same flag.
    Require(!scene.track || scene.flow,"Track drawing has no original GameFlow owner");
    auto* scenery=scene.flow && scene.flow->inGameFlag && scene.track &&
        scene.track->trackFile && scene.track->trackFile->Loaded() ?
        scene.track->trackFile->Resources().scenery:nullptr;
    captureCheckpoint(1);
    for(std::size_t i=1;i<10;++i) {
        auto& view=views.screens[i];if(!active(view))continue;
        // PAL113030..11304C supplies32 only to the cubemap culling far
        // override. Raster/depth projection retains the actual camera range.
        const float clipFar=view.id>=4 && view.id<10 ? 32.0f:0.0f;
        Require(ecstasyViewCalculateMatrices(&views,&view,0,0,clipFar)==CAR_CINEMATIC_OK,
            "Original world projection could not be calculated");
        ecstasyViewSetCurrent(&views,&view);
        // PAL10F928 reads TheRaceParameters+0C (the retained reverse owner).
        // Its0/1 gates are independent of the auxiliary-view bits.
        std::uint32_t mask=(views.presentationLayout==3 || views.presentationLayout==4) ? 0x10u:0x11u;
        if(view.id>=4 && view.id<10)mask|=0x102u;
        // Only view 1 updates scenery LOD hysteresis. A discarded frame
        // needs that state update, but no mirror or cubemap scenery walk.
        if(scenery && (scene.emitRaceGeometry || view.id==1)) {
            Require(scene.parameters,"Scenery source filter has no live RaceParameters");
            if(scene.parameters->reverse==0)mask|=8u;
            if(scene.parameters->reverse==1)mask|=4u;
            const auto sceneryStarted=std::chrono::steady_clock::now();
            scenery->Render(view,frame,mask,scene.clock->worldTimeFrames,scene.emitRaceGeometry);
            if(scene.sceneryCaptureTiming || scene.sceneryFrameTiming) {
                const auto elapsed=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()-sceneryStarted).count());
                if(scene.sceneryCaptureTiming)scene.sceneryCaptureTiming->Observe(elapsed);
                if(scene.sceneryFrameTiming)scene.sceneryFrameTiming->Observe(elapsed);
            }
        }
        // PAL113250 follows StuffScenery11321C and precedes the sun and
        // active camera mover. NIS actors retain their actual replay poses.
        if(i<4 && scene.track && scene.track->NisManagers())
            (void)DrawNisObjects(*scene.track->NisManagers(),view,frame);
        // PAL113284 follows NIS and precedes RenderClans. Only the source
        // transfer operation is gated by InGameFlag; glare owns its timer.
        if(i<4)resources->Sun().StuffSun(view,frame,*scene.clock,
            scene.flow && scene.flow->inGameFlag!=0);
        // PAL1132C4 -> RenderClans1AB810 consumes the original world's
        // live SkidSets on primary views1..3. The source1AACC0 polygons
        // already contain world positions and the authored material texture.
        if(i<4 && simulation && simulation->world) {
            std::vector<SkidRenderQuad> skidQuads;
            WorldClans(*simulation->world).PrepareRender(view.matrices.worldToClip,view,skidQuads,
                scene.advancePresentation,scene.emitRaceGeometry);
            for(const auto& quad:skidQuads) {
                frontend::FlailerPoly polygon;polygon.rasterState=quad.rasterState;
                for(std::size_t vertex=0;vertex<quad.vertices.size();++vertex) {
                    polygon.vertices[vertex].position=quad.vertices[vertex].position;
                    polygon.vertices[vertex].textureCoordinate=quad.vertices[vertex].textureCoordinate;
                    polygon.vertices[vertex].color=quad.vertices[vertex].color;
                }
                frame.RenderPolygon3D(polygon,textureInfoForDescriptor(quad.texture),eMathIdentityMatrix,view);
            }
        }
        // PAL11333C consumes the active (native last) mover virtual Render.
        // ZonePreview emits into FrameEffects; ordinary camera Render is empty.
        if(i<4)Require(carCinematicRenderMovers(&view,&frame)==CAR_CINEMATIC_OK,
            "Camera mover rendering lost its canonical active owner");
        if(i<4 && scene.track && scene.track->Players())
            Require(carFramePlayerRenderPlayers(scene.track->Players(),&view,&frame)!=0,
                "Player visible driving line lost its original owner");
        // PAL11334C: the original WorldObject list follows scenery on the
        // primary views1..3 and precedes RenderFlailerModels. Fern shakers
        // hide their authored static placement while this very model lives.
        if(scene.emitRaceGeometry && i<4 && simulation && simulation->world)
            (void)RenderWorldObjects(WorldObjects(*simulation->world),view,frame,
                scene.clock->worldTimeFrames);
        // PAL11335C follows world objects with the authored light-flare pack.
        if(scene.emitRaceGeometry && i<4 && scene.track && scene.flow)
            (void)RenderWorldLightFlares(scene.track->files.Registry().Lights(),
                scene.track->files.Registry(),view,frame,scene.flow->inGameFlag!=0);
    }
    // The source RenderFlailerModels stage follows background/scenery and
    // precedes RenderCars. Each queued model retains its actual view; the
    // view0 Flailer projection must not replace the track/car preview camera.
    if(scene.frontendFrame && !scene.frontendFrame->models.empty()) {
        for(const auto& request:scene.frontendFrame->models) {
            Require(request.model && request.viewNumber>=0 && request.viewNumber<10,
                "Frontend model request lost its actual resource or view owner");
            auto& modelView=views.screens[request.viewNumber];
            Require(active(modelView),"Frontend model request targets a disabled original view");
            ecstasyViewSetCurrent(&views,&modelView);
            frame.RenderModel(*const_cast<EditorMarkerModel*>(request.model),request.transform.data(),
                modelView,0,1.0f,scene.clock->worldTimeFrames);
        }
        scene.frontendFrame->modelDrawCpuClosureRequired=false;
    }
    captureCheckpoint(2);
    // PAL113378/113388: separate FrameEffects and WorldEffects lists share
    // allocation pools, not live particles. Both borrow the retained catalogue.
    if(scene.emitRaceGeometry && scene.effects) {
        auto& catalogue=resources->Effects();
        for(std::size_t i=1;i<4;++i)if(active(views.screens[i])) {
            ecstasyViewSetCurrent(&views,&views.screens[i]);
            (void)RenderWorldParticles(scene.effects->FrameParticles(),catalogue,views.screens[i],frame);
            (void)RenderWorldParticles(scene.effects->Particles(),catalogue,views.screens[i],frame);
        }
    }
    captureCheckpoint(3);
    // PAL1134A4 calls AttachSolidPolys10BB20, not AttachAlphaPolys10B838.
    // Keep the particles/scenery alpha list pending until after car solids.
    // The scenery reflected strips read the previous mapped image; later
    // cars read the current update. Capture that version in each packet.
    frame.AttachSolidPolys();
    if(scene.environmentMap && scene.environmentMap->enabled && scene.flow && scene.flow->inGameFlag &&
        active(frontend))scene.environmentMap->CreatePrimitives(frame);
    frame.AttachSolidPolys(); // PAL1135B8: remap solid chain only.
    const auto carCaptureStarted=std::chrono::steady_clock::now();
    if(simulation && simulation->world)for(std::size_t i=1;i<4;++i) {
        auto& view=views.screens[i];if(!active(view))continue;
        ecstasyViewSetCurrent(&views,&view);
        Require(scene.parameters && scene.applicationState && scene.track->Players(),
            "RenderCars damage pass lacks the original race/player owners");
        Require(scene.track->Session(),"Car damage display has no canonical RaceCoordinator");
        const CarDamageDisplayBindings display{scene.track->files.Registry(),*scene.parameters,
            scene.track->Session()->state,*scene.track->Players(),simulation->pursuit,view,frame};
        RenderCars(*simulation->world,view,frame,*scene.clock,views.presentationLayout,&display,scene.track->ViewMovers(),scene.emitRaceGeometry);
    }
    if(scene.carCaptureMilliseconds && simulation && simulation->world)
        *scene.carCaptureMilliseconds+=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-carCaptureStarted).count();
    // PAL1136BC/1136D4: reflected frontend cars, then the transparent
    // normal pass, after the ordinary race-car pass on views1..3.
    for(std::size_t i=1;i<4;++i)if(active(views.screens[i])) {
        ecstasyViewSetCurrent(&views,&views.screens[i]);
        RenderFrontEndCars(views.screens[i],frame,scene.clock->worldTimeFrames,
            views.presentationLayout,false,true,true,true);
        RenderFrontEndCars(views.screens[i],frame,scene.clock->worldTimeFrames,
            views.presentationLayout,true,false,false,true);
    }
    frame.AttachSolidPolys(); // PAL113714 completes the car solid chains.
    captureCheckpoint(4);
    // PAL1137CC flushes the shared world alpha list after particles AND cars.
    // OverlayRVMImages1137F4 has its own following solid/alpha flush, before
    // DisplayRVMs later samples the completed offscreen image.
    frame.AttachAlphaPolys();
    if(scene.track)frame.OverlayRVMImages(views,scene.track->files.Registry());
    frame.AttachAlphaPolys();
    // PAL113818..113880: cockpit models use the third captured projection,
    // then complete their own solid/alpha stage before the depth effects.
    for(std::size_t i=1;i<4;++i)if(active(views.screens[i])) {
        ecstasyViewSetCurrent(&views,&views.screens[i]);
        RenderDashboards(views.screens[i],frame,scene.clock->worldTimeFrames);
    }
    frame.AttachSolidPolys();frame.AttachAlphaPolys();
    captureCheckpoint(5);
    if(scene.emitRaceGeometry && scene.flow && scene.flow->inGameFlag) {
        frame.AttachFog(views);
        frame.AttachDepthOfField(views);
    }
    frame.AttachBlackAndWhite(views); // PAL113900, before DisplayFilter113A44.
    // PAL1139F0..113A8C: mask alpha precedes destination-alpha history blend.
    frame.SetZWriteState(0);
    if(resources)frame.DisplayFilter(views,*resources);
    if(frontend.target)frame.MarkSceneHistoryBoundary(*frontend.target,true);
    frame.DisplayRadialBlur(views);
    frame.DisplayBlendPrevious(views);
    frame.SetZWriteState(1);
    if(frontend.target)frame.MarkSceneHistoryBoundary(*frontend.target);
    captureCheckpoint(6);
    // Source113A90..113D80: effect-free HUD packets, followed by resumption
    // of the selected mode. Each packet captures its issuing render policy;
    // later Gather cannot leak the resumed scene state into these HUD draws.
    SuspendRenderEggs();
    try {
        if(active(frontend)) {
            ecstasyViewSetCurrent(&views,&frontend);
            // PAL113AEC..113B48 disables Z writes around DisplayRVMs, flushes
            // that target-texture polygon, then restores writes before the HUD.
            frame.SetZWriteState(0);frame.DisplayRVMs(views,frontend);frame.AttachAlphaPolys();frame.SetZWriteState(1);
            for(std::int32_t layer=15;layer>=0;--layer)renderFlailerLayer(layer);
            for(std::int32_t layer=19;layer>=16;--layer)renderFlailerLayer(layer);
            for(std::int32_t layer=28;layer>=25;--layer)renderFlailerLayer(layer);
            // Final113C1C..113C50 visits pPlayersByIndex after the HUD layers
            // and calls Player::RenderTrackMap181C08 for every live Player.
            // TrackMap's native immediate polygons use the same layer transport;
            // flush those newly submitted polygons before retiring this frame.
            if(auto* players=scene.track ? scene.track->Players():nullptr) {
                Require(players->count>=0 && players->count<=2,"TrackMap player count is outside the original table");
                for(std::int32_t index=0;index<players->count;++index) {
                    Require(players->players[index],"TrackMap player table has no live owner");
                    carFramePlayerRenderTrackMap(players->players[index]);
                }
                for(std::int32_t layer=15;layer>=0;--layer)renderFlailerLayer(layer);
            }
        }
        if(active(frontend)) {
            EcstasyDisplayFrameRate(*scene.ui.resources,render,frontend,frame);
            frame.AttachAlphaPolys();
        }
        render.EndFrame();
    } catch(...) {
        // Native allocation/resource errors must not leave Stomper suspended
        // after an aborted UI frame. Normal resource ownership remains live.
        ResumeRenderEggs();
        throw;
    }
    ResumeRenderEggs();
    frame.AttachAlphaPolys();
    captureCheckpoint(7);
    // PAL113FD8 is after every current-frame texture context. Native Gather
    // reads borrowed pixels later, so the equivalent update follows CPU capture.
    if(scene.advancePresentation && !scene.deferPresentationUpdates) {
        if(scene.effects)Require(worldParticlesAdvance(&scene.effects->FrameParticles(),scene.clock->realTimeElapsed)==WORLD_SERVICES_OK,
            "FrameEffects lost its original real-time update owner");
        frame.AdvanceTextureAnimationsAfterCompletion(*resources,*scene.clock);
    }
    ++views.renderFrameCounter; // PAL11405C/114064/114090 eFrameCounter, not VBlank/GameTime.
}

void EcstasyFrame::RetireStripAfterCompletion(frontend::FlailerStrip& strip)
{
    Require(phase_==Phase::Building && strip.pool && strip.retentionCount,
        "Graphics frame requires the actual retained Flailer strip");
    retainedStrips_.push_back(&strip);
}

void EcstasyFrame::MarkSceneHistoryBoundary(const EcstasyRenderTarget& display,bool source)
{
    Require(phase_==Phase::Building && display.id>=0 && display.id<7,
        "Scene history boundary lost its display target");
    AttachAlphaPolys();
    EcstasyDrawCommand command;command.kind=source ? EcstasyDrawKind::SceneHistorySource:EcstasyDrawKind::SceneHistoryBoundary;
    command.target=&display;commands_.push_back(command);
    pendingAlphaBegin_=commands_.size();
}

void EcstasyFrame::Seal()
{
    Require(phase_==Phase::Building,"Graphics frame was already sealed");
    AttachAlphaPolys();phase_=Phase::Ready;
}
void EcstasyFrame::MarkSubmitted()
{
    Require(phase_==Phase::Ready,"Graphics frame is not ready for submission");phase_=Phase::Submitted;
}
void EcstasyFrame::Complete()
{
    Require(phase_==Phase::Submitted,"CPU capture completion has no submitted frame");
    auto* textures=textureAnimationResources_;
    const auto worldFrames=textureAnimationWorldFrames_,realFrames=textureAnimationRealFrames_;
    if(environment_)environment_->CompleteCameraCapture(environmentCameraFaces_);
    Release();phase_=Phase::Complete;
    if(textures)texturePackRegistry.UpdateAnimations(textures->Models(),worldFrames,realFrames);
}
void EcstasyFrame::CancelBeforeSubmit()
{
    Require(phase_==Phase::Building || phase_==Phase::Ready,"Submitted graphics frame cannot be cancelled");
    Release();phase_=Phase::Complete;
}
void EcstasyFrame::BeginNextFrame()
{
    Require(phase_==Phase::Complete,"Only a completed graphics frame can be reused");
    Require(commands_.empty() && pendingAlphaCommands_.empty() && orderingScratch_.empty() &&
        visibleScratch_.empty() && modelStripScratch_.empty() && modelTextureScratch_.empty() && depthTransfers_.empty() &&
        retainedStrips_.empty() && worldPolygons_.empty() && worldStrips_.empty() &&
        animatedStrips_.empty() && liveParticleGroups_==0 && !textureAnimationResources_ &&
        liveInstances_==0 && liveProjections_==0 && !lastProjection_,
        "Completed graphics frame still owns a submitted borrower");
    reusedOpaqueSceneryStrips_=0;carShadowCaptureMilliseconds_={};
    zWriteEnabled_=true;dashboardProjection_=false;inGameMaterials_=false;pendingAlphaBegin_=0;nextAlphaGroup_=0;nextFeedbackGroup_=0;
    displayTranslation_={};sourceDisplayExtent_={};textureTime_=0;
    environment_=nullptr;frontendEnvironmentTexture_=nullptr;specularFalloffTexture_=nullptr;
    environmentApplicationState_=0;renderFrameCounter_=0;environmentUpdated_=false;advancesPresentation_=true;
    textureAnimationWorldFrames_=0;textureAnimationRealFrames_=0;
    phase_=Phase::Building;
}
void EcstasyFrame::Release() noexcept
{
    textureAnimationResources_=nullptr;environmentCameraFaces_=0;
    commands_.clear();pendingAlphaCommands_.clear();orderingScratch_.clear();visibleScratch_.clear();depthTransfers_.clear();
    modelStripScratch_.clear();modelTextureScratch_.clear();
    for(auto& chunk:modelCaptureScratch_) {
        chunk.commands.clear();chunk.animatedStrips.clear();chunk.alphaGroups=0;
    }
    worldPolygons_.clear();
    for(std::size_t index=0;index<liveParticleGroups_;++index)particleGroups_[index]->clear();
    liveParticleGroups_=0;
    worldStrips_.clear();
    animatedStrips_.clear();
    for (auto* strip:retainedStrips_)strip->pool->Complete(*strip);
    retainedStrips_.clear();
    liveInstances_=0;liveProjections_=0;lastProjection_=nullptr;
}
std::vector<std::uint8_t> DecodeTextureMipRgba(const TextureInfo& texture,
    std::uint32_t level,std::size_t sourceOffset,std::size_t sourceBytes,
    std::uint32_t& width,std::uint32_t& height,bool repeatedPadding)
{
    const auto* descriptor=texture.descriptor;
    Require(descriptor && descriptor->authoredRecord && descriptor->authoredRecordSize>=164,
        "Vulkan upload lacks the final TextureInfo descriptor");
    const auto* wire=descriptor->authoredRecord;
    const std::uint32_t authoredWidth=ReadU16(wire+0x24)>>level,authoredHeight=ReadU16(wire+0x26)>>level;
    Require(wire[0x48]<=14 && wire[0x49]<=14,"Invalid authored texture log dimensions");
    Require(level<=wire[0x48] && level<=wire[0x49],"Authored mip exceeds its texture dimensions");
    width=(1u<<wire[0x48])>>level;height=(1u<<wire[0x49])>>level;
    Require(authoredWidth && authoredHeight && authoredWidth<=width && authoredHeight<=height,
        "Invalid authored texture dimensions");
    const auto count=static_cast<std::size_t>(authoredWidth)*authoredHeight;
    const auto* source=texture.pixels.Data();const auto* palette=texture.palette.Data();
    const auto psm=wire[0x4a],palettePsm=wire[0x4b];
    Require(source && sourceOffset<=texture.pixels.size && sourceBytes<=texture.pixels.size-sourceOffset,
        "Vulkan upload has no complete retained authored mip");
    source+=sourceOffset;
    // The ordinary PAL109990 downloader chooses a preconverted32-bit stream
    // when this mip's byte55+4*level is nonzero (109CF0..109D20). The static108BB8
    // helper does not take that branch. TEX0 retains the original PSM and
    // power-of-two dimensions (10ABC0). Decode directly into a native image;
    // no console memory, command interpreter or emulated graphics state.
    const std::size_t required=psm==0 || psm==1 ? count*4 : psm==0x13 ? count : psm==0x14 ? (count+1)/2 : 0;
    Require(required && required<=sourceBytes,
        "Vulkan texture format is unsupported or its final pixel extent is truncated");
    std::size_t paletteColors{};
    if (psm==0x13 || psm==0x14) {
        // PAL109B80..109BE0 transfers the authored byte count at+3C.
        // Shipped PSMT8 textures can use only32 RGBA colors (128 bytes).
        // Validate that actual palette and each referenced entry; unused
        // entries need no fabricated colors or full256-entry allocation.
        Require(palette && palettePsm==0 && texture.paletteBytes>0 &&
            !(texture.paletteBytes%4) && texture.palette.size>=static_cast<std::size_t>(texture.paletteBytes),
            "Vulkan indexed texture requires its authored RGBA32 palette");
        paletteColors=static_cast<std::size_t>(texture.paletteBytes)/4u;
    }
    const auto bufferWidth=wire[0x54+4*level];
    const bool converted=wire[0x55+4*level]!=0;
    if(converted)Require((psm==0x13 || psm==0x14) && bufferWidth>=2 &&
        !(bufferWidth&1u) && !(authoredWidth&1u) &&
        authoredHeight%(psm==0x14 ? 4u:2u)==0,
        "Unsupported authored preconverted texture extent");
    // Each table entry names a physical block, not mutable runtime state.
    // GS manual6.0 sections8.3.1/8.3.3/8.3.4 supply the pixel layout.
    static constexpr std::uint8_t blocks4[8][4]={
        {0,2,8,10},{1,3,9,11},{4,6,12,14},{5,7,13,15},
        {16,18,24,26},{17,19,25,27},{20,22,28,30},{21,23,29,31}};
    static constexpr std::uint8_t blocks8[4][8]={
        {0,1,4,5,16,17,20,21},{2,3,6,7,18,19,22,23},
        {8,9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
    // Authored UVs use TEX0's power-of-two extent even when the uploaded
    // rectangle is smaller. Preserve the retained texels before extending
    // only repeated axes below; clamp-only padding remains transparent.
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width)*height*4);
    for (std::size_t pixel=0;pixel<count;++pixel) {
        const std::uint8_t* color{};
        if (psm==0 || psm==1) color=source+pixel*4;
        else {
            std::size_t byteOffset=psm==0x13 ? pixel:pixel/2;
            unsigned shift=psm==0x13 ? 0u:static_cast<unsigned>(pixel&1u)*4u;
            if(converted) {
                const auto x=static_cast<std::uint32_t>(pixel%authoredWidth);
                const auto y=static_cast<std::uint32_t>(pixel/authoredWidth);
                const auto pages=static_cast<std::uint32_t>(bufferWidth)/2u;
                const auto pageHeight=psm==0x14 ? 128u:64u;
                const auto blockWidth=psm==0x14 ? 32u:16u;
                const auto page=x/128u+(y/pageHeight)*pages;
                const auto block=psm==0x14 ? blocks4[(y%128u)/16u][(x%128u)/32u]:
                    blocks8[(y%64u)/16u][(x%128u)/16u];
                const auto column=(y%16u)/4u,cx=x%blockWidth,cy=y%4u;
                const auto word=(cx&1u)+((cy&1u)<<1u)+((cx&2u)<<1u)+
                    ((((cx>>2u)&1u)^(cy>>1u)^(column&1u))<<3u);
                const auto lane=(cx/8u)*2u+cy/2u;
                // Invert the32-bit layout to address the retained export
                // stream directly. This is a format conversion, not VRAM.
                const auto blockX=(block&1u)|((block>>1u)&2u)|((block>>2u)&4u);
                const auto blockY=((block>>1u)&1u)|((block>>2u)&2u);
                const auto sourceX=(page%pages)*64u+blockX*8u+(word&1u)+((word>>1u)&6u);
                const auto sourceY=(page/pages)*32u+blockY*8u+column*2u+((word>>1u)&1u);
                byteOffset=(static_cast<std::size_t>(sourceY)*(authoredWidth/2u)+sourceX)*4u+
                    (psm==0x14 ? lane/2u:lane);
                shift=psm==0x14 ? (lane&1u)*4u:0u;
                Require(byteOffset<required,"Preconverted texture addresses outside its actual authored mip");
            }
            std::uint32_t index=(source[byteOffset]>>shift)&(psm==0x13 ? 255u:15u);
            // Imported PSMT8 palettes retain their on-disk CLUT layout;
            // native-generated movie palettes have no console permutation.
            if(psm==0x13 && !texture.palette.nativeLinearPalette)
                index=(index&~24u)|((index&8u)<<1)|((index&16u)>>1);
            Require(index<paletteColors,"Vulkan indexed pixel exceeds its authored palette");
            color=palette+index*4;
        }
        const auto destination=(pixel/authoredWidth*width+pixel%authoredWidth)*4;
        std::copy_n(color,3,rgba.data()+destination);
        // Retain the original alpha byte. Expanding it to UNORM unity here
        // rounds authored48 upward and loses all values above128. The shader
        // converts the sampled byte to the GS128-unit scale before TFX.
        rgba[destination+3]=psm==1 ? 128u:color[3];
    }
    // Native PC texture conversion: bilinear/mip sampling at a repeated
    // NPOT edge must not mix the last authored texel with fabricated RGBA0.
    // RIVER_TOP is384 wide in a512-wide TEX0 domain. Extend its repeat using
    // actual decoded texels, including their original alpha, at EVERY mip.
    // This replaces console allocation aliases with a defined native image;
    // it does not reconstruct GS memory or change UVs/filter/material state.
    const bool repeatS=authoredWidth<width && (wire[0x4c]&1u)!=0;
    const bool repeatT=authoredHeight<height && (wire[0x4c]&2u)!=0;
    if(repeatedPadding && (repeatS || repeatT))for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x) {
        const bool outsideS=x>=authoredWidth,outsideT=y>=authoredHeight;
        if((!outsideS && !outsideT) || (outsideS && !repeatS) || (outsideT && !repeatT))continue;
        const auto sourcePixel=(static_cast<std::size_t>(y%authoredHeight)*width+x%authoredWidth)*4u;
        const auto destination=(static_cast<std::size_t>(y)*width+x)*4u;
        std::copy_n(rgba.data()+sourcePixel,4,rgba.data()+destination);
    }
    return rgba;
}

} // namespace hp2
void graphicsNotifyPresentationComplete() noexcept {
    graphicsVideoBlankCounter.fetch_add(1,std::memory_order_release);
}

#ifdef HP2_ENABLE_VULKAN
// Windows/Vulkan adaptation of the proven EcstasyE.cpp rendering boundary.
// It consumes the same native frame and final TextureInfo records. No GS,
// DMA, console memory or alternate gameplay state is emulated here.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#elif defined(__SWITCH__)
#ifndef VK_USE_PLATFORM_VI_NN
#define VK_USE_PLATFORM_VI_NN
#endif
#endif
#include <vulkan/vulkan.h>
#include <chrono>
#include <deque>
#include <future>
#include <fstream>
#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace hp2 {
namespace {
void vkRequired(VkResult result,const char* action)
{
    if (result!=VK_SUCCESS)
        throw std::runtime_error(std::string(action)+" failed (VkResult "+std::to_string(result)+")");
}

// GLSL and the validated SPIR-V arrays are maintained together here.
// Normal Visual Studio builds consume the embedded arrays directly and do
// not require Python or a shader compiler.
[[maybe_unused]] constexpr char vulkanVertexGLSL[]=R"glsl(#version 450
layout(location=0) in vec4 vertexPosition;
layout(location=1) in vec3 vertexTexture;
layout(location=2) in vec4 vertexColor;
layout(location=3) in vec2 vertexLod;
layout(location=4) in uint vertexGlyphBounds;
layout(location=5) in vec3 vertexNormal;
layout(std430,set=1,binding=0) readonly buffer NativeFrameData {
    vec4 values[];
} frameData;
layout(location=0) out vec3 uvq;
layout(location=1) out vec4 color;
layout(location=2) flat out vec2 lodParameters;
layout(location=3) flat out uint glyphBoundsIndex;
// All material passes retain one invariant, explicitly ordered projection.
invariant gl_Position;
layout(push_constant) uniform NativeDrawParameters {
    layout(offset=0) int textureFunction;
    layout(offset=4) int alphaTest;
    layout(offset=8) float lodScale;
    layout(offset=12) int minification;
    layout(offset=16) mat4 nativeModelToClip;
    layout(offset=80) vec4 nativeClipMapping;
    layout(offset=96) int nativeTransformMode;
    layout(offset=100) float nativeTextureQScale;
    layout(offset=104) uint nativeShadingIndex;
} native;
// The material record keeps the exact decoded FE/IG coefficients and the
// two original 1025-sample response tables. Three padding floats align each
// table; they are never samples. Programmatic materials retain native pow.
float nativeCoefficient(uint material,uint index) {
    return frameData.values[material+index/4u][index%4u];
}
float nativeSpecularResponse(uint material,float base,bool alphaMaterial) {
    precise float limited=max(base,nativeCoefficient(material,2u));
    if(frameData.values[material+4u].x!=0.0 && limited>=0.1 && limited<=1.0) {
        precise float scaled=(limited-0.1)*(1024.0/0.9);
        uint index=min(uint(scaled),1023u);
        precise float fraction=scaled-float(index);
        uint first=material+5u+(alphaMaterial ? 257u:0u);
        precise float lower=frameData.values[first+index/4u][index%4u];
        uint next=index+1u;
        precise float upper=frameData.values[first+next/4u][next%4u];
        precise float value=lower+fraction*(upper-lower);
        return value;
    }
    precise float value=nativeCoefficient(material,alphaMaterial ? 13u:1u)*
        pow(limited,nativeCoefficient(material,alphaMaterial ? 12u:0u));
    return value;
}
vec3 nativeReflection(uint shading,vec3 position,vec3 normal) {
    precise vec3 relative;
    precise float twiceDot=0.0;
    for(uint axis=0u;axis<3u;++axis) {
        relative[axis]=position[axis]-frameData.values[shading+3u][axis];
        twiceDot=twiceDot+relative[axis]*normal[axis];
        twiceDot=twiceDot+relative[axis]*normal[axis];
    }
    precise vec3 reflection;
    precise float squared=0.0;
    for(uint axis=0u;axis<3u;++axis) {
        reflection[axis]=relative[axis]-normal[axis]*twiceDot;
        squared=squared+reflection[axis]*reflection[axis];
    }
    precise float inverseLength=squared>0.0 ? 1.0/sqrt(squared):0.0;
    for(uint axis=0u;axis<3u;++axis)reflection[axis]=reflection[axis]*inverseLength;
    return reflection;
}
vec4 nativeLitColor(uint shading,uint material,uint flags,vec3 position,
    vec3 normal,vec3 authoredColor,vec3 preparedReflection,bool hasReflection) {
    bool materialAlpha=(flags&4u)!=0u;
    bool rgbSpecular=nativeCoefficient(material,8u)!=0.0 ||
        nativeCoefficient(material,9u)!=0.0 || nativeCoefficient(material,10u)!=0.0;
    bool litSpecular=(flags&128u)!=0u && (rgbSpecular ||
        (materialAlpha && nativeCoefficient(material,15u)!=0.0));
    bool alphaSpecular=(flags&64u)!=0u && nativeCoefficient(material,15u)!=0.0;
    precise vec3 reflection=vec3(0.0);
    if(litSpecular || alphaSpecular)
        reflection=hasReflection ? preparedReflection:nativeReflection(shading,position,normal);
    precise vec4 outputColor=vec4(authoredColor,1.0);
    if((flags&128u)!=0u) {
        precise vec3 diffuse=vec3(0.0);
        precise vec3 specular=vec3(0.0);
        for(uint light=0u;light<3u;++light) {
            precise float normalDot=0.0;
            for(uint axis=0u;axis<3u;++axis)
                normalDot=normalDot+frameData.values[shading+4u+axis][light]*normal[axis];
            diffuse[light]=max(normalDot,0.0);
            if(litSpecular) {
                precise float reflectionDot=0.0;
                for(uint axis=0u;axis<3u;++axis)
                    reflectionDot=reflectionDot+frameData.values[shading+4u+axis][light]*reflection[axis];
                specular[light]=nativeSpecularResponse(material,reflectionDot,materialAlpha);
            }
        }
        for(uint axis=0u;axis<4u;++axis) {
            precise float value=frameData.values[shading+12u][axis];
            for(uint light=0u;light<3u;++light)
                value=value+frameData.values[shading+13u+light][axis]*diffuse[light];
            for(uint light=0u;light<3u;++light)
                value=value+frameData.values[shading+16u+light][axis]*specular[light];
            if(axis<3u) {
                // Source 5-bit colors were divided by16 without rounding;
                // multiplying by128 recovers the exact authored integer*8.
                precise float authored=authoredColor[axis]*128.0;
                value=value*(1.0+(authored-255.0)*nativeCoefficient(material,3u));
            }
            outputColor[axis]=float(int(clamp(value,0.0,255.0)))/128.0;
        }
    }
    if((flags&64u)!=0u) {
        precise float reflectionDot=0.0;
        if(alphaSpecular)for(uint axis=0u;axis<3u;++axis)
            reflectionDot=reflectionDot+frameData.values[shading+4u+axis].x*reflection[axis];
        precise float specularAlpha=alphaSpecular ? nativeSpecularResponse(material,reflectionDot,true):0.0;
        precise float alpha=nativeCoefficient(material,14u)+nativeCoefficient(material,15u)*specularAlpha;
        outputColor.a=float(int(clamp(alpha,0.0,255.0)))/128.0;
    }
    return outputColor;
}
void nativeShade(uint shading,vec3 position,vec3 normal,out vec3 shadedTexture,out vec4 shadedColor) {
    uint flags=floatBitsToUint(frameData.values[shading].x);
    uint mode=floatBitsToUint(frameData.values[shading].y);
    uint material=floatBitsToUint(frameData.values[shading].z);
    precise vec4 parameters=frameData.values[shading+1u];
    precise vec3 coordinates=vec3(vertexTexture.xy+parameters.yz,vertexTexture.z);
    precise vec4 result=vec4(vertexColor.rgb,parameters.x);
    if((mode&1u)!=0u) {
        precise vec3 reflection=nativeReflection(shading,position,normal);
        precise vec3 mapped;
        for(uint axis=0u;axis<3u;++axis) {
            precise float sign=axis==1u ? 1.0:-1.0;
            mapped[axis]=(frameData.values[shading+8u][axis]*sign)*reflection.x;
            mapped[axis]=mapped[axis]+(frameData.values[shading+9u][axis]*sign)*reflection.y;
            mapped[axis]=mapped[axis]+(frameData.values[shading+10u][axis]*sign)*reflection.z;
        }
        precise float z=mapped.z+1.0;
        precise float squared=mapped.x*mapped.x+mapped.y*mapped.y;
        squared=squared+z*z;
        precise float sphereInverse=squared>0.0 ? 1.0/sqrt(squared):0.0;
        coordinates=vec3(0.5+(mapped.x*sphereInverse)*0.5,0.5+(mapped.y*sphereInverse)*0.5,1.0);
        result=vec4(1.0);
        if((flags&4u)!=0u) {
            if((flags&128u)!=0u)
                result=nativeLitColor(shading,material,flags,position,normal,vertexColor.rgb,reflection,true);
            else result=vec4(vertexColor.rgb,1.0);
        }
        precise float alpha=nativeCoefficient(material,7u)+nativeCoefficient(material,11u)*mapped.z;
        // convertFloatToInt32 saturates at the signed bounds before the
        // released low-byte wrap. Preserve that operation without clamping
        // material alpha to an invented transparency range.
        int integerAlpha=alpha>=2147483648.0 ? 2147483647:
            alpha< -2147483648.0 ? (-2147483647-1):int(alpha);
        result.a=float(uint(integerAlpha)&255u)/128.0;
        if((mode&2u)!=0u)result.a=result.a*(128.0/255.0);
    } else if((flags&192u)!=0u) {
        result=nativeLitColor(shading,material,flags,position,normal,vertexColor.rgb,vec3(0.0),false);
        result.a=result.a*parameters.x;
    }
    shadedTexture=coordinates;shadedColor=result;
}
void main() {
    mat4 modelToClip=native.nativeModelToClip;
    vec4 clipMapping=native.nativeClipMapping;
    float textureQScale=native.nativeTextureQScale;
    uint shading=native.nativeShadingIndex;
    if(native.nativeTransformMode==2) {
        // FirstInstance selects the immutable transform AND shading inputs;
        // no resident vertex is rewritten with a frame-local table offset.
        uint first=uint(gl_InstanceIndex);
        modelToClip=mat4(frameData.values[first],frameData.values[first+1u],
            frameData.values[first+2u],frameData.values[first+3u]);
        clipMapping=frameData.values[first+4u];
        textureQScale=frameData.values[first+5u].x;
        shading=floatBitsToUint(frameData.values[first+5u].y);
    }
    precise vec4 localPosition=vertexPosition;
    if(shading!=0u) {
        localPosition.xyz=vertexPosition.xyz+frameData.values[shading+2u].xyz;
        localPosition.w=frameData.values[shading+2u].w;
    }
    precise vec4 projected=modelToClip[0]*localPosition.x;
    projected=projected+modelToClip[1]*localPosition.y;
    projected=projected+modelToClip[2]*localPosition.z;
    projected=projected+modelToClip[3]*localPosition.w;
    projected.z=(projected.z+projected.w)*0.5;
    projected.xy=projected.xy*clipMapping.xy+projected.w*clipMapping.zw;
    gl_Position=native.nativeTransformMode!=0 ? projected : vertexPosition;
    uvq=vertexTexture;color=vertexColor;
    if(shading!=0u)nativeShade(shading,vertexPosition.xyz,vertexNormal,uvq,color);
    lodParameters=vec2(native.nativeTransformMode!=0 ? textureQScale : vertexLod.x,vertexLod.y);
    glyphBoundsIndex=native.nativeTransformMode==2 ? 0u:vertexGlyphBounds;
}
)glsl";
[[maybe_unused]] constexpr char vulkanFragmentGLSL[]=R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D imageTexture;
layout(set=0,binding=1) uniform sampler2D magnifiedTexture;
layout(set=0,binding=2) uniform sampler2D uiTexture;
layout(std430,set=1,binding=0) readonly buffer GlyphSampling {
    vec4 bounds[];
} glyphSampling;
layout(location=0) in vec3 uvq;
layout(location=1) in vec4 color;
layout(location=2) flat in vec2 lodParameters;
layout(location=3) flat in uint glyphBoundsIndex;
layout(location=0) out vec4 fragmentColor;
layout(push_constant) uniform DrawParameters {
    int textureFunction; int alphaTest; float lodScale; int minification;
} draw;
// These three values are already immutable for each native draw batch.
// Specializing the existing shader by that exact state lets the Vulkan
// compiler remove unrelated fog/focus/FXAA and alpha-test paths. Source
// equations and samples are unchanged; the fog table offset stays dynamic.
layout(constant_id=0) const int materialTextureFunction=0;
layout(constant_id=1) const int materialAlphaTest=0;
layout(constant_id=2) const int materialMinification=0;
layout(constant_id=3) const int materialTexturing=1;
layout(constant_id=4) const int materialClampTopLeft=0;
// Only the Select Car body env-map path uses this native Vulkan variant.
// It translates the retail GS RGBA/128 modulation and ALPHA_2 0x48 into
// premultiplied RGB followed by hardware ONE+ONE, without saturated
// Vulkan SRC_ALPHA. No console registers are interpreted at runtime.
layout(constant_id=5) const int showcaseEnvironmentBlend=0;
// PC final-image FXAA. Edge direction is estimated from diagonal luma;
// two edge-aligned spans select a bounded blend without touching scene Z,
// original motion-blur history, source material alpha or game effects.
vec4 filterFxaa(vec2 uv) {
    vec2 pixel=1.0/vec2(textureSize(uiTexture,0));
    vec4 center=textureLod(uiTexture,uv,0.0);
    const vec3 luma=vec3(0.299,0.587,0.114);
    float middle=dot(center.rgb,luma);
    float nw=dot(textureLod(uiTexture,uv+vec2(-1.0,-1.0)*pixel,0.0).rgb,luma);
    float ne=dot(textureLod(uiTexture,uv+vec2(1.0,-1.0)*pixel,0.0).rgb,luma);
    float sw=dot(textureLod(uiTexture,uv+vec2(-1.0,1.0)*pixel,0.0).rgb,luma);
    float se=dot(textureLod(uiTexture,uv+vec2(1.0,1.0)*pixel,0.0).rgb,luma);
    float low=min(middle,min(min(nw,ne),min(sw,se)));
    float high=max(middle,max(max(nw,ne),max(sw,se)));
    if(high-low<max(0.03125,high*0.125))return center;
    vec2 direction=vec2(sw+se-nw-ne,nw+sw-ne-se);
    float reduction=max((nw+ne+sw+se)*(0.25/8.0),1.0/128.0);
    direction=clamp(direction/(min(abs(direction.x),abs(direction.y))+reduction),vec2(-8.0),vec2(8.0))*pixel;
    vec3 narrow=0.5*(textureLod(uiTexture,uv-direction/6.0,0.0).rgb+
        textureLod(uiTexture,uv+direction/6.0,0.0).rgb);
    vec3 wide=narrow*0.5+0.25*(textureLod(uiTexture,uv-direction*0.5,0.0).rgb+
        textureLod(uiTexture,uv+direction*0.5,0.0).rgb);
    float candidate=dot(wide,luma);
    return vec4(candidate<low || candidate>high ? narrow:wide,center.a);
}
void main() {
    if(materialTextureFunction==9) {
        // Native replacement of PAL11A9F0's framebuffer/CLUT operation.
        // The original selects G. Preserve all8 bits on PC rather than
        // reproducing the unrelated display-buffer alpha-LSB artifact.
        float green=texelFetch(uiTexture,ivec2(gl_FragCoord.xy),0).g;
        fragmentColor=vec4(green,green,green,1.0);
        return;
    }
    if(materialTextureFunction==10) {
        // DisplayFilter's TFX0 source is the real FilterGrey image modulated
        // by the authored RGBA/128. Quantize Cs before the ALPHA equation;
        // FIX255/128 must stay above1 and never enter a clamped blendconstant.
        vec2 uv=uvq.xy/uvq.z;
        float lod=-log2(abs(uvq.z))*lodParameters.x+lodParameters.y;
        vec4 texel=materialMinification==-2 ? textureGrad(imageTexture,uv,dFdx(uv),dFdy(uv)):
            lod<=0.0 ? textureLod(magnifiedTexture,uv,0.0):
            textureLod(imageTexture,uv,materialMinification<2 ? 0.0:lod);
        vec3 source=floor(clamp((texel.rgb*255.0)*color.rgb,0.0,255.0));
        vec3 destination=round(texelFetch(uiTexture,ivec2(gl_FragCoord.xy),0).rgb*255.0);
        // Byte-domain numerator is integral and exact in float32. Divide
        // after subtraction so negative terms round as the source7-bit shift.
        vec3 result=floor((destination*128.0+(destination-source)*(draw.lodScale*128.0))/128.0);
        fragmentColor=vec4(clamp(result,0.0,255.0)/255.0,texel.a*color.a*(255.0/128.0));
        return;
    }
    if(materialTextureFunction==5 || materialTextureFunction==6) {
        // PAL reinterprets the Z24 buffer through Z16 copies; PSMT8H
        // reads source bits8..15 after that conversion, not bits16..23.
        // The final copy is Z-tested at65535 against the original Z24
        // buffer. Nearer pixels retain the preceding alpha255 clear.
        // The CLUT permutation is inverted when its values are uploaded.
        float depth=texelFetch(imageTexture,ivec2(gl_FragCoord.xy),0).r;
        uint sourceDepth=uint(round(clamp(1.0-depth,0.0,1.0)*draw.lodScale));
        fragmentColor=glyphSampling.bounds[uint(draw.minification)+min(sourceDepth>>8u,255u)];
        return;
    }
    if(materialTextureFunction==7 || materialTextureFunction==8) {
        // PAL119F70's two-tap half-size image and final bilinear composite.
        // Source FBA sets bit7 of alpha. Native color attachments express
        // GS alpha128 as1; RGB still uses the actual source ALPHA equation.
        fragmentColor=vec4(textureLod(uiTexture,uvq.xy/uvq.z,0.0).rgb,1.0);
        return;
    }
    // The released TEX1 has LCM0. Its LOD is based on authored projected
    // Q and per-strip K, not Vulkan's screen-space UV derivatives. Two
    // samplers preserve MMAG for LOD<=0 and the independent MMIN filter.
    float q=lodParameters.x==0.0 ? uvq.z:gl_FragCoord.w*lodParameters.x;
    float lod=-log2(abs(q))*draw.lodScale+lodParameters.y;
    vec2 uv=uvq.xy/uvq.z;
    // Authored glyph coordinates include the original GS raster endpoint.
    // Native filtering at a higher-resolution pixel center must not sample
    // the adjacent atlas cell, even for a completely transparent space.
    // Only original font producers supply bounds; world/bitmap UVs are intact.
    if(glyphBoundsIndex!=0u) {
        vec4 rectangle=glyphSampling.bounds[glyphBoundsIndex];
        uv=clamp(uv,rectangle.xy,rectangle.zw);
    }
    if(materialClampTopLeft!=0) {
        // eClampTopLeft's REGION_CLAMP bounds are texels[0,1] on both axes.
        vec2 extent=vec2(textureSize(imageTexture,0));
        uv=clamp(uv,vec2(0.5)/extent,vec2(1.5)/extent);
    }
    if(materialTextureFunction==4){fragmentColor=filterFxaa(uv);return;}
    vec4 sampleColor;
    if(materialMinification==-1)sampleColor=textureLod(uiTexture,uv,0.0);
    else if(materialMinification==-2)sampleColor=textureGrad(imageTexture,uv,dFdx(uv),dFdy(uv));
    else sampleColor=lod<=0.0 ? textureLod(magnifiedTexture,uv,0.0):
        textureLod(imageTexture,uv,materialMinification<2 ? 0.0:lod);
    sampleColor.a*=255.0/128.0;
    vec4 result;
    if(materialTexturing==0)result=vec4(color.rgb*(128.0/255.0),color.a);
    else if(materialTextureFunction==0) result=sampleColor*color;
    else if(materialTextureFunction==1) result=sampleColor;
    else if(materialTextureFunction==2) result=vec4(sampleColor.rgb*color.rgb+color.a,sampleColor.a+color.a);
    else result=vec4(sampleColor.rgb*color.rgb+color.a,sampleColor.a);
    if(materialAlphaTest==1 && result.a==0.0) discard;
    // GS TFX uses (A*B)>>7 and clamps to an8-bit result before TEST.
    // Compare that integer alpha; UNORM conversion can round exact48 slightly up.
    if(materialAlphaTest==2 && floor(clamp(result.a*128.0,0.0,255.0))<=48.0) discard;
    if(showcaseEnvironmentBlend!=0) {
        // PS2 GS TEX0_2 TCC1/TFX0 produces8-bit Cs,As before ALPHA_2.
        vec3 source=floor(clamp(result.rgb*255.0,vec3(0.0),vec3(255.0)))/255.0;
        float sourceAlpha=floor(clamp(result.a*128.0,0.0,255.0))/128.0;
        result.rgb=source*sourceAlpha;
    }
    fragmentColor=result;
}
)glsl";
// B33_VERTEX_NATIVE_PROJECTED
// BEGIN GENERATED VULKAN SHADERS
constexpr std::uint32_t vulkanVertexSPIRV[]={
    0x07230203u,0x00010000u,0x0008000bu,0x00000463u,0x00000000u,0x00020011u,0x00000001u,0x0006000bu,
    0x00000001u,0x4c534c47u,0x6474732eu,0x3035342eu,0x00000000u,0x0003000eu,0x00000000u,0x00000001u,
    0x0011000fu,0x00000000u,0x00000004u,0x6e69616du,0x00000000u,0x00000279u,0x00000288u,0x000003a4u,
    0x000003d9u,0x00000427u,0x00000433u,0x00000435u,0x0000043bu,0x00000449u,0x00000453u,0x0000045bu,
    0x00000460u,0x00030003u,0x00000002u,0x000001c2u,0x00040005u,0x00000004u,0x6e69616du,0x00000000u,
    0x00090005u,0x0000000cu,0x6974616eu,0x6f436576u,0x69666665u,0x6e656963u,0x31752874u,0x3b31753bu,
    0x00000000u,0x00050005u,0x0000000au,0x6574616du,0x6c616972u,0x00000000u,0x00040005u,0x0000000bu,
    0x65646e69u,0x00000078u,0x000b0005u,0x00000015u,0x6974616eu,0x70536576u,0x6c756365u,0x65527261u,
    0x6e6f7073u,0x75286573u,0x31663b31u,0x3b31623bu,0x00000000u,0x00050005u,0x00000012u,0x6574616du,
    0x6c616972u,0x00000000u,0x00040005u,0x00000013u,0x65736162u,0x00000000u,0x00060005u,0x00000014u,
    0x68706c61u,0x74614d61u,0x61697265u,0x0000006cu,0x000a0005u,0x0000001du,0x6974616eu,0x65526576u,
    0x63656c66u,0x6e6f6974u,0x3b317528u,0x3b336676u,0x3b336676u,0x00000000u,0x00040005u,0x0000001au,
    0x64616873u,0x00676e69u,0x00050005u,0x0000001bu,0x69736f70u,0x6e6f6974u,0x00000000u,0x00040005u,
    0x0000001cu,0x6d726f6eu,0x00006c61u,0x000d0005u,0x00000029u,0x6974616eu,0x694c6576u,0x6c6f4374u,
    0x7528726fu,0x31753b31u,0x3b31753bu,0x3b336676u,0x3b336676u,0x3b336676u,0x3b336676u,0x003b3162u,
    0x00040005u,0x00000021u,0x64616873u,0x00676e69u,0x00050005u,0x00000022u,0x6574616du,0x6c616972u,
    0x00000000u,0x00040005u,0x00000023u,0x67616c66u,0x00000073u,0x00050005u,0x00000024u,0x69736f70u,
    0x6e6f6974u,0x00000000u,0x00040005u,0x00000025u,0x6d726f6eu,0x00006c61u,0x00060005u,0x00000026u,
    0x68747561u,0x6465726fu,0x6f6c6f43u,0x00000072u,0x00070005u,0x00000027u,0x70657270u,0x64657261u,
    0x6c666552u,0x69746365u,0x00006e6fu,0x00060005u,0x00000028u,0x52736168u,0x656c6665u,0x6f697463u,
    0x0000006eu,0x000a0005u,0x00000032u,0x6974616eu,0x68536576u,0x28656461u,0x763b3175u,0x763b3366u,
    0x763b3366u,0x763b3366u,0x003b3466u,0x00040005u,0x0000002du,0x64616873u,0x00676e69u,0x00050005u,
    0x0000002eu,0x69736f70u,0x6e6f6974u,0x00000000u,0x00040005u,0x0000002fu,0x6d726f6eu,0x00006c61u,
    0x00060005u,0x00000030u,0x64616873u,0x65546465u,0x72757478u,0x00000065u,0x00050005u,0x00000031u,
    0x64616873u,0x6f436465u,0x00726f6cu,0x00060005u,0x00000035u,0x6974614eu,0x72466576u,0x44656d61u,
    0x00617461u,0x00050006u,0x00000035u,0x00000000u,0x756c6176u,0x00007365u,0x00050005u,0x00000037u,
    0x6d617266u,0x74614465u,0x00000061u,0x00040005u,0x00000046u,0x696d696cu,0x00646574u,0x00040005u,
    0x00000049u,0x61726170u,0x0000006du,0x00040005u,0x0000004bu,0x61726170u,0x0000006du,0x00040005u,
    0x0000005fu,0x6c616373u,0x00006465u,0x00040005u,0x00000064u,0x65646e69u,0x00000078u,0x00050005u,
    0x00000069u,0x63617266u,0x6e6f6974u,0x00000000u,0x00040005u,0x0000006eu,0x73726966u,0x00000074u,
    0x00040005u,0x00000076u,0x65776f6cu,0x00000072u,0x00040005u,0x0000007fu,0x7478656eu,0x00000000u,
    0x00040005u,0x00000083u,0x65707075u,0x00000072u,0x00040005u,0x0000008cu,0x756c6176u,0x00000065u,
    0x00040005u,0x00000096u,0x756c6176u,0x00000065u,0x00040005u,0x0000009au,0x61726170u,0x0000006du,
    0x00040005u,0x0000009cu,0x61726170u,0x0000006du,0x00040005u,0x000000a2u,0x61726170u,0x0000006du,
    0x00040005u,0x000000a4u,0x61726170u,0x0000006du,0x00050005u,0x000000abu,0x63697774u,0x746f4465u,
    0x00000000u,0x00040005u,0x000000acu,0x73697861u,0x00000000u,0x00050005u,0x000000b5u,0x616c6572u,
    0x65766974u,0x00000000u,0x00040005u,0x000000d6u,0x61757173u,0x00646572u,0x00040005u,0x000000d7u,
    0x73697861u,0x00000000u,0x00050005u,0x000000dfu,0x6c666572u,0x69746365u,0x00006e6fu,0x00060005u,
    0x000000f6u,0x65766e69u,0x4c657372u,0x74676e65u,0x00000068u,0x00040005u,0x00000101u,0x73697861u,
    0x00000000u,0x00060005u,0x00000115u,0x6574616du,0x6c616972u,0x68706c41u,0x00000061u,0x00050005u,
    0x00000119u,0x53626772u,0x75636570u,0x0072616cu,0x00040005u,0x0000011bu,0x61726170u,0x0000006du,
    0x00040005u,0x0000011du,0x61726170u,0x0000006du,0x00040005u,0x00000124u,0x61726170u,0x0000006du,
    0x00040005u,0x00000126u,0x61726170u,0x0000006du,0x00040005u,0x0000012eu,0x61726170u,0x0000006du,
    0x00040005u,0x00000130u,0x61726170u,0x0000006du,0x00050005u,0x00000134u,0x5374696cu,0x75636570u,
    0x0072616cu,0x00040005u,0x00000143u,0x61726170u,0x0000006du,0x00040005u,0x00000145u,0x61726170u,
    0x0000006du,0x00060005u,0x0000014bu,0x68706c61u,0x65705361u,0x616c7563u,0x00000072u,0x00040005u,
    0x00000152u,0x61726170u,0x0000006du,0x00040005u,0x00000154u,0x61726170u,0x0000006du,0x00050005u,
    0x00000158u,0x6c666572u,0x69746365u,0x00006e6fu,0x00040005u,0x00000165u,0x61726170u,0x0000006du,
    0x00040005u,0x00000167u,0x61726170u,0x0000006du,0x00040005u,0x00000169u,0x61726170u,0x0000006du,
    0x00050005u,0x0000016du,0x7074756fu,0x6f437475u,0x00726f6cu,0x00040005u,0x00000178u,0x66666964u,
    0x00657375u,0x00050005u,0x00000179u,0x63657073u,0x72616c75u,0x00000000u,0x00040005u,0x0000017au,
    0x6867696cu,0x00000074u,0x00050005u,0x00000182u,0x6d726f6eu,0x6f446c61u,0x00000074u,0x00040005u,
    0x00000183u,0x73697861u,0x00000000u,0x00060005u,0x000001a1u,0x6c666572u,0x69746365u,0x6f446e6fu,
    0x00000074u,0x00040005u,0x000001a2u,0x73697861u,0x00000000u,0x00040005u,0x000001bau,0x61726170u,
    0x0000006du,0x00040005u,0x000001bcu,0x61726170u,0x0000006du,0x00040005u,0x000001beu,0x61726170u,
    0x0000006du,0x00040005u,0x000001c4u,0x73697861u,0x00000000u,0x00040005u,0x000001ccu,0x756c6176u,
    0x00000065u,0x00040005u,0x000001d2u,0x6867696cu,0x00000074u,0x00040005u,0x000001e9u,0x6867696cu,
    0x00000074u,0x00050005u,0x00000205u,0x68747561u,0x6465726fu,0x00000000u,0x00040005u,0x0000020fu,
    0x61726170u,0x0000006du,0x00040005u,0x00000211u,0x61726170u,0x0000006du,0x00060005u,0x00000224u,
    0x6c666572u,0x69746365u,0x6f446e6fu,0x00000074u,0x00040005u,0x00000228u,0x73697861u,0x00000000u,
    0x00060005u,0x0000023eu,0x63657073u,0x72616c75u,0x68706c41u,0x00000061u,0x00040005u,0x00000244u,
    0x61726170u,0x0000006du,0x00040005u,0x00000246u,0x61726170u,0x0000006du,0x00040005u,0x00000248u,
    0x61726170u,0x0000006du,0x00040005u,0x0000024cu,0x68706c61u,0x00000061u,0x00040005u,0x0000024eu,
    0x61726170u,0x0000006du,0x00040005u,0x00000250u,0x61726170u,0x0000006du,0x00040005u,0x00000252u,
    0x61726170u,0x0000006du,0x00040005u,0x00000254u,0x61726170u,0x0000006du,0x00040005u,0x00000262u,
    0x67616c66u,0x00000073u,0x00040005u,0x00000267u,0x65646f6du,0x00000000u,0x00050005u,0x0000026cu,
    0x6574616du,0x6c616972u,0x00000000u,0x00050005u,0x00000271u,0x61726170u,0x6574656du,0x00007372u,
    0x00050005u,0x00000277u,0x726f6f63u,0x616e6964u,0x00736574u,0x00060005u,0x00000279u,0x74726576u,
    0x65547865u,0x72757478u,0x00000065u,0x00040005u,0x00000286u,0x75736572u,0x0000746cu,0x00050005u,
    0x00000288u,0x74726576u,0x6f437865u,0x00726f6cu,0x00050005u,0x00000296u,0x6c666572u,0x69746365u,
    0x00006e6fu,0x00040005u,0x00000297u,0x61726170u,0x0000006du,0x00040005u,0x00000299u,0x61726170u,
    0x0000006du,0x00040005u,0x0000029bu,0x61726170u,0x0000006du,0x00040005u,0x0000029eu,0x73697861u,
    0x00000000u,0x00040005u,0x000002a6u,0x6e676973u,0x00000000u,0x00040005u,0x000002abu,0x7070616du,
    0x00006465u,0x00030005u,0x000002dau,0x0000007au,0x00040005u,0x000002deu,0x61757173u,0x00646572u,
    0x00060005u,0x000002efu,0x65687073u,0x6e496572u,0x73726576u,0x00000065u,0x00040005u,0x00000313u,
    0x61726170u,0x0000006du,0x00040005u,0x00000315u,0x61726170u,0x0000006du,0x00040005u,0x00000317u,
    0x61726170u,0x0000006du,0x00040005u,0x00000319u,0x61726170u,0x0000006du,0x00040005u,0x0000031bu,
    0x61726170u,0x0000006du,0x00040005u,0x0000031du,0x61726170u,0x0000006du,0x00040005u,0x00000320u,
    0x61726170u,0x0000006du,0x00040005u,0x00000322u,0x61726170u,0x0000006du,0x00040005u,0x0000032bu,
    0x68706c61u,0x00000061u,0x00040005u,0x0000032du,0x61726170u,0x0000006du,0x00040005u,0x0000032fu,
    0x61726170u,0x0000006du,0x00040005u,0x00000332u,0x61726170u,0x0000006du,0x00040005u,0x00000334u,
    0x61726170u,0x0000006du,0x00060005u,0x0000033bu,0x65746e69u,0x41726567u,0x6168706cu,0x00000000u,
    0x00040005u,0x00000369u,0x61726170u,0x0000006du,0x00040005u,0x0000036bu,0x61726170u,0x0000006du,
    0x00040005u,0x0000036du,0x61726170u,0x0000006du,0x00040005u,0x0000036fu,0x61726170u,0x0000006du,
    0x00040005u,0x00000371u,0x61726170u,0x0000006du,0x00040005u,0x00000373u,0x61726170u,0x0000006du,
    0x00040005u,0x00000376u,0x61726170u,0x0000006du,0x00040005u,0x00000377u,0x61726170u,0x0000006du,
    0x00050005u,0x00000383u,0x65646f6du,0x436f546cu,0x0070696cu,0x00080005u,0x00000384u,0x6974614eu,
    0x72446576u,0x61507761u,0x656d6172u,0x73726574u,0x00000000u,0x00070006u,0x00000384u,0x00000000u,
    0x74786574u,0x46657275u,0x74636e75u,0x006e6f69u,0x00060006u,0x00000384u,0x00000001u,0x68706c61u,
    0x73655461u,0x00000074u,0x00060006u,0x00000384u,0x00000002u,0x53646f6cu,0x656c6163u,0x00000000u,
    0x00070006u,0x00000384u,0x00000003u,0x696e696du,0x61636966u,0x6e6f6974u,0x00000000u,0x00080006u,
    0x00000384u,0x00000004u,0x6974616eu,0x6f4d6576u,0x546c6564u,0x696c436fu,0x00000070u,0x00080006u,
    0x00000384u,0x00000005u,0x6974616eu,0x6c436576u,0x614d7069u,0x6e697070u,0x00000067u,0x00080006u,
    0x00000384u,0x00000006u,0x6974616eu,0x72546576u,0x66736e61u,0x4d6d726fu,0x0065646fu,0x00080006u,
    0x00000384u,0x00000007u,0x6974616eu,0x65546576u,0x72757478u,0x63535165u,0x00656c61u,0x00080006u,
    0x00000384u,0x00000008u,0x6974616eu,0x68536576u,0x6e696461u,0x646e4967u,0x00007865u,0x00040005u,
    0x00000386u,0x6974616eu,0x00006576u,0x00050005u,0x0000038bu,0x70696c63u,0x7070614du,0x00676e69u,
    0x00060005u,0x00000390u,0x74786574u,0x51657275u,0x6c616353u,0x00000065u,0x00040005u,0x00000395u,
    0x64616873u,0x00676e69u,0x00040005u,0x000003a2u,0x73726966u,0x00000074u,0x00070005u,0x000003a4u,
    0x495f6c67u,0x6174736eu,0x4965636eu,0x7865646eu,0x00000000u,0x00060005u,0x000003d8u,0x61636f6cu,
    0x736f506cu,0x6f697469u,0x0000006eu,0x00060005u,0x000003d9u,0x74726576u,0x6f507865u,0x69746973u,
    0x00006e6fu,0x00050005u,0x000003f2u,0x6a6f7270u,0x65746365u,0x00000064u,0x00060005u,0x00000425u,
    0x505f6c67u,0x65567265u,0x78657472u,0x00000000u,0x00060006u,0x00000425u,0x00000000u,0x505f6c67u,
    0x7469736fu,0x006e6f69u,0x00070006u,0x00000425u,0x00000001u,0x505f6c67u,0x746e696fu,0x657a6953u,
    0x00000000u,0x00070006u,0x00000425u,0x00000002u,0x435f6c67u,0x4470696cu,0x61747369u,0x0065636eu,
    0x00070006u,0x00000425u,0x00000003u,0x435f6c67u,0x446c6c75u,0x61747369u,0x0065636eu,0x00030005u,
    0x00000427u,0x00000000u,0x00030005u,0x00000433u,0x00717675u,0x00040005u,0x00000435u,0x6f6c6f63u,
    0x00000072u,0x00060005u,0x0000043bu,0x74726576u,0x6f4e7865u,0x6c616d72u,0x00000000u,0x00040005u,
    0x0000043cu,0x61726170u,0x0000006du,0x00040005u,0x0000043eu,0x61726170u,0x0000006du,0x00040005u,
    0x00000441u,0x61726170u,0x0000006du,0x00040005u,0x00000443u,0x61726170u,0x0000006du,0x00040005u,
    0x00000444u,0x61726170u,0x0000006du,0x00060005u,0x00000449u,0x50646f6cu,0x6d617261u,0x72657465u,
    0x00000073u,0x00050005u,0x00000453u,0x74726576u,0x6f4c7865u,0x00000064u,0x00070005u,0x0000045bu,
    0x70796c67u,0x756f4268u,0x4973646eu,0x7865646eu,0x00000000u,0x00070005u,0x00000460u,0x74726576u,
    0x6c477865u,0x42687079u,0x646e756fu,0x00000073u,0x00040047u,0x00000034u,0x00000006u,0x00000010u,
    0x00030047u,0x00000035u,0x00000003u,0x00040048u,0x00000035u,0x00000000u,0x00000018u,0x00050048u,
    0x00000035u,0x00000000u,0x00000023u,0x00000000u,0x00030047u,0x00000037u,0x00000018u,0x00040047u,
    0x00000037u,0x00000021u,0x00000000u,0x00040047u,0x00000037u,0x00000022u,0x00000001u,0x00030047u,
    0x00000061u,0x0000002au,0x00030047u,0x00000063u,0x0000002au,0x00030047u,0x0000006du,0x0000002au,
    0x00030047u,0x00000091u,0x0000002au,0x00030047u,0x00000092u,0x0000002au,0x00030047u,0x00000093u,
    0x0000002au,0x00030047u,0x000000a7u,0x0000002au,0x00030047u,0x000000bfu,0x0000002au,0x00030047u,
    0x000000c8u,0x0000002au,0x00030047u,0x000000c9u,0x0000002au,0x00030047u,0x000000d1u,0x0000002au,
    0x00030047u,0x000000d2u,0x0000002au,0x00030047u,0x000000e8u,0x0000002au,0x00030047u,0x000000e9u,
    0x0000002au,0x00030047u,0x000000f2u,0x0000002au,0x00030047u,0x000000f3u,0x0000002au,0x00030047u,
    0x000000feu,0x0000002au,0x00030047u,0x0000010eu,0x0000002au,0x00030047u,0x00000196u,0x0000002au,
    0x00030047u,0x00000197u,0x0000002au,0x00030047u,0x000001b5u,0x0000002au,0x00030047u,0x000001b6u,
    0x0000002au,0x00030047u,0x000001e5u,0x0000002au,0x00030047u,0x000001e6u,0x0000002au,0x00030047u,
    0x000001fdu,0x0000002au,0x00030047u,0x000001feu,0x0000002au,0x00030047u,0x0000020au,0x0000002au,
    0x00030047u,0x0000020eu,0x0000002au,0x00030047u,0x00000213u,0x0000002au,0x00030047u,0x00000214u,
    0x0000002au,0x00030047u,0x00000215u,0x0000002au,0x00030047u,0x0000021bu,0x0000002au,0x00030047u,
    0x0000023au,0x0000002au,0x00030047u,0x0000023bu,0x0000002au,0x00030047u,0x00000257u,0x0000002au,
    0x00030047u,0x00000258u,0x0000002au,0x00030047u,0x0000025du,0x0000002au,0x00040047u,0x00000279u,
    0x0000001eu,0x00000001u,0x00030047u,0x0000027fu,0x0000002au,0x00040047u,0x00000288u,0x0000001eu,
    0x00000002u,0x00030047u,0x000002b3u,0x0000002au,0x00030047u,0x000002b6u,0x0000002au,0x00030047u,
    0x000002c2u,0x0000002au,0x00030047u,0x000002c5u,0x0000002au,0x00030047u,0x000002c6u,0x0000002au,
    0x00030047u,0x000002d2u,0x0000002au,0x00030047u,0x000002d5u,0x0000002au,0x00030047u,0x000002d6u,
    0x0000002au,0x00030047u,0x000002d9u,0x0000002au,0x00030047u,0x000002ddu,0x0000002au,0x00030047u,
    0x000002e3u,0x0000002au,0x00030047u,0x000002e8u,0x0000002au,0x00030047u,0x000002e9u,0x0000002au,
    0x00030047u,0x000002edu,0x0000002au,0x00030047u,0x000002eeu,0x0000002au,0x00030047u,0x000002f7u,
    0x0000002au,0x00030047u,0x000002feu,0x0000002au,0x00030047u,0x000002ffu,0x0000002au,0x00030047u,
    0x00000300u,0x0000002au,0x00030047u,0x00000304u,0x0000002au,0x00030047u,0x00000305u,0x0000002au,
    0x00030047u,0x00000306u,0x0000002au,0x00030047u,0x00000338u,0x0000002au,0x00030047u,0x00000339u,
    0x0000002au,0x00030047u,0x00000355u,0x0000002au,0x00030047u,0x0000035fu,0x0000002au,0x00030047u,
    0x0000037du,0x0000002au,0x00030047u,0x00000384u,0x00000002u,0x00050048u,0x00000384u,0x00000000u,
    0x00000023u,0x00000000u,0x00050048u,0x00000384u,0x00000001u,0x00000023u,0x00000004u,0x00050048u,
    0x00000384u,0x00000002u,0x00000023u,0x00000008u,0x00050048u,0x00000384u,0x00000003u,0x00000023u,
    0x0000000cu,0x00040048u,0x00000384u,0x00000004u,0x00000005u,0x00050048u,0x00000384u,0x00000004u,
    0x00000007u,0x00000010u,0x00050048u,0x00000384u,0x00000004u,0x00000023u,0x00000010u,0x00050048u,
    0x00000384u,0x00000005u,0x00000023u,0x00000050u,0x00050048u,0x00000384u,0x00000006u,0x00000023u,
    0x00000060u,0x00050048u,0x00000384u,0x00000007u,0x00000023u,0x00000064u,0x00050048u,0x00000384u,
    0x00000008u,0x00000023u,0x00000068u,0x00040047u,0x000003a4u,0x0000000bu,0x0000002bu,0x00040047u,
    0x000003d9u,0x0000001eu,0x00000000u,0x00030047u,0x000003e6u,0x0000002au,0x00030047u,0x000003f7u,
    0x0000002au,0x00030047u,0x000003fdu,0x0000002au,0x00030047u,0x000003feu,0x0000002au,0x00030047u,
    0x00000404u,0x0000002au,0x00030047u,0x00000405u,0x0000002au,0x00030047u,0x0000040cu,0x0000002au,
    0x00030047u,0x0000040du,0x0000002au,0x00030047u,0x00000412u,0x0000002au,0x00030047u,0x00000413u,
    0x0000002au,0x00030047u,0x00000419u,0x0000002au,0x00030047u,0x0000041eu,0x0000002au,0x00030047u,
    0x0000041fu,0x0000002au,0x00030047u,0x00000425u,0x00000002u,0x00050048u,0x00000425u,0x00000000u,
    0x0000000bu,0x00000000u,0x00040048u,0x00000425u,0x00000000u,0x00000012u,0x00050048u,0x00000425u,
    0x00000001u,0x0000000bu,0x00000001u,0x00050048u,0x00000425u,0x00000002u,0x0000000bu,0x00000003u,
    0x00050048u,0x00000425u,0x00000003u,0x0000000bu,0x00000004u,0x00040047u,0x00000433u,0x0000001eu,
    0x00000000u,0x00040047u,0x00000435u,0x0000001eu,0x00000001u,0x00040047u,0x0000043bu,0x0000001eu,
    0x00000005u,0x00030047u,0x00000449u,0x0000000eu,0x00040047u,0x00000449u,0x0000001eu,0x00000002u,
    0x00040047u,0x00000453u,0x0000001eu,0x00000003u,0x00030047u,0x0000045bu,0x0000000eu,0x00040047u,
    0x0000045bu,0x0000001eu,0x00000003u,0x00040047u,0x00000460u,0x0000001eu,0x00000004u,0x00020013u,
    0x00000002u,0x00030021u,0x00000003u,0x00000002u,0x00040015u,0x00000006u,0x00000020u,0x00000000u,
    0x00040020u,0x00000007u,0x00000007u,0x00000006u,0x00030016u,0x00000008u,0x00000020u,0x00050021u,
    0x00000009u,0x00000008u,0x00000007u,0x00000007u,0x00040020u,0x0000000eu,0x00000007u,0x00000008u,
    0x00020014u,0x0000000fu,0x00040020u,0x00000010u,0x00000007u,0x0000000fu,0x00060021u,0x00000011u,
    0x00000008u,0x00000007u,0x0000000eu,0x00000010u,0x00040017u,0x00000017u,0x00000008u,0x00000003u,
    0x00040020u,0x00000018u,0x00000007u,0x00000017u,0x00060021u,0x00000019u,0x00000017u,0x00000007u,
    0x00000018u,0x00000018u,0x00040017u,0x0000001fu,0x00000008u,0x00000004u,0x000b0021u,0x00000020u,
    0x0000001fu,0x00000007u,0x00000007u,0x00000007u,0x00000018u,0x00000018u,0x00000018u,0x00000018u,
    0x00000010u,0x00040020u,0x0000002bu,0x00000007u,0x0000001fu,0x00080021u,0x0000002cu,0x00000002u,
    0x00000007u,0x00000018u,0x00000018u,0x00000018u,0x0000002bu,0x0003001du,0x00000034u,0x0000001fu,
    0x0003001eu,0x00000035u,0x00000034u,0x00040020u,0x00000036u,0x00000002u,0x00000035u,0x0004003bu,
    0x00000036u,0x00000037u,0x00000002u,0x00040015u,0x00000038u,0x00000020u,0x00000001u,0x0004002bu,
    0x00000038u,0x00000039u,0x00000000u,0x0004002bu,0x00000006u,0x0000003cu,0x00000004u,0x00040020u,
    0x00000041u,0x00000002u,0x00000008u,0x0004002bu,0x00000006u,0x00000048u,0x00000002u,0x0004002bu,
    0x00000006u,0x00000050u,0x00000000u,0x0004002bu,0x00000008u,0x00000053u,0x00000000u,0x0004002bu,
    0x00000008u,0x00000056u,0x3dcccccdu,0x0004002bu,0x00000008u,0x0000005au,0x3f800000u,0x0004002bu,
    0x00000008u,0x00000062u,0x448e38e4u,0x0004002bu,0x00000006u,0x00000067u,0x000003ffu,0x0004002bu,
    0x00000006u,0x00000070u,0x00000005u,0x0004002bu,0x00000006u,0x00000073u,0x00000101u,0x0004002bu,
    0x00000006u,0x00000081u,0x00000001u,0x0004002bu,0x00000006u,0x00000098u,0x0000000du,0x0004002bu,
    0x00000006u,0x000000a0u,0x0000000cu,0x0004002bu,0x00000006u,0x000000b3u,0x00000003u,0x0004002bu,
    0x00000038u,0x000000d4u,0x00000001u,0x0004002bu,0x00000006u,0x0000011au,0x00000008u,0x0004002bu,
    0x00000006u,0x00000123u,0x00000009u,0x0004002bu,0x00000006u,0x0000012du,0x0000000au,0x0004002bu,
    0x00000006u,0x00000136u,0x00000080u,0x0004002bu,0x00000006u,0x00000142u,0x0000000fu,0x0004002bu,
    0x00000006u,0x0000014du,0x00000040u,0x0006002cu,0x00000017u,0x00000159u,0x00000053u,0x00000053u,
    0x00000053u,0x0004002bu,0x00000006u,0x000001f3u,0x00000010u,0x0004002bu,0x00000008u,0x00000209u,
    0x43000000u,0x0004002bu,0x00000008u,0x0000020du,0x437f0000u,0x00030029u,0x0000000fu,0x00000243u,
    0x0004002bu,0x00000006u,0x0000024du,0x0000000eu,0x00040020u,0x00000274u,0x00000002u,0x0000001fu,
    0x00040020u,0x00000278u,0x00000001u,0x00000017u,0x0004003bu,0x00000278u,0x00000279u,0x00000001u,
    0x00040017u,0x0000027au,0x00000008u,0x00000002u,0x00040020u,0x00000280u,0x00000001u,0x00000008u,
    0x00040020u,0x00000287u,0x00000001u,0x0000001fu,0x0004003bu,0x00000287u,0x00000288u,0x00000001u,
    0x0004002bu,0x00000008u,0x000002a9u,0xbf800000u,0x0004002bu,0x00000008u,0x000002fau,0x3f000000u,
    0x0007002cu,0x0000001fu,0x00000308u,0x0000005au,0x0000005au,0x0000005au,0x0000005au,0x0004002bu,
    0x00000006u,0x0000032cu,0x00000007u,0x0004002bu,0x00000006u,0x00000331u,0x0000000bu,0x00040020u,
    0x0000033au,0x00000007u,0x00000038u,0x0004002bu,0x00000008u,0x0000033du,0x4f000000u,0x0004002bu,
    0x00000038u,0x00000342u,0x7fffffffu,0x0004002bu,0x00000008u,0x00000345u,0xcf000000u,0x0004002bu,
    0x00000038u,0x0000034au,0x80000000u,0x0004002bu,0x00000006u,0x00000352u,0x000000ffu,0x0004002bu,
    0x00000008u,0x0000035eu,0x3f008081u,0x0004002bu,0x00000006u,0x00000363u,0x000000c0u,0x0003002au,
    0x0000000fu,0x00000368u,0x00040018u,0x00000381u,0x0000001fu,0x00000004u,0x00040020u,0x00000382u,
    0x00000007u,0x00000381u,0x000b001eu,0x00000384u,0x00000038u,0x00000038u,0x00000008u,0x00000038u,
    0x00000381u,0x0000001fu,0x00000038u,0x00000008u,0x00000006u,0x00040020u,0x00000385u,0x00000009u,
    0x00000384u,0x0004003bu,0x00000385u,0x00000386u,0x00000009u,0x0004002bu,0x00000038u,0x00000387u,
    0x00000004u,0x00040020u,0x00000388u,0x00000009u,0x00000381u,0x0004002bu,0x00000038u,0x0000038cu,
    0x00000005u,0x00040020u,0x0000038du,0x00000009u,0x0000001fu,0x0004002bu,0x00000038u,0x00000391u,
    0x00000007u,0x00040020u,0x00000392u,0x00000009u,0x00000008u,0x0004002bu,0x00000038u,0x00000396u,
    0x00000008u,0x00040020u,0x00000397u,0x00000009u,0x00000006u,0x0004002bu,0x00000038u,0x0000039au,
    0x00000006u,0x00040020u,0x0000039bu,0x00000009u,0x00000038u,0x0004002bu,0x00000038u,0x0000039eu,
    0x00000002u,0x00040020u,0x000003a3u,0x00000001u,0x00000038u,0x0004003bu,0x000003a3u,0x000003a4u,
    0x00000001u,0x0004003bu,0x00000287u,0x000003d9u,0x00000001u,0x0004002bu,0x00000038u,0x00000407u,
    0x00000003u,0x0004001cu,0x00000424u,0x00000008u,0x00000081u,0x0006001eu,0x00000425u,0x0000001fu,
    0x00000008u,0x00000424u,0x00000424u,0x00040020u,0x00000426u,0x00000003u,0x00000425u,0x0004003bu,
    0x00000426u,0x00000427u,0x00000003u,0x00040017u,0x0000042du,0x0000000fu,0x00000004u,0x00040020u,
    0x00000430u,0x00000003u,0x0000001fu,0x00040020u,0x00000432u,0x00000003u,0x00000017u,0x0004003bu,
    0x00000432u,0x00000433u,0x00000003u,0x0004003bu,0x00000430u,0x00000435u,0x00000003u,0x0004003bu,
    0x00000278u,0x0000043bu,0x00000001u,0x00040020u,0x00000448u,0x00000003u,0x0000027au,0x0004003bu,
    0x00000448u,0x00000449u,0x00000003u,0x00040020u,0x00000452u,0x00000001u,0x0000027au,0x0004003bu,
    0x00000452u,0x00000453u,0x00000001u,0x00040020u,0x0000045au,0x00000003u,0x00000006u,0x0004003bu,
    0x0000045au,0x0000045bu,0x00000003u,0x00040020u,0x0000045fu,0x00000001u,0x00000006u,0x0004003bu,
    0x0000045fu,0x00000460u,0x00000001u,0x00050036u,0x00000002u,0x00000004u,0x00000000u,0x00000003u,
    0x000200f8u,0x00000005u,0x0004003bu,0x00000382u,0x00000383u,0x00000007u,0x0004003bu,0x0000002bu,
    0x0000038bu,0x00000007u,0x0004003bu,0x0000000eu,0x00000390u,0x00000007u,0x0004003bu,0x00000007u,
    0x00000395u,0x00000007u,0x0004003bu,0x00000007u,0x000003a2u,0x00000007u,0x0004003bu,0x0000002bu,
    0x000003d8u,0x00000007u,0x0004003bu,0x0000002bu,0x000003f2u,0x00000007u,0x0004003bu,0x00000007u,
    0x0000043cu,0x00000007u,0x0004003bu,0x00000018u,0x0000043eu,0x00000007u,0x0004003bu,0x00000018u,
    0x00000441u,0x00000007u,0x0004003bu,0x00000018u,0x00000443u,0x00000007u,0x0004003bu,0x0000002bu,
    0x00000444u,0x00000007u,0x0004003bu,0x0000000eu,0x0000044du,0x00000007u,0x00050041u,0x00000388u,
    0x00000389u,0x00000386u,0x00000387u,0x0004003du,0x00000381u,0x0000038au,0x00000389u,0x0003003eu,
    0x00000383u,0x0000038au,0x00050041u,0x0000038du,0x0000038eu,0x00000386u,0x0000038cu,0x0004003du,
    0x0000001fu,0x0000038fu,0x0000038eu,0x0003003eu,0x0000038bu,0x0000038fu,0x00050041u,0x00000392u,
    0x00000393u,0x00000386u,0x00000391u,0x0004003du,0x00000008u,0x00000394u,0x00000393u,0x0003003eu,
    0x00000390u,0x00000394u,0x00050041u,0x00000397u,0x00000398u,0x00000386u,0x00000396u,0x0004003du,
    0x00000006u,0x00000399u,0x00000398u,0x0003003eu,0x00000395u,0x00000399u,0x00050041u,0x0000039bu,
    0x0000039cu,0x00000386u,0x0000039au,0x0004003du,0x00000038u,0x0000039du,0x0000039cu,0x000500aau,
    0x0000000fu,0x0000039fu,0x0000039du,0x0000039eu,0x000300f7u,0x000003a1u,0x00000000u,0x000400fau,
    0x0000039fu,0x000003a0u,0x000003a1u,0x000200f8u,0x000003a0u,0x0004003du,0x00000038u,0x000003a5u,
    0x000003a4u,0x0004007cu,0x00000006u,0x000003a6u,0x000003a5u,0x0003003eu,0x000003a2u,0x000003a6u,
    0x0004003du,0x00000006u,0x000003a7u,0x000003a2u,0x00060041u,0x00000274u,0x000003a8u,0x00000037u,
    0x00000039u,0x000003a7u,0x0004003du,0x0000001fu,0x000003a9u,0x000003a8u,0x0004003du,0x00000006u,
    0x000003aau,0x000003a2u,0x00050080u,0x00000006u,0x000003abu,0x000003aau,0x00000081u,0x00060041u,
    0x00000274u,0x000003acu,0x00000037u,0x00000039u,0x000003abu,0x0004003du,0x0000001fu,0x000003adu,
    0x000003acu,0x0004003du,0x00000006u,0x000003aeu,0x000003a2u,0x00050080u,0x00000006u,0x000003afu,
    0x000003aeu,0x00000048u,0x00060041u,0x00000274u,0x000003b0u,0x00000037u,0x00000039u,0x000003afu,
    0x0004003du,0x0000001fu,0x000003b1u,0x000003b0u,0x0004003du,0x00000006u,0x000003b2u,0x000003a2u,
    0x00050080u,0x00000006u,0x000003b3u,0x000003b2u,0x000000b3u,0x00060041u,0x00000274u,0x000003b4u,
    0x00000037u,0x00000039u,0x000003b3u,0x0004003du,0x0000001fu,0x000003b5u,0x000003b4u,0x00050051u,
    0x00000008u,0x000003b6u,0x000003a9u,0x00000000u,0x00050051u,0x00000008u,0x000003b7u,0x000003a9u,
    0x00000001u,0x00050051u,0x00000008u,0x000003b8u,0x000003a9u,0x00000002u,0x00050051u,0x00000008u,
    0x000003b9u,0x000003a9u,0x00000003u,0x00050051u,0x00000008u,0x000003bau,0x000003adu,0x00000000u,
    0x00050051u,0x00000008u,0x000003bbu,0x000003adu,0x00000001u,0x00050051u,0x00000008u,0x000003bcu,
    0x000003adu,0x00000002u,0x00050051u,0x00000008u,0x000003bdu,0x000003adu,0x00000003u,0x00050051u,
    0x00000008u,0x000003beu,0x000003b1u,0x00000000u,0x00050051u,0x00000008u,0x000003bfu,0x000003b1u,
    0x00000001u,0x00050051u,0x00000008u,0x000003c0u,0x000003b1u,0x00000002u,0x00050051u,0x00000008u,
    0x000003c1u,0x000003b1u,0x00000003u,0x00050051u,0x00000008u,0x000003c2u,0x000003b5u,0x00000000u,
    0x00050051u,0x00000008u,0x000003c3u,0x000003b5u,0x00000001u,0x00050051u,0x00000008u,0x000003c4u,
    0x000003b5u,0x00000002u,0x00050051u,0x00000008u,0x000003c5u,0x000003b5u,0x00000003u,0x00070050u,
    0x0000001fu,0x000003c6u,0x000003b6u,0x000003b7u,0x000003b8u,0x000003b9u,0x00070050u,0x0000001fu,
    0x000003c7u,0x000003bau,0x000003bbu,0x000003bcu,0x000003bdu,0x00070050u,0x0000001fu,0x000003c8u,
    0x000003beu,0x000003bfu,0x000003c0u,0x000003c1u,0x00070050u,0x0000001fu,0x000003c9u,0x000003c2u,
    0x000003c3u,0x000003c4u,0x000003c5u,0x00070050u,0x00000381u,0x000003cau,0x000003c6u,0x000003c7u,
    0x000003c8u,0x000003c9u,0x0003003eu,0x00000383u,0x000003cau,0x0004003du,0x00000006u,0x000003cbu,
    0x000003a2u,0x00050080u,0x00000006u,0x000003ccu,0x000003cbu,0x0000003cu,0x00060041u,0x00000274u,
    0x000003cdu,0x00000037u,0x00000039u,0x000003ccu,0x0004003du,0x0000001fu,0x000003ceu,0x000003cdu,
    0x0003003eu,0x0000038bu,0x000003ceu,0x0004003du,0x00000006u,0x000003cfu,0x000003a2u,0x00050080u,
    0x00000006u,0x000003d0u,0x000003cfu,0x00000070u,0x00070041u,0x00000041u,0x000003d1u,0x00000037u,
    0x00000039u,0x000003d0u,0x00000050u,0x0004003du,0x00000008u,0x000003d2u,0x000003d1u,0x0003003eu,
    0x00000390u,0x000003d2u,0x0004003du,0x00000006u,0x000003d3u,0x000003a2u,0x00050080u,0x00000006u,
    0x000003d4u,0x000003d3u,0x00000070u,0x00070041u,0x00000041u,0x000003d5u,0x00000037u,0x00000039u,
    0x000003d4u,0x00000081u,0x0004003du,0x00000008u,0x000003d6u,0x000003d5u,0x0004007cu,0x00000006u,
    0x000003d7u,0x000003d6u,0x0003003eu,0x00000395u,0x000003d7u,0x000200f9u,0x000003a1u,0x000200f8u,
    0x000003a1u,0x0004003du,0x0000001fu,0x000003dau,0x000003d9u,0x0003003eu,0x000003d8u,0x000003dau,
    0x0004003du,0x00000006u,0x000003dbu,0x00000395u,0x000500abu,0x0000000fu,0x000003dcu,0x000003dbu,
    0x00000050u,0x000300f7u,0x000003deu,0x00000000u,0x000400fau,0x000003dcu,0x000003ddu,0x000003deu,
    0x000200f8u,0x000003ddu,0x0004003du,0x0000001fu,0x000003dfu,0x000003d9u,0x0008004fu,0x00000017u,
    0x000003e0u,0x000003dfu,0x000003dfu,0x00000000u,0x00000001u,0x00000002u,0x0004003du,0x00000006u,
    0x000003e1u,0x00000395u,0x00050080u,0x00000006u,0x000003e2u,0x000003e1u,0x00000048u,0x00060041u,
    0x00000274u,0x000003e3u,0x00000037u,0x00000039u,0x000003e2u,0x0004003du,0x0000001fu,0x000003e4u,
    0x000003e3u,0x0008004fu,0x00000017u,0x000003e5u,0x000003e4u,0x000003e4u,0x00000000u,0x00000001u,
    0x00000002u,0x00050081u,0x00000017u,0x000003e6u,0x000003e0u,0x000003e5u,0x00050041u,0x0000000eu,
    0x000003e7u,0x000003d8u,0x00000050u,0x00050051u,0x00000008u,0x000003e8u,0x000003e6u,0x00000000u,
    0x0003003eu,0x000003e7u,0x000003e8u,0x00050041u,0x0000000eu,0x000003e9u,0x000003d8u,0x00000081u,
    0x00050051u,0x00000008u,0x000003eau,0x000003e6u,0x00000001u,0x0003003eu,0x000003e9u,0x000003eau,
    0x00050041u,0x0000000eu,0x000003ebu,0x000003d8u,0x00000048u,0x00050051u,0x00000008u,0x000003ecu,
    0x000003e6u,0x00000002u,0x0003003eu,0x000003ebu,0x000003ecu,0x0004003du,0x00000006u,0x000003edu,
    0x00000395u,0x00050080u,0x00000006u,0x000003eeu,0x000003edu,0x00000048u,0x00070041u,0x00000041u,
    0x000003efu,0x00000037u,0x00000039u,0x000003eeu,0x000000b3u,0x0004003du,0x00000008u,0x000003f0u,
    0x000003efu,0x00050041u,0x0000000eu,0x000003f1u,0x000003d8u,0x000000b3u,0x0003003eu,0x000003f1u,
    0x000003f0u,0x000200f9u,0x000003deu,0x000200f8u,0x000003deu,0x00050041u,0x0000002bu,0x000003f3u,
    0x00000383u,0x00000039u,0x0004003du,0x0000001fu,0x000003f4u,0x000003f3u,0x00050041u,0x0000000eu,
    0x000003f5u,0x000003d8u,0x00000050u,0x0004003du,0x00000008u,0x000003f6u,0x000003f5u,0x0005008eu,
    0x0000001fu,0x000003f7u,0x000003f4u,0x000003f6u,0x0003003eu,0x000003f2u,0x000003f7u,0x0004003du,
    0x0000001fu,0x000003f8u,0x000003f2u,0x00050041u,0x0000002bu,0x000003f9u,0x00000383u,0x000000d4u,
    0x0004003du,0x0000001fu,0x000003fau,0x000003f9u,0x00050041u,0x0000000eu,0x000003fbu,0x000003d8u,
    0x00000081u,0x0004003du,0x00000008u,0x000003fcu,0x000003fbu,0x0005008eu,0x0000001fu,0x000003fdu,
    0x000003fau,0x000003fcu,0x00050081u,0x0000001fu,0x000003feu,0x000003f8u,0x000003fdu,0x0003003eu,
    0x000003f2u,0x000003feu,0x0004003du,0x0000001fu,0x000003ffu,0x000003f2u,0x00050041u,0x0000002bu,
    0x00000400u,0x00000383u,0x0000039eu,0x0004003du,0x0000001fu,0x00000401u,0x00000400u,0x00050041u,
    0x0000000eu,0x00000402u,0x000003d8u,0x00000048u,0x0004003du,0x00000008u,0x00000403u,0x00000402u,
    0x0005008eu,0x0000001fu,0x00000404u,0x00000401u,0x00000403u,0x00050081u,0x0000001fu,0x00000405u,
    0x000003ffu,0x00000404u,0x0003003eu,0x000003f2u,0x00000405u,0x0004003du,0x0000001fu,0x00000406u,
    0x000003f2u,0x00050041u,0x0000002bu,0x00000408u,0x00000383u,0x00000407u,0x0004003du,0x0000001fu,
    0x00000409u,0x00000408u,0x00050041u,0x0000000eu,0x0000040au,0x000003d8u,0x000000b3u,0x0004003du,
    0x00000008u,0x0000040bu,0x0000040au,0x0005008eu,0x0000001fu,0x0000040cu,0x00000409u,0x0000040bu,
    0x00050081u,0x0000001fu,0x0000040du,0x00000406u,0x0000040cu,0x0003003eu,0x000003f2u,0x0000040du,
    0x00050041u,0x0000000eu,0x0000040eu,0x000003f2u,0x00000048u,0x0004003du,0x00000008u,0x0000040fu,
    0x0000040eu,0x00050041u,0x0000000eu,0x00000410u,0x000003f2u,0x000000b3u,0x0004003du,0x00000008u,
    0x00000411u,0x00000410u,0x00050081u,0x00000008u,0x00000412u,0x0000040fu,0x00000411u,0x00050085u,
    0x00000008u,0x00000413u,0x00000412u,0x000002fau,0x00050041u,0x0000000eu,0x00000414u,0x000003f2u,
    0x00000048u,0x0003003eu,0x00000414u,0x00000413u,0x0004003du,0x0000001fu,0x00000415u,0x000003f2u,
    0x0007004fu,0x0000027au,0x00000416u,0x00000415u,0x00000415u,0x00000000u,0x00000001u,0x0004003du,
    0x0000001fu,0x00000417u,0x0000038bu,0x0007004fu,0x0000027au,0x00000418u,0x00000417u,0x00000417u,
    0x00000000u,0x00000001u,0x00050085u,0x0000027au,0x00000419u,0x00000416u,0x00000418u,0x00050041u,
    0x0000000eu,0x0000041au,0x000003f2u,0x000000b3u,0x0004003du,0x00000008u,0x0000041bu,0x0000041au,
    0x0004003du,0x0000001fu,0x0000041cu,0x0000038bu,0x0007004fu,0x0000027au,0x0000041du,0x0000041cu,
    0x0000041cu,0x00000002u,0x00000003u,0x0005008eu,0x0000027au,0x0000041eu,0x0000041du,0x0000041bu,
    0x00050081u,0x0000027au,0x0000041fu,0x00000419u,0x0000041eu,0x00050041u,0x0000000eu,0x00000420u,
    0x000003f2u,0x00000050u,0x00050051u,0x00000008u,0x00000421u,0x0000041fu,0x00000000u,0x0003003eu,
    0x00000420u,0x00000421u,0x00050041u,0x0000000eu,0x00000422u,0x000003f2u,0x00000081u,0x00050051u,
    0x00000008u,0x00000423u,0x0000041fu,0x00000001u,0x0003003eu,0x00000422u,0x00000423u,0x00050041u,
    0x0000039bu,0x00000428u,0x00000386u,0x0000039au,0x0004003du,0x00000038u,0x00000429u,0x00000428u,
    0x000500abu,0x0000000fu,0x0000042au,0x00000429u,0x00000039u,0x0004003du,0x0000001fu,0x0000042bu,
    0x000003f2u,0x0004003du,0x0000001fu,0x0000042cu,0x000003d9u,0x00070050u,0x0000042du,0x0000042eu,
    0x0000042au,0x0000042au,0x0000042au,0x0000042au,0x000600a9u,0x0000001fu,0x0000042fu,0x0000042eu,
    0x0000042bu,0x0000042cu,0x00050041u,0x00000430u,0x00000431u,0x00000427u,0x00000039u,0x0003003eu,
    0x00000431u,0x0000042fu,0x0004003du,0x00000017u,0x00000434u,0x00000279u,0x0003003eu,0x00000433u,
    0x00000434u,0x0004003du,0x0000001fu,0x00000436u,0x00000288u,0x0003003eu,0x00000435u,0x00000436u,
    0x0004003du,0x00000006u,0x00000437u,0x00000395u,0x000500abu,0x0000000fu,0x00000438u,0x00000437u,
    0x00000050u,0x000300f7u,0x0000043au,0x00000000u,0x000400fau,0x00000438u,0x00000439u,0x0000043au,
    0x000200f8u,0x00000439u,0x0004003du,0x00000006u,0x0000043du,0x00000395u,0x0003003eu,0x0000043cu,
    0x0000043du,0x0004003du,0x0000001fu,0x0000043fu,0x000003d9u,0x0008004fu,0x00000017u,0x00000440u,
    0x0000043fu,0x0000043fu,0x00000000u,0x00000001u,0x00000002u,0x0003003eu,0x0000043eu,0x00000440u,
    0x0004003du,0x00000017u,0x00000442u,0x0000043bu,0x0003003eu,0x00000441u,0x00000442u,0x00090039u,
    0x00000002u,0x00000445u,0x00000032u,0x0000043cu,0x0000043eu,0x00000441u,0x00000443u,0x00000444u,
    0x0004003du,0x00000017u,0x00000446u,0x00000443u,0x0003003eu,0x00000433u,0x00000446u,0x0004003du,
    0x0000001fu,0x00000447u,0x00000444u,0x0003003eu,0x00000435u,0x00000447u,0x000200f9u,0x0000043au,
    0x000200f8u,0x0000043au,0x00050041u,0x0000039bu,0x0000044au,0x00000386u,0x0000039au,0x0004003du,
    0x00000038u,0x0000044bu,0x0000044au,0x000500abu,0x0000000fu,0x0000044cu,0x0000044bu,0x00000039u,
    0x000300f7u,0x0000044fu,0x00000000u,0x000400fau,0x0000044cu,0x0000044eu,0x00000451u,0x000200f8u,
    0x0000044eu,0x0004003du,0x00000008u,0x00000450u,0x00000390u,0x0003003eu,0x0000044du,0x00000450u,
    0x000200f9u,0x0000044fu,0x000200f8u,0x00000451u,0x00050041u,0x00000280u,0x00000454u,0x00000453u,
    0x00000050u,0x0004003du,0x00000008u,0x00000455u,0x00000454u,0x0003003eu,0x0000044du,0x00000455u,
    0x000200f9u,0x0000044fu,0x000200f8u,0x0000044fu,0x0004003du,0x00000008u,0x00000456u,0x0000044du,
    0x00050041u,0x00000280u,0x00000457u,0x00000453u,0x00000081u,0x0004003du,0x00000008u,0x00000458u,
    0x00000457u,0x00050050u,0x0000027au,0x00000459u,0x00000456u,0x00000458u,0x0003003eu,0x00000449u,
    0x00000459u,0x00050041u,0x0000039bu,0x0000045cu,0x00000386u,0x0000039au,0x0004003du,0x00000038u,
    0x0000045du,0x0000045cu,0x000500aau,0x0000000fu,0x0000045eu,0x0000045du,0x0000039eu,0x0004003du,
    0x00000006u,0x00000461u,0x00000460u,0x000600a9u,0x00000006u,0x00000462u,0x0000045eu,0x00000050u,
    0x00000461u,0x0003003eu,0x0000045bu,0x00000462u,0x000100fdu,0x00010038u,0x00050036u,0x00000008u,
    0x0000000cu,0x00000000u,0x00000009u,0x00030037u,0x00000007u,0x0000000au,0x00030037u,0x00000007u,
    0x0000000bu,0x000200f8u,0x0000000du,0x0004003du,0x00000006u,0x0000003au,0x0000000au,0x0004003du,
    0x00000006u,0x0000003bu,0x0000000bu,0x00050086u,0x00000006u,0x0000003du,0x0000003bu,0x0000003cu,
    0x00050080u,0x00000006u,0x0000003eu,0x0000003au,0x0000003du,0x0004003du,0x00000006u,0x0000003fu,
    0x0000000bu,0x00050089u,0x00000006u,0x00000040u,0x0000003fu,0x0000003cu,0x00070041u,0x00000041u,
    0x00000042u,0x00000037u,0x00000039u,0x0000003eu,0x00000040u,0x0004003du,0x00000008u,0x00000043u,
    0x00000042u,0x000200feu,0x00000043u,0x00010038u,0x00050036u,0x00000008u,0x00000015u,0x00000000u,
    0x00000011u,0x00030037u,0x00000007u,0x00000012u,0x00030037u,0x0000000eu,0x00000013u,0x00030037u,
    0x00000010u,0x00000014u,0x000200f8u,0x00000016u,0x0004003bu,0x0000000eu,0x00000046u,0x00000007u,
    0x0004003bu,0x00000007u,0x00000049u,0x00000007u,0x0004003bu,0x00000007u,0x0000004bu,0x00000007u,
    0x0004003bu,0x0000000eu,0x0000005fu,0x00000007u,0x0004003bu,0x00000007u,0x00000064u,0x00000007u,
    0x0004003bu,0x0000000eu,0x00000069u,0x00000007u,0x0004003bu,0x00000007u,0x0000006eu,0x00000007u,
    0x0004003bu,0x0000000eu,0x00000076u,0x00000007u,0x0004003bu,0x00000007u,0x0000007fu,0x00000007u,
    0x0004003bu,0x0000000eu,0x00000083u,0x00000007u,0x0004003bu,0x0000000eu,0x0000008cu,0x00000007u,
    0x0004003bu,0x0000000eu,0x00000096u,0x00000007u,0x0004003bu,0x00000007u,0x0000009au,0x00000007u,
    0x0004003bu,0x00000007u,0x0000009cu,0x00000007u,0x0004003bu,0x00000007u,0x000000a2u,0x00000007u,
    0x0004003bu,0x00000007u,0x000000a4u,0x00000007u,0x0004003du,0x00000008u,0x00000047u,0x00000013u,
    0x0004003du,0x00000006u,0x0000004au,0x00000012u,0x0003003eu,0x00000049u,0x0000004au,0x0003003eu,
    0x0000004bu,0x00000048u,0x00060039u,0x00000008u,0x0000004cu,0x0000000cu,0x00000049u,0x0000004bu,
    0x0007000cu,0x00000008u,0x0000004du,0x00000001u,0x00000028u,0x00000047u,0x0000004cu,0x0003003eu,
    0x00000046u,0x0000004du,0x0004003du,0x00000006u,0x0000004eu,0x00000012u,0x00050080u,0x00000006u,
    0x0000004fu,0x0000004eu,0x0000003cu,0x00070041u,0x00000041u,0x00000051u,0x00000037u,0x00000039u,
    0x0000004fu,0x00000050u,0x0004003du,0x00000008u,0x00000052u,0x00000051u,0x000500b7u,0x0000000fu,
    0x00000054u,0x00000052u,0x00000053u,0x0004003du,0x00000008u,0x00000055u,0x00000046u,0x000500beu,
    0x0000000fu,0x00000057u,0x00000055u,0x00000056u,0x000500a7u,0x0000000fu,0x00000058u,0x00000054u,
    0x00000057u,0x0004003du,0x00000008u,0x00000059u,0x00000046u,0x000500bcu,0x0000000fu,0x0000005bu,
    0x00000059u,0x0000005au,0x000500a7u,0x0000000fu,0x0000005cu,0x00000058u,0x0000005bu,0x000300f7u,
    0x0000005eu,0x00000000u,0x000400fau,0x0000005cu,0x0000005du,0x0000005eu,0x000200f8u,0x0000005du,
    0x0004003du,0x00000008u,0x00000060u,0x00000046u,0x00050083u,0x00000008u,0x00000061u,0x00000060u,
    0x00000056u,0x00050085u,0x00000008u,0x00000063u,0x00000061u,0x00000062u,0x0003003eu,0x0000005fu,
    0x00000063u,0x0004003du,0x00000008u,0x00000065u,0x0000005fu,0x0004006du,0x00000006u,0x00000066u,
    0x00000065u,0x0007000cu,0x00000006u,0x00000068u,0x00000001u,0x00000026u,0x00000066u,0x00000067u,
    0x0003003eu,0x00000064u,0x00000068u,0x0004003du,0x00000008u,0x0000006au,0x0000005fu,0x0004003du,
    0x00000006u,0x0000006bu,0x00000064u,0x00040070u,0x00000008u,0x0000006cu,0x0000006bu,0x00050083u,
    0x00000008u,0x0000006du,0x0000006au,0x0000006cu,0x0003003eu,0x00000069u,0x0000006du,0x0004003du,
    0x00000006u,0x0000006fu,0x00000012u,0x00050080u,0x00000006u,0x00000071u,0x0000006fu,0x00000070u,
    0x0004003du,0x0000000fu,0x00000072u,0x00000014u,0x000600a9u,0x00000006u,0x00000074u,0x00000072u,
    0x00000073u,0x00000050u,0x00050080u,0x00000006u,0x00000075u,0x00000071u,0x00000074u,0x0003003eu,
    0x0000006eu,0x00000075u,0x0004003du,0x00000006u,0x00000077u,0x0000006eu,0x0004003du,0x00000006u,
    0x00000078u,0x00000064u,0x00050086u,0x00000006u,0x00000079u,0x00000078u,0x0000003cu,0x00050080u,
    0x00000006u,0x0000007au,0x00000077u,0x00000079u,0x0004003du,0x00000006u,0x0000007bu,0x00000064u,
    0x00050089u,0x00000006u,0x0000007cu,0x0000007bu,0x0000003cu,0x00070041u,0x00000041u,0x0000007du,
    0x00000037u,0x00000039u,0x0000007au,0x0000007cu,0x0004003du,0x00000008u,0x0000007eu,0x0000007du,
    0x0003003eu,0x00000076u,0x0000007eu,0x0004003du,0x00000006u,0x00000080u,0x00000064u,0x00050080u,
    0x00000006u,0x00000082u,0x00000080u,0x00000081u,0x0003003eu,0x0000007fu,0x00000082u,0x0004003du,
    0x00000006u,0x00000084u,0x0000006eu,0x0004003du,0x00000006u,0x00000085u,0x0000007fu,0x00050086u,
    0x00000006u,0x00000086u,0x00000085u,0x0000003cu,0x00050080u,0x00000006u,0x00000087u,0x00000084u,
    0x00000086u,0x0004003du,0x00000006u,0x00000088u,0x0000007fu,0x00050089u,0x00000006u,0x00000089u,
    0x00000088u,0x0000003cu,0x00070041u,0x00000041u,0x0000008au,0x00000037u,0x00000039u,0x00000087u,
    0x00000089u,0x0004003du,0x00000008u,0x0000008bu,0x0000008au,0x0003003eu,0x00000083u,0x0000008bu,
    0x0004003du,0x00000008u,0x0000008du,0x00000076u,0x0004003du,0x00000008u,0x0000008eu,0x00000069u,
    0x0004003du,0x00000008u,0x0000008fu,0x00000083u,0x0004003du,0x00000008u,0x00000090u,0x00000076u,
    0x00050083u,0x00000008u,0x00000091u,0x0000008fu,0x00000090u,0x00050085u,0x00000008u,0x00000092u,
    0x0000008eu,0x00000091u,0x00050081u,0x00000008u,0x00000093u,0x0000008du,0x00000092u,0x0003003eu,
    0x0000008cu,0x00000093u,0x0004003du,0x00000008u,0x00000094u,0x0000008cu,0x000200feu,0x00000094u,
    0x000200f8u,0x0000005eu,0x0004003du,0x0000000fu,0x00000097u,0x00000014u,0x000600a9u,0x00000006u,
    0x00000099u,0x00000097u,0x00000098u,0x00000081u,0x0004003du,0x00000006u,0x0000009bu,0x00000012u,
    0x0003003eu,0x0000009au,0x0000009bu,0x0003003eu,0x0000009cu,0x00000099u,0x00060039u,0x00000008u,
    0x0000009du,0x0000000cu,0x0000009au,0x0000009cu,0x0004003du,0x00000008u,0x0000009eu,0x00000046u,
    0x0004003du,0x0000000fu,0x0000009fu,0x00000014u,0x000600a9u,0x00000006u,0x000000a1u,0x0000009fu,
    0x000000a0u,0x00000050u,0x0004003du,0x00000006u,0x000000a3u,0x00000012u,0x0003003eu,0x000000a2u,
    0x000000a3u,0x0003003eu,0x000000a4u,0x000000a1u,0x00060039u,0x00000008u,0x000000a5u,0x0000000cu,
    0x000000a2u,0x000000a4u,0x0007000cu,0x00000008u,0x000000a6u,0x00000001u,0x0000001au,0x0000009eu,
    0x000000a5u,0x00050085u,0x00000008u,0x000000a7u,0x0000009du,0x000000a6u,0x0003003eu,0x00000096u,
    0x000000a7u,0x0004003du,0x00000008u,0x000000a8u,0x00000096u,0x000200feu,0x000000a8u,0x00010038u,
    0x00050036u,0x00000017u,0x0000001du,0x00000000u,0x00000019u,0x00030037u,0x00000007u,0x0000001au,
    0x00030037u,0x00000018u,0x0000001bu,0x00030037u,0x00000018u,0x0000001cu,0x000200f8u,0x0000001eu,
    0x0004003bu,0x0000000eu,0x000000abu,0x00000007u,0x0004003bu,0x00000007u,0x000000acu,0x00000007u,
    0x0004003bu,0x00000018u,0x000000b5u,0x00000007u,0x0004003bu,0x0000000eu,0x000000d6u,0x00000007u,
    0x0004003bu,0x00000007u,0x000000d7u,0x00000007u,0x0004003bu,0x00000018u,0x000000dfu,0x00000007u,
    0x0004003bu,0x0000000eu,0x000000f6u,0x00000007u,0x0004003bu,0x0000000eu,0x000000f9u,0x00000007u,
    0x0004003bu,0x00000007u,0x00000101u,0x00000007u,0x0003003eu,0x000000abu,0x00000053u,0x0003003eu,
    0x000000acu,0x00000050u,0x000200f9u,0x000000adu,0x000200f8u,0x000000adu,0x000400f6u,0x000000afu,
    0x000000b0u,0x00000000u,0x000200f9u,0x000000b1u,0x000200f8u,0x000000b1u,0x0004003du,0x00000006u,
    0x000000b2u,0x000000acu,0x000500b0u,0x0000000fu,0x000000b4u,0x000000b2u,0x000000b3u,0x000400fau,
    0x000000b4u,0x000000aeu,0x000000afu,0x000200f8u,0x000000aeu,0x0004003du,0x00000006u,0x000000b6u,
    0x000000acu,0x0004003du,0x00000006u,0x000000b7u,0x000000acu,0x00050041u,0x0000000eu,0x000000b8u,
    0x0000001bu,0x000000b7u,0x0004003du,0x00000008u,0x000000b9u,0x000000b8u,0x0004003du,0x00000006u,
    0x000000bau,0x0000001au,0x00050080u,0x00000006u,0x000000bbu,0x000000bau,0x000000b3u,0x0004003du,
    0x00000006u,0x000000bcu,0x000000acu,0x00070041u,0x00000041u,0x000000bdu,0x00000037u,0x00000039u,
    0x000000bbu,0x000000bcu,0x0004003du,0x00000008u,0x000000beu,0x000000bdu,0x00050083u,0x00000008u,
    0x000000bfu,0x000000b9u,0x000000beu,0x00050041u,0x0000000eu,0x000000c0u,0x000000b5u,0x000000b6u,
    0x0003003eu,0x000000c0u,0x000000bfu,0x0004003du,0x00000008u,0x000000c1u,0x000000abu,0x0004003du,
    0x00000006u,0x000000c2u,0x000000acu,0x00050041u,0x0000000eu,0x000000c3u,0x000000b5u,0x000000c2u,
    0x0004003du,0x00000008u,0x000000c4u,0x000000c3u,0x0004003du,0x00000006u,0x000000c5u,0x000000acu,
    0x00050041u,0x0000000eu,0x000000c6u,0x0000001cu,0x000000c5u,0x0004003du,0x00000008u,0x000000c7u,
    0x000000c6u,0x00050085u,0x00000008u,0x000000c8u,0x000000c4u,0x000000c7u,0x00050081u,0x00000008u,
    0x000000c9u,0x000000c1u,0x000000c8u,0x0003003eu,0x000000abu,0x000000c9u,0x0004003du,0x00000008u,
    0x000000cau,0x000000abu,0x0004003du,0x00000006u,0x000000cbu,0x000000acu,0x00050041u,0x0000000eu,
    0x000000ccu,0x000000b5u,0x000000cbu,0x0004003du,0x00000008u,0x000000cdu,0x000000ccu,0x0004003du,
    0x00000006u,0x000000ceu,0x000000acu,0x00050041u,0x0000000eu,0x000000cfu,0x0000001cu,0x000000ceu,
    0x0004003du,0x00000008u,0x000000d0u,0x000000cfu,0x00050085u,0x00000008u,0x000000d1u,0x000000cdu,
    0x000000d0u,0x00050081u,0x00000008u,0x000000d2u,0x000000cau,0x000000d1u,0x0003003eu,0x000000abu,
    0x000000d2u,0x000200f9u,0x000000b0u,0x000200f8u,0x000000b0u,0x0004003du,0x00000006u,0x000000d3u,
    0x000000acu,0x00050080u,0x00000006u,0x000000d5u,0x000000d3u,0x000000d4u,0x0003003eu,0x000000acu,
    0x000000d5u,0x000200f9u,0x000000adu,0x000200f8u,0x000000afu,0x0003003eu,0x000000d6u,0x00000053u,
    0x0003003eu,0x000000d7u,0x00000050u,0x000200f9u,0x000000d8u,0x000200f8u,0x000000d8u,0x000400f6u,
    0x000000dau,0x000000dbu,0x00000000u,0x000200f9u,0x000000dcu,0x000200f8u,0x000000dcu,0x0004003du,
    0x00000006u,0x000000ddu,0x000000d7u,0x000500b0u,0x0000000fu,0x000000deu,0x000000ddu,0x000000b3u,
    0x000400fau,0x000000deu,0x000000d9u,0x000000dau,0x000200f8u,0x000000d9u,0x0004003du,0x00000006u,
    0x000000e0u,0x000000d7u,0x0004003du,0x00000006u,0x000000e1u,0x000000d7u,0x00050041u,0x0000000eu,
    0x000000e2u,0x000000b5u,0x000000e1u,0x0004003du,0x00000008u,0x000000e3u,0x000000e2u,0x0004003du,
    0x00000006u,0x000000e4u,0x000000d7u,0x00050041u,0x0000000eu,0x000000e5u,0x0000001cu,0x000000e4u,
    0x0004003du,0x00000008u,0x000000e6u,0x000000e5u,0x0004003du,0x00000008u,0x000000e7u,0x000000abu,
    0x00050085u,0x00000008u,0x000000e8u,0x000000e6u,0x000000e7u,0x00050083u,0x00000008u,0x000000e9u,
    0x000000e3u,0x000000e8u,0x00050041u,0x0000000eu,0x000000eau,0x000000dfu,0x000000e0u,0x0003003eu,
    0x000000eau,0x000000e9u,0x0004003du,0x00000008u,0x000000ebu,0x000000d6u,0x0004003du,0x00000006u,
    0x000000ecu,0x000000d7u,0x00050041u,0x0000000eu,0x000000edu,0x000000dfu,0x000000ecu,0x0004003du,
    0x00000008u,0x000000eeu,0x000000edu,0x0004003du,0x00000006u,0x000000efu,0x000000d7u,0x00050041u,
    0x0000000eu,0x000000f0u,0x000000dfu,0x000000efu,0x0004003du,0x00000008u,0x000000f1u,0x000000f0u,
    0x00050085u,0x00000008u,0x000000f2u,0x000000eeu,0x000000f1u,0x00050081u,0x00000008u,0x000000f3u,
    0x000000ebu,0x000000f2u,0x0003003eu,0x000000d6u,0x000000f3u,0x000200f9u,0x000000dbu,0x000200f8u,
    0x000000dbu,0x0004003du,0x00000006u,0x000000f4u,0x000000d7u,0x00050080u,0x00000006u,0x000000f5u,
    0x000000f4u,0x000000d4u,0x0003003eu,0x000000d7u,0x000000f5u,0x000200f9u,0x000000d8u,0x000200f8u,
    0x000000dau,0x0004003du,0x00000008u,0x000000f7u,0x000000d6u,0x000500bau,0x0000000fu,0x000000f8u,
    0x000000f7u,0x00000053u,0x000300f7u,0x000000fbu,0x00000000u,0x000400fau,0x000000f8u,0x000000fau,
    0x000000ffu,0x000200f8u,0x000000fau,0x0004003du,0x00000008u,0x000000fcu,0x000000d6u,0x0006000cu,
    0x00000008u,0x000000fdu,0x00000001u,0x0000001fu,0x000000fcu,0x00050088u,0x00000008u,0x000000feu,
    0x0000005au,0x000000fdu,0x0003003eu,0x000000f9u,0x000000feu,0x000200f9u,0x000000fbu,0x000200f8u,
    0x000000ffu,0x0003003eu,0x000000f9u,0x00000053u,0x000200f9u,0x000000fbu,0x000200f8u,0x000000fbu,
    0x0004003du,0x00000008u,0x00000100u,0x000000f9u,0x0003003eu,0x000000f6u,0x00000100u,0x0003003eu,
    0x00000101u,0x00000050u,0x000200f9u,0x00000102u,0x000200f8u,0x00000102u,0x000400f6u,0x00000104u,
    0x00000105u,0x00000000u,0x000200f9u,0x00000106u,0x000200f8u,0x00000106u,0x0004003du,0x00000006u,
    0x00000107u,0x00000101u,0x000500b0u,0x0000000fu,0x00000108u,0x00000107u,0x000000b3u,0x000400fau,
    0x00000108u,0x00000103u,0x00000104u,0x000200f8u,0x00000103u,0x0004003du,0x00000006u,0x00000109u,
    0x00000101u,0x0004003du,0x00000006u,0x0000010au,0x00000101u,0x00050041u,0x0000000eu,0x0000010bu,
    0x000000dfu,0x0000010au,0x0004003du,0x00000008u,0x0000010cu,0x0000010bu,0x0004003du,0x00000008u,
    0x0000010du,0x000000f6u,0x00050085u,0x00000008u,0x0000010eu,0x0000010cu,0x0000010du,0x00050041u,
    0x0000000eu,0x0000010fu,0x000000dfu,0x00000109u,0x0003003eu,0x0000010fu,0x0000010eu,0x000200f9u,
    0x00000105u,0x000200f8u,0x00000105u,0x0004003du,0x00000006u,0x00000110u,0x00000101u,0x00050080u,
    0x00000006u,0x00000111u,0x00000110u,0x000000d4u,0x0003003eu,0x00000101u,0x00000111u,0x000200f9u,
    0x00000102u,0x000200f8u,0x00000104u,0x0004003du,0x00000017u,0x00000112u,0x000000dfu,0x000200feu,
    0x00000112u,0x00010038u,0x00050036u,0x0000001fu,0x00000029u,0x00000000u,0x00000020u,0x00030037u,
    0x00000007u,0x00000021u,0x00030037u,0x00000007u,0x00000022u,0x00030037u,0x00000007u,0x00000023u,
    0x00030037u,0x00000018u,0x00000024u,0x00030037u,0x00000018u,0x00000025u,0x00030037u,0x00000018u,
    0x00000026u,0x00030037u,0x00000018u,0x00000027u,0x00030037u,0x00000010u,0x00000028u,0x000200f8u,
    0x0000002au,0x0004003bu,0x00000010u,0x00000115u,0x00000007u,0x0004003bu,0x00000010u,0x00000119u,
    0x00000007u,0x0004003bu,0x00000007u,0x0000011bu,0x00000007u,0x0004003bu,0x00000007u,0x0000011du,
    0x00000007u,0x0004003bu,0x00000007u,0x00000124u,0x00000007u,0x0004003bu,0x00000007u,0x00000126u,
    0x00000007u,0x0004003bu,0x00000007u,0x0000012eu,0x00000007u,0x0004003bu,0x00000007u,0x00000130u,
    0x00000007u,0x0004003bu,0x00000010u,0x00000134u,0x00000007u,0x0004003bu,0x00000007u,0x00000143u,
    0x00000007u,0x0004003bu,0x00000007u,0x00000145u,0x00000007u,0x0004003bu,0x00000010u,0x0000014bu,
    0x00000007u,0x0004003bu,0x00000007u,0x00000152u,0x00000007u,0x0004003bu,0x00000007u,0x00000154u,
    0x00000007u,0x0004003bu,0x00000018u,0x00000158u,0x00000007u,0x0004003bu,0x00000018u,0x00000160u,
    0x00000007u,0x0004003bu,0x00000007u,0x00000165u,0x00000007u,0x0004003bu,0x00000018u,0x00000167u,
    0x00000007u,0x0004003bu,0x00000018u,0x00000169u,0x00000007u,0x0004003bu,0x0000002bu,0x0000016du,
    0x00000007u,0x0004003bu,0x00000018u,0x00000178u,0x00000007u,0x0004003bu,0x00000018u,0x00000179u,
    0x00000007u,0x0004003bu,0x00000007u,0x0000017au,0x00000007u,0x0004003bu,0x0000000eu,0x00000182u,
    0x00000007u,0x0004003bu,0x00000007u,0x00000183u,0x00000007u,0x0004003bu,0x0000000eu,0x000001a1u,
    0x00000007u,0x0004003bu,0x00000007u,0x000001a2u,0x00000007u,0x0004003bu,0x00000007u,0x000001bau,
    0x00000007u,0x0004003bu,0x0000000eu,0x000001bcu,0x00000007u,0x0004003bu,0x00000010u,0x000001beu,
    0x00000007u,0x0004003bu,0x00000007u,0x000001c4u,0x00000007u,0x0004003bu,0x0000000eu,0x000001ccu,
    0x00000007u,0x0004003bu,0x00000007u,0x000001d2u,0x00000007u,0x0004003bu,0x00000007u,0x000001e9u,
    0x00000007u,0x0004003bu,0x0000000eu,0x00000205u,0x00000007u,0x0004003bu,0x00000007u,0x0000020fu,
    0x00000007u,0x0004003bu,0x00000007u,0x00000211u,0x00000007u,0x0004003bu,0x0000000eu,0x00000224u,
    0x00000007u,0x0004003bu,0x00000007u,0x00000228u,0x00000007u,0x0004003bu,0x0000000eu,0x0000023eu,
    0x00000007u,0x0004003bu,0x0000000eu,0x00000240u,0x00000007u,0x0004003bu,0x00000007u,0x00000244u,
    0x00000007u,0x0004003bu,0x0000000eu,0x00000246u,0x00000007u,0x0004003bu,0x00000010u,0x00000248u,
    0x00000007u,0x0004003bu,0x0000000eu,0x0000024cu,0x00000007u,0x0004003bu,0x00000007u,0x0000024eu,
    0x00000007u,0x0004003bu,0x00000007u,0x00000250u,0x00000007u,0x0004003bu,0x00000007u,0x00000252u,
    0x00000007u,0x0004003bu,0x00000007u,0x00000254u,0x00000007u,0x0004003du,0x00000006u,0x00000116u,
    0x00000023u,0x000500c7u,0x00000006u,0x00000117u,0x00000116u,0x0000003cu,0x000500abu,0x0000000fu,
    0x00000118u,0x00000117u,0x00000050u,0x0003003eu,0x00000115u,0x00000118u,0x0004003du,0x00000006u,
    0x0000011cu,0x00000022u,0x0003003eu,0x0000011bu,0x0000011cu,0x0003003eu,0x0000011du,0x0000011au,
    0x00060039u,0x00000008u,0x0000011eu,0x0000000cu,0x0000011bu,0x0000011du,0x000500b7u,0x0000000fu,
    0x0000011fu,0x0000011eu,0x00000053u,0x000400a8u,0x0000000fu,0x00000120u,0x0000011fu,0x000300f7u,
    0x00000122u,0x00000000u,0x000400fau,0x00000120u,0x00000121u,0x00000122u,0x000200f8u,0x00000121u,
    0x0004003du,0x00000006u,0x00000125u,0x00000022u,0x0003003eu,0x00000124u,0x00000125u,0x0003003eu,
    0x00000126u,0x00000123u,0x00060039u,0x00000008u,0x00000127u,0x0000000cu,0x00000124u,0x00000126u,
    0x000500b7u,0x0000000fu,0x00000128u,0x00000127u,0x00000053u,0x000200f9u,0x00000122u,0x000200f8u,
    0x00000122u,0x000700f5u,0x0000000fu,0x00000129u,0x0000011fu,0x0000002au,0x00000128u,0x00000121u,
    0x000400a8u,0x0000000fu,0x0000012au,0x00000129u,0x000300f7u,0x0000012cu,0x00000000u,0x000400fau,
    0x0000012au,0x0000012bu,0x0000012cu,0x000200f8u,0x0000012bu,0x0004003du,0x00000006u,0x0000012fu,
    0x00000022u,0x0003003eu,0x0000012eu,0x0000012fu,0x0003003eu,0x00000130u,0x0000012du,0x00060039u,
    0x00000008u,0x00000131u,0x0000000cu,0x0000012eu,0x00000130u,0x000500b7u,0x0000000fu,0x00000132u,
    0x00000131u,0x00000053u,0x000200f9u,0x0000012cu,0x000200f8u,0x0000012cu,0x000700f5u,0x0000000fu,
    0x00000133u,0x00000129u,0x00000122u,0x00000132u,0x0000012bu,0x0003003eu,0x00000119u,0x00000133u,
    0x0004003du,0x00000006u,0x00000135u,0x00000023u,0x000500c7u,0x00000006u,0x00000137u,0x00000135u,
    0x00000136u,0x000500abu,0x0000000fu,0x00000138u,0x00000137u,0x00000050u,0x000300f7u,0x0000013au,
    0x00000000u,0x000400fau,0x00000138u,0x00000139u,0x0000013au,0x000200f8u,0x00000139u,0x0004003du,
    0x0000000fu,0x0000013bu,0x00000119u,0x000400a8u,0x0000000fu,0x0000013cu,0x0000013bu,0x000300f7u,
    0x0000013eu,0x00000000u,0x000400fau,0x0000013cu,0x0000013du,0x0000013eu,0x000200f8u,0x0000013du,
    0x0004003du,0x0000000fu,0x0000013fu,0x00000115u,0x000300f7u,0x00000141u,0x00000000u,0x000400fau,
    0x0000013fu,0x00000140u,0x00000141u,0x000200f8u,0x00000140u,0x0004003du,0x00000006u,0x00000144u,
    0x00000022u,0x0003003eu,0x00000143u,0x00000144u,0x0003003eu,0x00000145u,0x00000142u,0x00060039u,
    0x00000008u,0x00000146u,0x0000000cu,0x00000143u,0x00000145u,0x000500b7u,0x0000000fu,0x00000147u,
    0x00000146u,0x00000053u,0x000200f9u,0x00000141u,0x000200f8u,0x00000141u,0x000700f5u,0x0000000fu,
    0x00000148u,0x0000013fu,0x0000013du,0x00000147u,0x00000140u,0x000200f9u,0x0000013eu,0x000200f8u,
    0x0000013eu,0x000700f5u,0x0000000fu,0x00000149u,0x0000013bu,0x00000139u,0x00000148u,0x00000141u,
    0x000200f9u,0x0000013au,0x000200f8u,0x0000013au,0x000700f5u,0x0000000fu,0x0000014au,0x00000138u,
    0x0000012cu,0x00000149u,0x0000013eu,0x0003003eu,0x00000134u,0x0000014au,0x0004003du,0x00000006u,
    0x0000014cu,0x00000023u,0x000500c7u,0x00000006u,0x0000014eu,0x0000014cu,0x0000014du,0x000500abu,
    0x0000000fu,0x0000014fu,0x0000014eu,0x00000050u,0x000300f7u,0x00000151u,0x00000000u,0x000400fau,
    0x0000014fu,0x00000150u,0x00000151u,0x000200f8u,0x00000150u,0x0004003du,0x00000006u,0x00000153u,
    0x00000022u,0x0003003eu,0x00000152u,0x00000153u,0x0003003eu,0x00000154u,0x00000142u,0x00060039u,
    0x00000008u,0x00000155u,0x0000000cu,0x00000152u,0x00000154u,0x000500b7u,0x0000000fu,0x00000156u,
    0x00000155u,0x00000053u,0x000200f9u,0x00000151u,0x000200f8u,0x00000151u,0x000700f5u,0x0000000fu,
    0x00000157u,0x0000014fu,0x0000013au,0x00000156u,0x00000150u,0x0003003eu,0x0000014bu,0x00000157u,
    0x0003003eu,0x00000158u,0x00000159u,0x0004003du,0x0000000fu,0x0000015au,0x00000134u,0x0004003du,
    0x0000000fu,0x0000015bu,0x0000014bu,0x000500a6u,0x0000000fu,0x0000015cu,0x0000015au,0x0000015bu,
    0x000300f7u,0x0000015eu,0x00000000u,0x000400fau,0x0000015cu,0x0000015du,0x0000015eu,0x000200f8u,
    0x0000015du,0x0004003du,0x0000000fu,0x0000015fu,0x00000028u,0x000300f7u,0x00000162u,0x00000000u,
    0x000400fau,0x0000015fu,0x00000161u,0x00000164u,0x000200f8u,0x00000161u,0x0004003du,0x00000017u,
    0x00000163u,0x00000027u,0x0003003eu,0x00000160u,0x00000163u,0x000200f9u,0x00000162u,0x000200f8u,
    0x00000164u,0x0004003du,0x00000006u,0x00000166u,0x00000021u,0x0003003eu,0x00000165u,0x00000166u,
    0x0004003du,0x00000017u,0x00000168u,0x00000024u,0x0003003eu,0x00000167u,0x00000168u,0x0004003du,
    0x00000017u,0x0000016au,0x00000025u,0x0003003eu,0x00000169u,0x0000016au,0x00070039u,0x00000017u,
    0x0000016bu,0x0000001du,0x00000165u,0x00000167u,0x00000169u,0x0003003eu,0x00000160u,0x0000016bu,
    0x000200f9u,0x00000162u,0x000200f8u,0x00000162u,0x0004003du,0x00000017u,0x0000016cu,0x00000160u,
    0x0003003eu,0x00000158u,0x0000016cu,0x000200f9u,0x0000015eu,0x000200f8u,0x0000015eu,0x0004003du,
    0x00000017u,0x0000016eu,0x00000026u,0x00050051u,0x00000008u,0x0000016fu,0x0000016eu,0x00000000u,
    0x00050051u,0x00000008u,0x00000170u,0x0000016eu,0x00000001u,0x00050051u,0x00000008u,0x00000171u,
    0x0000016eu,0x00000002u,0x00070050u,0x0000001fu,0x00000172u,0x0000016fu,0x00000170u,0x00000171u,
    0x0000005au,0x0003003eu,0x0000016du,0x00000172u,0x0004003du,0x00000006u,0x00000173u,0x00000023u,
    0x000500c7u,0x00000006u,0x00000174u,0x00000173u,0x00000136u,0x000500abu,0x0000000fu,0x00000175u,
    0x00000174u,0x00000050u,0x000300f7u,0x00000177u,0x00000000u,0x000400fau,0x00000175u,0x00000176u,
    0x00000177u,0x000200f8u,0x00000176u,0x0003003eu,0x00000178u,0x00000159u,0x0003003eu,0x00000179u,
    0x00000159u,0x0003003eu,0x0000017au,0x00000050u,0x000200f9u,0x0000017bu,0x000200f8u,0x0000017bu,
    0x000400f6u,0x0000017du,0x0000017eu,0x00000000u,0x000200f9u,0x0000017fu,0x000200f8u,0x0000017fu,
    0x0004003du,0x00000006u,0x00000180u,0x0000017au,0x000500b0u,0x0000000fu,0x00000181u,0x00000180u,
    0x000000b3u,0x000400fau,0x00000181u,0x0000017cu,0x0000017du,0x000200f8u,0x0000017cu,0x0003003eu,
    0x00000182u,0x00000053u,0x0003003eu,0x00000183u,0x00000050u,0x000200f9u,0x00000184u,0x000200f8u,
    0x00000184u,0x000400f6u,0x00000186u,0x00000187u,0x00000000u,0x000200f9u,0x00000188u,0x000200f8u,
    0x00000188u,0x0004003du,0x00000006u,0x00000189u,0x00000183u,0x000500b0u,0x0000000fu,0x0000018au,
    0x00000189u,0x000000b3u,0x000400fau,0x0000018au,0x00000185u,0x00000186u,0x000200f8u,0x00000185u,
    0x0004003du,0x00000008u,0x0000018bu,0x00000182u,0x0004003du,0x00000006u,0x0000018cu,0x00000021u,
    0x00050080u,0x00000006u,0x0000018du,0x0000018cu,0x0000003cu,0x0004003du,0x00000006u,0x0000018eu,
    0x00000183u,0x00050080u,0x00000006u,0x0000018fu,0x0000018du,0x0000018eu,0x0004003du,0x00000006u,
    0x00000190u,0x0000017au,0x00070041u,0x00000041u,0x00000191u,0x00000037u,0x00000039u,0x0000018fu,
    0x00000190u,0x0004003du,0x00000008u,0x00000192u,0x00000191u,0x0004003du,0x00000006u,0x00000193u,
    0x00000183u,0x00050041u,0x0000000eu,0x00000194u,0x00000025u,0x00000193u,0x0004003du,0x00000008u,
    0x00000195u,0x00000194u,0x00050085u,0x00000008u,0x00000196u,0x00000192u,0x00000195u,0x00050081u,
    0x00000008u,0x00000197u,0x0000018bu,0x00000196u,0x0003003eu,0x00000182u,0x00000197u,0x000200f9u,
    0x00000187u,0x000200f8u,0x00000187u,0x0004003du,0x00000006u,0x00000198u,0x00000183u,0x00050080u,
    0x00000006u,0x00000199u,0x00000198u,0x000000d4u,0x0003003eu,0x00000183u,0x00000199u,0x000200f9u,
    0x00000184u,0x000200f8u,0x00000186u,0x0004003du,0x00000006u,0x0000019au,0x0000017au,0x0004003du,
    0x00000008u,0x0000019bu,0x00000182u,0x0007000cu,0x00000008u,0x0000019cu,0x00000001u,0x00000028u,
    0x0000019bu,0x00000053u,0x00050041u,0x0000000eu,0x0000019du,0x00000178u,0x0000019au,0x0003003eu,
    0x0000019du,0x0000019cu,0x0004003du,0x0000000fu,0x0000019eu,0x00000134u,0x000300f7u,0x000001a0u,
    0x00000000u,0x000400fau,0x0000019eu,0x0000019fu,0x000001a0u,0x000200f8u,0x0000019fu,0x0003003eu,
    0x000001a1u,0x00000053u,0x0003003eu,0x000001a2u,0x00000050u,0x000200f9u,0x000001a3u,0x000200f8u,
    0x000001a3u,0x000400f6u,0x000001a5u,0x000001a6u,0x00000000u,0x000200f9u,0x000001a7u,0x000200f8u,
    0x000001a7u,0x0004003du,0x00000006u,0x000001a8u,0x000001a2u,0x000500b0u,0x0000000fu,0x000001a9u,
    0x000001a8u,0x000000b3u,0x000400fau,0x000001a9u,0x000001a4u,0x000001a5u,0x000200f8u,0x000001a4u,
    0x0004003du,0x00000008u,0x000001aau,0x000001a1u,0x0004003du,0x00000006u,0x000001abu,0x00000021u,
    0x00050080u,0x00000006u,0x000001acu,0x000001abu,0x0000003cu,0x0004003du,0x00000006u,0x000001adu,
    0x000001a2u,0x00050080u,0x00000006u,0x000001aeu,0x000001acu,0x000001adu,0x0004003du,0x00000006u,
    0x000001afu,0x0000017au,0x00070041u,0x00000041u,0x000001b0u,0x00000037u,0x00000039u,0x000001aeu,
    0x000001afu,0x0004003du,0x00000008u,0x000001b1u,0x000001b0u,0x0004003du,0x00000006u,0x000001b2u,
    0x000001a2u,0x00050041u,0x0000000eu,0x000001b3u,0x00000158u,0x000001b2u,0x0004003du,0x00000008u,
    0x000001b4u,0x000001b3u,0x00050085u,0x00000008u,0x000001b5u,0x000001b1u,0x000001b4u,0x00050081u,
    0x00000008u,0x000001b6u,0x000001aau,0x000001b5u,0x0003003eu,0x000001a1u,0x000001b6u,0x000200f9u,
    0x000001a6u,0x000200f8u,0x000001a6u,0x0004003du,0x00000006u,0x000001b7u,0x000001a2u,0x00050080u,
    0x00000006u,0x000001b8u,0x000001b7u,0x000000d4u,0x0003003eu,0x000001a2u,0x000001b8u,0x000200f9u,
    0x000001a3u,0x000200f8u,0x000001a5u,0x0004003du,0x00000006u,0x000001b9u,0x0000017au,0x0004003du,
    0x00000006u,0x000001bbu,0x00000022u,0x0003003eu,0x000001bau,0x000001bbu,0x0004003du,0x00000008u,
    0x000001bdu,0x000001a1u,0x0003003eu,0x000001bcu,0x000001bdu,0x0004003du,0x0000000fu,0x000001bfu,
    0x00000115u,0x0003003eu,0x000001beu,0x000001bfu,0x00070039u,0x00000008u,0x000001c0u,0x00000015u,
    0x000001bau,0x000001bcu,0x000001beu,0x00050041u,0x0000000eu,0x000001c1u,0x00000179u,0x000001b9u,
    0x0003003eu,0x000001c1u,0x000001c0u,0x000200f9u,0x000001a0u,0x000200f8u,0x000001a0u,0x000200f9u,
    0x0000017eu,0x000200f8u,0x0000017eu,0x0004003du,0x00000006u,0x000001c2u,0x0000017au,0x00050080u,
    0x00000006u,0x000001c3u,0x000001c2u,0x000000d4u,0x0003003eu,0x0000017au,0x000001c3u,0x000200f9u,
    0x0000017bu,0x000200f8u,0x0000017du,0x0003003eu,0x000001c4u,0x00000050u,0x000200f9u,0x000001c5u,
    0x000200f8u,0x000001c5u,0x000400f6u,0x000001c7u,0x000001c8u,0x00000000u,0x000200f9u,0x000001c9u,
    0x000200f8u,0x000001c9u,0x0004003du,0x00000006u,0x000001cau,0x000001c4u,0x000500b0u,0x0000000fu,
    0x000001cbu,0x000001cau,0x0000003cu,0x000400fau,0x000001cbu,0x000001c6u,0x000001c7u,0x000200f8u,
    0x000001c6u,0x0004003du,0x00000006u,0x000001cdu,0x00000021u,0x00050080u,0x00000006u,0x000001ceu,
    0x000001cdu,0x000000a0u,0x0004003du,0x00000006u,0x000001cfu,0x000001c4u,0x00070041u,0x00000041u,
    0x000001d0u,0x00000037u,0x00000039u,0x000001ceu,0x000001cfu,0x0004003du,0x00000008u,0x000001d1u,
    0x000001d0u,0x0003003eu,0x000001ccu,0x000001d1u,0x0003003eu,0x000001d2u,0x00000050u,0x000200f9u,
    0x000001d3u,0x000200f8u,0x000001d3u,0x000400f6u,0x000001d5u,0x000001d6u,0x00000000u,0x000200f9u,
    0x000001d7u,0x000200f8u,0x000001d7u,0x0004003du,0x00000006u,0x000001d8u,0x000001d2u,0x000500b0u,
    0x0000000fu,0x000001d9u,0x000001d8u,0x000000b3u,0x000400fau,0x000001d9u,0x000001d4u,0x000001d5u,
    0x000200f8u,0x000001d4u,0x0004003du,0x00000008u,0x000001dau,0x000001ccu,0x0004003du,0x00000006u,
    0x000001dbu,0x00000021u,0x00050080u,0x00000006u,0x000001dcu,0x000001dbu,0x00000098u,0x0004003du,
    0x00000006u,0x000001ddu,0x000001d2u,0x00050080u,0x00000006u,0x000001deu,0x000001dcu,0x000001ddu,
    0x0004003du,0x00000006u,0x000001dfu,0x000001c4u,0x00070041u,0x00000041u,0x000001e0u,0x00000037u,
    0x00000039u,0x000001deu,0x000001dfu,0x0004003du,0x00000008u,0x000001e1u,0x000001e0u,0x0004003du,
    0x00000006u,0x000001e2u,0x000001d2u,0x00050041u,0x0000000eu,0x000001e3u,0x00000178u,0x000001e2u,
    0x0004003du,0x00000008u,0x000001e4u,0x000001e3u,0x00050085u,0x00000008u,0x000001e5u,0x000001e1u,
    0x000001e4u,0x00050081u,0x00000008u,0x000001e6u,0x000001dau,0x000001e5u,0x0003003eu,0x000001ccu,
    0x000001e6u,0x000200f9u,0x000001d6u,0x000200f8u,0x000001d6u,0x0004003du,0x00000006u,0x000001e7u,
    0x000001d2u,0x00050080u,0x00000006u,0x000001e8u,0x000001e7u,0x000000d4u,0x0003003eu,0x000001d2u,
    0x000001e8u,0x000200f9u,0x000001d3u,0x000200f8u,0x000001d5u,0x0003003eu,0x000001e9u,0x00000050u,
    0x000200f9u,0x000001eau,0x000200f8u,0x000001eau,0x000400f6u,0x000001ecu,0x000001edu,0x00000000u,
    0x000200f9u,0x000001eeu,0x000200f8u,0x000001eeu,0x0004003du,0x00000006u,0x000001efu,0x000001e9u,
    0x000500b0u,0x0000000fu,0x000001f0u,0x000001efu,0x000000b3u,0x000400fau,0x000001f0u,0x000001ebu,
    0x000001ecu,0x000200f8u,0x000001ebu,0x0004003du,0x00000008u,0x000001f1u,0x000001ccu,0x0004003du,
    0x00000006u,0x000001f2u,0x00000021u,0x00050080u,0x00000006u,0x000001f4u,0x000001f2u,0x000001f3u,
    0x0004003du,0x00000006u,0x000001f5u,0x000001e9u,0x00050080u,0x00000006u,0x000001f6u,0x000001f4u,
    0x000001f5u,0x0004003du,0x00000006u,0x000001f7u,0x000001c4u,0x00070041u,0x00000041u,0x000001f8u,
    0x00000037u,0x00000039u,0x000001f6u,0x000001f7u,0x0004003du,0x00000008u,0x000001f9u,0x000001f8u,
    0x0004003du,0x00000006u,0x000001fau,0x000001e9u,0x00050041u,0x0000000eu,0x000001fbu,0x00000179u,
    0x000001fau,0x0004003du,0x00000008u,0x000001fcu,0x000001fbu,0x00050085u,0x00000008u,0x000001fdu,
    0x000001f9u,0x000001fcu,0x00050081u,0x00000008u,0x000001feu,0x000001f1u,0x000001fdu,0x0003003eu,
    0x000001ccu,0x000001feu,0x000200f9u,0x000001edu,0x000200f8u,0x000001edu,0x0004003du,0x00000006u,
    0x000001ffu,0x000001e9u,0x00050080u,0x00000006u,0x00000200u,0x000001ffu,0x000000d4u,0x0003003eu,
    0x000001e9u,0x00000200u,0x000200f9u,0x000001eau,0x000200f8u,0x000001ecu,0x0004003du,0x00000006u,
    0x00000201u,0x000001c4u,0x000500b0u,0x0000000fu,0x00000202u,0x00000201u,0x000000b3u,0x000300f7u,
    0x00000204u,0x00000000u,0x000400fau,0x00000202u,0x00000203u,0x00000204u,0x000200f8u,0x00000203u,
    0x0004003du,0x00000006u,0x00000206u,0x000001c4u,0x00050041u,0x0000000eu,0x00000207u,0x00000026u,
    0x00000206u,0x0004003du,0x00000008u,0x00000208u,0x00000207u,0x00050085u,0x00000008u,0x0000020au,
    0x00000208u,0x00000209u,0x0003003eu,0x00000205u,0x0000020au,0x0004003du,0x00000008u,0x0000020bu,
    0x000001ccu,0x0004003du,0x00000008u,0x0000020cu,0x00000205u,0x00050083u,0x00000008u,0x0000020eu,
    0x0000020cu,0x0000020du,0x0004003du,0x00000006u,0x00000210u,0x00000022u,0x0003003eu,0x0000020fu,
    0x00000210u,0x0003003eu,0x00000211u,0x000000b3u,0x00060039u,0x00000008u,0x00000212u,0x0000000cu,
    0x0000020fu,0x00000211u,0x00050085u,0x00000008u,0x00000213u,0x0000020eu,0x00000212u,0x00050081u,
    0x00000008u,0x00000214u,0x0000005au,0x00000213u,0x00050085u,0x00000008u,0x00000215u,0x0000020bu,
    0x00000214u,0x0003003eu,0x000001ccu,0x00000215u,0x000200f9u,0x00000204u,0x000200f8u,0x00000204u,
    0x0004003du,0x00000006u,0x00000216u,0x000001c4u,0x0004003du,0x00000008u,0x00000217u,0x000001ccu,
    0x0008000cu,0x00000008u,0x00000218u,0x00000001u,0x0000002bu,0x00000217u,0x00000053u,0x0000020du,
    0x0004006eu,0x00000038u,0x00000219u,0x00000218u,0x0004006fu,0x00000008u,0x0000021au,0x00000219u,
    0x00050088u,0x00000008u,0x0000021bu,0x0000021au,0x00000209u,0x00050041u,0x0000000eu,0x0000021cu,
    0x0000016du,0x00000216u,0x0003003eu,0x0000021cu,0x0000021bu,0x000200f9u,0x000001c8u,0x000200f8u,
    0x000001c8u,0x0004003du,0x00000006u,0x0000021du,0x000001c4u,0x00050080u,0x00000006u,0x0000021eu,
    0x0000021du,0x000000d4u,0x0003003eu,0x000001c4u,0x0000021eu,0x000200f9u,0x000001c5u,0x000200f8u,
    0x000001c7u,0x000200f9u,0x00000177u,0x000200f8u,0x00000177u,0x0004003du,0x00000006u,0x0000021fu,
    0x00000023u,0x000500c7u,0x00000006u,0x00000220u,0x0000021fu,0x0000014du,0x000500abu,0x0000000fu,
    0x00000221u,0x00000220u,0x00000050u,0x000300f7u,0x00000223u,0x00000000u,0x000400fau,0x00000221u,
    0x00000222u,0x00000223u,0x000200f8u,0x00000222u,0x0003003eu,0x00000224u,0x00000053u,0x0004003du,
    0x0000000fu,0x00000225u,0x0000014bu,0x000300f7u,0x00000227u,0x00000000u,0x000400fau,0x00000225u,
    0x00000226u,0x00000227u,0x000200f8u,0x00000226u,0x0003003eu,0x00000228u,0x00000050u,0x000200f9u,
    0x00000229u,0x000200f8u,0x00000229u,0x000400f6u,0x0000022bu,0x0000022cu,0x00000000u,0x000200f9u,
    0x0000022du,0x000200f8u,0x0000022du,0x0004003du,0x00000006u,0x0000022eu,0x00000228u,0x000500b0u,
    0x0000000fu,0x0000022fu,0x0000022eu,0x000000b3u,0x000400fau,0x0000022fu,0x0000022au,0x0000022bu,
    0x000200f8u,0x0000022au,0x0004003du,0x00000008u,0x00000230u,0x00000224u,0x0004003du,0x00000006u,
    0x00000231u,0x00000021u,0x00050080u,0x00000006u,0x00000232u,0x00000231u,0x0000003cu,0x0004003du,
    0x00000006u,0x00000233u,0x00000228u,0x00050080u,0x00000006u,0x00000234u,0x00000232u,0x00000233u,
    0x00070041u,0x00000041u,0x00000235u,0x00000037u,0x00000039u,0x00000234u,0x00000050u,0x0004003du,
    0x00000008u,0x00000236u,0x00000235u,0x0004003du,0x00000006u,0x00000237u,0x00000228u,0x00050041u,
    0x0000000eu,0x00000238u,0x00000158u,0x00000237u,0x0004003du,0x00000008u,0x00000239u,0x00000238u,
    0x00050085u,0x00000008u,0x0000023au,0x00000236u,0x00000239u,0x00050081u,0x00000008u,0x0000023bu,
    0x00000230u,0x0000023au,0x0003003eu,0x00000224u,0x0000023bu,0x000200f9u,0x0000022cu,0x000200f8u,
    0x0000022cu,0x0004003du,0x00000006u,0x0000023cu,0x00000228u,0x00050080u,0x00000006u,0x0000023du,
    0x0000023cu,0x000000d4u,0x0003003eu,0x00000228u,0x0000023du,0x000200f9u,0x00000229u,0x000200f8u,
    0x0000022bu,0x000200f9u,0x00000227u,0x000200f8u,0x00000227u,0x0004003du,0x0000000fu,0x0000023fu,
    0x0000014bu,0x000300f7u,0x00000242u,0x00000000u,0x000400fau,0x0000023fu,0x00000241u,0x0000024au,
    0x000200f8u,0x00000241u,0x0004003du,0x00000006u,0x00000245u,0x00000022u,0x0003003eu,0x00000244u,
    0x00000245u,0x0004003du,0x00000008u,0x00000247u,0x00000224u,0x0003003eu,0x00000246u,0x00000247u,
    0x0003003eu,0x00000248u,0x00000243u,0x00070039u,0x00000008u,0x00000249u,0x00000015u,0x00000244u,
    0x00000246u,0x00000248u,0x0003003eu,0x00000240u,0x00000249u,0x000200f9u,0x00000242u,0x000200f8u,
    0x0000024au,0x0003003eu,0x00000240u,0x00000053u,0x000200f9u,0x00000242u,0x000200f8u,0x00000242u,
    0x0004003du,0x00000008u,0x0000024bu,0x00000240u,0x0003003eu,0x0000023eu,0x0000024bu,0x0004003du,
    0x00000006u,0x0000024fu,0x00000022u,0x0003003eu,0x0000024eu,0x0000024fu,0x0003003eu,0x00000250u,
    0x0000024du,0x00060039u,0x00000008u,0x00000251u,0x0000000cu,0x0000024eu,0x00000250u,0x0004003du,
    0x00000006u,0x00000253u,0x00000022u,0x0003003eu,0x00000252u,0x00000253u,0x0003003eu,0x00000254u,
    0x00000142u,0x00060039u,0x00000008u,0x00000255u,0x0000000cu,0x00000252u,0x00000254u,0x0004003du,
    0x00000008u,0x00000256u,0x0000023eu,0x00050085u,0x00000008u,0x00000257u,0x00000255u,0x00000256u,
    0x00050081u,0x00000008u,0x00000258u,0x00000251u,0x00000257u,0x0003003eu,0x0000024cu,0x00000258u,
    0x0004003du,0x00000008u,0x00000259u,0x0000024cu,0x0008000cu,0x00000008u,0x0000025au,0x00000001u,
    0x0000002bu,0x00000259u,0x00000053u,0x0000020du,0x0004006eu,0x00000038u,0x0000025bu,0x0000025au,
    0x0004006fu,0x00000008u,0x0000025cu,0x0000025bu,0x00050088u,0x00000008u,0x0000025du,0x0000025cu,
    0x00000209u,0x00050041u,0x0000000eu,0x0000025eu,0x0000016du,0x000000b3u,0x0003003eu,0x0000025eu,
    0x0000025du,0x000200f9u,0x00000223u,0x000200f8u,0x00000223u,0x0004003du,0x0000001fu,0x0000025fu,
    0x0000016du,0x000200feu,0x0000025fu,0x00010038u,0x00050036u,0x00000002u,0x00000032u,0x00000000u,
    0x0000002cu,0x00030037u,0x00000007u,0x0000002du,0x00030037u,0x00000018u,0x0000002eu,0x00030037u,
    0x00000018u,0x0000002fu,0x00030037u,0x00000018u,0x00000030u,0x00030037u,0x0000002bu,0x00000031u,
    0x000200f8u,0x00000033u,0x0004003bu,0x00000007u,0x00000262u,0x00000007u,0x0004003bu,0x00000007u,
    0x00000267u,0x00000007u,0x0004003bu,0x00000007u,0x0000026cu,0x00000007u,0x0004003bu,0x0000002bu,
    0x00000271u,0x00000007u,0x0004003bu,0x00000018u,0x00000277u,0x00000007u,0x0004003bu,0x0000002bu,
    0x00000286u,0x00000007u,0x0004003bu,0x00000018u,0x00000296u,0x00000007u,0x0004003bu,0x00000007u,
    0x00000297u,0x00000007u,0x0004003bu,0x00000018u,0x00000299u,0x00000007u,0x0004003bu,0x00000018u,
    0x0000029bu,0x00000007u,0x0004003bu,0x00000007u,0x0000029eu,0x00000007u,0x0004003bu,0x0000000eu,
    0x000002a6u,0x00000007u,0x0004003bu,0x00000018u,0x000002abu,0x00000007u,0x0004003bu,0x0000000eu,
    0x000002dau,0x00000007u,0x0004003bu,0x0000000eu,0x000002deu,0x00000007u,0x0004003bu,0x0000000eu,
    0x000002efu,0x00000007u,0x0004003bu,0x0000000eu,0x000002f2u,0x00000007u,0x0004003bu,0x00000007u,
    0x00000313u,0x00000007u,0x0004003bu,0x00000007u,0x00000315u,0x00000007u,0x0004003bu,0x00000007u,
    0x00000317u,0x00000007u,0x0004003bu,0x00000018u,0x00000319u,0x00000007u,0x0004003bu,0x00000018u,
    0x0000031bu,0x00000007u,0x0004003bu,0x00000018u,0x0000031du,0x00000007u,0x0004003bu,0x00000018u,
    0x00000320u,0x00000007u,0x0004003bu,0x00000010u,0x00000322u,0x00000007u,0x0004003bu,0x0000000eu,
    0x0000032bu,0x00000007u,0x0004003bu,0x00000007u,0x0000032du,0x00000007u,0x0004003bu,0x00000007u,
    0x0000032fu,0x00000007u,0x0004003bu,0x00000007u,0x00000332u,0x00000007u,0x0004003bu,0x00000007u,
    0x00000334u,0x00000007u,0x0004003bu,0x0000033au,0x0000033bu,0x00000007u,0x0004003bu,0x0000033au,
    0x0000033fu,0x00000007u,0x0004003bu,0x0000033au,0x00000347u,0x00000007u,0x0004003bu,0x00000007u,
    0x00000369u,0x00000007u,0x0004003bu,0x00000007u,0x0000036bu,0x00000007u,0x0004003bu,0x00000007u,
    0x0000036du,0x00000007u,0x0004003bu,0x00000018u,0x0000036fu,0x00000007u,0x0004003bu,0x00000018u,
    0x00000371u,0x00000007u,0x0004003bu,0x00000018u,0x00000373u,0x00000007u,0x0004003bu,0x00000018u,
    0x00000376u,0x00000007u,0x0004003bu,0x00000010u,0x00000377u,0x00000007u,0x0004003du,0x00000006u,
    0x00000263u,0x0000002du,0x00070041u,0x00000041u,0x00000264u,0x00000037u,0x00000039u,0x00000263u,
    0x00000050u,0x0004003du,0x00000008u,0x00000265u,0x00000264u,0x0004007cu,0x00000006u,0x00000266u,
    0x00000265u,0x0003003eu,0x00000262u,0x00000266u,0x0004003du,0x00000006u,0x00000268u,0x0000002du,
    0x00070041u,0x00000041u,0x00000269u,0x00000037u,0x00000039u,0x00000268u,0x00000081u,0x0004003du,
    0x00000008u,0x0000026au,0x00000269u,0x0004007cu,0x00000006u,0x0000026bu,0x0000026au,0x0003003eu,
    0x00000267u,0x0000026bu,0x0004003du,0x00000006u,0x0000026du,0x0000002du,0x00070041u,0x00000041u,
    0x0000026eu,0x00000037u,0x00000039u,0x0000026du,0x00000048u,0x0004003du,0x00000008u,0x0000026fu,
    0x0000026eu,0x0004007cu,0x00000006u,0x00000270u,0x0000026fu,0x0003003eu,0x0000026cu,0x00000270u,
    0x0004003du,0x00000006u,0x00000272u,0x0000002du,0x00050080u,0x00000006u,0x00000273u,0x00000272u,
    0x00000081u,0x00060041u,0x00000274u,0x00000275u,0x00000037u,0x00000039u,0x00000273u,0x0004003du,
    0x0000001fu,0x00000276u,0x00000275u,0x0003003eu,0x00000271u,0x00000276u,0x0004003du,0x00000017u,
    0x0000027bu,0x00000279u,0x0007004fu,0x0000027au,0x0000027cu,0x0000027bu,0x0000027bu,0x00000000u,
    0x00000001u,0x0004003du,0x0000001fu,0x0000027du,0x00000271u,0x0007004fu,0x0000027au,0x0000027eu,
    0x0000027du,0x0000027du,0x00000001u,0x00000002u,0x00050081u,0x0000027au,0x0000027fu,0x0000027cu,
    0x0000027eu,0x00050041u,0x00000280u,0x00000281u,0x00000279u,0x00000048u,0x0004003du,0x00000008u,
    0x00000282u,0x00000281u,0x00050051u,0x00000008u,0x00000283u,0x0000027fu,0x00000000u,0x00050051u,
    0x00000008u,0x00000284u,0x0000027fu,0x00000001u,0x00060050u,0x00000017u,0x00000285u,0x00000283u,
    0x00000284u,0x00000282u,0x0003003eu,0x00000277u,0x00000285u,0x0004003du,0x0000001fu,0x00000289u,
    0x00000288u,0x0008004fu,0x00000017u,0x0000028au,0x00000289u,0x00000289u,0x00000000u,0x00000001u,
    0x00000002u,0x00050041u,0x0000000eu,0x0000028bu,0x00000271u,0x00000050u,0x0004003du,0x00000008u,
    0x0000028cu,0x0000028bu,0x00050051u,0x00000008u,0x0000028du,0x0000028au,0x00000000u,0x00050051u,
    0x00000008u,0x0000028eu,0x0000028au,0x00000001u,0x00050051u,0x00000008u,0x0000028fu,0x0000028au,
    0x00000002u,0x00070050u,0x0000001fu,0x00000290u,0x0000028du,0x0000028eu,0x0000028fu,0x0000028cu,
    0x0003003eu,0x00000286u,0x00000290u,0x0004003du,0x00000006u,0x00000291u,0x00000267u,0x000500c7u,
    0x00000006u,0x00000292u,0x00000291u,0x00000081u,0x000500abu,0x0000000fu,0x00000293u,0x00000292u,
    0x00000050u,0x000300f7u,0x00000295u,0x00000000u,0x000400fau,0x00000293u,0x00000294u,0x00000361u,
    0x000200f8u,0x00000294u,0x0004003du,0x00000006u,0x00000298u,0x0000002du,0x0003003eu,0x00000297u,
    0x00000298u,0x0004003du,0x00000017u,0x0000029au,0x0000002eu,0x0003003eu,0x00000299u,0x0000029au,
    0x0004003du,0x00000017u,0x0000029cu,0x0000002fu,0x0003003eu,0x0000029bu,0x0000029cu,0x00070039u,
    0x00000017u,0x0000029du,0x0000001du,0x00000297u,0x00000299u,0x0000029bu,0x0003003eu,0x00000296u,
    0x0000029du,0x0003003eu,0x0000029eu,0x00000050u,0x000200f9u,0x0000029fu,0x000200f8u,0x0000029fu,
    0x000400f6u,0x000002a1u,0x000002a2u,0x00000000u,0x000200f9u,0x000002a3u,0x000200f8u,0x000002a3u,
    0x0004003du,0x00000006u,0x000002a4u,0x0000029eu,0x000500b0u,0x0000000fu,0x000002a5u,0x000002a4u,
    0x000000b3u,0x000400fau,0x000002a5u,0x000002a0u,0x000002a1u,0x000200f8u,0x000002a0u,0x0004003du,
    0x00000006u,0x000002a7u,0x0000029eu,0x000500aau,0x0000000fu,0x000002a8u,0x000002a7u,0x00000081u,
    0x000600a9u,0x00000008u,0x000002aau,0x000002a8u,0x0000005au,0x000002a9u,0x0003003eu,0x000002a6u,
    0x000002aau,0x0004003du,0x00000006u,0x000002acu,0x0000029eu,0x0004003du,0x00000006u,0x000002adu,
    0x0000002du,0x00050080u,0x00000006u,0x000002aeu,0x000002adu,0x0000011au,0x0004003du,0x00000006u,
    0x000002afu,0x0000029eu,0x00070041u,0x00000041u,0x000002b0u,0x00000037u,0x00000039u,0x000002aeu,
    0x000002afu,0x0004003du,0x00000008u,0x000002b1u,0x000002b0u,0x0004003du,0x00000008u,0x000002b2u,
    0x000002a6u,0x00050085u,0x00000008u,0x000002b3u,0x000002b1u,0x000002b2u,0x00050041u,0x0000000eu,
    0x000002b4u,0x00000296u,0x00000050u,0x0004003du,0x00000008u,0x000002b5u,0x000002b4u,0x00050085u,
    0x00000008u,0x000002b6u,0x000002b3u,0x000002b5u,0x00050041u,0x0000000eu,0x000002b7u,0x000002abu,
    0x000002acu,0x0003003eu,0x000002b7u,0x000002b6u,0x0004003du,0x00000006u,0x000002b8u,0x0000029eu,
    0x0004003du,0x00000006u,0x000002b9u,0x0000029eu,0x00050041u,0x0000000eu,0x000002bau,0x000002abu,
    0x000002b9u,0x0004003du,0x00000008u,0x000002bbu,0x000002bau,0x0004003du,0x00000006u,0x000002bcu,
    0x0000002du,0x00050080u,0x00000006u,0x000002bdu,0x000002bcu,0x00000123u,0x0004003du,0x00000006u,
    0x000002beu,0x0000029eu,0x00070041u,0x00000041u,0x000002bfu,0x00000037u,0x00000039u,0x000002bdu,
    0x000002beu,0x0004003du,0x00000008u,0x000002c0u,0x000002bfu,0x0004003du,0x00000008u,0x000002c1u,
    0x000002a6u,0x00050085u,0x00000008u,0x000002c2u,0x000002c0u,0x000002c1u,0x00050041u,0x0000000eu,
    0x000002c3u,0x00000296u,0x00000081u,0x0004003du,0x00000008u,0x000002c4u,0x000002c3u,0x00050085u,
    0x00000008u,0x000002c5u,0x000002c2u,0x000002c4u,0x00050081u,0x00000008u,0x000002c6u,0x000002bbu,
    0x000002c5u,0x00050041u,0x0000000eu,0x000002c7u,0x000002abu,0x000002b8u,0x0003003eu,0x000002c7u,
    0x000002c6u,0x0004003du,0x00000006u,0x000002c8u,0x0000029eu,0x0004003du,0x00000006u,0x000002c9u,
    0x0000029eu,0x00050041u,0x0000000eu,0x000002cau,0x000002abu,0x000002c9u,0x0004003du,0x00000008u,
    0x000002cbu,0x000002cau,0x0004003du,0x00000006u,0x000002ccu,0x0000002du,0x00050080u,0x00000006u,
    0x000002cdu,0x000002ccu,0x0000012du,0x0004003du,0x00000006u,0x000002ceu,0x0000029eu,0x00070041u,
    0x00000041u,0x000002cfu,0x00000037u,0x00000039u,0x000002cdu,0x000002ceu,0x0004003du,0x00000008u,
    0x000002d0u,0x000002cfu,0x0004003du,0x00000008u,0x000002d1u,0x000002a6u,0x00050085u,0x00000008u,
    0x000002d2u,0x000002d0u,0x000002d1u,0x00050041u,0x0000000eu,0x000002d3u,0x00000296u,0x00000048u,
    0x0004003du,0x00000008u,0x000002d4u,0x000002d3u,0x00050085u,0x00000008u,0x000002d5u,0x000002d2u,
    0x000002d4u,0x00050081u,0x00000008u,0x000002d6u,0x000002cbu,0x000002d5u,0x00050041u,0x0000000eu,
    0x000002d7u,0x000002abu,0x000002c8u,0x0003003eu,0x000002d7u,0x000002d6u,0x000200f9u,0x000002a2u,
    0x000200f8u,0x000002a2u,0x0004003du,0x00000006u,0x000002d8u,0x0000029eu,0x00050080u,0x00000006u,
    0x000002d9u,0x000002d8u,0x000000d4u,0x0003003eu,0x0000029eu,0x000002d9u,0x000200f9u,0x0000029fu,
    0x000200f8u,0x000002a1u,0x00050041u,0x0000000eu,0x000002dbu,0x000002abu,0x00000048u,0x0004003du,
    0x00000008u,0x000002dcu,0x000002dbu,0x00050081u,0x00000008u,0x000002ddu,0x000002dcu,0x0000005au,
    0x0003003eu,0x000002dau,0x000002ddu,0x00050041u,0x0000000eu,0x000002dfu,0x000002abu,0x00000050u,
    0x0004003du,0x00000008u,0x000002e0u,0x000002dfu,0x00050041u,0x0000000eu,0x000002e1u,0x000002abu,
    0x00000050u,0x0004003du,0x00000008u,0x000002e2u,0x000002e1u,0x00050085u,0x00000008u,0x000002e3u,
    0x000002e0u,0x000002e2u,0x00050041u,0x0000000eu,0x000002e4u,0x000002abu,0x00000081u,0x0004003du,
    0x00000008u,0x000002e5u,0x000002e4u,0x00050041u,0x0000000eu,0x000002e6u,0x000002abu,0x00000081u,
    0x0004003du,0x00000008u,0x000002e7u,0x000002e6u,0x00050085u,0x00000008u,0x000002e8u,0x000002e5u,
    0x000002e7u,0x00050081u,0x00000008u,0x000002e9u,0x000002e3u,0x000002e8u,0x0003003eu,0x000002deu,
    0x000002e9u,0x0004003du,0x00000008u,0x000002eau,0x000002deu,0x0004003du,0x00000008u,0x000002ebu,
    0x000002dau,0x0004003du,0x00000008u,0x000002ecu,0x000002dau,0x00050085u,0x00000008u,0x000002edu,
    0x000002ebu,0x000002ecu,0x00050081u,0x00000008u,0x000002eeu,0x000002eau,0x000002edu,0x0003003eu,
    0x000002deu,0x000002eeu,0x0004003du,0x00000008u,0x000002f0u,0x000002deu,0x000500bau,0x0000000fu,
    0x000002f1u,0x000002f0u,0x00000053u,0x000300f7u,0x000002f4u,0x00000000u,0x000400fau,0x000002f1u,
    0x000002f3u,0x000002f8u,0x000200f8u,0x000002f3u,0x0004003du,0x00000008u,0x000002f5u,0x000002deu,
    0x0006000cu,0x00000008u,0x000002f6u,0x00000001u,0x0000001fu,0x000002f5u,0x00050088u,0x00000008u,
    0x000002f7u,0x0000005au,0x000002f6u,0x0003003eu,0x000002f2u,0x000002f7u,0x000200f9u,0x000002f4u,
    0x000200f8u,0x000002f8u,0x0003003eu,0x000002f2u,0x00000053u,0x000200f9u,0x000002f4u,0x000200f8u,
    0x000002f4u,0x0004003du,0x00000008u,0x000002f9u,0x000002f2u,0x0003003eu,0x000002efu,0x000002f9u,
    0x00050041u,0x0000000eu,0x000002fbu,0x000002abu,0x00000050u,0x0004003du,0x00000008u,0x000002fcu,
    0x000002fbu,0x0004003du,0x00000008u,0x000002fdu,0x000002efu,0x00050085u,0x00000008u,0x000002feu,
    0x000002fcu,0x000002fdu,0x00050085u,0x00000008u,0x000002ffu,0x000002feu,0x000002fau,0x00050081u,
    0x00000008u,0x00000300u,0x000002fau,0x000002ffu,0x00050041u,0x0000000eu,0x00000301u,0x000002abu,
    0x00000081u,0x0004003du,0x00000008u,0x00000302u,0x00000301u,0x0004003du,0x00000008u,0x00000303u,
    0x000002efu,0x00050085u,0x00000008u,0x00000304u,0x00000302u,0x00000303u,0x00050085u,0x00000008u,
    0x00000305u,0x00000304u,0x000002fau,0x00050081u,0x00000008u,0x00000306u,0x000002fau,0x00000305u,
    0x00060050u,0x00000017u,0x00000307u,0x00000300u,0x00000306u,0x0000005au,0x0003003eu,0x00000277u,
    0x00000307u,0x0003003eu,0x00000286u,0x00000308u,0x0004003du,0x00000006u,0x00000309u,0x00000262u,
    0x000500c7u,0x00000006u,0x0000030au,0x00000309u,0x0000003cu,0x000500abu,0x0000000fu,0x0000030bu,
    0x0000030au,0x00000050u,0x000300f7u,0x0000030du,0x00000000u,0x000400fau,0x0000030bu,0x0000030cu,
    0x0000030du,0x000200f8u,0x0000030cu,0x0004003du,0x00000006u,0x0000030eu,0x00000262u,0x000500c7u,
    0x00000006u,0x0000030fu,0x0000030eu,0x00000136u,0x000500abu,0x0000000fu,0x00000310u,0x0000030fu,
    0x00000050u,0x000300f7u,0x00000312u,0x00000000u,0x000400fau,0x00000310u,0x00000311u,0x00000324u,
    0x000200f8u,0x00000311u,0x0004003du,0x00000006u,0x00000314u,0x0000002du,0x0003003eu,0x00000313u,
    0x00000314u,0x0004003du,0x00000006u,0x00000316u,0x0000026cu,0x0003003eu,0x00000315u,0x00000316u,
    0x0004003du,0x00000006u,0x00000318u,0x00000262u,0x0003003eu,0x00000317u,0x00000318u,0x0004003du,
    0x00000017u,0x0000031au,0x0000002eu,0x0003003eu,0x00000319u,0x0000031au,0x0004003du,0x00000017u,
    0x0000031cu,0x0000002fu,0x0003003eu,0x0000031bu,0x0000031cu,0x0004003du,0x0000001fu,0x0000031eu,
    0x00000288u,0x0008004fu,0x00000017u,0x0000031fu,0x0000031eu,0x0000031eu,0x00000000u,0x00000001u,
    0x00000002u,0x0003003eu,0x0000031du,0x0000031fu,0x0004003du,0x00000017u,0x00000321u,0x00000296u,
    0x0003003eu,0x00000320u,0x00000321u,0x0003003eu,0x00000322u,0x00000243u,0x000c0039u,0x0000001fu,
    0x00000323u,0x00000029u,0x00000313u,0x00000315u,0x00000317u,0x00000319u,0x0000031bu,0x0000031du,
    0x00000320u,0x00000322u,0x0003003eu,0x00000286u,0x00000323u,0x000200f9u,0x00000312u,0x000200f8u,
    0x00000324u,0x0004003du,0x0000001fu,0x00000325u,0x00000288u,0x0008004fu,0x00000017u,0x00000326u,
    0x00000325u,0x00000325u,0x00000000u,0x00000001u,0x00000002u,0x00050051u,0x00000008u,0x00000327u,
    0x00000326u,0x00000000u,0x00050051u,0x00000008u,0x00000328u,0x00000326u,0x00000001u,0x00050051u,
    0x00000008u,0x00000329u,0x00000326u,0x00000002u,0x00070050u,0x0000001fu,0x0000032au,0x00000327u,
    0x00000328u,0x00000329u,0x0000005au,0x0003003eu,0x00000286u,0x0000032au,0x000200f9u,0x00000312u,
    0x000200f8u,0x00000312u,0x000200f9u,0x0000030du,0x000200f8u,0x0000030du,0x0004003du,0x00000006u,
    0x0000032eu,0x0000026cu,0x0003003eu,0x0000032du,0x0000032eu,0x0003003eu,0x0000032fu,0x0000032cu,
    0x00060039u,0x00000008u,0x00000330u,0x0000000cu,0x0000032du,0x0000032fu,0x0004003du,0x00000006u,
    0x00000333u,0x0000026cu,0x0003003eu,0x00000332u,0x00000333u,0x0003003eu,0x00000334u,0x00000331u,
    0x00060039u,0x00000008u,0x00000335u,0x0000000cu,0x00000332u,0x00000334u,0x00050041u,0x0000000eu,
    0x00000336u,0x000002abu,0x00000048u,0x0004003du,0x00000008u,0x00000337u,0x00000336u,0x00050085u,
    0x00000008u,0x00000338u,0x00000335u,0x00000337u,0x00050081u,0x00000008u,0x00000339u,0x00000330u,
    0x00000338u,0x0003003eu,0x0000032bu,0x00000339u,0x0004003du,0x00000008u,0x0000033cu,0x0000032bu,
    0x000500beu,0x0000000fu,0x0000033eu,0x0000033cu,0x0000033du,0x000300f7u,0x00000341u,0x00000000u,
    0x000400fau,0x0000033eu,0x00000340u,0x00000343u,0x000200f8u,0x00000340u,0x0003003eu,0x0000033fu,
    0x00000342u,0x000200f9u,0x00000341u,0x000200f8u,0x00000343u,0x0004003du,0x00000008u,0x00000344u,
    0x0000032bu,0x000500b8u,0x0000000fu,0x00000346u,0x00000344u,0x00000345u,0x000300f7u,0x00000349u,
    0x00000000u,0x000400fau,0x00000346u,0x00000348u,0x0000034bu,0x000200f8u,0x00000348u,0x0003003eu,
    0x00000347u,0x0000034au,0x000200f9u,0x00000349u,0x000200f8u,0x0000034bu,0x0004003du,0x00000008u,
    0x0000034cu,0x0000032bu,0x0004006eu,0x00000038u,0x0000034du,0x0000034cu,0x0003003eu,0x00000347u,
    0x0000034du,0x000200f9u,0x00000349u,0x000200f8u,0x00000349u,0x0004003du,0x00000038u,0x0000034eu,
    0x00000347u,0x0003003eu,0x0000033fu,0x0000034eu,0x000200f9u,0x00000341u,0x000200f8u,0x00000341u,
    0x0004003du,0x00000038u,0x0000034fu,0x0000033fu,0x0003003eu,0x0000033bu,0x0000034fu,0x0004003du,
    0x00000038u,0x00000350u,0x0000033bu,0x0004007cu,0x00000006u,0x00000351u,0x00000350u,0x000500c7u,
    0x00000006u,0x00000353u,0x00000351u,0x00000352u,0x00040070u,0x00000008u,0x00000354u,0x00000353u,
    0x00050088u,0x00000008u,0x00000355u,0x00000354u,0x00000209u,0x00050041u,0x0000000eu,0x00000356u,
    0x00000286u,0x000000b3u,0x0003003eu,0x00000356u,0x00000355u,0x0004003du,0x00000006u,0x00000357u,
    0x00000267u,0x000500c7u,0x00000006u,0x00000358u,0x00000357u,0x00000048u,0x000500abu,0x0000000fu,
    0x00000359u,0x00000358u,0x00000050u,0x000300f7u,0x0000035bu,0x00000000u,0x000400fau,0x00000359u,
    0x0000035au,0x0000035bu,0x000200f8u,0x0000035au,0x00050041u,0x0000000eu,0x0000035cu,0x00000286u,
    0x000000b3u,0x0004003du,0x00000008u,0x0000035du,0x0000035cu,0x00050085u,0x00000008u,0x0000035fu,
    0x0000035du,0x0000035eu,0x00050041u,0x0000000eu,0x00000360u,0x00000286u,0x000000b3u,0x0003003eu,
    0x00000360u,0x0000035fu,0x000200f9u,0x0000035bu,0x000200f8u,0x0000035bu,0x000200f9u,0x00000295u,
    0x000200f8u,0x00000361u,0x0004003du,0x00000006u,0x00000362u,0x00000262u,0x000500c7u,0x00000006u,
    0x00000364u,0x00000362u,0x00000363u,0x000500abu,0x0000000fu,0x00000365u,0x00000364u,0x00000050u,
    0x000300f7u,0x00000367u,0x00000000u,0x000400fau,0x00000365u,0x00000366u,0x00000367u,0x000200f8u,
    0x00000366u,0x0004003du,0x00000006u,0x0000036au,0x0000002du,0x0003003eu,0x00000369u,0x0000036au,
    0x0004003du,0x00000006u,0x0000036cu,0x0000026cu,0x0003003eu,0x0000036bu,0x0000036cu,0x0004003du,
    0x00000006u,0x0000036eu,0x00000262u,0x0003003eu,0x0000036du,0x0000036eu,0x0004003du,0x00000017u,
    0x00000370u,0x0000002eu,0x0003003eu,0x0000036fu,0x00000370u,0x0004003du,0x00000017u,0x00000372u,
    0x0000002fu,0x0003003eu,0x00000371u,0x00000372u,0x0004003du,0x0000001fu,0x00000374u,0x00000288u,
    0x0008004fu,0x00000017u,0x00000375u,0x00000374u,0x00000374u,0x00000000u,0x00000001u,0x00000002u,
    0x0003003eu,0x00000373u,0x00000375u,0x0003003eu,0x00000376u,0x00000159u,0x0003003eu,0x00000377u,
    0x00000368u,0x000c0039u,0x0000001fu,0x00000378u,0x00000029u,0x00000369u,0x0000036bu,0x0000036du,
    0x0000036fu,0x00000371u,0x00000373u,0x00000376u,0x00000377u,0x0003003eu,0x00000286u,0x00000378u,
    0x00050041u,0x0000000eu,0x00000379u,0x00000286u,0x000000b3u,0x0004003du,0x00000008u,0x0000037au,
    0x00000379u,0x00050041u,0x0000000eu,0x0000037bu,0x00000271u,0x00000050u,0x0004003du,0x00000008u,
    0x0000037cu,0x0000037bu,0x00050085u,0x00000008u,0x0000037du,0x0000037au,0x0000037cu,0x00050041u,
    0x0000000eu,0x0000037eu,0x00000286u,0x000000b3u,0x0003003eu,0x0000037eu,0x0000037du,0x000200f9u,
    0x00000367u,0x000200f8u,0x00000367u,0x000200f9u,0x00000295u,0x000200f8u,0x00000295u,0x0004003du,
    0x00000017u,0x0000037fu,0x00000277u,0x0003003eu,0x00000030u,0x0000037fu,0x0004003du,0x0000001fu,
    0x00000380u,0x00000286u,0x0003003eu,0x00000031u,0x00000380u,0x000100fdu,0x00010038u,
};
constexpr std::uint32_t vulkanFragmentSPIRV[]={
    0x07230203u,0x00010000u,0x0008000bu,0x0000029fu,0x00000000u,0x00020011u,0x00000001u,0x00020011u,
    0x00000032u,0x0006000bu,0x00000001u,0x4c534c47u,0x6474732eu,0x3035342eu,0x00000000u,0x0003000eu,
    0x00000000u,0x00000001u,0x000b000fu,0x00000004u,0x00000004u,0x6e69616du,0x00000000u,0x000000f4u,
    0x000000fcu,0x00000106u,0x00000116u,0x00000145u,0x000001e0u,0x00030010u,0x00000004u,0x00000007u,
    0x00030003u,0x00000002u,0x000001c2u,0x00040005u,0x00000004u,0x6e69616du,0x00000000u,0x00060005u,
    0x0000000cu,0x746c6966u,0x78467265u,0x76286161u,0x003b3266u,0x00030005u,0x0000000bu,0x00007675u,
    0x00040005u,0x0000000eu,0x65786970u,0x0000006cu,0x00050005u,0x00000013u,0x65546975u,0x72757478u,
    0x00000065u,0x00040005u,0x0000001eu,0x746e6563u,0x00007265u,0x00040005u,0x00000024u,0x6464696du,
    0x0000656cu,0x00030005u,0x0000002du,0x0000776eu,0x00030005u,0x00000038u,0x0000656eu,0x00030005u,
    0x00000042u,0x00007773u,0x00030005u,0x0000004cu,0x00006573u,0x00030005u,0x00000056u,0x00776f6cu,
    0x00040005u,0x00000060u,0x68676968u,0x00000000u,0x00050005u,0x00000078u,0x65726964u,0x6f697463u,
    0x0000006eu,0x00050005u,0x00000088u,0x75646572u,0x6f697463u,0x0000006eu,0x00040005u,0x000000aau,
    0x7272616eu,0x0000776fu,0x00040005u,0x000000bfu,0x65646977u,0x00000000u,0x00050005u,0x000000d4u,
    0x646e6163u,0x74616469u,0x00000065u,0x00080005u,0x000000ecu,0x6574616du,0x6c616972u,0x74786554u,
    0x46657275u,0x74636e75u,0x006e6f69u,0x00040005u,0x000000f1u,0x65657267u,0x0000006eu,0x00060005u,
    0x000000f4u,0x465f6c67u,0x43676172u,0x64726f6fu,0x00000000u,0x00060005u,0x000000fcu,0x67617266u,
    0x746e656du,0x6f6c6f43u,0x00000072u,0x00030005u,0x00000104u,0x00007675u,0x00030005u,0x00000106u,
    0x00717675u,0x00030005u,0x0000010fu,0x00646f6cu,0x00060005u,0x00000116u,0x50646f6cu,0x6d617261u,
    0x72657465u,0x00000073u,0x00040005u,0x0000011du,0x65786574u,0x0000006cu,0x00080005u,0x0000011eu,
    0x6574616du,0x6c616972u,0x696e694du,0x61636966u,0x6e6f6974u,0x00000000u,0x00060005u,0x00000124u,
    0x67616d69u,0x78655465u,0x65727574u,0x00000000u,0x00070005u,0x00000132u,0x6e67616du,0x65696669u,
    0x78655464u,0x65727574u,0x00000000u,0x00040005u,0x00000140u,0x72756f73u,0x00006563u,0x00040005u,
    0x00000145u,0x6f6c6f63u,0x00000072u,0x00050005u,0x0000014du,0x74736564u,0x74616e69u,0x006e6f69u,
    0x00040005u,0x00000157u,0x75736572u,0x0000746cu,0x00060005u,0x0000015eu,0x77617244u,0x61726150u,
    0x6574656du,0x00007372u,0x00070006u,0x0000015eu,0x00000000u,0x74786574u,0x46657275u,0x74636e75u,
    0x006e6f69u,0x00060006u,0x0000015eu,0x00000001u,0x68706c61u,0x73655461u,0x00000074u,0x00060006u,
    0x0000015eu,0x00000002u,0x53646f6cu,0x656c6163u,0x00000000u,0x00070006u,0x0000015eu,0x00000003u,
    0x696e696du,0x61636966u,0x6e6f6974u,0x00000000u,0x00040005u,0x00000160u,0x77617264u,0x00000000u,
    0x00040005u,0x00000183u,0x74706564u,0x00000068u,0x00050005u,0x0000018cu,0x72756f73u,0x65446563u,
    0x00687470u,0x00060005u,0x00000196u,0x70796c47u,0x6d615368u,0x6e696c70u,0x00000067u,0x00050006u,
    0x00000196u,0x00000000u,0x6e756f62u,0x00007364u,0x00060005u,0x00000198u,0x70796c67u,0x6d615368u,
    0x6e696c70u,0x00000067u,0x00030005u,0x000001bdu,0x00000071u,0x00030005u,0x000001cdu,0x00646f6cu,
    0x00030005u,0x000001d8u,0x00007675u,0x00070005u,0x000001e0u,0x70796c67u,0x756f4268u,0x4973646eu,
    0x7865646eu,0x00000000u,0x00050005u,0x000001e5u,0x74636572u,0x6c676e61u,0x00000065u,0x00080005u,
    0x000001efu,0x6574616du,0x6c616972u,0x6d616c43u,0x706f5470u,0x7466654cu,0x00000000u,0x00040005u,
    0x000001f3u,0x65747865u,0x0000746eu,0x00040005u,0x00000205u,0x61726170u,0x0000006du,0x00050005u,
    0x0000020du,0x706d6173u,0x6f43656cu,0x00726f6cu,0x00070005u,0x00000231u,0x6574616du,0x6c616972u,
    0x74786554u,0x6e697275u,0x00000067u,0x00040005u,0x00000235u,0x75736572u,0x0000746cu,0x00070005u,
    0x00000273u,0x6574616du,0x6c616972u,0x68706c41u,0x73655461u,0x00000074u,0x00040047u,0x00000013u,
    0x00000021u,0x00000002u,0x00040047u,0x00000013u,0x00000022u,0x00000000u,0x00040047u,0x000000ecu,
    0x00000001u,0x00000000u,0x00040047u,0x000000f4u,0x0000000bu,0x0000000fu,0x00040047u,0x000000fcu,
    0x0000001eu,0x00000000u,0x00040047u,0x00000106u,0x0000001eu,0x00000000u,0x00030047u,0x00000116u,
    0x0000000eu,0x00040047u,0x00000116u,0x0000001eu,0x00000002u,0x00040047u,0x0000011eu,0x00000001u,
    0x00000002u,0x00040047u,0x00000124u,0x00000021u,0x00000000u,0x00040047u,0x00000124u,0x00000022u,
    0x00000000u,0x00040047u,0x00000132u,0x00000021u,0x00000001u,0x00040047u,0x00000132u,0x00000022u,
    0x00000000u,0x00040047u,0x00000145u,0x0000001eu,0x00000001u,0x00030047u,0x0000015eu,0x00000002u,
    0x00050048u,0x0000015eu,0x00000000u,0x00000023u,0x00000000u,0x00050048u,0x0000015eu,0x00000001u,
    0x00000023u,0x00000004u,0x00050048u,0x0000015eu,0x00000002u,0x00000023u,0x00000008u,0x00050048u,
    0x0000015eu,0x00000003u,0x00000023u,0x0000000cu,0x00040047u,0x00000195u,0x00000006u,0x00000010u,
    0x00030047u,0x00000196u,0x00000003u,0x00040048u,0x00000196u,0x00000000u,0x00000018u,0x00050048u,
    0x00000196u,0x00000000u,0x00000023u,0x00000000u,0x00030047u,0x00000198u,0x00000018u,0x00040047u,
    0x00000198u,0x00000021u,0x00000000u,0x00040047u,0x00000198u,0x00000022u,0x00000001u,0x00030047u,
    0x000001e0u,0x0000000eu,0x00040047u,0x000001e0u,0x0000001eu,0x00000003u,0x00040047u,0x000001efu,
    0x00000001u,0x00000004u,0x00040047u,0x00000231u,0x00000001u,0x00000003u,0x00040047u,0x00000273u,
    0x00000001u,0x00000001u,0x00040047u,0x0000028du,0x00000001u,0x00000005u,0x00020013u,0x00000002u,
    0x00030021u,0x00000003u,0x00000002u,0x00030016u,0x00000006u,0x00000020u,0x00040017u,0x00000007u,
    0x00000006u,0x00000002u,0x00040020u,0x00000008u,0x00000007u,0x00000007u,0x00040017u,0x00000009u,
    0x00000006u,0x00000004u,0x00040021u,0x0000000au,0x00000009u,0x00000008u,0x0004002bu,0x00000006u,
    0x0000000fu,0x3f800000u,0x00090019u,0x00000010u,0x00000006u,0x00000001u,0x00000000u,0x00000000u,
    0x00000000u,0x00000001u,0x00000000u,0x0003001bu,0x00000011u,0x00000010u,0x00040020u,0x00000012u,
    0x00000000u,0x00000011u,0x0004003bu,0x00000012u,0x00000013u,0x00000000u,0x00040015u,0x00000015u,
    0x00000020u,0x00000001u,0x0004002bu,0x00000015u,0x00000016u,0x00000000u,0x00040017u,0x00000018u,
    0x00000015u,0x00000002u,0x00040020u,0x0000001du,0x00000007u,0x00000009u,0x0004002bu,0x00000006u,
    0x00000021u,0x00000000u,0x00040020u,0x00000023u,0x00000007u,0x00000006u,0x00040017u,0x00000025u,
    0x00000006u,0x00000003u,0x0004002bu,0x00000006u,0x00000028u,0x3e991687u,0x0004002bu,0x00000006u,
    0x00000029u,0x3f1645a2u,0x0004002bu,0x00000006u,0x0000002au,0x3de978d5u,0x0006002cu,0x00000025u,
    0x0000002bu,0x00000028u,0x00000029u,0x0000002au,0x0004002bu,0x00000006u,0x00000030u,0xbf800000u,
    0x0005002cu,0x00000007u,0x00000031u,0x00000030u,0x00000030u,0x0005002cu,0x00000007u,0x0000003bu,
    0x0000000fu,0x00000030u,0x0005002cu,0x00000007u,0x00000045u,0x00000030u,0x0000000fu,0x0005002cu,
    0x00000007u,0x0000004fu,0x0000000fu,0x0000000fu,0x0004002bu,0x00000006u,0x0000006du,0x3d000000u,
    0x0004002bu,0x00000006u,0x0000006fu,0x3e000000u,0x00020014u,0x00000072u,0x0004002bu,0x00000006u,
    0x00000091u,0x3c000000u,0x00040015u,0x00000094u,0x00000020u,0x00000000u,0x0004002bu,0x00000094u,
    0x00000095u,0x00000000u,0x0004002bu,0x00000094u,0x00000099u,0x00000001u,0x0004002bu,0x00000006u,
    0x000000a2u,0xc1000000u,0x0005002cu,0x00000007u,0x000000a3u,0x000000a2u,0x000000a2u,0x0004002bu,
    0x00000006u,0x000000a4u,0x41000000u,0x0005002cu,0x00000007u,0x000000a5u,0x000000a4u,0x000000a4u,
    0x00040020u,0x000000a9u,0x00000007u,0x00000025u,0x0004002bu,0x00000006u,0x000000abu,0x3f000000u,
    0x0004002bu,0x00000006u,0x000000afu,0x40c00000u,0x0004002bu,0x00000006u,0x000000c2u,0x3e800000u,
    0x00040017u,0x000000e0u,0x00000072u,0x00000003u,0x0004002bu,0x00000094u,0x000000e3u,0x00000003u,
    0x00040032u,0x00000015u,0x000000ecu,0x00000000u,0x0004002bu,0x00000015u,0x000000edu,0x00000009u,
    0x00060034u,0x00000072u,0x000000eeu,0x000000aau,0x000000ecu,0x000000edu,0x00040020u,0x000000f3u,
    0x00000001u,0x00000009u,0x0004003bu,0x000000f3u,0x000000f4u,0x00000001u,0x00040020u,0x000000fbu,
    0x00000003u,0x00000009u,0x0004003bu,0x000000fbu,0x000000fcu,0x00000003u,0x0004002bu,0x00000015u,
    0x00000100u,0x0000000au,0x00060034u,0x00000072u,0x00000101u,0x000000aau,0x000000ecu,0x00000100u,
    0x00040020u,0x00000105u,0x00000001u,0x00000025u,0x0004003bu,0x00000105u,0x00000106u,0x00000001u,
    0x0004002bu,0x00000094u,0x00000109u,0x00000002u,0x00040020u,0x0000010au,0x00000001u,0x00000006u,
    0x00040020u,0x00000115u,0x00000001u,0x00000007u,0x0004003bu,0x00000115u,0x00000116u,0x00000001u,
    0x00040032u,0x00000015u,0x0000011eu,0x00000000u,0x0004002bu,0x00000015u,0x0000011fu,0xfffffffeu,
    0x00060034u,0x00000072u,0x00000120u,0x000000aau,0x0000011eu,0x0000011fu,0x0004003bu,0x00000012u,
    0x00000124u,0x00000000u,0x0004003bu,0x00000012u,0x00000132u,0x00000000u,0x0004002bu,0x00000015u,
    0x00000139u,0x00000002u,0x00060034u,0x00000072u,0x0000013au,0x000000b1u,0x0000011eu,0x00000139u,
    0x0004002bu,0x00000006u,0x00000143u,0x437f0000u,0x0004003bu,0x000000f3u,0x00000145u,0x00000001u,
    0x0004002bu,0x00000006u,0x00000159u,0x43000000u,0x0006001eu,0x0000015eu,0x00000015u,0x00000015u,
    0x00000006u,0x00000015u,0x00040020u,0x0000015fu,0x00000009u,0x0000015eu,0x0004003bu,0x0000015fu,
    0x00000160u,0x00000009u,0x00040020u,0x00000161u,0x00000009u,0x00000006u,0x0004002bu,0x00000006u,
    0x00000175u,0x3fff0000u,0x0004002bu,0x00000015u,0x0000017cu,0x00000005u,0x00060034u,0x00000072u,
    0x0000017du,0x000000aau,0x000000ecu,0x0000017cu,0x0004002bu,0x00000015u,0x0000017eu,0x00000006u,
    0x00060034u,0x00000072u,0x0000017fu,0x000000aau,0x000000ecu,0x0000017eu,0x00060034u,0x00000072u,
    0x00000180u,0x000000a6u,0x0000017du,0x0000017fu,0x00040020u,0x0000018bu,0x00000007u,0x00000094u,
    0x0003001du,0x00000195u,0x00000009u,0x0003001eu,0x00000196u,0x00000195u,0x00040020u,0x00000197u,
    0x00000002u,0x00000196u,0x0004003bu,0x00000197u,0x00000198u,0x00000002u,0x0004002bu,0x00000015u,
    0x00000199u,0x00000003u,0x00040020u,0x0000019au,0x00000009u,0x00000015u,0x0004002bu,0x00000094u,
    0x0000019fu,0x00000008u,0x0004002bu,0x00000094u,0x000001a1u,0x000000ffu,0x00040020u,0x000001a4u,
    0x00000002u,0x00000009u,0x0004002bu,0x00000015u,0x000001a8u,0x00000007u,0x00060034u,0x00000072u,
    0x000001a9u,0x000000aau,0x000000ecu,0x000001a8u,0x0004002bu,0x00000015u,0x000001aau,0x00000008u,
    0x00060034u,0x00000072u,0x000001abu,0x000000aau,0x000000ecu,0x000001aau,0x00060034u,0x00000072u,
    0x000001acu,0x000000a6u,0x000001a9u,0x000001abu,0x00040020u,0x000001dfu,0x00000001u,0x00000094u,
    0x0004003bu,0x000001dfu,0x000001e0u,0x00000001u,0x00040032u,0x00000015u,0x000001efu,0x00000000u,
    0x00060034u,0x00000072u,0x000001f0u,0x000000abu,0x000001efu,0x00000016u,0x0005002cu,0x00000007u,
    0x000001f9u,0x000000abu,0x000000abu,0x0004002bu,0x00000006u,0x000001fcu,0x3fc00000u,0x0005002cu,
    0x00000007u,0x000001fdu,0x000001fcu,0x000001fcu,0x0004002bu,0x00000015u,0x00000201u,0x00000004u,
    0x00060034u,0x00000072u,0x00000202u,0x000000aau,0x000000ecu,0x00000201u,0x0004002bu,0x00000015u,
    0x00000209u,0xffffffffu,0x00060034u,0x00000072u,0x0000020au,0x000000aau,0x0000011eu,0x00000209u,
    0x00060034u,0x00000072u,0x00000212u,0x000000aau,0x0000011eu,0x0000011fu,0x00060034u,0x00000072u,
    0x00000228u,0x000000b1u,0x0000011eu,0x00000139u,0x00040032u,0x00000015u,0x00000231u,0x00000001u,
    0x00060034u,0x00000072u,0x00000232u,0x000000aau,0x00000231u,0x00000016u,0x0004002bu,0x00000006u,
    0x00000238u,0x3f008081u,0x00060034u,0x00000072u,0x00000241u,0x000000aau,0x000000ecu,0x00000016u,
    0x0004002bu,0x00000015u,0x00000248u,0x00000001u,0x00060034u,0x00000072u,0x00000249u,0x000000aau,
    0x000000ecu,0x00000248u,0x00060034u,0x00000072u,0x0000024eu,0x000000aau,0x000000ecu,0x00000139u,
    0x00040032u,0x00000015u,0x00000273u,0x00000000u,0x00060034u,0x00000072u,0x00000274u,0x000000aau,
    0x00000273u,0x00000248u,0x00060034u,0x00000072u,0x0000027eu,0x000000aau,0x00000273u,0x00000139u,
    0x0004002bu,0x00000006u,0x00000286u,0x42400000u,0x00040032u,0x00000015u,0x0000028du,0x00000000u,
    0x0004002bu,0x00000006u,0x0000028eu,0x3b808081u,0x0006002cu,0x00000025u,0x0000028fu,0x00000021u,
    0x00000021u,0x00000021u,0x0006002cu,0x00000025u,0x00000290u,0x00000143u,0x00000143u,0x00000143u,
    0x00050036u,0x00000002u,0x00000004u,0x00000000u,0x00000003u,0x000200f8u,0x00000005u,0x0004003bu,
    0x00000023u,0x000000f1u,0x00000007u,0x0004003bu,0x00000008u,0x00000104u,0x00000007u,0x0004003bu,
    0x00000023u,0x0000010fu,0x00000007u,0x0004003bu,0x0000001du,0x0000011du,0x00000007u,0x0004003bu,
    0x0000001du,0x00000121u,0x00000007u,0x0004003bu,0x0000001du,0x0000012fu,0x00000007u,0x0004003bu,
    0x000000a9u,0x00000140u,0x00000007u,0x0004003bu,0x000000a9u,0x0000014du,0x00000007u,0x0004003bu,
    0x000000a9u,0x00000157u,0x00000007u,0x0004003bu,0x00000023u,0x00000183u,0x00000007u,0x0004003bu,
    0x0000018bu,0x0000018cu,0x00000007u,0x0004003bu,0x00000023u,0x000001bdu,0x00000007u,0x0004003bu,
    0x00000023u,0x000001c1u,0x00000007u,0x0004003bu,0x00000023u,0x000001cdu,0x00000007u,0x0004003bu,
    0x00000008u,0x000001d8u,0x00000007u,0x0004003bu,0x0000001du,0x000001e5u,0x00000007u,0x0004003bu,
    0x00000008u,0x000001f3u,0x00000007u,0x0004003bu,0x00000008u,0x00000205u,0x00000007u,0x0004003bu,
    0x0000001du,0x0000020du,0x00000007u,0x0004003bu,0x0000001du,0x0000021fu,0x00000007u,0x0004003bu,
    0x0000001du,0x00000235u,0x00000007u,0x000300f7u,0x000000f0u,0x00000000u,0x000400fau,0x000000eeu,
    0x000000efu,0x000000f0u,0x000200f8u,0x000000efu,0x0004003du,0x00000011u,0x000000f2u,0x00000013u,
    0x0004003du,0x00000009u,0x000000f5u,0x000000f4u,0x0007004fu,0x00000007u,0x000000f6u,0x000000f5u,
    0x000000f5u,0x00000000u,0x00000001u,0x0004006eu,0x00000018u,0x000000f7u,0x000000f6u,0x00040064u,
    0x00000010u,0x000000f8u,0x000000f2u,0x0007005fu,0x00000009u,0x000000f9u,0x000000f8u,0x000000f7u,
    0x00000002u,0x00000016u,0x00050051u,0x00000006u,0x000000fau,0x000000f9u,0x00000001u,0x0003003eu,
    0x000000f1u,0x000000fau,0x0004003du,0x00000006u,0x000000fdu,0x000000f1u,0x00070050u,0x00000009u,
    0x000000feu,0x000000fdu,0x000000fdu,0x000000fdu,0x0000000fu,0x0003003eu,0x000000fcu,0x000000feu,
    0x000100fdu,0x000200f8u,0x000000f0u,0x000300f7u,0x00000103u,0x00000000u,0x000400fau,0x00000101u,
    0x00000102u,0x00000103u,0x000200f8u,0x00000102u,0x0004003du,0x00000025u,0x00000107u,0x00000106u,
    0x0007004fu,0x00000007u,0x00000108u,0x00000107u,0x00000107u,0x00000000u,0x00000001u,0x00050041u,
    0x0000010au,0x0000010bu,0x00000106u,0x00000109u,0x0004003du,0x00000006u,0x0000010cu,0x0000010bu,
    0x00050050u,0x00000007u,0x0000010du,0x0000010cu,0x0000010cu,0x00050088u,0x00000007u,0x0000010eu,
    0x00000108u,0x0000010du,0x0003003eu,0x00000104u,0x0000010eu,0x00050041u,0x0000010au,0x00000110u,
    0x00000106u,0x00000109u,0x0004003du,0x00000006u,0x00000111u,0x00000110u,0x0006000cu,0x00000006u,
    0x00000112u,0x00000001u,0x00000004u,0x00000111u,0x0006000cu,0x00000006u,0x00000113u,0x00000001u,
    0x0000001eu,0x00000112u,0x0004007fu,0x00000006u,0x00000114u,0x00000113u,0x00050041u,0x0000010au,
    0x00000117u,0x00000116u,0x00000095u,0x0004003du,0x00000006u,0x00000118u,0x00000117u,0x00050085u,
    0x00000006u,0x00000119u,0x00000114u,0x00000118u,0x00050041u,0x0000010au,0x0000011au,0x00000116u,
    0x00000099u,0x0004003du,0x00000006u,0x0000011bu,0x0000011au,0x00050081u,0x00000006u,0x0000011cu,
    0x00000119u,0x0000011bu,0x0003003eu,0x0000010fu,0x0000011cu,0x000300f7u,0x00000123u,0x00000000u,
    0x000400fau,0x00000120u,0x00000122u,0x0000012cu,0x000200f8u,0x00000122u,0x0004003du,0x00000011u,
    0x00000125u,0x00000124u,0x0004003du,0x00000007u,0x00000126u,0x00000104u,0x0004003du,0x00000007u,
    0x00000127u,0x00000104u,0x000400cfu,0x00000007u,0x00000128u,0x00000127u,0x0004003du,0x00000007u,
    0x00000129u,0x00000104u,0x000400d0u,0x00000007u,0x0000012au,0x00000129u,0x00080058u,0x00000009u,
    0x0000012bu,0x00000125u,0x00000126u,0x00000004u,0x00000128u,0x0000012au,0x0003003eu,0x00000121u,
    0x0000012bu,0x000200f9u,0x00000123u,0x000200f8u,0x0000012cu,0x0004003du,0x00000006u,0x0000012du,
    0x0000010fu,0x000500bcu,0x00000072u,0x0000012eu,0x0000012du,0x00000021u,0x000300f7u,0x00000131u,
    0x00000000u,0x000400fau,0x0000012eu,0x00000130u,0x00000136u,0x000200f8u,0x00000130u,0x0004003du,
    0x00000011u,0x00000133u,0x00000132u,0x0004003du,0x00000007u,0x00000134u,0x00000104u,0x00070058u,
    0x00000009u,0x00000135u,0x00000133u,0x00000134u,0x00000002u,0x00000021u,0x0003003eu,0x0000012fu,
    0x00000135u,0x000200f9u,0x00000131u,0x000200f8u,0x00000136u,0x0004003du,0x00000011u,0x00000137u,
    0x00000124u,0x0004003du,0x00000007u,0x00000138u,0x00000104u,0x0004003du,0x00000006u,0x0000013bu,
    0x0000010fu,0x000600a9u,0x00000006u,0x0000013cu,0x0000013au,0x00000021u,0x0000013bu,0x00070058u,
    0x00000009u,0x0000013du,0x00000137u,0x00000138u,0x00000002u,0x0000013cu,0x0003003eu,0x0000012fu,
    0x0000013du,0x000200f9u,0x00000131u,0x000200f8u,0x00000131u,0x0004003du,0x00000009u,0x0000013eu,
    0x0000012fu,0x0003003eu,0x00000121u,0x0000013eu,0x000200f9u,0x00000123u,0x000200f8u,0x00000123u,
    0x0004003du,0x00000009u,0x0000013fu,0x00000121u,0x0003003eu,0x0000011du,0x0000013fu,0x0004003du,
    0x00000009u,0x00000141u,0x0000011du,0x0008004fu,0x00000025u,0x00000142u,0x00000141u,0x00000141u,
    0x00000000u,0x00000001u,0x00000002u,0x0005008eu,0x00000025u,0x00000144u,0x00000142u,0x00000143u,
    0x0004003du,0x00000009u,0x00000146u,0x00000145u,0x0008004fu,0x00000025u,0x00000147u,0x00000146u,
    0x00000146u,0x00000000u,0x00000001u,0x00000002u,0x00050085u,0x00000025u,0x00000148u,0x00000144u,
    0x00000147u,0x00060050u,0x00000025u,0x00000149u,0x00000021u,0x00000021u,0x00000021u,0x00060050u,
    0x00000025u,0x0000014au,0x00000143u,0x00000143u,0x00000143u,0x0008000cu,0x00000025u,0x0000014bu,
    0x00000001u,0x0000002bu,0x00000148u,0x00000149u,0x0000014au,0x0006000cu,0x00000025u,0x0000014cu,
    0x00000001u,0x00000008u,0x0000014bu,0x0003003eu,0x00000140u,0x0000014cu,0x0004003du,0x00000011u,
    0x0000014eu,0x00000013u,0x0004003du,0x00000009u,0x0000014fu,0x000000f4u,0x0007004fu,0x00000007u,
    0x00000150u,0x0000014fu,0x0000014fu,0x00000000u,0x00000001u,0x0004006eu,0x00000018u,0x00000151u,
    0x00000150u,0x00040064u,0x00000010u,0x00000152u,0x0000014eu,0x0007005fu,0x00000009u,0x00000153u,
    0x00000152u,0x00000151u,0x00000002u,0x00000016u,0x0008004fu,0x00000025u,0x00000154u,0x00000153u,
    0x00000153u,0x00000000u,0x00000001u,0x00000002u,0x0005008eu,0x00000025u,0x00000155u,0x00000154u,
    0x00000143u,0x0006000cu,0x00000025u,0x00000156u,0x00000001u,0x00000001u,0x00000155u,0x0003003eu,
    0x0000014du,0x00000156u,0x0004003du,0x00000025u,0x00000158u,0x0000014du,0x0005008eu,0x00000025u,
    0x0000015au,0x00000158u,0x00000159u,0x0004003du,0x00000025u,0x0000015bu,0x0000014du,0x0004003du,
    0x00000025u,0x0000015cu,0x00000140u,0x00050083u,0x00000025u,0x0000015du,0x0000015bu,0x0000015cu,
    0x00050041u,0x00000161u,0x00000162u,0x00000160u,0x00000139u,0x0004003du,0x00000006u,0x00000163u,
    0x00000162u,0x00050085u,0x00000006u,0x00000164u,0x00000163u,0x00000159u,0x0005008eu,0x00000025u,
    0x00000165u,0x0000015du,0x00000164u,0x00050081u,0x00000025u,0x00000166u,0x0000015au,0x00000165u,
    0x00060050u,0x00000025u,0x00000167u,0x00000159u,0x00000159u,0x00000159u,0x00050088u,0x00000025u,
    0x00000168u,0x00000166u,0x00000167u,0x0006000cu,0x00000025u,0x00000169u,0x00000001u,0x00000008u,
    0x00000168u,0x0003003eu,0x00000157u,0x00000169u,0x0004003du,0x00000025u,0x0000016au,0x00000157u,
    0x00060050u,0x00000025u,0x0000016bu,0x00000021u,0x00000021u,0x00000021u,0x00060050u,0x00000025u,
    0x0000016cu,0x00000143u,0x00000143u,0x00000143u,0x0008000cu,0x00000025u,0x0000016du,0x00000001u,
    0x0000002bu,0x0000016au,0x0000016bu,0x0000016cu,0x00060050u,0x00000025u,0x0000016eu,0x00000143u,
    0x00000143u,0x00000143u,0x00050088u,0x00000025u,0x0000016fu,0x0000016du,0x0000016eu,0x00050041u,
    0x00000023u,0x00000170u,0x0000011du,0x000000e3u,0x0004003du,0x00000006u,0x00000171u,0x00000170u,
    0x00050041u,0x0000010au,0x00000172u,0x00000145u,0x000000e3u,0x0004003du,0x00000006u,0x00000173u,
    0x00000172u,0x00050085u,0x00000006u,0x00000174u,0x00000171u,0x00000173u,0x00050085u,0x00000006u,
    0x00000176u,0x00000174u,0x00000175u,0x00050051u,0x00000006u,0x00000177u,0x0000016fu,0x00000000u,
    0x00050051u,0x00000006u,0x00000178u,0x0000016fu,0x00000001u,0x00050051u,0x00000006u,0x00000179u,
    0x0000016fu,0x00000002u,0x00070050u,0x00000009u,0x0000017au,0x00000177u,0x00000178u,0x00000179u,
    0x00000176u,0x0003003eu,0x000000fcu,0x0000017au,0x000100fdu,0x000200f8u,0x00000103u,0x000300f7u,
    0x00000182u,0x00000000u,0x000400fau,0x00000180u,0x00000181u,0x00000182u,0x000200f8u,0x00000181u,
    0x0004003du,0x00000011u,0x00000184u,0x00000124u,0x0004003du,0x00000009u,0x00000185u,0x000000f4u,
    0x0007004fu,0x00000007u,0x00000186u,0x00000185u,0x00000185u,0x00000000u,0x00000001u,0x0004006eu,
    0x00000018u,0x00000187u,0x00000186u,0x00040064u,0x00000010u,0x00000188u,0x00000184u,0x0007005fu,
    0x00000009u,0x00000189u,0x00000188u,0x00000187u,0x00000002u,0x00000016u,0x00050051u,0x00000006u,
    0x0000018au,0x00000189u,0x00000000u,0x0003003eu,0x00000183u,0x0000018au,0x0004003du,0x00000006u,
    0x0000018du,0x00000183u,0x00050083u,0x00000006u,0x0000018eu,0x0000000fu,0x0000018du,0x0008000cu,
    0x00000006u,0x0000018fu,0x00000001u,0x0000002bu,0x0000018eu,0x00000021u,0x0000000fu,0x00050041u,
    0x00000161u,0x00000190u,0x00000160u,0x00000139u,0x0004003du,0x00000006u,0x00000191u,0x00000190u,
    0x00050085u,0x00000006u,0x00000192u,0x0000018fu,0x00000191u,0x0006000cu,0x00000006u,0x00000193u,
    0x00000001u,0x00000001u,0x00000192u,0x0004006du,0x00000094u,0x00000194u,0x00000193u,0x0003003eu,
    0x0000018cu,0x00000194u,0x00050041u,0x0000019au,0x0000019bu,0x00000160u,0x00000199u,0x0004003du,
    0x00000015u,0x0000019cu,0x0000019bu,0x0004007cu,0x00000094u,0x0000019du,0x0000019cu,0x0004003du,
    0x00000094u,0x0000019eu,0x0000018cu,0x000500c2u,0x00000094u,0x000001a0u,0x0000019eu,0x0000019fu,
    0x0007000cu,0x00000094u,0x000001a2u,0x00000001u,0x00000026u,0x000001a0u,0x000001a1u,0x00050080u,
    0x00000094u,0x000001a3u,0x0000019du,0x000001a2u,0x00060041u,0x000001a4u,0x000001a5u,0x00000198u,
    0x00000016u,0x000001a3u,0x0004003du,0x00000009u,0x000001a6u,0x000001a5u,0x0003003eu,0x000000fcu,
    0x000001a6u,0x000100fdu,0x000200f8u,0x00000182u,0x000300f7u,0x000001aeu,0x00000000u,0x000400fau,
    0x000001acu,0x000001adu,0x000001aeu,0x000200f8u,0x000001adu,0x0004003du,0x00000011u,0x000001afu,
    0x00000013u,0x0004003du,0x00000025u,0x000001b0u,0x00000106u,0x0007004fu,0x00000007u,0x000001b1u,
    0x000001b0u,0x000001b0u,0x00000000u,0x00000001u,0x00050041u,0x0000010au,0x000001b2u,0x00000106u,
    0x00000109u,0x0004003du,0x00000006u,0x000001b3u,0x000001b2u,0x00050050u,0x00000007u,0x000001b4u,
    0x000001b3u,0x000001b3u,0x00050088u,0x00000007u,0x000001b5u,0x000001b1u,0x000001b4u,0x00070058u,
    0x00000009u,0x000001b6u,0x000001afu,0x000001b5u,0x00000002u,0x00000021u,0x0008004fu,0x00000025u,
    0x000001b7u,0x000001b6u,0x000001b6u,0x00000000u,0x00000001u,0x00000002u,0x00050051u,0x00000006u,
    0x000001b8u,0x000001b7u,0x00000000u,0x00050051u,0x00000006u,0x000001b9u,0x000001b7u,0x00000001u,
    0x00050051u,0x00000006u,0x000001bau,0x000001b7u,0x00000002u,0x00070050u,0x00000009u,0x000001bbu,
    0x000001b8u,0x000001b9u,0x000001bau,0x0000000fu,0x0003003eu,0x000000fcu,0x000001bbu,0x000100fdu,
    0x000200f8u,0x000001aeu,0x00050041u,0x0000010au,0x000001beu,0x00000116u,0x00000095u,0x0004003du,
    0x00000006u,0x000001bfu,0x000001beu,0x000500b4u,0x00000072u,0x000001c0u,0x000001bfu,0x00000021u,
    0x000300f7u,0x000001c3u,0x00000000u,0x000400fau,0x000001c0u,0x000001c2u,0x000001c6u,0x000200f8u,
    0x000001c2u,0x00050041u,0x0000010au,0x000001c4u,0x00000106u,0x00000109u,0x0004003du,0x00000006u,
    0x000001c5u,0x000001c4u,0x0003003eu,0x000001c1u,0x000001c5u,0x000200f9u,0x000001c3u,0x000200f8u,
    0x000001c6u,0x00050041u,0x0000010au,0x000001c7u,0x000000f4u,0x000000e3u,0x0004003du,0x00000006u,
    0x000001c8u,0x000001c7u,0x00050041u,0x0000010au,0x000001c9u,0x00000116u,0x00000095u,0x0004003du,
    0x00000006u,0x000001cau,0x000001c9u,0x00050085u,0x00000006u,0x000001cbu,0x000001c8u,0x000001cau,
    0x0003003eu,0x000001c1u,0x000001cbu,0x000200f9u,0x000001c3u,0x000200f8u,0x000001c3u,0x0004003du,
    0x00000006u,0x000001ccu,0x000001c1u,0x0003003eu,0x000001bdu,0x000001ccu,0x0004003du,0x00000006u,
    0x000001ceu,0x000001bdu,0x0006000cu,0x00000006u,0x000001cfu,0x00000001u,0x00000004u,0x000001ceu,
    0x0006000cu,0x00000006u,0x000001d0u,0x00000001u,0x0000001eu,0x000001cfu,0x0004007fu,0x00000006u,
    0x000001d1u,0x000001d0u,0x00050041u,0x00000161u,0x000001d2u,0x00000160u,0x00000139u,0x0004003du,
    0x00000006u,0x000001d3u,0x000001d2u,0x00050085u,0x00000006u,0x000001d4u,0x000001d1u,0x000001d3u,
    0x00050041u,0x0000010au,0x000001d5u,0x00000116u,0x00000099u,0x0004003du,0x00000006u,0x000001d6u,
    0x000001d5u,0x00050081u,0x00000006u,0x000001d7u,0x000001d4u,0x000001d6u,0x0003003eu,0x000001cdu,
    0x000001d7u,0x0004003du,0x00000025u,0x000001d9u,0x00000106u,0x0007004fu,0x00000007u,0x000001dau,
    0x000001d9u,0x000001d9u,0x00000000u,0x00000001u,0x00050041u,0x0000010au,0x000001dbu,0x00000106u,
    0x00000109u,0x0004003du,0x00000006u,0x000001dcu,0x000001dbu,0x00050050u,0x00000007u,0x000001ddu,
    0x000001dcu,0x000001dcu,0x00050088u,0x00000007u,0x000001deu,0x000001dau,0x000001ddu,0x0003003eu,
    0x000001d8u,0x000001deu,0x0004003du,0x00000094u,0x000001e1u,0x000001e0u,0x000500abu,0x00000072u,
    0x000001e2u,0x000001e1u,0x00000095u,0x000300f7u,0x000001e4u,0x00000000u,0x000400fau,0x000001e2u,
    0x000001e3u,0x000001e4u,0x000200f8u,0x000001e3u,0x0004003du,0x00000094u,0x000001e6u,0x000001e0u,
    0x00060041u,0x000001a4u,0x000001e7u,0x00000198u,0x00000016u,0x000001e6u,0x0004003du,0x00000009u,
    0x000001e8u,0x000001e7u,0x0003003eu,0x000001e5u,0x000001e8u,0x0004003du,0x00000007u,0x000001e9u,
    0x000001d8u,0x0004003du,0x00000009u,0x000001eau,0x000001e5u,0x0007004fu,0x00000007u,0x000001ebu,
    0x000001eau,0x000001eau,0x00000000u,0x00000001u,0x0004003du,0x00000009u,0x000001ecu,0x000001e5u,
    0x0007004fu,0x00000007u,0x000001edu,0x000001ecu,0x000001ecu,0x00000002u,0x00000003u,0x0008000cu,
    0x00000007u,0x000001eeu,0x00000001u,0x0000002bu,0x000001e9u,0x000001ebu,0x000001edu,0x0003003eu,
    0x000001d8u,0x000001eeu,0x000200f9u,0x000001e4u,0x000200f8u,0x000001e4u,0x000300f7u,0x000001f2u,
    0x00000000u,0x000400fau,0x000001f0u,0x000001f1u,0x000001f2u,0x000200f8u,0x000001f1u,0x0004003du,
    0x00000011u,0x000001f4u,0x00000124u,0x00040064u,0x00000010u,0x000001f5u,0x000001f4u,0x00050067u,
    0x00000018u,0x000001f6u,0x000001f5u,0x00000016u,0x0004006fu,0x00000007u,0x000001f7u,0x000001f6u,
    0x0003003eu,0x000001f3u,0x000001f7u,0x0004003du,0x00000007u,0x000001f8u,0x000001d8u,0x0004003du,
    0x00000007u,0x000001fau,0x000001f3u,0x00050088u,0x00000007u,0x000001fbu,0x000001f9u,0x000001fau,
    0x0004003du,0x00000007u,0x000001feu,0x000001f3u,0x00050088u,0x00000007u,0x000001ffu,0x000001fdu,
    0x000001feu,0x0008000cu,0x00000007u,0x00000200u,0x00000001u,0x0000002bu,0x000001f8u,0x000001fbu,
    0x000001ffu,0x0003003eu,0x000001d8u,0x00000200u,0x000200f9u,0x000001f2u,0x000200f8u,0x000001f2u,
    0x000300f7u,0x00000204u,0x00000000u,0x000400fau,0x00000202u,0x00000203u,0x00000204u,0x000200f8u,
    0x00000203u,0x0004003du,0x00000007u,0x00000206u,0x000001d8u,0x0003003eu,0x00000205u,0x00000206u,
    0x00050039u,0x00000009u,0x00000207u,0x0000000cu,0x00000205u,0x0003003eu,0x000000fcu,0x00000207u,
    0x000100fdu,0x000200f8u,0x00000204u,0x000300f7u,0x0000020cu,0x00000000u,0x000400fau,0x0000020au,
    0x0000020bu,0x00000211u,0x000200f8u,0x0000020bu,0x0004003du,0x00000011u,0x0000020eu,0x00000013u,
    0x0004003du,0x00000007u,0x0000020fu,0x000001d8u,0x00070058u,0x00000009u,0x00000210u,0x0000020eu,
    0x0000020fu,0x00000002u,0x00000021u,0x0003003eu,0x0000020du,0x00000210u,0x000200f9u,0x0000020cu,
    0x000200f8u,0x00000211u,0x000300f7u,0x00000214u,0x00000000u,0x000400fau,0x00000212u,0x00000213u,
    0x0000021cu,0x000200f8u,0x00000213u,0x0004003du,0x00000011u,0x00000215u,0x00000124u,0x0004003du,
    0x00000007u,0x00000216u,0x000001d8u,0x0004003du,0x00000007u,0x00000217u,0x000001d8u,0x000400cfu,
    0x00000007u,0x00000218u,0x00000217u,0x0004003du,0x00000007u,0x00000219u,0x000001d8u,0x000400d0u,
    0x00000007u,0x0000021au,0x00000219u,0x00080058u,0x00000009u,0x0000021bu,0x00000215u,0x00000216u,
    0x00000004u,0x00000218u,0x0000021au,0x0003003eu,0x0000020du,0x0000021bu,0x000200f9u,0x00000214u,
    0x000200f8u,0x0000021cu,0x0004003du,0x00000006u,0x0000021du,0x000001cdu,0x000500bcu,0x00000072u,
    0x0000021eu,0x0000021du,0x00000021u,0x000300f7u,0x00000221u,0x00000000u,0x000400fau,0x0000021eu,
    0x00000220u,0x00000225u,0x000200f8u,0x00000220u,0x0004003du,0x00000011u,0x00000222u,0x00000132u,
    0x0004003du,0x00000007u,0x00000223u,0x000001d8u,0x00070058u,0x00000009u,0x00000224u,0x00000222u,
    0x00000223u,0x00000002u,0x00000021u,0x0003003eu,0x0000021fu,0x00000224u,0x000200f9u,0x00000221u,
    0x000200f8u,0x00000225u,0x0004003du,0x00000011u,0x00000226u,0x00000124u,0x0004003du,0x00000007u,
    0x00000227u,0x000001d8u,0x0004003du,0x00000006u,0x00000229u,0x000001cdu,0x000600a9u,0x00000006u,
    0x0000022au,0x00000228u,0x00000021u,0x00000229u,0x00070058u,0x00000009u,0x0000022bu,0x00000226u,
    0x00000227u,0x00000002u,0x0000022au,0x0003003eu,0x0000021fu,0x0000022bu,0x000200f9u,0x00000221u,
    0x000200f8u,0x00000221u,0x0004003du,0x00000009u,0x0000022cu,0x0000021fu,0x0003003eu,0x0000020du,
    0x0000022cu,0x000200f9u,0x00000214u,0x000200f8u,0x00000214u,0x000200f9u,0x0000020cu,0x000200f8u,
    0x0000020cu,0x00050041u,0x00000023u,0x0000022du,0x0000020du,0x000000e3u,0x0004003du,0x00000006u,
    0x0000022eu,0x0000022du,0x00050085u,0x00000006u,0x0000022fu,0x0000022eu,0x00000175u,0x00050041u,
    0x00000023u,0x00000230u,0x0000020du,0x000000e3u,0x0003003eu,0x00000230u,0x0000022fu,0x000300f7u,
    0x00000234u,0x00000000u,0x000400fau,0x00000232u,0x00000233u,0x00000240u,0x000200f8u,0x00000233u,
    0x0004003du,0x00000009u,0x00000236u,0x00000145u,0x0008004fu,0x00000025u,0x00000237u,0x00000236u,
    0x00000236u,0x00000000u,0x00000001u,0x00000002u,0x0005008eu,0x00000025u,0x00000239u,0x00000237u,
    0x00000238u,0x00050041u,0x0000010au,0x0000023au,0x00000145u,0x000000e3u,0x0004003du,0x00000006u,
    0x0000023bu,0x0000023au,0x00050051u,0x00000006u,0x0000023cu,0x00000239u,0x00000000u,0x00050051u,
    0x00000006u,0x0000023du,0x00000239u,0x00000001u,0x00050051u,0x00000006u,0x0000023eu,0x00000239u,
    0x00000002u,0x00070050u,0x00000009u,0x0000023fu,0x0000023cu,0x0000023du,0x0000023eu,0x0000023bu,
    0x0003003eu,0x00000235u,0x0000023fu,0x000200f9u,0x00000234u,0x000200f8u,0x00000240u,0x000300f7u,
    0x00000243u,0x00000000u,0x000400fau,0x00000241u,0x00000242u,0x00000247u,0x000200f8u,0x00000242u,
    0x0004003du,0x00000009u,0x00000244u,0x0000020du,0x0004003du,0x00000009u,0x00000245u,0x00000145u,
    0x00050085u,0x00000009u,0x00000246u,0x00000244u,0x00000245u,0x0003003eu,0x00000235u,0x00000246u,
    0x000200f9u,0x00000243u,0x000200f8u,0x00000247u,0x000300f7u,0x0000024bu,0x00000000u,0x000400fau,
    0x00000249u,0x0000024au,0x0000024du,0x000200f8u,0x0000024au,0x0004003du,0x00000009u,0x0000024cu,
    0x0000020du,0x0003003eu,0x00000235u,0x0000024cu,0x000200f9u,0x0000024bu,0x000200f8u,0x0000024du,
    0x000300f7u,0x00000250u,0x00000000u,0x000400fau,0x0000024eu,0x0000024fu,0x00000263u,0x000200f8u,
    0x0000024fu,0x0004003du,0x00000009u,0x00000251u,0x0000020du,0x0008004fu,0x00000025u,0x00000252u,
    0x00000251u,0x00000251u,0x00000000u,0x00000001u,0x00000002u,0x0004003du,0x00000009u,0x00000253u,
    0x00000145u,0x0008004fu,0x00000025u,0x00000254u,0x00000253u,0x00000253u,0x00000000u,0x00000001u,
    0x00000002u,0x00050085u,0x00000025u,0x00000255u,0x00000252u,0x00000254u,0x00050041u,0x0000010au,
    0x00000256u,0x00000145u,0x000000e3u,0x0004003du,0x00000006u,0x00000257u,0x00000256u,0x00060050u,
    0x00000025u,0x00000258u,0x00000257u,0x00000257u,0x00000257u,0x00050081u,0x00000025u,0x00000259u,
    0x00000255u,0x00000258u,0x00050041u,0x00000023u,0x0000025au,0x0000020du,0x000000e3u,0x0004003du,
    0x00000006u,0x0000025bu,0x0000025au,0x00050041u,0x0000010au,0x0000025cu,0x00000145u,0x000000e3u,
    0x0004003du,0x00000006u,0x0000025du,0x0000025cu,0x00050081u,0x00000006u,0x0000025eu,0x0000025bu,
    0x0000025du,0x00050051u,0x00000006u,0x0000025fu,0x00000259u,0x00000000u,0x00050051u,0x00000006u,
    0x00000260u,0x00000259u,0x00000001u,0x00050051u,0x00000006u,0x00000261u,0x00000259u,0x00000002u,
    0x00070050u,0x00000009u,0x00000262u,0x0000025fu,0x00000260u,0x00000261u,0x0000025eu,0x0003003eu,
    0x00000235u,0x00000262u,0x000200f9u,0x00000250u,0x000200f8u,0x00000263u,0x0004003du,0x00000009u,
    0x00000264u,0x0000020du,0x0008004fu,0x00000025u,0x00000265u,0x00000264u,0x00000264u,0x00000000u,
    0x00000001u,0x00000002u,0x0004003du,0x00000009u,0x00000266u,0x00000145u,0x0008004fu,0x00000025u,
    0x00000267u,0x00000266u,0x00000266u,0x00000000u,0x00000001u,0x00000002u,0x00050085u,0x00000025u,
    0x00000268u,0x00000265u,0x00000267u,0x00050041u,0x0000010au,0x00000269u,0x00000145u,0x000000e3u,
    0x0004003du,0x00000006u,0x0000026au,0x00000269u,0x00060050u,0x00000025u,0x0000026bu,0x0000026au,
    0x0000026au,0x0000026au,0x00050081u,0x00000025u,0x0000026cu,0x00000268u,0x0000026bu,0x00050041u,
    0x00000023u,0x0000026du,0x0000020du,0x000000e3u,0x0004003du,0x00000006u,0x0000026eu,0x0000026du,
    0x00050051u,0x00000006u,0x0000026fu,0x0000026cu,0x00000000u,0x00050051u,0x00000006u,0x00000270u,
    0x0000026cu,0x00000001u,0x00050051u,0x00000006u,0x00000271u,0x0000026cu,0x00000002u,0x00070050u,
    0x00000009u,0x00000272u,0x0000026fu,0x00000270u,0x00000271u,0x0000026eu,0x0003003eu,0x00000235u,
    0x00000272u,0x000200f9u,0x00000250u,0x000200f8u,0x00000250u,0x000200f9u,0x0000024bu,0x000200f8u,
    0x0000024bu,0x000200f9u,0x00000243u,0x000200f8u,0x00000243u,0x000200f9u,0x00000234u,0x000200f8u,
    0x00000234u,0x000300f7u,0x00000276u,0x00000000u,0x000400fau,0x00000274u,0x00000275u,0x00000276u,
    0x000200f8u,0x00000275u,0x00050041u,0x00000023u,0x00000277u,0x00000235u,0x000000e3u,0x0004003du,
    0x00000006u,0x00000278u,0x00000277u,0x000500b4u,0x00000072u,0x00000279u,0x00000278u,0x00000021u,
    0x000200f9u,0x00000276u,0x000200f8u,0x00000276u,0x000700f5u,0x00000072u,0x0000027au,0x00000274u,
    0x00000234u,0x00000279u,0x00000275u,0x000300f7u,0x0000027cu,0x00000000u,0x000400fau,0x0000027au,
    0x0000027bu,0x0000027cu,0x000200f8u,0x0000027bu,0x000100fcu,0x000200f8u,0x0000027cu,0x000300f7u,
    0x00000280u,0x00000000u,0x000400fau,0x0000027eu,0x0000027fu,0x00000280u,0x000200f8u,0x0000027fu,
    0x00050041u,0x00000023u,0x00000281u,0x00000235u,0x000000e3u,0x0004003du,0x00000006u,0x00000282u,
    0x00000281u,0x00050085u,0x00000006u,0x00000283u,0x00000282u,0x00000159u,0x0008000cu,0x00000006u,
    0x00000284u,0x00000001u,0x0000002bu,0x00000283u,0x00000021u,0x00000143u,0x0006000cu,0x00000006u,
    0x00000285u,0x00000001u,0x00000008u,0x00000284u,0x000500bcu,0x00000072u,0x00000287u,0x00000285u,
    0x00000286u,0x000200f9u,0x00000280u,0x000200f8u,0x00000280u,0x000700f5u,0x00000072u,0x00000288u,
    0x0000027eu,0x0000027cu,0x00000287u,0x0000027fu,0x000300f7u,0x0000028au,0x00000000u,0x000400fau,
    0x00000288u,0x00000289u,0x0000028au,0x000200f8u,0x00000289u,0x000100fcu,0x000200f8u,0x0000028au,
    0x0004003du,0x00000009u,0x0000028cu,0x00000235u,0x000500aau,0x00000072u,0x00000291u,0x0000028du,
    0x00000248u,0x0008004fu,0x00000025u,0x00000292u,0x0000028cu,0x0000028cu,0x00000000u,0x00000001u,
    0x00000002u,0x00050051u,0x00000006u,0x00000293u,0x0000028cu,0x00000003u,0x0005008eu,0x00000025u,
    0x00000294u,0x00000292u,0x00000143u,0x0008000cu,0x00000025u,0x00000295u,0x00000001u,0x0000002bu,
    0x00000294u,0x0000028fu,0x00000290u,0x0006000cu,0x00000025u,0x00000296u,0x00000001u,0x00000008u,
    0x00000295u,0x0005008eu,0x00000025u,0x00000297u,0x00000296u,0x0000028eu,0x00050085u,0x00000006u,
    0x00000298u,0x00000293u,0x00000159u,0x0008000cu,0x00000006u,0x00000299u,0x00000001u,0x0000002bu,
    0x00000298u,0x00000021u,0x00000143u,0x0006000cu,0x00000006u,0x0000029au,0x00000001u,0x00000008u,
    0x00000299u,0x00050088u,0x00000006u,0x0000029bu,0x0000029au,0x00000159u,0x0005008eu,0x00000025u,
    0x0000029cu,0x00000297u,0x0000029bu,0x00050050u,0x00000009u,0x0000029du,0x0000029cu,0x00000293u,
    0x000600a9u,0x00000009u,0x0000029eu,0x00000291u,0x0000029du,0x0000028cu,0x0003003eu,0x000000fcu,
    0x0000029eu,0x000100fdu,0x00010038u,0x00050036u,0x00000009u,0x0000000cu,0x00000000u,0x0000000au,
    0x00030037u,0x00000008u,0x0000000bu,0x000200f8u,0x0000000du,0x0004003bu,0x00000008u,0x0000000eu,
    0x00000007u,0x0004003bu,0x0000001du,0x0000001eu,0x00000007u,0x0004003bu,0x00000023u,0x00000024u,
    0x00000007u,0x0004003bu,0x00000023u,0x0000002du,0x00000007u,0x0004003bu,0x00000023u,0x00000038u,
    0x00000007u,0x0004003bu,0x00000023u,0x00000042u,0x00000007u,0x0004003bu,0x00000023u,0x0000004cu,
    0x00000007u,0x0004003bu,0x00000023u,0x00000056u,0x00000007u,0x0004003bu,0x00000023u,0x00000060u,
    0x00000007u,0x0004003bu,0x00000008u,0x00000078u,0x00000007u,0x0004003bu,0x00000023u,0x00000088u,
    0x00000007u,0x0004003bu,0x000000a9u,0x000000aau,0x00000007u,0x0004003bu,0x000000a9u,0x000000bfu,
    0x00000007u,0x0004003bu,0x00000023u,0x000000d4u,0x00000007u,0x0004003du,0x00000011u,0x00000014u,
    0x00000013u,0x00040064u,0x00000010u,0x00000017u,0x00000014u,0x00050067u,0x00000018u,0x00000019u,
    0x00000017u,0x00000016u,0x0004006fu,0x00000007u,0x0000001au,0x00000019u,0x00050050u,0x00000007u,
    0x0000001bu,0x0000000fu,0x0000000fu,0x00050088u,0x00000007u,0x0000001cu,0x0000001bu,0x0000001au,
    0x0003003eu,0x0000000eu,0x0000001cu,0x0004003du,0x00000011u,0x0000001fu,0x00000013u,0x0004003du,
    0x00000007u,0x00000020u,0x0000000bu,0x00070058u,0x00000009u,0x00000022u,0x0000001fu,0x00000020u,
    0x00000002u,0x00000021u,0x0003003eu,0x0000001eu,0x00000022u,0x0004003du,0x00000009u,0x00000026u,
    0x0000001eu,0x0008004fu,0x00000025u,0x00000027u,0x00000026u,0x00000026u,0x00000000u,0x00000001u,
    0x00000002u,0x00050094u,0x00000006u,0x0000002cu,0x00000027u,0x0000002bu,0x0003003eu,0x00000024u,
    0x0000002cu,0x0004003du,0x00000011u,0x0000002eu,0x00000013u,0x0004003du,0x00000007u,0x0000002fu,
    0x0000000bu,0x0004003du,0x00000007u,0x00000032u,0x0000000eu,0x00050085u,0x00000007u,0x00000033u,
    0x00000031u,0x00000032u,0x00050081u,0x00000007u,0x00000034u,0x0000002fu,0x00000033u,0x00070058u,
    0x00000009u,0x00000035u,0x0000002eu,0x00000034u,0x00000002u,0x00000021u,0x0008004fu,0x00000025u,
    0x00000036u,0x00000035u,0x00000035u,0x00000000u,0x00000001u,0x00000002u,0x00050094u,0x00000006u,
    0x00000037u,0x00000036u,0x0000002bu,0x0003003eu,0x0000002du,0x00000037u,0x0004003du,0x00000011u,
    0x00000039u,0x00000013u,0x0004003du,0x00000007u,0x0000003au,0x0000000bu,0x0004003du,0x00000007u,
    0x0000003cu,0x0000000eu,0x00050085u,0x00000007u,0x0000003du,0x0000003bu,0x0000003cu,0x00050081u,
    0x00000007u,0x0000003eu,0x0000003au,0x0000003du,0x00070058u,0x00000009u,0x0000003fu,0x00000039u,
    0x0000003eu,0x00000002u,0x00000021u,0x0008004fu,0x00000025u,0x00000040u,0x0000003fu,0x0000003fu,
    0x00000000u,0x00000001u,0x00000002u,0x00050094u,0x00000006u,0x00000041u,0x00000040u,0x0000002bu,
    0x0003003eu,0x00000038u,0x00000041u,0x0004003du,0x00000011u,0x00000043u,0x00000013u,0x0004003du,
    0x00000007u,0x00000044u,0x0000000bu,0x0004003du,0x00000007u,0x00000046u,0x0000000eu,0x00050085u,
    0x00000007u,0x00000047u,0x00000045u,0x00000046u,0x00050081u,0x00000007u,0x00000048u,0x00000044u,
    0x00000047u,0x00070058u,0x00000009u,0x00000049u,0x00000043u,0x00000048u,0x00000002u,0x00000021u,
    0x0008004fu,0x00000025u,0x0000004au,0x00000049u,0x00000049u,0x00000000u,0x00000001u,0x00000002u,
    0x00050094u,0x00000006u,0x0000004bu,0x0000004au,0x0000002bu,0x0003003eu,0x00000042u,0x0000004bu,
    0x0004003du,0x00000011u,0x0000004du,0x00000013u,0x0004003du,0x00000007u,0x0000004eu,0x0000000bu,
    0x0004003du,0x00000007u,0x00000050u,0x0000000eu,0x00050085u,0x00000007u,0x00000051u,0x0000004fu,
    0x00000050u,0x00050081u,0x00000007u,0x00000052u,0x0000004eu,0x00000051u,0x00070058u,0x00000009u,
    0x00000053u,0x0000004du,0x00000052u,0x00000002u,0x00000021u,0x0008004fu,0x00000025u,0x00000054u,
    0x00000053u,0x00000053u,0x00000000u,0x00000001u,0x00000002u,0x00050094u,0x00000006u,0x00000055u,
    0x00000054u,0x0000002bu,0x0003003eu,0x0000004cu,0x00000055u,0x0004003du,0x00000006u,0x00000057u,
    0x00000024u,0x0004003du,0x00000006u,0x00000058u,0x0000002du,0x0004003du,0x00000006u,0x00000059u,
    0x00000038u,0x0007000cu,0x00000006u,0x0000005au,0x00000001u,0x00000025u,0x00000058u,0x00000059u,
    0x0004003du,0x00000006u,0x0000005bu,0x00000042u,0x0004003du,0x00000006u,0x0000005cu,0x0000004cu,
    0x0007000cu,0x00000006u,0x0000005du,0x00000001u,0x00000025u,0x0000005bu,0x0000005cu,0x0007000cu,
    0x00000006u,0x0000005eu,0x00000001u,0x00000025u,0x0000005au,0x0000005du,0x0007000cu,0x00000006u,
    0x0000005fu,0x00000001u,0x00000025u,0x00000057u,0x0000005eu,0x0003003eu,0x00000056u,0x0000005fu,
    0x0004003du,0x00000006u,0x00000061u,0x00000024u,0x0004003du,0x00000006u,0x00000062u,0x0000002du,
    0x0004003du,0x00000006u,0x00000063u,0x00000038u,0x0007000cu,0x00000006u,0x00000064u,0x00000001u,
    0x00000028u,0x00000062u,0x00000063u,0x0004003du,0x00000006u,0x00000065u,0x00000042u,0x0004003du,
    0x00000006u,0x00000066u,0x0000004cu,0x0007000cu,0x00000006u,0x00000067u,0x00000001u,0x00000028u,
    0x00000065u,0x00000066u,0x0007000cu,0x00000006u,0x00000068u,0x00000001u,0x00000028u,0x00000064u,
    0x00000067u,0x0007000cu,0x00000006u,0x00000069u,0x00000001u,0x00000028u,0x00000061u,0x00000068u,
    0x0003003eu,0x00000060u,0x00000069u,0x0004003du,0x00000006u,0x0000006au,0x00000060u,0x0004003du,
    0x00000006u,0x0000006bu,0x00000056u,0x00050083u,0x00000006u,0x0000006cu,0x0000006au,0x0000006bu,
    0x0004003du,0x00000006u,0x0000006eu,0x00000060u,0x00050085u,0x00000006u,0x00000070u,0x0000006eu,
    0x0000006fu,0x0007000cu,0x00000006u,0x00000071u,0x00000001u,0x00000028u,0x0000006du,0x00000070u,
    0x000500b8u,0x00000072u,0x00000073u,0x0000006cu,0x00000071u,0x000300f7u,0x00000075u,0x00000000u,
    0x000400fau,0x00000073u,0x00000074u,0x00000075u,0x000200f8u,0x00000074u,0x0004003du,0x00000009u,
    0x00000076u,0x0000001eu,0x000200feu,0x00000076u,0x000200f8u,0x00000075u,0x0004003du,0x00000006u,
    0x00000079u,0x00000042u,0x0004003du,0x00000006u,0x0000007au,0x0000004cu,0x00050081u,0x00000006u,
    0x0000007bu,0x00000079u,0x0000007au,0x0004003du,0x00000006u,0x0000007cu,0x0000002du,0x00050083u,
    0x00000006u,0x0000007du,0x0000007bu,0x0000007cu,0x0004003du,0x00000006u,0x0000007eu,0x00000038u,
    0x00050083u,0x00000006u,0x0000007fu,0x0000007du,0x0000007eu,0x0004003du,0x00000006u,0x00000080u,
    0x0000002du,0x0004003du,0x00000006u,0x00000081u,0x00000042u,0x00050081u,0x00000006u,0x00000082u,
    0x00000080u,0x00000081u,0x0004003du,0x00000006u,0x00000083u,0x00000038u,0x00050083u,0x00000006u,
    0x00000084u,0x00000082u,0x00000083u,0x0004003du,0x00000006u,0x00000085u,0x0000004cu,0x00050083u,
    0x00000006u,0x00000086u,0x00000084u,0x00000085u,0x00050050u,0x00000007u,0x00000087u,0x0000007fu,
    0x00000086u,0x0003003eu,0x00000078u,0x00000087u,0x0004003du,0x00000006u,0x00000089u,0x0000002du,
    0x0004003du,0x00000006u,0x0000008au,0x00000038u,0x00050081u,0x00000006u,0x0000008bu,0x00000089u,
    0x0000008au,0x0004003du,0x00000006u,0x0000008cu,0x00000042u,0x00050081u,0x00000006u,0x0000008du,
    0x0000008bu,0x0000008cu,0x0004003du,0x00000006u,0x0000008eu,0x0000004cu,0x00050081u,0x00000006u,
    0x0000008fu,0x0000008du,0x0000008eu,0x00050085u,0x00000006u,0x00000090u,0x0000008fu,0x0000006du,
    0x0007000cu,0x00000006u,0x00000092u,0x00000001u,0x00000028u,0x00000090u,0x00000091u,0x0003003eu,
    0x00000088u,0x00000092u,0x0004003du,0x00000007u,0x00000093u,0x00000078u,0x00050041u,0x00000023u,
    0x00000096u,0x00000078u,0x00000095u,0x0004003du,0x00000006u,0x00000097u,0x00000096u,0x0006000cu,
    0x00000006u,0x00000098u,0x00000001u,0x00000004u,0x00000097u,0x00050041u,0x00000023u,0x0000009au,
    0x00000078u,0x00000099u,0x0004003du,0x00000006u,0x0000009bu,0x0000009au,0x0006000cu,0x00000006u,
    0x0000009cu,0x00000001u,0x00000004u,0x0000009bu,0x0007000cu,0x00000006u,0x0000009du,0x00000001u,
    0x00000025u,0x00000098u,0x0000009cu,0x0004003du,0x00000006u,0x0000009eu,0x00000088u,0x00050081u,
    0x00000006u,0x0000009fu,0x0000009du,0x0000009eu,0x00050050u,0x00000007u,0x000000a0u,0x0000009fu,
    0x0000009fu,0x00050088u,0x00000007u,0x000000a1u,0x00000093u,0x000000a0u,0x0008000cu,0x00000007u,
    0x000000a6u,0x00000001u,0x0000002bu,0x000000a1u,0x000000a3u,0x000000a5u,0x0004003du,0x00000007u,
    0x000000a7u,0x0000000eu,0x00050085u,0x00000007u,0x000000a8u,0x000000a6u,0x000000a7u,0x0003003eu,
    0x00000078u,0x000000a8u,0x0004003du,0x00000011u,0x000000acu,0x00000013u,0x0004003du,0x00000007u,
    0x000000adu,0x0000000bu,0x0004003du,0x00000007u,0x000000aeu,0x00000078u,0x00050050u,0x00000007u,
    0x000000b0u,0x000000afu,0x000000afu,0x00050088u,0x00000007u,0x000000b1u,0x000000aeu,0x000000b0u,
    0x00050083u,0x00000007u,0x000000b2u,0x000000adu,0x000000b1u,0x00070058u,0x00000009u,0x000000b3u,
    0x000000acu,0x000000b2u,0x00000002u,0x00000021u,0x0008004fu,0x00000025u,0x000000b4u,0x000000b3u,
    0x000000b3u,0x00000000u,0x00000001u,0x00000002u,0x0004003du,0x00000011u,0x000000b5u,0x00000013u,
    0x0004003du,0x00000007u,0x000000b6u,0x0000000bu,0x0004003du,0x00000007u,0x000000b7u,0x00000078u,
    0x00050050u,0x00000007u,0x000000b8u,0x000000afu,0x000000afu,0x00050088u,0x00000007u,0x000000b9u,
    0x000000b7u,0x000000b8u,0x00050081u,0x00000007u,0x000000bau,0x000000b6u,0x000000b9u,0x00070058u,
    0x00000009u,0x000000bbu,0x000000b5u,0x000000bau,0x00000002u,0x00000021u,0x0008004fu,0x00000025u,
    0x000000bcu,0x000000bbu,0x000000bbu,0x00000000u,0x00000001u,0x00000002u,0x00050081u,0x00000025u,
    0x000000bdu,0x000000b4u,0x000000bcu,0x0005008eu,0x00000025u,0x000000beu,0x000000bdu,0x000000abu,
    0x0003003eu,0x000000aau,0x000000beu,0x0004003du,0x00000025u,0x000000c0u,0x000000aau,0x0005008eu,
    0x00000025u,0x000000c1u,0x000000c0u,0x000000abu,0x0004003du,0x00000011u,0x000000c3u,0x00000013u,
    0x0004003du,0x00000007u,0x000000c4u,0x0000000bu,0x0004003du,0x00000007u,0x000000c5u,0x00000078u,
    0x0005008eu,0x00000007u,0x000000c6u,0x000000c5u,0x000000abu,0x00050083u,0x00000007u,0x000000c7u,
    0x000000c4u,0x000000c6u,0x00070058u,0x00000009u,0x000000c8u,0x000000c3u,0x000000c7u,0x00000002u,
    0x00000021u,0x0008004fu,0x00000025u,0x000000c9u,0x000000c8u,0x000000c8u,0x00000000u,0x00000001u,
    0x00000002u,0x0004003du,0x00000011u,0x000000cau,0x00000013u,0x0004003du,0x00000007u,0x000000cbu,
    0x0000000bu,0x0004003du,0x00000007u,0x000000ccu,0x00000078u,0x0005008eu,0x00000007u,0x000000cdu,
    0x000000ccu,0x000000abu,0x00050081u,0x00000007u,0x000000ceu,0x000000cbu,0x000000cdu,0x00070058u,
    0x00000009u,0x000000cfu,0x000000cau,0x000000ceu,0x00000002u,0x00000021u,0x0008004fu,0x00000025u,
    0x000000d0u,0x000000cfu,0x000000cfu,0x00000000u,0x00000001u,0x00000002u,0x00050081u,0x00000025u,
    0x000000d1u,0x000000c9u,0x000000d0u,0x0005008eu,0x00000025u,0x000000d2u,0x000000d1u,0x000000c2u,
    0x00050081u,0x00000025u,0x000000d3u,0x000000c1u,0x000000d2u,0x0003003eu,0x000000bfu,0x000000d3u,
    0x0004003du,0x00000025u,0x000000d5u,0x000000bfu,0x00050094u,0x00000006u,0x000000d6u,0x000000d5u,
    0x0000002bu,0x0003003eu,0x000000d4u,0x000000d6u,0x0004003du,0x00000006u,0x000000d7u,0x000000d4u,
    0x0004003du,0x00000006u,0x000000d8u,0x00000056u,0x000500b8u,0x00000072u,0x000000d9u,0x000000d7u,
    0x000000d8u,0x0004003du,0x00000006u,0x000000dau,0x000000d4u,0x0004003du,0x00000006u,0x000000dbu,
    0x00000060u,0x000500bau,0x00000072u,0x000000dcu,0x000000dau,0x000000dbu,0x000500a6u,0x00000072u,
    0x000000ddu,0x000000d9u,0x000000dcu,0x0004003du,0x00000025u,0x000000deu,0x000000aau,0x0004003du,
    0x00000025u,0x000000dfu,0x000000bfu,0x00060050u,0x000000e0u,0x000000e1u,0x000000ddu,0x000000ddu,
    0x000000ddu,0x000600a9u,0x00000025u,0x000000e2u,0x000000e1u,0x000000deu,0x000000dfu,0x00050041u,
    0x00000023u,0x000000e4u,0x0000001eu,0x000000e3u,0x0004003du,0x00000006u,0x000000e5u,0x000000e4u,
    0x00050051u,0x00000006u,0x000000e6u,0x000000e2u,0x00000000u,0x00050051u,0x00000006u,0x000000e7u,
    0x000000e2u,0x00000001u,0x00050051u,0x00000006u,0x000000e8u,0x000000e2u,0x00000002u,0x00070050u,
    0x00000009u,0x000000e9u,0x000000e6u,0x000000e7u,0x000000e8u,0x000000e5u,0x000200feu,0x000000e9u,
    0x00010038u,
};
// END GENERATED VULKAN SHADERS

struct VkVertex {
    std::array<float,4> position;std::array<float,3> uvq;std::array<float,4> color;
    // Constant per original strip, carried with vertices so adjacent strips
    // with different K can still share the same native draw/material state.
    std::array<float,2> lod{};
    std::uint32_t glyphBoundsIndex{};
    std::array<float,3> normal{};
};
using VkNativeDrawPush = NativeVulkanDrawPush;

// One job packs a disjoint raw source range on a cold geometry-cache miss.
// Texture resolution, topology and draw order stay on the recording thread;
// no CPU job evaluates lighting or reflection for the native Vulkan path.
struct VkSolidStripJob {
    const EcstasyDrawCommand* draw{};
    std::uint32_t destination{};
    std::array<float,2> scroll{},lod{};
    std::uint32_t glyphBoundsIndex{};
    std::uint32_t workUnits{};
    std::array<std::uint8_t,48> sourceOrder{};
    std::uint8_t count{};
    bool reflection{},targetSample{};
    bool lineTopology{};
    VkVertex* residentOutput{};
};
std::uint32_t NativePendingVertexWork(const VkSolidStripJob& job)
{
    return job.count;
}

struct VkNativeStripWorkRange {
    std::size_t first{},last{};
};

struct VkSolidTopology {
    std::array<std::uint8_t,48> sourceOrder{};
    std::array<std::uint8_t,138> indices{};
    std::uint32_t indexCount{};
    std::uint8_t count{};
};

// Only topology inputs are captured here. Positions, UVs and lighting still
// have their normal owners and are evaluated independently for each view.
struct VkSolidTopologyInputs {
    std::size_t count{},uniqueCount{},triangleCount{},preparedCount{};
    std::uint64_t disableMask{},preparedMask{};
    std::array<std::uint8_t,48> unique{};
    std::array<std::uint8_t,138> triangles{};
    bool Matches(const EditorMarkerSolidGeometry::Strip& strip) const {
        return count==strip.positions.size() && uniqueCount==strip.nativeUniqueOrder.size() &&
            triangleCount==strip.nativeTriangleOrder.size() && preparedCount==strip.nativeTopologyCount &&
            disableMask==strip.drawDisableMask && preparedMask==strip.nativeTopologyMask &&
            (!uniqueCount || std::memcmp(unique.data(),strip.nativeUniqueOrder.data(),uniqueCount)==0) &&
            (!triangleCount || std::memcmp(triangles.data(),strip.nativeTriangleOrder.data(),triangleCount)==0);
    }
    void Capture(const EditorMarkerSolidGeometry::Strip& strip) {
        count=strip.positions.size();uniqueCount=strip.nativeUniqueOrder.size();
        triangleCount=strip.nativeTriangleOrder.size();preparedCount=strip.nativeTopologyCount;
        disableMask=strip.drawDisableMask;preparedMask=strip.nativeTopologyMask;
        if(uniqueCount)std::memcpy(unique.data(),strip.nativeUniqueOrder.data(),uniqueCount);
        if(triangleCount)std::memcpy(triangles.data(),strip.nativeTriangleOrder.data(),triangleCount);
    }
};

VkSolidTopology DecodeNativeSolidTopology(const EditorMarkerSolidGeometry::Strip& strip,bool lines=false)
{
    const auto count=strip.positions.size();
    Require(count<=48 && strip.nativeUniqueOrder.size()<=48 && strip.nativeTriangleOrder.size()<=138,
        "Native strip topology exceeds its source packet bound");
    VkSolidTopology result;std::array<std::uint8_t,48> decoded{};std::uint64_t mask{};
    const auto vertex=[&](std::size_t index) {
        Require(index<count && result.count<48,"Native strip vertex ordinal is invalid");
        decoded[index]=result.count;result.sourceOrder[result.count++]=static_cast<std::uint8_t>(index);
        mask|=std::uint64_t(1)<<index;
    };
    if(lines) {
        // PAL AttachMicroCode selects GS LINE_STRIP, not triangle outlines.
        // XYZ with ADC1 still advances the vertex FIFO but suppresses its
        // draw kick. Explicit pairs preserve that exact FIFO/ADC sequence.
        for(std::size_t current=1;current<count;++current) {
            if(strip.drawDisableMask&(std::uint64_t(1)<<current))continue;
            for(const auto index:{current-1,current}) {
                if(!(mask&(std::uint64_t(1)<<index)))vertex(index);
                result.indices[result.indexCount++]=decoded[index];
            }
        }
    } else if(strip.nativeTopologyCount==count && strip.nativeTopologyMask==strip.drawDisableMask) {
        for(const auto index:strip.nativeUniqueOrder)vertex(index);
        for(const auto index:strip.nativeTriangleOrder) {
            Require(index<count && (mask&(std::uint64_t(1)<<index)),
                "Native triangle references an absent source vertex");
            result.indices[result.indexCount++]=decoded[index];
        }
    } else {
        for(std::size_t triangle=2;triangle<count;++triangle) {
            if(strip.drawDisableMask&(std::uint64_t(1)<<triangle))continue;
            const std::size_t corners[]={triangle-2+(triangle&1u),triangle-1-(triangle&1u),triangle};
            for(const auto index:corners) {
                if(!(mask&(std::uint64_t(1)<<index)))vertex(index);
                result.indices[result.indexCount++]=decoded[index];
            }
        }
    }
    return result;
}

// Immutable source geometry only. Lighting, reflection direction/sphere UV,
// material alpha, scroll and unit-vector input are evaluated by the GPU from
// a separately captured draw record, including on cache-capacity misses.
void PrepareNativeSolidStripVertices(const VkSolidStripJob& job,VkVertex* output)
{
    const auto& strip=*job.draw->strip;
    for(std::size_t ordinal=0;ordinal<job.count;++ordinal) {
        const auto index=job.sourceOrder[ordinal];const auto packed=strip.colors[index];
        auto& vertex=output[job.destination+ordinal];
        vertex={{{strip.positions[index][0],strip.positions[index][1],strip.positions[index][2],1.0f}},
            {{strip.textureCoordinates[index][0],strip.textureCoordinates[index][1],1.0f}},
            {{static_cast<float>(packed&31u)/16.0f,static_cast<float>((packed>>5)&31u)/16.0f,
              static_cast<float>((packed>>10)&31u)/16.0f,1.0f}},job.lod,job.glyphBoundsIndex};
        if(index<strip.normals.size())vertex.normal=strip.normals[index];
        for(const auto value:vertex.position)
            if(!std::isfinite(value))throw std::runtime_error("Nonfinite original vertex position");
        for(const auto value:vertex.uvq)
            if(!std::isfinite(value))throw std::runtime_error("Nonfinite original texture coordinate");
        for(const auto value:vertex.normal)
            if(!std::isfinite(value))throw std::runtime_error("Nonfinite original vertex normal");
    }
}

// Only topology/strip metadata remain in the key. Exact source bytes below
// invalidate position/normal/UV/color changes, including live animation and
// address reuse after unload. All passes share raw geometry, never final RGB.
struct VkResidentGeometryKey {
    std::array<std::uint32_t,3> words{};
    bool operator==(const VkResidentGeometryKey& other) const noexcept {return words==other.words;}
};

bool NativeResidentGeometryKey(const VkSolidStripJob& job,VkResidentGeometryKey& key)
{
    key.words[0]=job.lineTopology ? 1u:0u;
    std::memcpy(&key.words[1],job.lod.data()+1,sizeof(float));
    key.words[2]=job.glyphBoundsIndex;
    return true;
}

// Frame-owned immutable inputs share the existing fenced vertex/fragment
// SSBO. Its offsets never enter resident geometry. Each decoded material is
// snapshotted once per frame; FE and IG variants retain their actual tables.
class VkNativeFrameShading {
    using Record=std::array<std::array<float,4>,19>;
    using Key=std::array<std::uint32_t,76>;
    struct Input {
        const EditorMarkerLightMaterial* material{};
        std::uint32_t flags{},mode{},index{};
        float alpha{};
        std::array<float,2> scroll{};
        std::array<float,4> unit{};
        std::array<float,16> coefficients{};
        SceneLightContext context;
        bool valid{};
        bool Matches(const VkSolidStripJob& job,std::uint32_t requestedMode) const noexcept {
            const auto& draw=*job.draw;
            if(!valid || flags!=draw.drawFlags || mode!=requestedMode ||
                std::memcmp(&alpha,&draw.alphaScale,sizeof(alpha)) ||
                std::memcmp(scroll.data(),job.scroll.data(),sizeof(scroll)) ||
                std::memcmp(unit.data(),eUnitVector4,sizeof(unit)))return false;
            if(!(job.reflection || (draw.drawFlags&0xc0u)))return true;
            if(material!=draw.lightMaterial || !draw.lightMaterial || !draw.LightContext())return false;
            const auto& light=*draw.LightContext();
            return std::memcmp(coefficients.data(),draw.lightMaterial->NativeCoefficients(),sizeof(coefficients))==0 &&
                std::memcmp(context.colorMatrix.data(),light.colorMatrix.data(),sizeof(context.colorMatrix))==0 &&
                std::memcmp(context.directionMatrix.data(),light.directionMatrix.data(),sizeof(context.directionMatrix))==0 &&
                std::memcmp(context.environmentMatrix.data(),light.environmentMatrix.data(),sizeof(context.environmentMatrix))==0 &&
                std::memcmp(context.localEye.data(),light.localEye.data(),sizeof(context.localEye))==0;
        }
        void Capture(const VkSolidStripJob& job,std::uint32_t requestedMode,std::uint32_t first) noexcept {
            const auto& draw=*job.draw;
            flags=draw.drawFlags;mode=requestedMode;alpha=draw.alphaScale;
            scroll=job.scroll;std::copy_n(eUnitVector4,unit.size(),unit.begin());index=first;
            if(job.reflection || (draw.drawFlags&0xc0u)) {
                material=draw.lightMaterial;context=*draw.LightContext();
                std::copy_n(draw.lightMaterial->NativeCoefficients(),coefficients.size(),coefficients.begin());
            } else material=nullptr;
            valid=true;
        }
    };
    struct RecordSlot {
        std::uint64_t hash{};
        std::uint32_t first{};
    };
public:
    void Clear() {
        materials_.clear();recordCount_=0;lastIndex_=0;
        std::fill(records_.begin(),records_.end(),RecordSlot{});
        for(auto& input:inputs_)input.valid=false;
        lastInput_=inputs_.size();
    }
    std::uint32_t Capture(const VkSolidStripJob& job,std::vector<std::array<float,4>>& data) {
        const auto& draw=*job.draw;
        const auto mode=(job.reflection ? 1u:0u)|(job.targetSample ? 2u:0u);
        // Commands refer to immutable frame-owned lighting snapshots.
        // Compare the consumed source bits before rebuilding the same record
        // for every strip. Snapshots also detect changes when a diagnostic
        // caller reuses the same draw object. Nothing survives Clear().
        if(lastInput_<inputs_.size() && inputs_[lastInput_].Matches(job,mode))return inputs_[lastInput_].index;
        const auto inputSlot=InputSlot(job,mode);
        if(inputs_[inputSlot].Matches(job,mode)) {
            lastInput_=inputSlot;return inputs_[inputSlot].index;
        }
        const auto remember=[&](std::uint32_t first) {
            inputs_[inputSlot].Capture(job,mode,first);lastInput_=inputSlot;return first;
        };
        Record record{};
        const auto finite=[](float value) {
            if(!std::isfinite(value))throw std::runtime_error("Native GPU shading input is nonfinite");
        };
        finite(draw.alphaScale);for(const auto value:job.scroll)finite(value);
        record[1]={{draw.alphaScale,job.scroll[0],job.scroll[1],0}};
        for(std::size_t axis=0;axis<4;++axis) {finite(eUnitVector4[axis]);record[2][axis]=eUnitVector4[axis];}
        std::uint32_t materialIndex=0;
        if(job.reflection || (draw.drawFlags&0xc0u)) {
            Require(draw.LightContext() && draw.lightMaterial,"Native GPU shading lost its immutable inputs");
            const auto& context=*draw.LightContext();
            materialIndex=Material(*draw.lightMaterial,data);
            record[3]=context.localEye;
            for(std::size_t column=0;column<4;++column) {
                std::copy_n(context.directionMatrix.data()+column*4,4,record[4+column].data());
                std::copy_n(context.environmentMatrix.data()+column*4,4,record[8+column].data());
            }
            // Products are constant for this draw; per-vertex dot products,
            // normalization, response lookup, accumulation and quantization
            // remain on the GPU in the original precise arithmetic order.
            const auto lighting=PrepareNativeVertexLighting(context,*draw.lightMaterial,draw.drawFlags);
            record[12]=lighting.ambient;
            for(std::size_t light=0;light<3;++light)for(std::size_t axis=0;axis<4;++axis) {
                record[13+light][axis]=lighting.diffuse[axis][light];
                record[16+light][axis]=lighting.specular[axis][light];
            }
            for(std::size_t at=3;at<record.size();++at)for(const auto value:record[at])finite(value);
        }
        std::memcpy(&record[0][0],&draw.drawFlags,sizeof(draw.drawFlags));
        std::memcpy(&record[0][1],&mode,sizeof(mode));
        std::memcpy(&record[0][2],&materialIndex,sizeof(materialIndex));
        Key key{};
        static_assert(sizeof(record)==sizeof(key),"Native shading record packing changed");
        std::memcpy(key.data(),record.data(),sizeof(record));
        if(lastIndex_ && key==lastKey_)return remember(lastIndex_);
        const auto hash=RecordHash(key);
        if(!records_.empty()) {
            auto slot=static_cast<std::size_t>(hash)&(records_.size()-1);
            while(records_[slot].hash) {
                const auto& entry=records_[slot];
                // Hash collisions never establish equality. The retained
                // frame table supplies all 304 exact bytes without a second
                // large key allocation or a pointer tree rebuilt each frame.
                if(entry.hash==hash && std::memcmp(data.data()+entry.first,record.data(),sizeof(record))==0) {
                    lastKey_=key;lastIndex_=entry.first;return remember(lastIndex_);
                }
                slot=(slot+1)&(records_.size()-1);
            }
        }
        Require(data.size()<=UINT32_MAX-record.size(),"Native GPU shading table exceeds32-bit addressing");
        ReserveRecord();
        const auto first=static_cast<std::uint32_t>(data.size());
        data.insert(data.end(),record.begin(),record.end());
        auto slot=static_cast<std::size_t>(hash)&(records_.size()-1);
        while(records_[slot].hash)slot=(slot+1)&(records_.size()-1);
        records_[slot]={hash,first};++recordCount_;
        lastKey_=key;lastIndex_=first;return remember(first);
    }
private:
    std::size_t InputSlot(const VkSolidStripJob& job,std::uint32_t mode) const noexcept {
        const auto& draw=*job.draw;
        const auto bits=[](float value) {std::uint32_t result;std::memcpy(&result,&value,sizeof(result));return result;};
        auto hash=static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(draw.lightMaterial));
        hash=(hash>>4)^(hash>>20)^draw.drawFlags^(static_cast<std::uint64_t>(mode)<<16);
        hash=(hash^bits(draw.alphaScale))*1099511628211ull;
        hash=(hash^bits(job.scroll[0]))*1099511628211ull;
        if((job.reflection || (draw.drawFlags&0xc0u)) && draw.LightContext()) {
            hash=(hash^bits(draw.LightContext()->localEye[0]))*1099511628211ull;
            hash=(hash^bits(draw.LightContext()->localEye[2]))*1099511628211ull;
        }
        return static_cast<std::size_t>(hash^(hash>>32))&(inputs_.size()-1);
    }
    static std::uint64_t RecordHash(const Key& key) noexcept {
        std::uint64_t first=1469598103934665603ull,second=1099511628211ull;
        for(std::size_t index=0;index<key.size();index+=2) {
            first=(first^key[index])*1099511628211ull;
            second=(second^key[index+1])*1099511628211ull;
        }
        const auto hash=first^(second+(first<<6)+(first>>2));
        return hash ? hash:1u;
    }
    void ReserveRecord() {
        if(recordCount_<records_.size()/2)return;
        std::vector<RecordSlot> next(std::max<std::size_t>(128,records_.size()*2));
        for(const auto& entry:records_)if(entry.hash) {
            auto slot=static_cast<std::size_t>(entry.hash)&(next.size()-1);
            while(next[slot].hash)slot=(slot+1)&(next.size()-1);
            next[slot]=entry;
        }
        records_.swap(next);
    }
    std::uint32_t Material(const EditorMarkerLightMaterial& material,std::vector<std::array<float,4>>& data) {
        const auto found=materials_.find(&material);if(found!=materials_.end())return found->second;
        constexpr std::size_t tableVectors=(EditorMarkerLightMaterial::kNativeResponseSamples+3)/4;
        constexpr std::size_t materialVectors=5+2*tableVectors;
        static_assert(EditorMarkerLightMaterial::kNativeResponseSamples==1025 && tableVectors==257,
            "Native GPU response-table addressing differs from the authored material");
        Require(data.size()<=UINT32_MAX-materialVectors,"Native GPU material table exceeds32-bit addressing");
        const auto first=static_cast<std::uint32_t>(data.size());
        data.resize(data.size()+materialVectors);
        const auto* coefficients=material.NativeCoefficients();
        for(std::size_t index=0;index<16;++index) {
            if(!std::isfinite(coefficients[index]))throw std::runtime_error("Native GPU material coefficient is nonfinite");
            data[first+index/4][index%4]=coefficients[index];
        }
        data[first+4][0]=material.nativeSpecularReady ? 1.0f:0.0f;
        if(material.nativeSpecularReady)for(std::size_t table=0;table<2;++table)
            for(std::size_t index=0;index<EditorMarkerLightMaterial::kNativeResponseSamples;++index) {
                const auto value=material.nativeSpecularResponse[table][index];
                if(!std::isfinite(value))throw std::runtime_error("Native GPU specular response sample is nonfinite");
                data[first+5+table*tableVectors+index/4][index%4]=value;
            }
        materials_.emplace(&material,first);return first;
    }
    std::unordered_map<const EditorMarkerLightMaterial*,std::uint32_t> materials_;
    std::vector<RecordSlot> records_;
    std::size_t recordCount_{};
    std::array<Input,256> inputs_{};
    std::size_t lastInput_{inputs_.size()};
    Key lastKey_{};
    std::uint32_t lastIndex_{};
};

struct VkResidentGeometrySource {
    const EditorMarkerSolidGeometry::Strip* token{};
    std::uint64_t validatedGather{},generation{},queuedValidationGather{},vertexRevision{};
    std::size_t queuedValidation{};
    std::size_t count{},normalCount{},uniqueCount{},triangleCount{},topologyCount{};
    std::uint64_t disableMask{},topologyMask{};
    std::array<std::array<float,3>,48> positions{},normals{};
    std::array<std::array<float,2>,48> coordinates{};
    std::array<std::uint16_t,48> colors{};
    std::array<std::uint8_t,48> unique{};
    std::array<std::uint8_t,138> triangles{};
    std::array<std::uint32_t,4> variants{};
    std::size_t variantCount{};
    VkSolidTopology topology;
    VkSolidTopology lineTopology;

    bool Matches(const EditorMarkerSolidGeometry::Strip& strip) const {
        if(count!=strip.positions.size() || normalCount!=strip.normals.size() ||
            uniqueCount!=strip.nativeUniqueOrder.size() || triangleCount!=strip.nativeTriangleOrder.size() ||
            topologyCount!=strip.nativeTopologyCount || disableMask!=strip.drawDisableMask ||
            topologyMask!=strip.nativeTopologyMask)return false;
        const auto equal=[&](const void* a,const void* b,std::size_t bytes) {
            return !bytes || std::memcmp(a,b,bytes)==0;
        };
        return equal(positions.data(),strip.positions.data(),count*sizeof(positions[0])) &&
            equal(coordinates.data(),strip.textureCoordinates.data(),count*sizeof(coordinates[0])) &&
            equal(colors.data(),strip.colors.data(),count*sizeof(colors[0])) &&
            equal(normals.data(),strip.normals.data(),std::min<std::size_t>(normalCount,48)*sizeof(normals[0])) &&
            equal(unique.data(),strip.nativeUniqueOrder.data(),uniqueCount) &&
            equal(triangles.data(),strip.nativeTriangleOrder.data(),triangleCount);
    }
    void Capture(const EditorMarkerSolidGeometry::Strip& strip) {
        const auto cooked=DecodeNativeSolidTopology(strip);
        const auto cookedLines=DecodeNativeSolidTopology(strip,true);
        count=strip.positions.size();normalCount=strip.normals.size();
        uniqueCount=strip.nativeUniqueOrder.size();triangleCount=strip.nativeTriangleOrder.size();
        topologyCount=strip.nativeTopologyCount;disableMask=strip.drawDisableMask;topologyMask=strip.nativeTopologyMask;
        const auto copy=[](void* to,const void* from,std::size_t bytes) {if(bytes)std::memcpy(to,from,bytes);};
        copy(positions.data(),strip.positions.data(),count*sizeof(positions[0]));
        copy(coordinates.data(),strip.textureCoordinates.data(),count*sizeof(coordinates[0]));
        copy(colors.data(),strip.colors.data(),count*sizeof(colors[0]));
        copy(normals.data(),strip.normals.data(),std::min<std::size_t>(normalCount,48)*sizeof(normals[0]));
        copy(unique.data(),strip.nativeUniqueOrder.data(),uniqueCount);
        copy(triangles.data(),strip.nativeTriangleOrder.data(),triangleCount);
        topology=cooked;lineTopology=cookedLines;++generation;
    }
};

// A resident hit reads this header without walking through the 3 KiB
// vertex shadow and index payload. Payload bytes and the GPU slot layout
// are unchanged; the small counts retain their source packet bounds.
struct VkResidentGeometryEntry {
    const EditorMarkerSolidGeometry::Strip* source{};
    std::uint64_t sourceGeneration{},lastGather{},indexToken{};
    std::list<std::uint32_t>::iterator recency;
    VkResidentGeometryKey key;
    std::uint32_t slot{};
    std::uint16_t indexCount{};
    std::uint8_t count{};
    bool prepared{},uploaded{};
    std::array<VkVertex,48> shadow{};
    std::array<std::uint32_t,138> indices{};
};

class VkResidentGeometryCache {
    struct Validation {
        const VkResidentGeometrySource* source{};
        const EditorMarkerSolidGeometry::Strip* token{};
        bool matches{};
    };
public:
    static constexpr std::size_t VerticesPerSlot=48;
    static constexpr std::size_t SlotBytes=VerticesPerSlot*sizeof(VkVertex);

    void Configure(std::size_t count) {
        Require(entries_.empty(),"Resident geometry capacity cannot change while entries exist");
        entries_.resize(count);free_.reserve(count);
        for(std::size_t at=count;at;--at)free_.push_back(static_cast<std::uint32_t>(at-1));
    }
    std::size_t Capacity() const noexcept {return entries_.size();}
    std::size_t Size() const noexcept {return recency_.size();}
    const std::vector<std::uint32_t>& Active() const noexcept {return active_;}
    VkResidentGeometryEntry& Entry(std::uint32_t slot) {return *entries_.at(slot);}
    void Begin(std::uint64_t gather) {
        // These entries and shadows are CPU-owned. A fully cached frame can
        // prepare them while the GPU reads the separate native buffers; the
        // presenter still waits before any GPU allocation, upload or reuse.
        // No current-Gather slot is recycled, even under cache pressure.
        if(gather<=gather_) {
            sources_.clear();recency_.clear();free_.clear();
            for(std::size_t at=entries_.size();at;--at) {
                entries_[at-1].reset();free_.push_back(static_cast<std::uint32_t>(at-1));
            }
        }
        gather_=gather;active_.clear();frameEntries_.fill(nullptr);frameSources_.fill(nullptr);
        validations_.clear();validationsReady_=false;
        while(!recency_.empty() && gather_-entries_[recency_.front()]->lastGather>600)
            Evict(recency_.front());
    }
    std::size_t PreparedValidationCount() const noexcept {return validationsReady_ ? validations_.size():0;}
    void ValidateSources(const std::vector<EcstasyDrawCommand>& commands,unsigned workers) {
        if(workers<2 || commands.size()<1024 || sources_.empty())return;
        // Resolve and deduplicate all borrowed sources on the owner. The
        // workers only compare immutable bytes with separate result slots;
        // they never insert/erase cache entries or publish a generation.
        for(const auto& draw:commands) {
            if(draw.kind!=EcstasyDrawKind::SolidStrip && draw.kind!=EcstasyDrawKind::SolidReflectionStrip &&
                draw.kind!=EcstasyDrawKind::SolidSpecularStrip)continue;
            const auto* token=draw.strip;
            if(!token || token->nativeVertexRevision)continue;
            // Malformed or cropped-out commands keep their existing Gather
            // validation point. Do not add a failure for a draw that would
            // never reach Acquire after its authored scissor is applied.
            const auto count=token->positions.size();
            if(count>48 || token->textureCoordinates.size()!=count || token->colors.size()!=count)continue;
            auto* source=FindSource(token);
            if(!source || source->validatedGather==gather_ || source->queuedValidationGather==gather_)continue;
            const auto index=validations_.size();validations_.push_back({source,token,false});
            source->queuedValidation=index;source->queuedValidationGather=gather_;
        }
        if(validations_.size()<512)return;
        RunNativeSceneJobs(validations_.size(),[&](std::size_t index) {
            auto& validation=validations_[index];
            validation.matches=validation.source->generation && validation.source->Matches(*validation.token);
        },512,workers);
        // RunNativeSceneJobs drains every reader, including its exception
        // path, before the owner can evict a snapshot or unwind the frame.
        validationsReady_=true;
    }
    VkResidentGeometryEntry* Acquire(VkSolidStripJob& job,bool& prepare) {
        prepare=false;VkResidentGeometryKey key;
        if(!Capacity() || !NativeResidentGeometryKey(job,key))return nullptr;
        const auto* token=job.draw->strip;
        const auto identity=reinterpret_cast<std::uintptr_t>(token);
        const auto hash=(identity>>4)^(identity>>12)^(identity>>20)^
            (static_cast<std::size_t>(key.words[1])*0x9e3779b9u)^key.words[0]^key.words[2];
        auto& cached=frameEntries_[hash&(frameEntries_.size()-1)];
        // Only an acquired current-Gather entry enters this table. It has
        // already passed the exact source-byte/topology validation and is
        // protected against eviction until this Gather finishes. This skips
        // repeated map/variant traversal for another pass of the same strip,
        // not the mandatory validation at its first use in each frame.
        if(cached && cached->source==token && cached->key==key) {
            job.count=cached->count;Touch(*cached);return cached;
        }
        auto* resolved=FindSource(token);
        if(!resolved) {
            if(free_.empty() && (!recency_.empty() && entries_[recency_.front()]->lastGather==gather_)) {
                return nullptr;
            }
            resolved=sources_.emplace(token,std::make_unique<VkResidentGeometrySource>()).first->second.get();
            resolved->token=token;FrameSource(token)=resolved;
        }
        auto& source=*resolved;
        if(source.validatedGather!=gather_) {
            const auto revision=token->nativeVertexRevision;
            const bool compared=validationsReady_ && source.queuedValidationGather==gather_;
            const bool sameRevision=revision!=0 && revision==source.vertexRevision;
            const bool matches=sameRevision || (compared ? validations_[source.queuedValidation].matches:
                source.generation && source.Matches(*token));
            if(!matches)source.Capture(*token);
            source.vertexRevision=revision;
            // Publication stays at the original first used strip. A
            // completed comparison alone never marks source bytes valid.
            source.validatedGather=gather_;
        }
        for(std::size_t at=0;at<source.variantCount;++at) {
            auto& entry=*entries_[source.variants[at]];
            if(entry.sourceGeneration==source.generation && entry.key==key) {
                job.count=entry.count;prepare=!entry.prepared && entry.lastGather!=gather_;
                if(prepare)job.sourceOrder=(job.lineTopology ? source.lineTopology:source.topology).sourceOrder;
                Touch(entry);cached=&entry;return &entry;
            }
        }
        const auto& topology=job.lineTopology ? source.lineTopology:source.topology;
        job.count=topology.count;
        if(!job.count) {
            if(!source.variantCount)ForgetSource(token);
            return nullptr;
        }
        job.sourceOrder=topology.sourceOrder;
        if(source.variantCount==source.variants.size()) {
            auto victim=source.variants[0];
            for(std::size_t at=1;at<source.variantCount;++at)
                if(entries_[source.variants[at]]->lastGather<entries_[victim]->lastGather)victim=source.variants[at];
            if(entries_[victim]->lastGather==gather_) {return nullptr;}
            // A changed strip metadata value needs new vertex bytes, not
            // a new allocation or a new identity for identical GPU indices.
            // This variant has not been referenced by this Gather. Its CPU
            // shadow can be prepared without invalidating a current shade
            // copy; native-buffer writes remain behind the preceding fence.
            auto& entry=*entries_[victim];
            const auto first=entry.slot*static_cast<std::uint32_t>(VerticesPerSlot);
            bool sameIndices=entry.indexCount==topology.indexCount;
            for(std::size_t index=0;sameIndices && index<topology.indexCount;++index)
                sameIndices=entry.indices[index]==first+topology.indices[index];
            if(!sameIndices)Require(nextIndexToken_!=std::numeric_limits<std::uint64_t>::max(),
                "Native geometry index token exhausted");
            entry.sourceGeneration=source.generation;entry.key=key;entry.count=job.count;
            if(!sameIndices) {
                entry.indexCount=static_cast<std::uint16_t>(topology.indexCount);entry.indexToken=++nextIndexToken_;
                for(std::size_t index=0;index<entry.indexCount;++index)
                    entry.indices[index]=first+topology.indices[index];

            }
            entry.prepared=false;entry.uploaded=false;
            Touch(entry);prepare=true;cached=&entry;return &entry;
        }
        if(free_.empty()) {
            if(recency_.empty() || entries_[recency_.front()]->lastGather==gather_) {
                if(!source.variantCount)ForgetSource(token);
                return nullptr;
            }
            Evict(recency_.front());
        }
        const auto slot=free_.back();free_.pop_back();
        auto owned=std::make_unique<VkResidentGeometryEntry>();auto& entry=*owned;
        entry.source=token;entry.sourceGeneration=source.generation;entry.key=key;
        entry.slot=slot;entry.count=job.count;
        Require(nextIndexToken_!=std::numeric_limits<std::uint64_t>::max(),"Native geometry index token exhausted");
        entry.indexToken=++nextIndexToken_;entry.indexCount=static_cast<std::uint16_t>(topology.indexCount);
        const auto first=slot*static_cast<std::uint32_t>(VerticesPerSlot);
        for(std::size_t index=0;index<entry.indexCount;++index)entry.indices[index]=first+topology.indices[index];
        recency_.push_back(slot);entry.recency=std::prev(recency_.end());
        entries_[slot]=std::move(owned);source.variants[source.variantCount++]=slot;
        Touch(entry);prepare=true;cached=&entry;return &entry;
    }
    void CompletePreparation() {
        for(const auto slot:active_)entries_[slot]->prepared=true;
    }
    void PublishUploads(const std::vector<std::uint32_t>& slots) {
        for(const auto slot:slots)entries_[slot]->uploaded=true;
    }
    void InvalidateUploads() {
        for(auto& entry:entries_)if(entry)entry->uploaded=false;
    }
private:
    VkResidentGeometrySource*& FrameSource(const EditorMarkerSolidGeometry::Strip* token) noexcept {
        const auto identity=reinterpret_cast<std::uintptr_t>(token);
        const auto hash=(identity>>4)^(identity>>12)^(identity>>20);
        return frameSources_[hash&(frameSources_.size()-1)];
    }
    VkResidentGeometrySource* FindSource(const EditorMarkerSolidGeometry::Strip* token) {
        auto& cached=FrameSource(token);
        if(cached && cached->token==token)return cached;
        const auto found=sources_.find(token);
        if(found==sources_.end())return nullptr;
        cached=found->second.get();return cached;
    }
    void ForgetSource(const EditorMarkerSolidGeometry::Strip* token) {
        auto& cached=FrameSource(token);
        if(cached && cached->token==token)cached=nullptr;
        sources_.erase(token);
    }
    void Touch(VkResidentGeometryEntry& entry) {
        if(entry.lastGather!=gather_) {active_.push_back(entry.slot);entry.lastGather=gather_;}
        recency_.splice(recency_.end(),recency_,entry.recency);
    }
    void Evict(std::uint32_t slot) {
        auto& entry=*entries_[slot];
        Require(entry.lastGather!=gather_,"Cannot recycle resident geometry referenced by the current frame");
        auto found=sources_.find(entry.source);
        if(found!=sources_.end()) {
            auto& source=*found->second;
            for(std::size_t at=0;at<source.variantCount;++at)if(source.variants[at]==slot) {
                source.variants[at]=source.variants[--source.variantCount];break;
            }
            if(!source.variantCount && source.validatedGather!=gather_)ForgetSource(entry.source);
        }
        recency_.erase(entry.recency);entries_[slot].reset();free_.push_back(slot);
    }
    std::uint64_t gather_{};
    std::uint64_t nextIndexToken_{};
    std::vector<std::unique_ptr<VkResidentGeometryEntry>> entries_;
    std::array<VkResidentGeometryEntry*,1024> frameEntries_{};
    std::array<VkResidentGeometrySource*,1024> frameSources_{};
    std::vector<Validation> validations_;
    bool validationsReady_{};
    std::vector<std::uint32_t> free_,active_;
    std::list<std::uint32_t> recency_;
    std::unordered_map<const EditorMarkerSolidGeometry::Strip*,std::unique_ptr<VkResidentGeometrySource>> sources_;
};

// An index token identifies one immutable sequence of absolute GPU indices.
// Updating vertex bytes in the same slot preserves it only while that exact
// index sequence is unchanged. Tokens are never reassigned to other indices.
// A merged draw is keyed by their exact ordered tokens, not a hash alone.
// The cache retains only tokens and index bytes, never geometry pointers.
struct VkResidentIndexEntry {
    std::vector<std::uint64_t> tokens;
    std::vector<std::uint32_t> shadow;
    std::uint64_t hash{},lastGather{};
    std::uint32_t first{},allocated{};
    bool uploaded{};
};

class VkResidentIndexCache {
public:
    void Configure(std::uint32_t capacity) {
        Require(entries_.empty(),"Resident index capacity cannot change while entries exist");
        capacity_=capacity&~std::uint32_t(63);free_.clear();
        if(capacity_)free_.emplace(0,capacity_);
    }
    std::uint32_t Capacity() const noexcept {return capacity_;}
    std::size_t Size() const noexcept {return entries_.size();}
    const std::vector<VkResidentIndexEntry*>& Active() const noexcept {return active_;}
    void Begin(std::uint64_t gather) {
        if(gather<=gather_) {
            entries_.clear();lookup_.clear();free_.clear();
            if(capacity_)free_.emplace(0,capacity_);
        }
        gather_=gather;active_.clear();
        while(!entries_.empty() && gather_-entries_.front().lastGather>600)Evict(entries_.begin());
    }
    VkResidentIndexEntry* Acquire(const std::vector<const VkResidentGeometryEntry*>& spans,
        std::uint32_t first,std::uint32_t count)
    {
        Require(first<=spans.size() && count<=spans.size()-first,"Resident index spans exceed their frame owner");
        std::uint64_t hash=1469598103934665603ull;std::uint64_t indices=0;
        for(std::size_t at=first;at<static_cast<std::size_t>(first)+count;++at) {
            const auto& span=*spans[at];indices+=span.indexCount;
            hash=(hash^span.indexToken)*1099511628211ull;
        }
        if(!indices || indices>capacity_) {return nullptr;}
        const auto candidates=lookup_.equal_range(hash);
        for(auto found=candidates.first;found!=candidates.second;++found) {
            auto entry=found->second;
            if(entry->tokens.size()!=count)continue;
            bool equal=true;
            for(std::size_t at=0;at<count;++at)
                if(entry->tokens[at]!=spans[first+at]->indexToken) {equal=false;break;}
            if(!equal)continue;
            Touch(entry);return &*entry;
        }
        const auto needed=(static_cast<std::uint32_t>(indices)+63u)&~std::uint32_t(63);
        auto range=Find(needed);
        while(range==free_.end()) {
            if(entries_.empty() || entries_.front().lastGather==gather_) {return nullptr;}
            Evict(entries_.begin());range=Find(needed);
        }
        // Allocate payload before consuming a free range, so allocation
        // failures cannot leave a partially published GPU index entry.
        VkResidentIndexEntry value;value.hash=hash;value.first=range->first;value.allocated=needed;
        value.tokens.reserve(count);value.shadow.reserve(static_cast<std::size_t>(indices));
        for(std::size_t at=first;at<static_cast<std::size_t>(first)+count;++at) {
            const auto& span=*spans[at];value.tokens.push_back(span.indexToken);
            value.shadow.insert(value.shadow.end(),span.indices.begin(),span.indices.begin()+span.indexCount);
        }
        const auto remaining=range->second-needed;const auto after=range->first+needed;
        // Publish both associative owners before removing the available range.
        entries_.push_back(std::move(value));auto entry=std::prev(entries_.end());
        try {lookup_.emplace(hash,entry);}catch(...) {entries_.erase(entry);throw;}
        if(remaining) {
            try {free_.emplace(after,remaining);}catch(...) {
                EraseLookup(entry);entries_.erase(entry);throw;
            }
        }
        free_.erase(range);Touch(entry);
        return &*entry;
    }
    void InvalidateUploads() {for(auto& entry:entries_)entry.uploaded=false;}
    void PublishUploads(const std::vector<VkResidentIndexEntry*>& entries) {
        for(auto* entry:entries)entry->uploaded=true;
    }
private:
    using Entries=std::list<VkResidentIndexEntry>;
    using Ranges=std::map<std::uint32_t,std::uint32_t>;
    Ranges::iterator Find(std::uint32_t count) {
        return std::find_if(free_.begin(),free_.end(),[&](const auto& range){return range.second>=count;});
    }
    void Touch(Entries::iterator entry) {
        if(entry->lastGather!=gather_) {active_.push_back(&*entry);entry->lastGather=gather_;}
        entries_.splice(entries_.end(),entries_,entry);
    }
    void EraseLookup(Entries::iterator entry) {
        const auto candidates=lookup_.equal_range(entry->hash);
        for(auto at=candidates.first;at!=candidates.second;++at)if(at->second==entry) {lookup_.erase(at);return;}
    }
    void Evict(Entries::iterator entry) {
        Require(entry->lastGather!=gather_,"Cannot recycle resident indices referenced by the current frame");
        auto first=entry->first;auto count=entry->allocated;
        auto next=free_.lower_bound(first);
        if(next!=free_.begin()) {
            auto previous=std::prev(next);
            if(previous->first+previous->second==first) {
                first=previous->first;count+=previous->second;free_.erase(previous);
            }
        }
        next=free_.lower_bound(first);
        if(next!=free_.end() && first+count==next->first) {count+=next->second;free_.erase(next);}
        free_.emplace(first,count);EraseLookup(entry);entries_.erase(entry);
    }
    std::uint64_t gather_{};
    std::uint32_t capacity_{};
    Entries entries_;
    Ranges free_;
    std::unordered_multimap<std::uint64_t,Entries::iterator> lookup_;
    std::vector<VkResidentIndexEntry*> active_;
};

struct VkNativeBuffer {VkBuffer buffer{};VkDeviceMemory memory{};VkDeviceSize size{};void* mapped{};};
struct VkNativeImage {VkImage image{};VkDeviceMemory memory{};VkImageView view{};};
struct VkNativeTextureKey {
    const TextureInfo* source{};
    bool repeatedPadding{true};
    bool operator==(const VkNativeTextureKey& other) const noexcept
    {return source==other.source && repeatedPadding==other.repeatedPadding;}
    bool operator<(const VkNativeTextureKey& other) const noexcept
    {return source!=other.source ? std::less<const TextureInfo*>{}(source,other.source):repeatedPadding<other.repeatedPadding;}
};
struct VkNativeTextureKeyHash {
    std::size_t operator()(const VkNativeTextureKey& key) const noexcept
    {return std::hash<const TextureInfo*>{}(key.source)^(key.repeatedPadding ? std::size_t(0x9e3779b9u):0u);}
};
bool NativeDrawClampsTexture(const EcstasyDrawCommand& draw) noexcept
{
    return !draw.sampledTarget && draw.kind!=EcstasyDrawKind::ScreenFilterPolygon &&
        draw.kind!=EcstasyDrawKind::RenderTargetStrip && draw.kind!=EcstasyDrawKind::DestinationFilterPass &&
        (draw.nativeRenderEggState&EcstasyRenderEggClampTopLeft)!=0;
}
VkNativeTextureKey NativeTextureKey(const TextureInfo& texture,bool forcedClamp)
{
    const auto* descriptor=texture.descriptor;
    Require(descriptor && descriptor->authoredRecord && descriptor->authoredRecordSize>=164,
        "Native texture context lost its retained descriptor");
    const auto* wire=descriptor->authoredRecord;
    Require(wire[0x48]<=14 && wire[0x49]<=14,"Native texture context has invalid dimensions");
    const bool repeatNpot=((wire[0x4c]&1u) && ReadU16(wire+0x24)<(1u<<wire[0x48])) ||
        ((wire[0x4c]&2u) && ReadU16(wire+0x26)<(1u<<wire[0x49]));
    // The same retained texture can be drawn in both source contexts. A
    // forced region-clamp uses the previous padded image byte-for-byte;
    // only a repeating NPOT context gets the repaired native extension.
    return {&texture,!(forcedClamp && repeatNpot)};
}
void CollectNativeTextureReferences(std::vector<VkNativeTextureKey>& referenced,
    const std::vector<EcstasyDrawCommand>& commands)
{
    std::array<std::byte,32768> localStorage;
    std::pmr::monotonic_buffer_resource scratch(localStorage.data(),localStorage.size());
    std::pmr::unordered_set<VkNativeTextureKey,VkNativeTextureKeyHash> seen{&scratch};
    if(!referenced.empty())seen.reserve(referenced.size());
    referenced.clear();
    const TextureInfo* lastTexture=nullptr;
    bool lastClamp=false;
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
        if(seen.insert(key).second)referenced.push_back(key);
    }
    std::sort(referenced.begin(),referenced.end());
}
// CPU scratch for one sealed frame only. Collection stays at its original
// first-use point, preserving preflight short-circuit/error/fence ordering.
// The view owns no resource and never survives this Render invocation.
class VkNativeFrameTextureReferences {
public:
    VkNativeFrameTextureReferences(const std::vector<EcstasyDrawCommand>& commands,
        std::vector<VkNativeTextureKey>& scratch) noexcept
        :commands_(commands),keys_(scratch) {}
    const std::vector<VkNativeTextureKey>& Get() const {
        if(!collected_) {
            CollectNativeTextureReferences(keys_,commands_);
            collected_=true;
        }
        return keys_;
    }
private:
    const std::vector<EcstasyDrawCommand>& commands_;
    std::vector<VkNativeTextureKey>& keys_;
    mutable bool collected_{};
};

bool NativeTextureReferenced(const std::vector<VkNativeTextureKey>& referenced,VkNativeTextureKey key)
{return std::binary_search(referenced.begin(),referenced.end(),key);}

struct VkDrawTexture {
    struct OpaqueSceneryState {
        std::uint32_t blend{};
        float blendConstant{},lodScale{};
        std::int32_t textureFunction{},minification{};
        bool depthWrite{},fullAlpha{};
        bool valid{};
        // Wireframe, texturing, clamp, depth-write and alpha-test variants.
        // Pipelines remain owned by Storage::pipelines.
        std::array<VkPipeline,32> pipelines{};
    } scenery;
    VkNativeImage image;VkSampler sampler{},magnifiedSampler{},uiSampler{};VkDescriptorSet descriptor{};
    std::uint32_t width{},height{},levels{};
    // Identity is only an associative token. It is never dereferenced after
    // its source frame. Reuse requires exact descriptors and either the
    // same published immutable byte range or a fresh byte comparison.
    // Escaped writable pointers and borrowed movie planes always use the
    // latter path. No game resource owner is retained here.
    const TextureInfo* source{};
    bool repeatedPadding{true};
    std::array<std::uint8_t,164> sourceDescriptor{};
    std::vector<std::uint8_t> sourcePixels,sourcePalette;
    std::int32_t sourcePaletteBytes{};
    // Weak lifetime witnesses keep no track/menu resource alive. A texture
    // absent from one alternating environment face is still reusable while
    // its actual pack metadata and loaded bytes remain owned by the game.
    std::weak_ptr<const TextureMetadataStorage> metadataOwner;
    std::weak_ptr<TexturePixelStorage> pixelOwner,paletteOwner;
    std::size_t sourcePixelOffset{},sourcePaletteOffset{};
    bool hasPaletteOwner{},sourcePaletteLinear{};
    std::uint64_t lastReferencedFrame{};
    std::uint64_t generation{};
    std::uint64_t validatedGather{};
    bool uploadCompleted{};
    bool OwnersAlive() const noexcept
    {
        return !metadataOwner.expired() && !pixelOwner.expired() &&
            (!hasPaletteOwner || !paletteOwner.expired());
    }
    void CaptureOwners(const TextureInfo& texture)
    {
        metadataOwner=textureMetadataOwner(texture.descriptor);
        pixelOwner=texture.pixels.borrowed ? std::weak_ptr<TexturePixelStorage>{}:texture.pixels.owner;
        paletteOwner=texture.palette.borrowed ? std::weak_ptr<TexturePixelStorage>{}:texture.palette.owner;
        sourcePixelOffset=texture.pixels.offset;sourcePaletteOffset=texture.palette.offset;
        sourcePaletteLinear=texture.palette.nativeLinearPalette;
        hasPaletteOwner=!sourcePalette.empty();
    }
    bool Matches(const TextureInfo& texture) const noexcept
    {
        const auto* textureDescriptor=texture.descriptor;
        if(!uploadCompleted || !image.image || !textureDescriptor || !textureDescriptor->authoredRecord ||
            textureDescriptor->authoredRecordSize<sourceDescriptor.size() ||
            std::memcmp(textureDescriptor->authoredRecord,sourceDescriptor.data(),sourceDescriptor.size()) ||
            !texture.pixels.Data() || texture.pixels.size<sourcePixels.size() ||
            texture.paletteBytes!=sourcePaletteBytes || texture.palette.nativeLinearPalette!=sourcePaletteLinear ||
            (!sourcePalette.empty() && (!texture.palette.Data() || texture.palette.size<sourcePalette.size())))return false;
        const auto sameBytes=[&](const TextureByteView& view,
            const std::weak_ptr<TexturePixelStorage>& owner,std::size_t offset,
            const std::vector<std::uint8_t>& bytes) {
            if(!view.borrowed && view.owner && view.offset==offset &&
                view.owner->PublishedReadOnly() && owner.lock()==view.owner) {
                return true;
            }

            return std::memcmp(view.Data(),bytes.data(),bytes.size())==0;
        };
        return sameBytes(texture.pixels,pixelOwner,sourcePixelOffset,sourcePixels) &&
            (sourcePalette.empty() || sameBytes(texture.palette,paletteOwner,sourcePaletteOffset,sourcePalette));
    }
};
VkDrawTexture::OpaqueSceneryState& ResolveOpaqueSceneryMaterial(
    VkDrawTexture& texture,const std::uint8_t* wire,EcstasyTextureFilter filter)
{
    auto& material=texture.scenery;
    if(!material.valid) {
        const auto alpha=wire[0x76];
        material.blend=(alpha&3u)|(((alpha>>2)&3u)<<8)|
            (((alpha>>4)&3u)<<16)|(((alpha>>6)&3u)<<24);
        material.blendConstant=static_cast<float>(wire[0x77])/128.0f;
        material.lodScale=static_cast<float>(1u<<wire[0x71]);
        material.textureFunction=wire[0x4e];
        material.minification=filter==EcstasyTextureFilter::Authored?wire[0x70]:-2;
        material.depthWrite=wire[0x50]!=0;
        material.fullAlpha=wire[0x4f]!=0;
        material.valid=true;
    }
    return material;
}

struct VkDrawTarget {
    VkNativeImage color,depth,previous;
    VkImageView reflectionSampleView{},previousReflectionSampleView{};
    // Current-stage snapshot, never aliases previous-frame history.
    VkNativeImage feedback;
    VkDescriptorPool feedbackPool{};
    VkDescriptorSet feedbackDescriptor{},filterFeedbackDescriptor{};
    bool feedbackInitialized{};
    VkFramebuffer framebuffer{};
    VkSampler sampler{},magnifiedSampler{};
    VkDescriptorPool descriptorPool{};
    VkDescriptorSet descriptor{},previousDescriptor{};
    std::uint32_t width{},height{};
    bool initialized{};
};

// Timing only contiguous category runs avoids a clock call per strip.
// Detailed packet timing is enabled on one submission in32.
class NativeGatherCategoryTimer {
    using Clock=std::chrono::steady_clock;
    EcstasyVulkanRenderProfile* profile_{};
    std::size_t active_{6};
    Clock::time_point started_{};
public:
    explicit NativeGatherCategoryTimer(EcstasyVulkanRenderProfile* profile):profile_(profile) {}
    void Enter(const EcstasyDrawCommand& draw) {
        if(!profile_)return;
        const auto category=Category(draw);
        ++profile_->gatherCategoryCommands[category];
        if(category==active_)return;
        const auto now=Clock::now();
        if(active_<6)profile_->gatherCategoryMilliseconds[active_]+=
            std::chrono::duration<double,std::milli>(now-started_).count();
        active_=category;started_=now;
    }
    void Finish() {
        if(!profile_ || active_>=6)return;
        profile_->gatherCategoryMilliseconds[active_]+=
            std::chrono::duration<double,std::milli>(Clock::now()-started_).count();
        active_=6;
    }
    static std::size_t Category(const EcstasyDrawCommand& draw) noexcept {
        if(draw.cachedOpaqueScenery)return 0;
        switch(draw.kind) {
        case EcstasyDrawKind::SolidStrip:case EcstasyDrawKind::SolidReflectionStrip:
        case EcstasyDrawKind::SolidSpecularStrip:return 1;
        case EcstasyDrawKind::ParticleGroup:return 2;
        case EcstasyDrawKind::WorldPolygon:case EcstasyDrawKind::WorldStrip:return 3;
        case EcstasyDrawKind::FlailerPolygon:case EcstasyDrawKind::RenderTargetPolygon:
        case EcstasyDrawKind::MirrorOverlayPolygon:case EcstasyDrawKind::ScreenPolygon:return 4;
        default:return 5;
        }
    }
};

struct VkNativeTransformKeyHash {
    std::size_t operator()(const std::array<std::uint32_t,22>& key) const noexcept {
        std::size_t hash=1469598103934665603ull;
        for(const auto word:key)hash=(hash^word)*1099511628211ull;
        return hash;
    }
};

struct VkDrawBatch {
    std::uint32_t first{},count{};
    bool indexed{};
    bool cachedOpaqueScenery{};
    bool residentGeometry{};
    bool residentIndices{};
    std::uint32_t nativeSpanFirst{},nativeSpanCount{};
    std::size_t texture{};
    std::int32_t target{},sampledTarget{-1};
    std::uint32_t feedbackGroup{};
    bool sampledPreviousFrame{};
    std::uint32_t blend{};
    float blendConstant{};
    bool depth{};
    bool depthWrite{};
    bool postProcess{};
    bool showcaseEnvironmentBlend{};
    bool wireframe{},untextured{},clampTopLeft{};
    VkRect2D scissor{};
    VkViewport viewport{};
    std::int32_t textureFunction{},alphaTest{};
    float lodScale{1.0f};
    std::int32_t minification{};
    std::uint32_t focusFirst{};
    std::int32_t focusPassCount{};
    float focusBlend{};
    // This model-owned transform is no longer redundantly applied to every
    // source vertex on the CPU. GPU push constants affect draw continuity:
    // different instances must never merge even with identical materials.
    bool nativeGpuPosition{},nativeIndirectTransform{};
    std::uint32_t nativeTransformIndex{},nativeShadingIndex{};
    float nativeTextureQScale{};
    std::array<float,16> nativeLocalToClip{};
    std::array<float,4> nativeClipMapping{{1.0f,1.0f,0.0f,0.0f}};
    bool SameDrawState(const VkDrawBatch& previous) const noexcept
    {
        return residentGeometry==previous.residentGeometry && residentIndices==previous.residentIndices &&
            nativeGpuPosition==previous.nativeGpuPosition && nativeIndirectTransform==previous.nativeIndirectTransform &&
            indexed==previous.indexed && texture==previous.texture &&
            target==previous.target && sampledTarget==previous.sampledTarget &&
            feedbackGroup==previous.feedbackGroup &&
            sampledPreviousFrame==previous.sampledPreviousFrame && blend==previous.blend &&
            blendConstant==previous.blendConstant && depth==previous.depth && depthWrite==previous.depthWrite &&
            postProcess==previous.postProcess && showcaseEnvironmentBlend==previous.showcaseEnvironmentBlend &&
            wireframe==previous.wireframe &&
            untextured==previous.untextured && clampTopLeft==previous.clampTopLeft &&
            textureFunction==previous.textureFunction && alphaTest==previous.alphaTest &&
            focusFirst==previous.focusFirst && focusPassCount==previous.focusPassCount && focusBlend==previous.focusBlend &&
            lodScale==previous.lodScale && minification==previous.minification &&
            scissor.offset.x==previous.scissor.offset.x && scissor.offset.y==previous.scissor.offset.y &&
            scissor.extent.width==previous.scissor.extent.width && scissor.extent.height==previous.scissor.extent.height &&
            viewport.x==previous.viewport.x && viewport.y==previous.viewport.y &&
            viewport.width==previous.viewport.width && viewport.height==previous.viewport.height &&
            viewport.minDepth==previous.viewport.minDepth && viewport.maxDepth==previous.viewport.maxDepth;
    }
    bool Continues(const VkDrawBatch& previous) const noexcept
    {
        return SameDrawState(previous) && previous.first+previous.count==first &&
            (!residentGeometry || previous.nativeSpanFirst+previous.nativeSpanCount==nativeSpanFirst) &&
            (!nativeGpuPosition || (nativeIndirectTransform ? nativeTransformIndex==previous.nativeTransformIndex :
                nativeLocalToClip==previous.nativeLocalToClip && nativeClipMapping==previous.nativeClipMapping &&
                nativeTextureQScale==previous.nativeTextureQScale && nativeShadingIndex==previous.nativeShadingIndex));
    }
    bool CanBatchIndirectly(const VkDrawBatch& previous) const noexcept
    {
        // Only geometry ranges and per-subdraw transforms may differ. All
        // raster/material/resource/pass state and original order stay intact.
        return indexed && nativeIndirectTransform && previous.nativeIndirectTransform && SameDrawState(previous);
    }
};

class VkSceneryPacketCache {
    struct Entry {
        std::uint64_t indexToken{},geometryGeneration{},textureGeneration{};
        VkResidentGeometryKey geometryKey;
        std::uint32_t flags{};
        VkDrawBatch batch;
        bool Matches(const VkResidentGeometryEntry& resident,std::uint64_t texture,
            std::uint32_t state) const noexcept {
            return indexToken && indexToken==resident.indexToken &&
                geometryGeneration==resident.sourceGeneration && geometryKey==resident.key &&
                textureGeneration==texture && flags==state;
        }
    };
public:
    void Configure(std::size_t count) {entries_.resize(count);}
    const VkDrawBatch* Find(const VkResidentGeometryEntry& resident,
        std::uint64_t texture,std::uint32_t flags) const noexcept {
        const auto& entry=entries_[resident.slot];
        return entry.Matches(resident,texture,flags) ? &entry.batch:nullptr;
    }
    void Store(const VkResidentGeometryEntry& resident,std::uint64_t texture,
        std::uint32_t flags,const VkDrawBatch& batch) noexcept {
        auto& entry=entries_[resident.slot];
        entry.batch=batch;
        entry.geometryKey=resident.key;entry.indexToken=resident.indexToken;
        entry.geometryGeneration=resident.sourceGeneration;
        entry.textureGeneration=texture;entry.flags=flags;
    }
private:
    // One packet per bounded resident slot avoids conflicts in large scenes.
    // This cache owns no texture, source geometry, Vulkan handle or resource lifetime.
    std::vector<Entry> entries_;
};

struct VkFogDepthPlan {
    std::size_t first{},last{};
    bool present{},direct{};
};

VkFogDepthPlan PlanFogDepth(const std::vector<VkDrawBatch*>& packets)
{
    VkFogDepthPlan plan;
    for(std::size_t index=0;index<packets.size();++index) {
        const auto function=packets[index]->textureFunction;
        if(function!=5 && function!=6)continue;
        if(!plan.present)plan.first=index;
        plan.last=index;plan.present=true;
    }
    if(!plan.present)return plan;
    plan.direct=true;
    // All source views share the resolved main Z image. A later view must
    // still read the FIRST fog stage's snapshot if an authored packet uses
    // depth between readers. That stream needs the original separate copy.
    // Otherwise omit the depth attachment entirely while sampling it: even
    // a depth-disabled attachment STORE/resolve may write the same image.
    for(auto index=plan.first;index<=plan.last;++index) {
        const auto& packet=*packets[index];
        if(packet.depth || packet.depthWrite) {plan.direct=false;break;}
    }
    return plan;
}

void ResolveNativeResidentIndices(VkResidentIndexCache& cache,
    const std::vector<const VkResidentGeometryEntry*>& spans,std::vector<std::uint32_t>& indices,
    std::vector<VkDrawBatch>& batches)
{
    for(auto& batch:batches) {
        if(!batch.residentGeometry)continue;
        if(auto* entry=cache.Acquire(spans,batch.nativeSpanFirst,batch.nativeSpanCount)) {
            Require(entry->shadow.size()==batch.count,"Resident merged index extent changed during Gather");
            batch.residentIndices=true;batch.first=entry->first;
        } else {
            // Capacity pressure changes only transport; preserve one indexed
            // draw and the exact same triangle order in the frame buffer.
            Require(indices.size()+batch.count<=std::numeric_limits<std::uint32_t>::max(),
                "Native streamed frame index count exceeds32-bit draw range");
            batch.first=static_cast<std::uint32_t>(indices.size());
            for(std::size_t at=batch.nativeSpanFirst;at<static_cast<std::size_t>(batch.nativeSpanFirst)+batch.nativeSpanCount;++at) {
                const auto& source=*spans[at];
                indices.insert(indices.end(),source.indices.begin(),source.indices.begin()+source.indexCount);
            }
        }
    }
}


std::array<float,4> vulkanColor(std::uint32_t color,float alphaScale)
{
    return {static_cast<float>(color&255u)/128.0f,static_cast<float>((color>>8)&255u)/128.0f,
        static_cast<float>((color>>16)&255u)/128.0f,static_cast<float>(color>>24)*alphaScale/128.0f};
}
}

struct EcstasyVulkanDevice::Storage {
    EcstasyVulkanOptions options;
    std::uint64_t completedFrameSerial{};
    float actualAnisotropy{1.0f};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t queueFamily{},timestampValidBits{};
    VkQueryPool timestampQueries{};
    // One setup boundary and one completion boundary for each target slot,
    // in addition to the two full-frame timestamp queries.
    static constexpr std::uint32_t TimestampQueryCapacity=18;
    std::array<std::int32_t,TimestampQueryCapacity> timestampTargetIds{};
    std::uint32_t submittedTimestampCount{2};
    std::uint64_t nextSubmission{},querySubmission{};
    EcstasyVulkanFrameStatistics statistics;
    VkSurfaceKHR surface{};
    VkSwapchainKHR swapchain{};
    VkFormat swapFormat{};
    std::vector<VkImage> swapImages;
    VkSemaphore acquired{};
    std::vector<VkSemaphore> presentationSemaphores;
    VkCommandPool commandPool{};
    VkCommandBuffer commands{};
    struct NativeRecordingLane {
        VkCommandPool pool{};
        std::vector<VkCommandBuffer> buffers;
        std::size_t used{};
    };
    // One externally synchronized command pool per simultaneous recording
    // job. A lane can be reused by a later job only after the prior batch
    // drains; its buffers remain intact until this frame's GPU fence.
    std::vector<NativeRecordingLane> recordingLanes;
    VkFence fence{};
    VkRenderPass renderPass{},preserveRenderPass{},postProcessRenderPass{},resumeRenderPass{};
    VkRenderPass focusBlurRenderPass{};
    VkRenderPass multisampleRenderPass{},resumeMultisampleRenderPass{};
    VkRenderPass depthSamplingRenderPass{};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    VkExtent2D presentationExtent{};
    VkFramebuffer framebuffer{},postProcessFramebuffer{};
    VkFramebuffer depthSamplingFramebuffer{};
    VkNativeImage color,depth;
    VkNativeImage multisampleColor,multisampleDepth;
    // Targets7..14 are the original rear/face/remap images. Target0 owns
    // only the actual preceding presentation image; IDs0..6 alias the same
    // main color attachment. Histories prevent illegal attachment feedback.
    std::map<std::int32_t,VkDrawTarget> renderedTargets;
    VkDrawTarget fogDepth,focusSource,focusBlur;
    // This set borrows depth.view; fogDepth owns only its sampler/pool and
    // the optional COPY image. Never alias the source image's ownership.
    VkDescriptorSet fogSourceDescriptor{};
    VkNativeBuffer vertices,indices,glyphBounds,indirectCommands;
    bool nativeMultiDraw{};
    std::vector<VkDrawIndexedIndirectCommand> nativeDrawCommands;
    VkNativeBuffer vertexStaging,indexStaging;
    VkNativeBuffer residentVertices,residentStaging;
    VkNativeBuffer residentIndices,residentIndexStaging;
    VkResidentGeometryCache residentGeometry;
    VkResidentIndexCache residentIndexCache;
    VkSceneryPacketCache sceneryPackets;
    std::uint64_t nextTextureGeneration{};
    std::vector<const VkResidentGeometryEntry*> gatheredResidentIndexSpans;
    std::vector<VkResidentIndexEntry*> residentIndexPendingUploads;
    std::vector<VkBufferCopy> residentIndexCopies;
    std::vector<std::uint32_t> residentPendingUploads;
    std::vector<VkBufferCopy> residentCopies;
    std::uint64_t residentFrameVertices{};
    std::uint64_t residentFrameIndices{};
    enum class GeometryMemory { HostCoherent, LocalCoherent, StagedDeviceLocal };
    GeometryMemory geometryMemory{GeometryMemory::HostCoherent};
    VkDescriptorSetLayout descriptorLayout{};
    VkDescriptorPool descriptorPool{};
    VkDescriptorSetLayout frameDescriptorLayout{};
    VkDescriptorPool frameDescriptorPool{};
    VkDescriptorSet frameDescriptor{};
    VkPipelineLayout pipelineLayout{};
    VkPipelineCache pipelineCache{};
    VkShaderModule vertexShader{},fragmentShader{};
    using PipelineKey=std::tuple<std::uint32_t,bool,bool,bool,bool,bool,std::int32_t,std::int32_t,std::int32_t,bool,bool,bool,bool>;
    std::map<PipelineKey,VkPipeline> pipelines;
    PipelineKey lastPipelineKey{};
    VkPipeline lastPipeline{};
    std::map<PipelineKey,std::future<VkPipeline>> pendingPipelines;
    std::mutex pipelineMutex;
    std::condition_variable pipelineWake;
    std::deque<std::packaged_task<VkPipeline()>> pipelineJobs;
    std::thread pipelineWorker;
    bool stopPipelineWorker{};
    std::vector<VkDrawTexture> textures;
    std::unordered_map<VkNativeTextureKey,std::size_t,VkNativeTextureKeyHash> textureIndices;
    std::vector<VkNativeTextureKey> referencedTextures;
    std::uint64_t gatherSerial{};
    std::uint32_t journalWorkTicks{};
    bool journalWorkReported{};
    std::vector<VkVertex> gatheredVertices;
    std::vector<std::uint32_t> gatheredIndices;
    std::vector<VkDrawBatch> gatheredBatches;
    // Keep lists in the original per-target packet order, but do not scan
    // the entire draw stream again for every sampled target and effect view.
    std::array<std::vector<VkDrawBatch*>,15> targetDraws;
    std::array<std::vector<VkPipeline>,15> preparedTargetPipelines;
    struct NativeRecordedSegment {
        std::size_t first{},last{};
        VkRenderPass renderPass{};
        VkFramebuffer framebuffer{};
        std::vector<VkCommandBuffer> buffers;
        std::uint64_t draws{};
    };
    struct NativeRecordedPass {
        std::size_t packetCount{};
        bool secondary{};
        VkFogDepthPlan fog;
        std::vector<NativeRecordedSegment> segments;
    };
    std::array<NativeRecordedPass,15> recordedPasses;
    std::array<VkBuffer,6> recordedBuffers{};
    VkDescriptorSet recordedFrameDescriptor{};
    VkPipelineLayout recordedPipelineLayout{};
    bool recordedCommandsValid{};
    void InvalidateRecordedCommands() noexcept {
        recordedCommandsValid=false;
        for(auto& pass:recordedPasses)pass.segments.clear();
    }
    std::array<VkBuffer,6> RecordedBufferHandles() const noexcept {
        return {{vertices.buffer,indices.buffer,residentVertices.buffer,
            residentIndices.buffer,glyphBounds.buffer,indirectCommands.buffer}};
    }
    void UpdateDescriptors(std::uint32_t count,const VkWriteDescriptorSet* writes) {
        // An ordinary descriptor write invalidates executable command buffers,
        // even if its contents are unchanged. Never retain that recording.
        if(recordedCommandsValid)InvalidateRecordedCommands();
        vkUpdateDescriptorSets(device,count,writes,0,nullptr);
    }
    std::array<std::vector<std::int32_t>,15> targetDependencies;
    std::vector<std::array<float,4>> gatheredGlyphBounds;
    struct NativePreparedDepthInput {
        std::shared_ptr<SunDepthSamples> destination;
        SunDepthSamples captured;
        std::uint32_t visibleSamples{};
        bool visibleSamplesValid{};
    };
    struct NativePreparedFrame {
        bool valid{};
        std::size_t sourceCommands{},textureKeys{},vertexCount{},glyphCount{};
        std::uint64_t particles{};
        std::array<double,2> displayTranslation{};
        EcstasyNativeHistoryReaders historyReaders;
        bool sceneHistoryBoundary{},useDisplayHistory{};
        std::set<std::int32_t> destinations;
        std::vector<EcstasyDepthTransfer> depthTransfers;
        std::vector<NativePreparedDepthInput> depthInputs;
    } preparedFrame;
    // Reuse is admitted only by the host's unchanged visual-generation
    // contract. These values own their storage; no old EcstasyFrame, draw,
    // strip, TextureInfo or gameplay view is read by a repeated submission.
    // Depth destinations retain their established shared readback lifetime.
    bool CanRenderPrepared() const noexcept {
        if(!preparedFrame.valid || !hasFrame || preparedFrame.historyReaders.display)return false;
        for(const auto& input:preparedFrame.depthInputs) {
            std::uint32_t visibleSamples{};
            if(!input.destination || !input.visibleSamplesValid ||
                !EcstasyNativeSunVisibleSampleCount(*input.destination,visibleSamples) ||
                visibleSamples!=input.visibleSamples)return false;
        }
        return true;
    }
    void InvalidatePrepared() noexcept {
        InvalidateRecordedCommands();
        preparedFrame.valid=false;
        preparedFrame.depthInputs.clear();preparedFrame.depthTransfers.clear();
        preparedFrame.destinations.clear();
    }
    void CapturePreparedInputs(const EcstasyFrame& frame) {
        InvalidateRecordedCommands();
        preparedFrame.valid=false;
        preparedFrame.sourceCommands=frame.Commands().size();
        preparedFrame.displayTranslation=frame.DisplayTranslation();
        preparedFrame.historyReaders=EcstasyFindNativeHistoryReaders(frame.Commands());
        preparedFrame.sceneHistoryBoundary=std::any_of(frame.Commands().begin(),frame.Commands().end(),
            [](const EcstasyDrawCommand& draw){return draw.kind==EcstasyDrawKind::SceneHistoryBoundary;});
        preparedFrame.useDisplayHistory=!preparedFrame.sceneHistoryBoundary || (hasFrame && displaySceneHistoryValid);
        preparedFrame.destinations.clear();
        preparedFrame.depthTransfers=frame.DepthTransfers();
        preparedFrame.depthInputs.clear();
        // Capture the inputs seen by source display construction, BEFORE
        // Render publishes the preceding GPU submission's depth samples.
        // An eligibility check never waits for or publishes newer samples.
        for(const auto& request:preparedFrame.depthTransfers) {
            NativePreparedDepthInput input;input.destination=request.destination;
            if(input.destination)input.captured=*input.destination;
            if(request.consumedBoundsValid)input.captured.bounds=request.consumedBounds;
            input.visibleSamplesValid=EcstasyNativeSunVisibleSampleCount(input.captured,input.visibleSamples);
            preparedFrame.depthInputs.push_back(std::move(input));
        }
    }
    struct TextureUploadPage {
        VkNativeBuffer buffer;
        VkDeviceSize used{};
        std::uint64_t lastFrame{};
    };
    std::vector<TextureUploadPage> textureUploadPages;
    struct NativeDepthPalette {
        const EcstasyDepthPalettePass* source{};
        std::array<std::uint32_t,256> colors{};
        std::array<std::array<float,4>,256> converted{};
        std::uint64_t gathered{};
        std::uint32_t offset{};
        bool valid{};
    };
    std::array<NativeDepthPalette,8> depthPaletteCache{};
    // The sole released producer is SunResources, once per active target.
    // Retain one mapped native buffer, not one Vulkan allocation per tile
    // and frame. All reuse and growth happen after the preceding frame fence.
    static constexpr std::size_t MaximumDepthReadbacks=15;
    VkNativeBuffer depthStaging;
    struct DepthReadback {
        EcstasyDepthTransfer request;
        VkDeviceSize byteOffset{};
        std::int32_t left{},top{},width{},height{};
        float scaleX{},scaleY{};
        double translationX{},translationY{};
        double SampleX(std::int32_t index) const noexcept {
            const float source=static_cast<float>(static_cast<std::int64_t>(request.x)+index)+0.5f;
            return static_cast<double>((static_cast<float>(request.targetLeft)+
                source*request.nativeSampleScale)*scaleX)+translationX;
        }
        double SampleY(std::int32_t index) const noexcept {
            const float source=static_cast<float>(static_cast<std::int64_t>(request.y)+index)+0.5f;
            return static_cast<double>((static_cast<float>(request.targetTop)+
                source*request.nativeSampleScale)*scaleY)+translationY;
        }
    };
    std::vector<DepthReadback> depthReadbacks;
    bool hasFrame{};
    bool displaySceneHistoryValid{};
    // Only Vulkan-owned resources remain live after CPU frame completion.
    // Wait for this fence before reusing the command buffer, mapped geometry,
    // image descriptors or temporary staging allocations. The CPU may build
    // the NEXT game frame while the GPU executes the previous submission.
    bool nativeSubmissionPending{};

    explicit Storage(const EcstasyVulkanOptions& value):options(value)
    {
        try {Initialize();}
        catch(const std::exception& error) {
            platform::RecordSessionEvent("renderer.initialize.failed",error.what());
            Close();throw;
        } catch(...) {
            platform::RecordSessionEvent("renderer.initialize.failed","Unknown native renderer failure");
            Close();throw;
        }
    }
    ~Storage(){Close();}
    void ResetRecordingLanes()
    {
        InvalidateRecordedCommands();
        for(auto& lane:recordingLanes) {
            vkRequired(vkResetCommandPool(device,lane.pool,0),"vkResetCommandPool(recording)");
            lane.used=0;
        }
    }
    std::vector<VkCommandBuffer> AcquireRecordingBuffers(std::size_t count)
    {
        while(recordingLanes.size()<count) {
            recordingLanes.emplace_back();
            VkCommandPoolCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            create.queueFamilyIndex=queueFamily;create.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            try {
                vkRequired(vkCreateCommandPool(device,&create,nullptr,&recordingLanes.back().pool),
                    "vkCreateCommandPool(recording)");
            } catch(...) {recordingLanes.pop_back();throw;}
        }
        std::vector<VkCommandBuffer> result;result.reserve(count);
        for(std::size_t index=0;index<count;++index) {
            auto& lane=recordingLanes[index];
            if(lane.used==lane.buffers.size()) {
                // Retain the slot before allocation: an allocation exception
                // cannot leak a command-buffer handle outside its owner.
                lane.buffers.push_back(VK_NULL_HANDLE);
                VkCommandBufferAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocate.commandPool=lane.pool;allocate.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;
                allocate.commandBufferCount=1;
                try {
                    vkRequired(vkAllocateCommandBuffers(device,&allocate,&lane.buffers.back()),
                        "vkAllocateCommandBuffers(recording)");
                } catch(...) {lane.buffers.pop_back();throw;}
            }
            result.push_back(lane.buffers[lane.used++]);
        }
        return result;
    }
    std::uint32_t MemoryType(std::uint32_t mask,VkMemoryPropertyFlags flags) const
    {
        for (std::uint32_t index=0;index<memoryProperties.memoryTypeCount;++index)
            if ((mask&(1u<<index)) && (memoryProperties.memoryTypes[index].propertyFlags&flags)==flags)return index;
        throw std::runtime_error("Vulkan device has no memory type with the required properties");
    }
    void Destroy(VkNativeBuffer& buffer) noexcept
    {
        if(buffer.buffer) {
            const auto bound=RecordedBufferHandles();
            if(std::find(bound.begin(),bound.end(),buffer.buffer)!=bound.end())InvalidateRecordedCommands();
        }
        if (buffer.mapped)vkUnmapMemory(device,buffer.memory);
        if (buffer.buffer)vkDestroyBuffer(device,buffer.buffer,nullptr);
        if (buffer.memory)vkFreeMemory(device,buffer.memory,nullptr);
        buffer={};
    }
    void Destroy(VkNativeImage& image) noexcept
    {
        if(image.image || image.view)InvalidateRecordedCommands();
        if (image.view)vkDestroyImageView(device,image.view,nullptr);
        if (image.image)vkDestroyImage(device,image.image,nullptr);
        if (image.memory)vkFreeMemory(device,image.memory,nullptr);
        image={};
    }
    void CreateBuffer(VkNativeBuffer& output,VkDeviceSize size,VkBufferUsageFlags usage,
        VkMemoryPropertyFlags memoryFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        bool persistentMap=false)
    {
        VkBufferCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        create.size=std::max<VkDeviceSize>(size,4);create.usage=usage;create.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        vkRequired(vkCreateBuffer(device,&create,nullptr,&output.buffer),"vkCreateBuffer");
        VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,output.buffer,&requirements);
        VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=MemoryType(requirements.memoryTypeBits,memoryFlags);
        vkRequired(vkAllocateMemory(device,&allocation,nullptr,&output.memory),"vkAllocateMemory(buffer)");
        vkRequired(vkBindBufferMemory(device,output.buffer,output.memory,0),"vkBindBufferMemory");output.size=create.size;
        // The preceding frame fence protects the shared upload storage. A
        // persistent coherent mapping removes Vulkan mapping/driver calls
        // from every geometry-heavy frame without changing a single byte.
        if(persistentMap)vkRequired(vkMapMemory(device,output.memory,0,output.size,0,&output.mapped),
            "vkMapMemory(persistent buffer)");
    }
    void EnsureDepthReadbackCapacity(VkDeviceSize required,VkDeviceSize maximum)
    {
        Require(!nativeSubmissionPending,"Depth storage reuse precedes its completed frame fence");
        Require(required && required<=maximum && required<=std::numeric_limits<std::size_t>::max(),
            "Native depth readback exceeds its actual target/request bound");
        if(depthStaging.size>=required) {
            Require(depthStaging.mapped,"Retained depth buffer lost its persistent mapping");
            return;
        }
        Require(depthStaging.size<=maximum,"Retained depth buffer exceeds its current attachment bound");
        const auto extra=std::min(depthStaging.size/2,maximum-depthStaging.size);
        const auto capacity=std::max(required,depthStaging.size+extra);
        VkNativeBuffer replacement;
        try {
            CreateBuffer(replacement,capacity,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,true);
        } catch(...) {Destroy(replacement);throw;}
        // Keep the preceding allocation intact until creation/mapping succeeds.
        Destroy(depthStaging);depthStaging=replacement;
    }
    void CreateImage(VkNativeImage& output,std::uint32_t width,std::uint32_t height,VkFormat format,
        VkImageUsageFlags usage,VkImageAspectFlags aspect,std::uint32_t levels=1,
        VkSampleCountFlagBits imageSamples=VK_SAMPLE_COUNT_1_BIT)
    {
        VkImageCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        create.imageType=VK_IMAGE_TYPE_2D;create.format=format;create.extent={width,height,1};
        create.mipLevels=levels;create.arrayLayers=1;create.samples=imageSamples;
        create.tiling=VK_IMAGE_TILING_OPTIMAL;create.usage=usage;create.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        vkRequired(vkCreateImage(device,&create,nullptr,&output.image),"vkCreateImage");
        VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(device,output.image,&requirements);
        VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=MemoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkRequired(vkAllocateMemory(device,&allocation,nullptr,&output.memory),"vkAllocateMemory(image)");
        vkRequired(vkBindImageMemory(device,output.image,output.memory,0),"vkBindImageMemory");
        VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image=output.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=format;
        view.subresourceRange={aspect,0,levels,0,1};
        vkRequired(vkCreateImageView(device,&view,nullptr,&output.view),"vkCreateImageView");
    }
    void Barrier(VkImage image,VkImageLayout oldLayout,VkImageLayout newLayout,
        VkAccessFlags sourceAccess,VkAccessFlags destinationAccess,
        VkPipelineStageFlags sourceStage,VkPipelineStageFlags destinationStage,
        VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT,std::uint32_t levels=1)
    {
        VkImageMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout=oldLayout;barrier.newLayout=newLayout;barrier.srcAccessMask=sourceAccess;
        barrier.dstAccessMask=destinationAccess;barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=image;
        barrier.subresourceRange={aspect,0,levels,0,1};
        vkCmdPipelineBarrier(commands,sourceStage,destinationStage,0,0,nullptr,0,nullptr,1,&barrier);
    }
    void Destroy(VkDrawTexture& texture) noexcept
    {
        InvalidateRecordedCommands();
        if(texture.descriptor)(void)vkFreeDescriptorSets(device,descriptorPool,1,&texture.descriptor);
        if(texture.sampler)vkDestroySampler(device,texture.sampler,nullptr);
        if(texture.magnifiedSampler)vkDestroySampler(device,texture.magnifiedSampler,nullptr);
        if(texture.uiSampler)vkDestroySampler(device,texture.uiSampler,nullptr);

        Destroy(texture.image);texture={};
    }
    struct TextureUploadSlice {VkBuffer buffer{};VkDeviceSize offset{};};
    TextureUploadSlice StageTexture(const std::vector<std::uint8_t>& pixels)
    {
        Require(!nativeSubmissionPending,"Texture staging reuse precedes its completed frame fence");
        const auto bytes=static_cast<VkDeviceSize>(pixels.size());
        Require(bytes!=0,"Texture staging payload is empty");
        const auto alignment=std::max<VkDeviceSize>(4,properties.limits.optimalBufferCopyOffsetAlignment);
        for(auto& page:textureUploadPages) {
            const auto remainder=page.used%alignment;
            const auto offset=page.used+(remainder ? alignment-remainder:0);
            if(offset<=page.buffer.size && bytes<=page.buffer.size-offset) {
                Require(page.buffer.mapped,"Retained texture upload page lost its mapping");
                std::memcpy(static_cast<std::uint8_t*>(page.buffer.mapped)+offset,pixels.data(),pixels.size());
                page.used=offset+bytes;page.lastFrame=completedFrameSerial;
                return {page.buffer.buffer,offset};
            }
        }
        TextureUploadPage page;
        const auto capacity=std::max<VkDeviceSize>(4u*1024u*1024u,bytes);
        try {
            CreateBuffer(page.buffer,capacity,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,true);
            textureUploadPages.push_back(page);
        } catch(...) {Destroy(page.buffer);throw;}
        ++statistics.textureStagingAllocations;
        auto& retained=textureUploadPages.back();
        std::memcpy(retained.buffer.mapped,pixels.data(),pixels.size());
        retained.used=bytes;retained.lastFrame=completedFrameSerial;
        return {retained.buffer.buffer,0};
    }
    void ClearFrameAllocations() noexcept
    {
        // Every caller has joined the actual previous fence (or device idle).
        // Keep hot mapped pages; discard idle peaks after two seconds at60Hz.
        for(auto it=textureUploadPages.begin();it!=textureUploadPages.end();) {
            if(completedFrameSerial-it->lastFrame>120) {Destroy(it->buffer);it=textureUploadPages.erase(it);}
            else {it->used=0;++it;}
        }
        depthReadbacks.clear();
    }
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
    void PublishGpuTime()
    {
        if(!timestampQueries || !querySubmission)return;
        std::array<std::uint64_t,TimestampQueryCapacity> ticks{};
        // This path follows the already-required submission fence. Never add
        // WAIT_BIT or stall a current GPU frame merely to obtain diagnostics.
        const auto result=vkGetQueryPoolResults(device,timestampQueries,0,submittedTimestampCount,
            sizeof(ticks),ticks.data(),sizeof(ticks[0]),VK_QUERY_RESULT_64_BIT);
        if(result==VK_NOT_READY)return;
        vkRequired(result,"vkGetQueryPoolResults(completed frame)");
        const auto mask=timestampValidBits==64 ? UINT64_MAX:(std::uint64_t{1}<<timestampValidBits)-1u;
        const auto elapsed=(ticks[1]-ticks[0])&mask;
        const auto milliseconds=[&](std::uint64_t delta) {
            return static_cast<double>(delta)*properties.limits.timestampPeriod/1000000.0;
        };
        statistics.gpuMilliseconds=milliseconds(elapsed);
        statistics.completedGpuFrame=querySubmission;
        statistics.gpuTimeAvailable=elapsed!=0 && std::isfinite(statistics.gpuMilliseconds);
        auto& profile=statistics.gpuProfile;
        profile={};
        if(statistics.gpuTimeAvailable && submittedTimestampCount>2) {
            auto previous=(ticks[2]-ticks[0])&mask;
            if(previous>elapsed)return;
            profile.setupMilliseconds=milliseconds(previous);
            for(std::uint32_t query=3;query<submittedTimestampCount;++query) {
                const auto finish=(ticks[query]-ticks[0])&mask;
                if(finish<previous || finish>elapsed)return;
                const auto span=milliseconds(finish-previous);
                const auto id=timestampTargetIds[query];
                if(id==0)profile.mainMilliseconds+=span;
                else if(id==7)profile.mirrorMilliseconds+=span;
                else profile.auxMilliseconds+=span;
                previous=finish;
            }
            profile.tailMilliseconds=milliseconds(elapsed-previous);
            profile.available=true;
        }
    }
    bool RetireUnusedTextures(const VkNativeFrameTextureReferences& frameTextures)
    {
        // Called only after the prior GPU fence and command-buffer reset.
        // Residency follows the real loaded resource lifetime, rather than
        // one frame's visibility. Alternate environment views revisit live
        // textures; destroying them on every absent frame causes upload churn.
        // Borrowed movie planes have no independent owner and are retired
        // when absent. No expired TextureInfo pointer is dereferenced here.
        const auto& referenced=frameTextures.Get();
        const auto isReferenced=[&](const VkDrawTexture& texture) {
            return NativeTextureReferenced(referenced,{texture.source,texture.repeatedPadding});
        };
        Require(referenced.size()<=4096,"Frame exceeds the Vulkan descriptor allocation capacity");
        bool changed=false;
        for(auto at=textures.begin();at!=textures.end();) {
            if(isReferenced(*at)) {
                at->lastReferencedFrame=completedFrameSerial;++at;
            } else if(!at->OwnersAlive()) {Destroy(*at);at=textures.erase(at);changed=true;}
            else ++at;
        }
        std::size_t missing=referenced.size();
        for(const auto& texture:textures)if(isReferenced(texture))--missing;
        // The existing native descriptor budget remains bounded. Only
        // unreferenced images may be evicted, oldest use first; a frame's
        // full actual resource set always fits before recording starts.
        while(textures.size()+missing>4096) {
            auto oldest=textures.end();
            for(auto at=textures.begin();at!=textures.end();++at)
                if(!isReferenced(*at) && (oldest==textures.end() ||
                    at->lastReferencedFrame<oldest->lastReferencedFrame))oldest=at;
            Require(oldest!=textures.end(),"Texture residency exceeds the actual frame budget");
            Destroy(*oldest);textures.erase(oldest);changed=true;
        }
        if(changed) {
            textureIndices.clear();textureIndices.reserve(textures.size());
            for(std::size_t index=0;index<textures.size();++index)
                textureIndices.emplace(VkNativeTextureKey{textures[index].source,textures[index].repeatedPadding},index);
        }
        return changed;
    }
    void Destroy(VkDrawTarget& target) noexcept
    {
        InvalidateRecordedCommands();
        if(target.feedbackPool)vkDestroyDescriptorPool(device,target.feedbackPool,nullptr);
        if(target.framebuffer)vkDestroyFramebuffer(device,target.framebuffer,nullptr);
        if(target.descriptorPool)vkDestroyDescriptorPool(device,target.descriptorPool,nullptr);
        if(target.sampler)vkDestroySampler(device,target.sampler,nullptr);
        if(target.magnifiedSampler)vkDestroySampler(device,target.magnifiedSampler,nullptr);
        if(target.reflectionSampleView)vkDestroyImageView(device,target.reflectionSampleView,nullptr);
        if(target.previousReflectionSampleView)vkDestroyImageView(device,target.previousReflectionSampleView,nullptr);
        Destroy(target.color);Destroy(target.depth);Destroy(target.previous);Destroy(target.feedback);target={};
    }
    void CreateSampledTarget(std::int32_t id,std::uint32_t width,std::uint32_t height);
    void InitializeSampledTargets();
    void CreateDepthSamplingPass();
    void CreateFogDepth(bool direct);
    void SetFogDepthReadable(bool readable);
    void CopyFogDepth();
    void CreateFocusTargets();
    void BlurFocus(const VkDrawBatch& batch,VkPipeline pipeline);
    void CopyHistory(std::int32_t id,VkImage current,bool readableLayout);
    void CreateFeedback(std::int32_t id,const VkDrawTexture* source);
    void CopyFeedback(std::int32_t id);
    void RecordDepthTransfers(const std::vector<EcstasyDepthTransfer>& requests,
        const std::array<double,2>& displayTranslation);
    void PublishDepthTransfers();
    EcstasyVulkanColorReadback ReadColorTarget(std::int32_t targetId);
    void CloseTargets() noexcept
    {
        // A resize retires the actual depth image. A request for that image
        // cannot later be applied to a differently sized replacement.
        InvalidateRecordedCommands();preparedFrame.valid=false;
        depthReadbacks.clear();Destroy(depthStaging);hasFrame=false;displaySceneHistoryValid=false;
        nativeSubmissionPending=false;
        for(auto& entry:renderedTargets)Destroy(entry.second);
        renderedTargets.clear();
        Destroy(fogDepth);fogSourceDescriptor=VK_NULL_HANDLE;
        Destroy(focusSource);Destroy(focusBlur);
        if (framebuffer)vkDestroyFramebuffer(device,framebuffer,nullptr);
        framebuffer=VK_NULL_HANDLE;
        if(postProcessFramebuffer)vkDestroyFramebuffer(device,postProcessFramebuffer,nullptr);
        postProcessFramebuffer=VK_NULL_HANDLE;
        if(depthSamplingFramebuffer)vkDestroyFramebuffer(device,depthSamplingFramebuffer,nullptr);
        depthSamplingFramebuffer=VK_NULL_HANDLE;
        Destroy(color);Destroy(depth);Destroy(multisampleColor);Destroy(multisampleDepth);
        for (auto semaphore:presentationSemaphores)vkDestroySemaphore(device,semaphore,nullptr);
        presentationSemaphores.clear();
        if (swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);
        swapchain=VK_NULL_HANDLE;swapImages.clear();
    }
    void Close() noexcept
    {
        StopPipelineWorker();
        if (device) {
            (void)vkDeviceWaitIdle(device);
            ClearFrameAllocations();
            for(auto& page:textureUploadPages)Destroy(page.buffer);
            textureUploadPages.clear();Destroy(vertices);Destroy(indices);Destroy(indirectCommands);
            Destroy(residentVertices);Destroy(residentStaging);
            Destroy(residentIndices);Destroy(residentIndexStaging);
            Destroy(vertexStaging);Destroy(indexStaging);Destroy(glyphBounds);
            for(auto& texture:textures)Destroy(texture);
            textures.clear();CloseTargets();
            for (const auto& pipeline:pipelines)vkDestroyPipeline(device,pipeline.second,nullptr);
            pipelines.clear();lastPipeline=VK_NULL_HANDLE;
            if(pipelineCache) {
                SavePipelineCache();
                vkDestroyPipelineCache(device,pipelineCache,nullptr);
            }
            if(timestampQueries)vkDestroyQueryPool(device,timestampQueries,nullptr);
            if(vertexShader)vkDestroyShaderModule(device,vertexShader,nullptr);
            if(fragmentShader)vkDestroyShaderModule(device,fragmentShader,nullptr);
            if(pipelineLayout)vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
            if(descriptorPool)vkDestroyDescriptorPool(device,descriptorPool,nullptr);
            if(descriptorLayout)vkDestroyDescriptorSetLayout(device,descriptorLayout,nullptr);
            if(frameDescriptorPool)vkDestroyDescriptorPool(device,frameDescriptorPool,nullptr);
            if(frameDescriptorLayout)vkDestroyDescriptorSetLayout(device,frameDescriptorLayout,nullptr);
            if(renderPass)vkDestroyRenderPass(device,renderPass,nullptr);
            if(resumeRenderPass)vkDestroyRenderPass(device,resumeRenderPass,nullptr);
            if(preserveRenderPass)vkDestroyRenderPass(device,preserveRenderPass,nullptr);
            if(postProcessRenderPass)vkDestroyRenderPass(device,postProcessRenderPass,nullptr);
            if(focusBlurRenderPass)vkDestroyRenderPass(device,focusBlurRenderPass,nullptr);
            if(multisampleRenderPass)vkDestroyRenderPass(device,multisampleRenderPass,nullptr);
            if(resumeMultisampleRenderPass)vkDestroyRenderPass(device,resumeMultisampleRenderPass,nullptr);
            if(depthSamplingRenderPass)vkDestroyRenderPass(device,depthSamplingRenderPass,nullptr);
            if(fence)vkDestroyFence(device,fence,nullptr);
            for(auto& lane:recordingLanes)if(lane.pool)vkDestroyCommandPool(device,lane.pool,nullptr);
            recordingLanes.clear();
            if(commandPool)vkDestroyCommandPool(device,commandPool,nullptr);
            if(acquired)vkDestroySemaphore(device,acquired,nullptr);
            vkDestroyDevice(device,nullptr);device=VK_NULL_HANDLE;
        }
        if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);
        surface=VK_NULL_HANDLE;
        if(instance)vkDestroyInstance(instance,nullptr);
        instance=VK_NULL_HANDLE;
    }
    void Initialize();
    void CreateTargets();
    VkPipeline Pipeline(const VkDrawBatch& batch,bool samplingDepth=false);
    VkPipeline CompilePipeline(const VkDrawBatch& batch,bool samplingDepth,bool multisampled,
        std::int32_t materialMinification);
    void RunPipelineWorker();
    void StopPipelineWorker() noexcept;
    void SavePipelineCache() noexcept;
    void UploadTexture(const TextureInfo& texture,std::size_t index,bool repeatedPadding);
    // Reused across frames. Input commands belong to the sealed source frame;
    // workers only read that frame and write disjoint completed vertex slots.
    std::vector<VkSolidStripJob> nativeStripJobs;
    VkNativeFrameShading nativeFrameShading;
    std::vector<VkNativeStripWorkRange> nativeWorkRanges;
    void Gather(const EcstasyFrame& frame,
        std::vector<VkVertex>& output,std::vector<std::uint32_t>& elementIndices,
        std::vector<VkDrawBatch>& batches);
    EcstasyNativeMirrorPresentation MirrorPresentation(const EcstasyFrame& frame) const;
    bool CanGatherBeforeCompletion(const EcstasyFrame& frame,
        const EcstasyNativeMirrorPresentation& mirror,const VkNativeFrameTextureReferences& frameTextures);
    std::vector<VkNativeTextureKey> gatheredTextureKeys;
    void RemapGatheredTextures(std::vector<VkDrawBatch>& batches) const;
    bool Render(EcstasyFrame* frame);
};

void EcstasyVulkanDevice::Storage::Initialize()
{
    Require(options.width && options.height && options.logicalWidth && options.logicalHeight,"Vulkan target dimensions must be positive");
    Require(options.anisotropy==2 || options.anisotropy==4 || options.anisotropy==8 || options.anisotropy==16,
        "Native anisotropy must be 2, 4, 8 or 16");
    switch(options.antialiasing) {
    case EcstasyAntialiasing::Off:case EcstasyAntialiasing::Fxaa:break;
    case EcstasyAntialiasing::Msaa2:samples=VK_SAMPLE_COUNT_2_BIT;break;
    case EcstasyAntialiasing::Msaa4:samples=VK_SAMPLE_COUNT_4_BIT;break;
    case EcstasyAntialiasing::Msaa8:samples=VK_SAMPLE_COUNT_8_BIT;break;
    default:throw std::runtime_error("Unknown native antialiasing mode");
    }
    std::uint32_t loaderVersion=VK_API_VERSION_1_0;
    const auto instanceVersion=reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE,"vkEnumerateInstanceVersion"));
    if(instanceVersion)vkRequired(instanceVersion(&loaderVersion),"vkEnumerateInstanceVersion");
    Require(loaderVersion>=VK_API_VERSION_1_1,"The native renderer requires Vulkan 1.1 or newer");
    const auto apiVersion=std::min(loaderVersion,VK_API_VERSION_1_2);
    std::vector<const char*> extensions;
    if (options.nativeWindow) {
#ifdef _WIN32
        Require(options.nativeInstance,"A Win32 Vulkan surface requires its real HINSTANCE");
        extensions={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
#elif defined(__SWITCH__)
        extensions={VK_KHR_SURFACE_EXTENSION_NAME,VK_NN_VI_SURFACE_EXTENSION_NAME};
#else
        throw std::runtime_error("This Vulkan build has no native window surface for this platform");
#endif
    }
    VkApplicationInfo application{};application.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName="NFSHP2 native PAL port";application.pEngineName="Ecstasy";
    application.apiVersion=apiVersion;
    VkInstanceCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo=&application;create.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size());
    create.ppEnabledExtensionNames=extensions.data();
    vkRequired(vkCreateInstance(&create,nullptr,&instance),"vkCreateInstance");
#ifdef _WIN32
    if(options.nativeWindow) {
        VkWin32SurfaceCreateInfoKHR createSurface{};createSurface.sType=VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        createSurface.hinstance=static_cast<HINSTANCE>(options.nativeInstance);
        createSurface.hwnd=static_cast<HWND>(options.nativeWindow);
        vkRequired(vkCreateWin32SurfaceKHR(instance,&createSurface,nullptr,&surface),"vkCreateWin32SurfaceKHR");
    }
#elif defined(__SWITCH__)
    if(options.nativeWindow) {
        const auto createViSurface=reinterpret_cast<PFN_vkCreateViSurfaceNN>(
            vkGetInstanceProcAddr(instance,"vkCreateViSurfaceNN"));
        Require(createViSurface,"Vulkan driver does not expose vkCreateViSurfaceNN");
        VkViSurfaceCreateInfoNN createSurface{};
        createSurface.sType=VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
        createSurface.window=options.nativeWindow;
        vkRequired(createViSurface(instance,&createSurface,nullptr,&surface),"vkCreateViSurfaceNN");
    }
#endif
    std::uint32_t count{};vkRequired(vkEnumeratePhysicalDevices(instance,&count,nullptr),"vkEnumeratePhysicalDevices");
    Require(count!=0,"No Vulkan physical device is available");
    std::vector<VkPhysicalDevice> devices(count);vkRequired(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"vkEnumeratePhysicalDevices");
    int selectedScore=-1;
    for (auto candidate:devices) {
        VkPhysicalDeviceProperties candidateProperties{};vkGetPhysicalDeviceProperties(candidate,&candidateProperties);
        if(candidateProperties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU && !options.allowSoftwareDevice)continue;
        std::uint32_t families{};vkGetPhysicalDeviceQueueFamilyProperties(candidate,&families,nullptr);
        std::vector<VkQueueFamilyProperties> familyProperties(families);vkGetPhysicalDeviceQueueFamilyProperties(candidate,&families,familyProperties.data());
        for (std::uint32_t family=0;family<families;++family) {
            if (!(familyProperties[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))continue;
            if (surface) {
                VkBool32 supported{};vkRequired(vkGetPhysicalDeviceSurfaceSupportKHR(candidate,family,surface,&supported),"vkGetPhysicalDeviceSurfaceSupportKHR");
                if (!supported)continue;
            }
            const int score=candidateProperties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                candidateProperties.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if(score>selectedScore) {physical=candidate;queueFamily=family;properties=candidateProperties;
                timestampValidBits=familyProperties[family].timestampValidBits;selectedScore=score;}
            break;
        }
    }
    Require(physical!=VK_NULL_HANDLE,options.allowSoftwareDevice ?
        "No Vulkan graphics queue supports the actual window surface":
        "No hardware Vulkan graphics device supports the requested surface; CPU software devices are disabled");
    vkGetPhysicalDeviceMemoryProperties(physical,&memoryProperties);
    VkDeviceSize geometryHeap=0;
    for(std::uint32_t heap=0;heap<memoryProperties.memoryHeapCount;++heap)
        if(memoryProperties.memoryHeaps[heap].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            geometryHeap=std::max(geometryHeap,memoryProperties.memoryHeaps[heap].size);
    const auto geometryBudget=std::min<VkDeviceSize>(64u*1024u*1024u,geometryHeap/64u);
    residentGeometry.Configure(static_cast<std::size_t>(geometryBudget/VkResidentGeometryCache::SlotBytes));
    sceneryPackets.Configure(residentGeometry.Capacity());
    // Index ranges are cached per already merged draw, not per-strip draws.
    // Their GPU budget and retained CPU payloads are independently bounded.
    residentIndexCache.Configure(static_cast<std::uint32_t>((geometryBudget/4)/sizeof(std::uint32_t)));
    // On UMA (or coherent device-local BAR memory), the GPU may read the
    // mapped vertex/index buffers directly. A discrete GPU with separate
    // host and local heaps instead receives a single GPU-side transfer per
    // frame, rather than fetching every indexed vertex over host memory.
    // No source vertex, index, draw order, shader or attachment is changed.
    const VkMemoryPropertyFlags coherentLocal=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(std::uint32_t index=0;index<memoryProperties.memoryTypeCount;++index)
        if((memoryProperties.memoryTypes[index].propertyFlags&coherentLocal)==coherentLocal)
            geometryMemory=GeometryMemory::LocalCoherent;
    if(geometryMemory==GeometryMemory::HostCoherent &&
        properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        geometryMemory=GeometryMemory::StagedDeviceLocal;
    char adapterDetails[384]{};
    const auto* memoryMode=geometryMemory==GeometryMemory::LocalCoherent ? "local-coherent":
        geometryMemory==GeometryMemory::StagedDeviceLocal ? "staged-device-local":"host-coherent";
    std::snprintf(adapterDetails,sizeof(adapterDetails),
        "adapter=%s vendor=%04x device=%04x api=%u.%u.%u geometry=%s",
        properties.deviceName,properties.vendorID,properties.deviceID,VK_VERSION_MAJOR(properties.apiVersion),
        VK_VERSION_MINOR(properties.apiVersion),VK_VERSION_PATCH(properties.apiVersion),memoryMode);
    platform::RecordSessionEvent("renderer.adapter",adapterDetails);
    // A sample count must work for both physical attachment formats, not
    // merely appear in the device's general color-limit mask.
    const auto imageProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(
        vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceImageFormatProperties"));
    Require(imageProperties,"Vulkan image-format capability query is unavailable");
    VkImageFormatProperties colorProperties{},depthProperties{};
    vkRequired(imageProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,0,&colorProperties),"vkGetPhysicalDeviceImageFormatProperties(color)");
    VkImageFormatProperties sampledColorProperties{};
    vkRequired(imageProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
        0,&sampledColorProperties),"vkGetPhysicalDeviceImageFormatProperties(sampled resolved color)");
    Require((sampledColorProperties.sampleCounts&VK_SAMPLE_COUNT_1_BIT)!=0,
        "Selected GPU cannot sample its resolved scene color image");
    vkRequired(imageProperties(physical,VK_FORMAT_D32_SFLOAT,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,0,&depthProperties),"vkGetPhysicalDeviceImageFormatProperties(depth)");
    VkImageFormatProperties sampledDepthProperties{};
    vkRequired(imageProperties(physical,VK_FORMAT_D32_SFLOAT,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
        0,&sampledDepthProperties),"vkGetPhysicalDeviceImageFormatProperties(sampled resolved depth)");
    Require((sampledDepthProperties.sampleCounts&VK_SAMPLE_COUNT_1_BIT)!=0,
        "Selected GPU cannot sample its resolved D32 depth image");
    const auto supportedSampleCounts=properties.limits.framebufferColorSampleCounts&
        properties.limits.framebufferDepthSampleCounts&colorProperties.sampleCounts&depthProperties.sampleCounts;
    Require((supportedSampleCounts&samples)!=0,
        "Selected GPU does not support the requested MSAA sample count for both color and depth; choose a lower MSAA mode");
    const bool coreDepthResolve=std::min(apiVersion,properties.apiVersion)>=VK_API_VERSION_1_2;
    std::vector<const char*> deviceExtensions;
    if(surface)deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if(samples!=VK_SAMPLE_COUNT_1_BIT) {
        if(!coreDepthResolve) {
            const auto enumerate=reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                vkGetInstanceProcAddr(instance,"vkEnumerateDeviceExtensionProperties"));
            Require(enumerate,"Vulkan device-extension query is unavailable");
            std::uint32_t extensionCount{};
            vkRequired(enumerate(physical,nullptr,&extensionCount,nullptr),"vkEnumerateDeviceExtensionProperties");
            std::vector<VkExtensionProperties> available(extensionCount);
            vkRequired(enumerate(physical,nullptr,&extensionCount,available.data()),"vkEnumerateDeviceExtensionProperties");
            for(const auto* extension:{VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME}) {
                Require(std::any_of(available.begin(),available.end(),[&](const VkExtensionProperties& value) {
                    return std::strcmp(value.extensionName,extension)==0;}),
                    "Selected GPU cannot resolve multisample scene depth; MSAA requires Vulkan 1.2 or the depth-stencil-resolve extensions");
                deviceExtensions.push_back(extension);
            }
        }
        VkPhysicalDeviceDepthStencilResolveProperties resolveProperties{};
        resolveProperties.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES;
        VkPhysicalDeviceProperties2 allProperties{};allProperties.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        allProperties.pNext=&resolveProperties;
        const auto getProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceProperties2"));
        Require(getProperties,"Vulkan depth-resolve capability query is unavailable");
        getProperties(physical,&allProperties);
        Require((resolveProperties.supportedDepthResolveModes&VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)!=0,
            "Selected GPU does not provide the required sample-zero depth resolve");
    }
    const float priority=1.0f;VkDeviceQueueCreateInfo queueCreate{};queueCreate.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreate.queueFamilyIndex=queueFamily;queueCreate.queueCount=1;queueCreate.pQueuePriorities=&priority;
    VkDeviceCreateInfo deviceCreate{};deviceCreate.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreate.queueCreateInfoCount=1;deviceCreate.pQueueCreateInfos=&queueCreate;
    VkPhysicalDeviceFeatures supported{},enabled{};vkGetPhysicalDeviceFeatures(physical,&supported);
    if(options.textureFilter==EcstasyTextureFilter::Anisotropic && supported.samplerAnisotropy) {
        enabled.samplerAnisotropy=VK_TRUE;
        actualAnisotropy=std::min(static_cast<float>(options.anisotropy),properties.limits.maxSamplerAnisotropy);
    }
    nativeMultiDraw=options.streamTransformBatching && supported.multiDrawIndirect &&
        supported.drawIndirectFirstInstance && properties.limits.maxDrawIndirectCount>1;
    if(nativeMultiDraw) {
        enabled.multiDrawIndirect=VK_TRUE;enabled.drawIndirectFirstInstance=VK_TRUE;
    }
    deviceCreate.pEnabledFeatures=&enabled;
    deviceCreate.enabledExtensionCount=static_cast<std::uint32_t>(deviceExtensions.size());
    deviceCreate.ppEnabledExtensionNames=deviceExtensions.data();
    vkRequired(vkCreateDevice(physical,&deviceCreate,nullptr,&device),"vkCreateDevice");vkGetDeviceQueue(device,queueFamily,0,&queue);
    std::vector<char> cacheData;
    if(!options.pipelineCachePath.empty()) {
        std::ifstream cache(options.pipelineCachePath,std::ios::binary|std::ios::ate);
        const auto bytes=cache.tellg();
        if(bytes>=32 && bytes<=16*1024*1024) {
            cacheData.resize(static_cast<std::size_t>(bytes));cache.seekg(0);
            if(!cache.read(cacheData.data(),static_cast<std::streamsize>(cacheData.size())))cacheData.clear();
        }
    }
    NativePipelineCacheIdentity identity{properties.vendorID,properties.deviceID};
    std::copy_n(properties.pipelineCacheUUID,identity.uuid.size(),identity.uuid.begin());
    const auto createCache=[&](const void* data,std::size_t size) {
        VkPipelineCacheCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        create.initialDataSize=size;create.pInitialData=data;
        VkPipelineCache result{};
        const auto status=vkCreatePipelineCache(device,&create,nullptr,&result);
        if(!size)vkRequired(status,"vkCreatePipelineCache(empty)");
        return status==VK_SUCCESS ? result:VK_NULL_HANDLE;
    };
    pipelineCache=CreateNativePipelineCache<VkPipelineCache>(cacheData.data(),cacheData.size(),
        options.pipelineCacheSeedData,options.pipelineCacheSeedSize,identity,createCache,
        [&](VkPipelineCache destination,VkPipelineCache seed){(void)vkMergePipelineCaches(device,destination,1,&seed);},
        [&](VkPipelineCache seed){vkDestroyPipelineCache(device,seed,nullptr);});
    if(timestampValidBits && timestampValidBits<=64 && properties.limits.timestampPeriod>0.0f) {
        VkQueryPoolCreateInfo queries{};queries.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queries.queryType=VK_QUERY_TYPE_TIMESTAMP;queries.queryCount=TimestampQueryCapacity;
        vkRequired(vkCreateQueryPool(device,&queries,nullptr,&timestampQueries),"vkCreateQueryPool(timestamps)");
    }

    VkCommandPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pool.queueFamilyIndex=queueFamily;
    vkRequired(vkCreateCommandPool(device,&pool,nullptr,&commandPool),"vkCreateCommandPool");
    VkCommandBufferAllocateInfo commandAllocation{};commandAllocation.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandAllocation.commandPool=commandPool;commandAllocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;commandAllocation.commandBufferCount=1;
    vkRequired(vkAllocateCommandBuffers(device,&commandAllocation,&commands),"vkAllocateCommandBuffers");
    VkFenceCreateInfo fenceCreate{};fenceCreate.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;fenceCreate.flags=VK_FENCE_CREATE_SIGNALED_BIT;
    vkRequired(vkCreateFence(device,&fenceCreate,nullptr,&fence),"vkCreateFence");
    VkSemaphoreCreateInfo semaphore{};semaphore.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (surface) {
        vkRequired(vkCreateSemaphore(device,&semaphore,nullptr,&acquired),"vkCreateSemaphore(acquire)");
    }
    VkAttachmentDescription attachments[2]{};
    attachments[0].format=VK_FORMAT_R8G8B8A8_UNORM;attachments[0].samples=VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachments[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachments[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachments[0].finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format=VK_FORMAT_D32_SFLOAT;attachments[1].samples=VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachments[1].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachments[1].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachments[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorReference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depthReference{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=1;subpass.pColorAttachments=&colorReference;subpass.pDepthStencilAttachment=&depthReference;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
    // Framebuffers/pipelines are shared by clear, resume and preserved
    // passes. Compatibility includes dependencies, so all three use the
    // same complete union; only their allowed load/store/layouts differ.
    dependencies[0].srcStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT|
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dependencies[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;pass.attachmentCount=2;pass.pAttachments=attachments;
    pass.subpassCount=1;pass.pSubpasses=&subpass;pass.dependencyCount=2;pass.pDependencies=dependencies;
    vkRequired(vkCreateRenderPass(device,&pass,nullptr,&renderPass),"vkCreateRenderPass");
    // Resume the original attachments after reading scene Z for effects.
    // The multisampled attachments likewise survive this explicit boundary.
    attachments[0].loadOp=attachments[1].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].initialLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    vkRequired(vkCreateRenderPass(device,&pass,nullptr,&resumeRenderPass),"vkCreateRenderPass(resume source effects)");
    attachments[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    // PAL target14 is updated partially, without a frame clear. This
    // compatible render pass retains every unmodified texel between updates.
    attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].initialLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkRequired(vkCreateRenderPass(device,&pass,nullptr,&preserveRenderPass),"vkCreateRenderPass(preserve target)");
    // A color-only final pass leaves the scene depth attachment intact for
    // the source's delayed Sun visibility readback on the next frame.
    attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].initialLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    subpass.pDepthStencilAttachment=nullptr;pass.attachmentCount=1;
    // A later focus invocation can clear an image sampled by its preceding
    // composite. Preserve that read-before-write dependency in BOTH compatible
    // color-only passes; the antialiasing source also retains transfer reads.
    dependencies[0].srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkRequired(vkCreateRenderPass(device,&pass,nullptr,&postProcessRenderPass),"vkCreateRenderPass(PC antialiasing)");
    // The half-size tap image is color-only. Keep the same subpass and
    // dependencies as the existing one-sample postprocess pipeline, with
    // an actual black/alpha-one CLEAR instead of undefined initial pixels.
    attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkRenderPass focusClear{};
    try {
        vkRequired(vkCreateRenderPass(device,&pass,nullptr,&focusClear),"vkCreateRenderPass(focus color clear)");
    }catch(...) {if(focusClear)vkDestroyRenderPass(device,focusClear,nullptr);throw;}
    focusBlurRenderPass=focusClear;
    if(samples!=VK_SAMPLE_COUNT_1_BIT) {
        // Resolve color and source Z independently. Multisampled scene Z
        // must never be copied as if it were a one-sample D32 image: the
        // original delayed Sun tile reads the real sample-zero resolve.
        const auto createRenderPass2=reinterpret_cast<PFN_vkCreateRenderPass2>(
            vkGetInstanceProcAddr(instance,coreDepthResolve ? "vkCreateRenderPass2":"vkCreateRenderPass2KHR"));
        Require(createRenderPass2,"Vulkan multisample render-pass entry point is unavailable");
        VkAttachmentDescription2 multisampled[4]{};
        for(auto& attachment:multisampled) {
            attachment.sType=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        }
        multisampled[0].format=multisampled[2].format=VK_FORMAT_R8G8B8A8_UNORM;
        multisampled[1].format=multisampled[3].format=VK_FORMAT_D32_SFLOAT;
        multisampled[0].samples=multisampled[1].samples=samples;
        multisampled[2].samples=multisampled[3].samples=VK_SAMPLE_COUNT_1_BIT;
        multisampled[0].loadOp=multisampled[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
        multisampled[2].loadOp=multisampled[3].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        multisampled[0].storeOp=multisampled[1].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        multisampled[2].storeOp=multisampled[3].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        multisampled[0].finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        multisampled[2].finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        multisampled[1].finalLayout=multisampled[3].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference2 references[4]{};
        for(std::uint32_t i=0;i<4;++i) {
            references[i].sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;references[i].attachment=i;
            references[i].layout=i&1u ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            references[i].aspectMask=i&1u ? VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
        }
        VkSubpassDescriptionDepthStencilResolve resolve{};
        resolve.sType=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
        resolve.depthResolveMode=VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;resolve.stencilResolveMode=VK_RESOLVE_MODE_NONE;
        resolve.pDepthStencilResolveAttachment=references+3;
        VkSubpassDescription2 multisampleSubpass{};multisampleSubpass.sType=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        multisampleSubpass.pNext=&resolve;multisampleSubpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        multisampleSubpass.colorAttachmentCount=1;multisampleSubpass.pColorAttachments=references;
        multisampleSubpass.pDepthStencilAttachment=references+1;multisampleSubpass.pResolveAttachments=references+2;
        VkSubpassDependency2 multisampleDependencies[2]{};
        for(auto& dependency:multisampleDependencies)dependency.sType=VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        multisampleDependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;multisampleDependencies[0].dstSubpass=0;
        multisampleDependencies[0].srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        multisampleDependencies[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT|
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        multisampleDependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        multisampleDependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        multisampleDependencies[1].srcSubpass=0;multisampleDependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
        multisampleDependencies[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        multisampleDependencies[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        multisampleDependencies[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
        multisampleDependencies[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo2 multisamplePass{};multisamplePass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
        multisamplePass.attachmentCount=4;multisamplePass.pAttachments=multisampled;
        multisamplePass.subpassCount=1;multisamplePass.pSubpasses=&multisampleSubpass;
        multisamplePass.dependencyCount=2;multisamplePass.pDependencies=multisampleDependencies;
        vkRequired(createRenderPass2(device,&multisamplePass,nullptr,&multisampleRenderPass),"vkCreateRenderPass2(MSAA color and depth resolve)");
        multisampled[0].loadOp=multisampled[1].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
        for(auto& attachment:multisampled)attachment.initialLayout=attachment.finalLayout;
        vkRequired(createRenderPass2(device,&multisamplePass,nullptr,&resumeMultisampleRenderPass),"vkCreateRenderPass2(resume MSAA source effects)");
    }
    CreateDepthSamplingPass();
    VkDescriptorSetLayoutBinding bindings[3]{};
    for(std::uint32_t i=0;i<3;++i) {
        bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout{};layout.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount=3;layout.pBindings=bindings;
    vkRequired(vkCreateDescriptorSetLayout(device,&layout,nullptr,&descriptorLayout),"vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,12288};
    VkDescriptorPoolCreateInfo descriptorCreate{};descriptorCreate.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorCreate.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descriptorCreate.maxSets=4096;descriptorCreate.poolSizeCount=1;descriptorCreate.pPoolSizes=&poolSize;
    vkRequired(vkCreateDescriptorPool(device,&descriptorCreate,nullptr,&descriptorPool),"vkCreateDescriptorPool");
    // A separate small frame table avoids a new draw/push-constant boundary
    // for every letter and adds only one32-bit index to a native vertex.
    const VkDescriptorSetLayoutBinding frameBinding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,
        VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_VERTEX_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo frameLayout{};frameLayout.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    frameLayout.bindingCount=1;frameLayout.pBindings=&frameBinding;
    vkRequired(vkCreateDescriptorSetLayout(device,&frameLayout,nullptr,&frameDescriptorLayout),"vkCreateDescriptorSetLayout(glyph bounds)");
    const VkDescriptorPoolSize framePoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo framePool{};framePool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    framePool.maxSets=1;framePool.poolSizeCount=1;framePool.pPoolSizes=&framePoolSize;
    vkRequired(vkCreateDescriptorPool(device,&framePool,nullptr,&frameDescriptorPool),"vkCreateDescriptorPool(glyph bounds)");
    VkDescriptorSetAllocateInfo frameSet{};frameSet.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    frameSet.descriptorPool=frameDescriptorPool;frameSet.descriptorSetCount=1;frameSet.pSetLayouts=&frameDescriptorLayout;
    vkRequired(vkAllocateDescriptorSets(device,&frameSet,&frameDescriptor),"vkAllocateDescriptorSets(glyph bounds)");
    VkPushConstantRange push{};
    push.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_VERTEX_BIT;
    push.size=static_cast<std::uint32_t>(sizeof(VkNativeDrawPush));
    Require(properties.limits.maxPushConstantsSize>=push.size,
        "Native GPU transform exceeds physical device push constant range");
    VkPipelineLayoutCreateInfo pipelineCreate{};pipelineCreate.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const VkDescriptorSetLayout layouts[]={descriptorLayout,frameDescriptorLayout};
    pipelineCreate.setLayoutCount=2;pipelineCreate.pSetLayouts=layouts;pipelineCreate.pushConstantRangeCount=1;pipelineCreate.pPushConstantRanges=&push;
    vkRequired(vkCreatePipelineLayout(device,&pipelineCreate,nullptr,&pipelineLayout),"vkCreatePipelineLayout");
    auto shader=[&](const std::uint32_t* words,std::size_t bytes,VkShaderModule& result) {
        VkShaderModuleCreateInfo module{};module.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;module.codeSize=bytes;module.pCode=words;
        vkRequired(vkCreateShaderModule(device,&module,nullptr,&result),"vkCreateShaderModule");
    };
    shader(vulkanVertexSPIRV,sizeof(vulkanVertexSPIRV),vertexShader);
    shader(vulkanFragmentSPIRV,sizeof(vulkanFragmentSPIRV),fragmentShader);
    CreateTargets();
}

void EcstasyVulkanDevice::Storage::CreateTargets()
{
    if (surface) {
        VkSurfaceCapabilitiesKHR capabilities{};vkRequired(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&capabilities),"vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        Require((capabilities.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT)!=0,"Window surface cannot receive a Vulkan transfer");
        std::uint32_t count{};vkRequired(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,nullptr),"vkGetPhysicalDeviceSurfaceFormatsKHR");
        std::vector<VkSurfaceFormatKHR> formats(count);vkRequired(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats.data()),"vkGetPhysicalDeviceSurfaceFormatsKHR");
        Require(!formats.empty(),"Window surface offers no Vulkan formats");auto selected=formats.front();
        for (const auto& format:formats)if (format.format==VK_FORMAT_B8G8R8A8_UNORM || format.format==VK_FORMAT_R8G8B8A8_UNORM) {selected=format;break;}
        if (selected.format==VK_FORMAT_UNDEFINED)selected.format=VK_FORMAT_B8G8R8A8_UNORM;
        VkExtent2D extent{options.width,options.height};
        if(capabilities.currentExtent.width!=std::numeric_limits<std::uint32_t>::max())extent=capabilities.currentExtent;
        else {
            extent.width=std::clamp(extent.width,capabilities.minImageExtent.width,capabilities.maxImageExtent.width);
            extent.height=std::clamp(extent.height,capabilities.minImageExtent.height,capabilities.maxImageExtent.height);
        }
        Require(extent.width && extent.height,"Cannot recreate a Vulkan swapchain while the window is minimized");
        presentationExtent=extent;
        if(!options.fixedRenderExtent){options.width=extent.width;options.height=extent.height;}
        std::uint32_t images=capabilities.minImageCount+1;
        if(capabilities.maxImageCount)images=std::min(images,capabilities.maxImageCount);
        VkSwapchainCreateInfoKHR create{};create.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create.surface=surface;create.minImageCount=images;create.imageFormat=selected.format;create.imageColorSpace=selected.colorSpace;
        create.imageExtent=extent;create.imageArrayLayers=1;create.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;create.preTransform=capabilities.currentTransform;
        const VkCompositeAlphaFlagBitsKHR alphas[]={VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
        for(auto alpha:alphas)if(capabilities.supportedCompositeAlpha&alpha) {create.compositeAlpha=alpha;break;}
        create.presentMode=VK_PRESENT_MODE_FIFO_KHR;create.clipped=VK_TRUE;
        if(!options.vsync) {
            std::uint32_t modesCount{};
            vkRequired(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&modesCount,nullptr),"vkGetPhysicalDeviceSurfacePresentModesKHR");
            std::vector<VkPresentModeKHR> modes(modesCount);
            vkRequired(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&modesCount,modes.data()),"vkGetPhysicalDeviceSurfacePresentModesKHR");
            if(std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_IMMEDIATE_KHR)!=modes.end())create.presentMode=VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if(std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_MAILBOX_KHR)!=modes.end())create.presentMode=VK_PRESENT_MODE_MAILBOX_KHR;
            else throw std::runtime_error("This surface supports only synchronized FIFO presentation");
        }
        vkRequired(vkCreateSwapchainKHR(device,&create,nullptr,&swapchain),"vkCreateSwapchainKHR");swapFormat=selected.format;
#ifdef __SWITCH2__
        // The driver's FIFO setup can set a 60 Hz swap interval. Restore the
        // application's VI2 rate after every swapchain creation, including resume.
        nx::UpdateDisplayRefresh(true);
#endif
        vkRequired(vkGetSwapchainImagesKHR(device,swapchain,&count,nullptr),"vkGetSwapchainImagesKHR");swapImages.resize(count);
        vkRequired(vkGetSwapchainImagesKHR(device,swapchain,&count,swapImages.data()),"vkGetSwapchainImagesKHR");
        presentationSemaphores.resize(count);
        VkSemaphoreCreateInfo semaphore{};semaphore.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for(auto& handle:presentationSemaphores)vkRequired(vkCreateSemaphore(device,&semaphore,nullptr,&handle),"vkCreateSemaphore(presentation image)");
    }
    CreateImage(color,options.width,options.height,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    CreateImage(depth,options.width,options.height,VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
    const VkImageView resolvedViews[]={color.view,depth.view};
    std::array<VkImageView,4> multisampleViews{};
    if(samples!=VK_SAMPLE_COUNT_1_BIT) {
        CreateImage(multisampleColor,options.width,options.height,VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,VK_IMAGE_ASPECT_COLOR_BIT,1,samples);
        CreateImage(multisampleDepth,options.width,options.height,VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,1,samples);
        multisampleViews={{multisampleColor.view,multisampleDepth.view,color.view,depth.view}};
    }
    VkFramebufferCreateInfo framebufferCreate{};framebufferCreate.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferCreate.renderPass=multisampleRenderPass ? multisampleRenderPass:renderPass;
    framebufferCreate.attachmentCount=multisampleRenderPass ? 4u:2u;
    framebufferCreate.pAttachments=multisampleRenderPass ? multisampleViews.data():resolvedViews;
    framebufferCreate.width=options.width;framebufferCreate.height=options.height;framebufferCreate.layers=1;
    vkRequired(vkCreateFramebuffer(device,&framebufferCreate,nullptr,&framebuffer),"vkCreateFramebuffer");
    framebufferCreate.renderPass=postProcessRenderPass;framebufferCreate.attachmentCount=1;
    framebufferCreate.pAttachments=resolvedViews;
    vkRequired(vkCreateFramebuffer(device,&framebufferCreate,nullptr,&postProcessFramebuffer),"vkCreateFramebuffer(PC antialiasing)");
    // Color is retained, including its multisample coverage. Resolved Z is
    // sampled through a descriptor and is NOT an attachment of this pass.
    const VkImageView samplingViews[]={samples==VK_SAMPLE_COUNT_1_BIT ? color.view:multisampleColor.view,color.view};
    framebufferCreate.renderPass=depthSamplingRenderPass;
    framebufferCreate.attachmentCount=samples==VK_SAMPLE_COUNT_1_BIT ? 1u:2u;
    framebufferCreate.pAttachments=samplingViews;
    VkFramebuffer replacement{};
    try {
        vkRequired(vkCreateFramebuffer(device,&framebufferCreate,nullptr,&replacement),"vkCreateFramebuffer(depth sampling colors)");
    }catch(...) {if(replacement)vkDestroyFramebuffer(device,replacement,nullptr);throw;}
    depthSamplingFramebuffer=replacement;
    hasFrame=false;displaySceneHistoryValid=false;
}

void EcstasyVulkanDevice::Storage::CreateDepthSamplingPass()
{
    VkAttachmentDescription attachments[2]{};
    auto& source=attachments[0];source.format=VK_FORMAT_R8G8B8A8_UNORM;source.samples=samples;
    source.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;source.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    source.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;source.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    source.initialLayout=source.finalLayout=samples==VK_SAMPLE_COUNT_1_BIT ?
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    auto& resolved=attachments[1];resolved=source;resolved.samples=VK_SAMPLE_COUNT_1_BIT;
    resolved.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolved.initialLayout=resolved.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkAttachmentReference colorReference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference resolveReference{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=1;subpass.pColorAttachments=&colorReference;
    if(samples!=VK_SAMPLE_COUNT_1_BIT)subpass.pResolveAttachments=&resolveReference;
    VkSubpassDependency dependencies[2]{};
    auto& before=dependencies[0];before.srcSubpass=VK_SUBPASS_EXTERNAL;before.dstSubpass=0;
    before.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|
        VK_PIPELINE_STAGE_TRANSFER_BIT;
    before.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
    before.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    before.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
    auto& after=dependencies[1];after.srcSubpass=0;after.dstSubpass=VK_SUBPASS_EXTERNAL;
    after.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    after.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
    after.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    after.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create.attachmentCount=samples==VK_SAMPLE_COUNT_1_BIT ? 1u:2u;create.pAttachments=attachments;
    create.subpassCount=1;create.pSubpasses=&subpass;create.dependencyCount=2;create.pDependencies=dependencies;
    VkRenderPass replacement{};
    try {
        vkRequired(vkCreateRenderPass(device,&create,nullptr,&replacement),"vkCreateRenderPass(depth sampling colors)");
    }catch(...) {if(replacement)vkDestroyRenderPass(device,replacement,nullptr);throw;}
    depthSamplingRenderPass=replacement;
}

void EcstasyVulkanDevice::Storage::CreateSampledTarget(std::int32_t id,std::uint32_t width,std::uint32_t height)
{
    Require((id==0 || (id>=7 && id<=14)) && width && height,"Invalid original sampled render target");
    auto found=renderedTargets.find(id);
    if(found!=renderedTargets.end()) {
        Require(found->second.width==width && found->second.height==height,
            "An original sampled target changed extent without device recreation");
        return;
    }
    auto& target=renderedTargets[id];target.width=width;target.height=height;
    try {
        if(id) {
            CreateImage(target.color,width,height,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
            CreateImage(target.depth,width,height,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
            const VkImageView views[]={target.color.view,target.depth.view};
            VkFramebufferCreateInfo framebufferCreate{};framebufferCreate.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferCreate.renderPass=renderPass;framebufferCreate.attachmentCount=2;framebufferCreate.pAttachments=views;
            framebufferCreate.width=width;framebufferCreate.height=height;framebufferCreate.layers=1;
            vkRequired(vkCreateFramebuffer(device,&framebufferCreate,nullptr,&target.framebuffer),"vkCreateFramebuffer(original target)");
        }
        // Only source14 and the preceding display have historical readers.
        // No stale image is copied into an authored TextureInfo resource.
        if(id==0 || id==14)CreateImage(target.previous,width,height,VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
        if(id==14) {
            // PAL106E9C/1086E8: mapped environment samples have opaque
            // framebuffer alpha. Sampling views preserve the original RGB.
            VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=VK_FORMAT_R8G8B8A8_UNORM;
            view.components.a=VK_COMPONENT_SWIZZLE_ONE;
            view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            view.image=target.color.image;
            vkRequired(vkCreateImageView(device,&view,nullptr,&target.reflectionSampleView),"vkCreateImageView(environment sample)");
            view.image=target.previous.image;
            vkRequired(vkCreateImageView(device,&view,nullptr,&target.previousReflectionSampleView),"vkCreateImageView(environment history sample)");
        }
        VkSamplerCreateInfo sampler{};sampler.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        // UpdateGifTags105F4C/54/68/74: generated target mag1,min2,TFX0.
        sampler.magFilter=sampler.minFilter=options.textureFilter==EcstasyTextureFilter::Authored ? VK_FILTER_NEAREST:VK_FILTER_LINEAR;
        sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod=0;
        vkRequired(vkCreateSampler(device,&sampler,nullptr,&target.sampler),"vkCreateSampler(original target)");
        sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
        vkRequired(vkCreateSampler(device,&sampler,nullptr,&target.magnifiedSampler),"vkCreateSampler(original target mag)");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,6};
        VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets=2;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        vkRequired(vkCreateDescriptorPool(device,&pool,nullptr,&target.descriptorPool),"vkCreateDescriptorPool(original target)");
        const auto descriptor=[&](VkImageView image,VkDescriptorSet& output) {
            if(!image)return;
            VkDescriptorSetAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocation.descriptorPool=target.descriptorPool;allocation.descriptorSetCount=1;allocation.pSetLayouts=&descriptorLayout;
            vkRequired(vkAllocateDescriptorSets(device,&allocation,&output),"vkAllocateDescriptorSets(original target)");
            const VkDescriptorImageInfo info[]={{target.sampler,image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {target.magnifiedSampler,image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {target.magnifiedSampler,image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkWriteDescriptorSet writes[3]{};
            for(std::uint32_t i=0;i<3;++i) {
                auto& write=writes[i];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=output;
                write.dstBinding=i;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo=info+i;
            }
            UpdateDescriptors(3,writes);
        };
        descriptor(target.reflectionSampleView ? target.reflectionSampleView:target.color.view,target.descriptor);
        descriptor(target.previousReflectionSampleView ? target.previousReflectionSampleView:target.previous.view,target.previousDescriptor);
    }catch(...) {Destroy(target);renderedTargets.erase(id);throw;}
}

void EcstasyVulkanDevice::Storage::InitializeSampledTargets()
{
    // Native device allocations have no prior frame. Give their initial
    // untouched pixels defined black contents; subsequent frames retain the
    // actual rendered images and never substitute a CPU texture or cubemap.
    const VkClearColorValue black{};const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    for(auto& entry:renderedTargets)if(!entry.second.initialized) {
        for(auto* image:{&entry.second.color,&entry.second.previous})if(image->image) {
            Barrier(image->image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdClearColorImage(commands,image->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
            Barrier(image->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
    }
}

void EcstasyVulkanDevice::Storage::CreateFogDepth(bool direct)
{
    if(direct ? fogSourceDescriptor!=VK_NULL_HANDLE:fogDepth.descriptor!=VK_NULL_HANDLE)return;
    try {
        fogDepth.width=options.width;fogDepth.height=options.height;
        if(!fogDepth.sampler) {
            VkSamplerCreateInfo sampler{};sampler.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
            sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            vkRequired(vkCreateSampler(device,&sampler,nullptr,&fogDepth.sampler),"vkCreateSampler(fog depth)");
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,6};
            VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool.maxSets=2;pool.poolSizeCount=1;pool.pPoolSizes=&size;
            vkRequired(vkCreateDescriptorPool(device,&pool,nullptr,&fogDepth.descriptorPool),"vkCreateDescriptorPool(fog depth)");
        }
        if(!direct && !fogDepth.color.image)CreateImage(fogDepth.color,options.width,options.height,VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
        Require(!direct || depth.view,"Fog source depth has no retained view");
        VkDescriptorSet replacement{};
        VkDescriptorSetAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool=fogDepth.descriptorPool;allocation.descriptorSetCount=1;allocation.pSetLayouts=&descriptorLayout;
        vkRequired(vkAllocateDescriptorSets(device,&allocation,&replacement),"vkAllocateDescriptorSets(fog depth)");
        const VkDescriptorImageInfo image{fogDepth.sampler,direct ? depth.view:fogDepth.color.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[3]{};
        for(std::uint32_t index=0;index<3;++index) {
            auto& write=writes[index];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet=replacement;write.dstBinding=index;write.descriptorCount=1;
            write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
        }
        UpdateDescriptors(3,writes);
        if(direct)fogSourceDescriptor=replacement;
        else fogDepth.descriptor=replacement;
    }catch(...) {Destroy(fogDepth);fogSourceDescriptor=VK_NULL_HANDLE;throw;}
}

void EcstasyVulkanDevice::Storage::SetFogDepthReadable(bool readable)
{
    // Depth/stencil RESOLVE writes run at color output, unlike ordinary
    // depth tests/stores. Include both writers in the layout transition.
    if(readable)Barrier(depth.image,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    else Barrier(depth.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
}

void EcstasyVulkanDevice::Storage::CopyFogDepth()
{
    // Copy the actual source stage's completed depth, including MSAA's
    // sample-zero resolve. Later fog/HUD draws cannot feed back into it.
    Barrier(depth.image,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    Barrier(fogDepth.color.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};copy.dstSubresource=copy.srcSubresource;
    copy.extent={options.width,options.height,1};
    vkCmdCopyImage(commands,depth.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        fogDepth.color.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    Barrier(depth.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    Barrier(fogDepth.color.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);

}

void EcstasyVulkanDevice::Storage::CreateFocusTargets()
{
    if(focusSource.descriptor)return;
    try {
        Require(color.view,"Focus source color has no retained view");
        for(auto* target:{&focusSource,&focusBlur}) {
            const bool half=target==&focusBlur;
            target->width=half ? std::max(1u,options.width/2):options.width;
            target->height=half ? std::max(1u,options.height/2):options.height;
            if(half) {
                CreateImage(target->color,target->width,target->height,VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
                const VkImageView views[]={target->color.view};
                VkFramebufferCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                create.renderPass=focusBlurRenderPass;create.attachmentCount=1;create.pAttachments=views;
                create.width=target->width;create.height=target->height;create.layers=1;
                vkRequired(vkCreateFramebuffer(device,&create,nullptr,&target->framebuffer),"vkCreateFramebuffer(focus half image)");
            }
            VkSamplerCreateInfo sampler{};sampler.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sampler.minFilter=sampler.magFilter=VK_FILTER_LINEAR;sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            vkRequired(vkCreateSampler(device,&sampler,nullptr,&target->sampler),"vkCreateSampler(focus image)");
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3};
            VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool.maxSets=1;pool.poolSizeCount=1;pool.pPoolSizes=&size;
            vkRequired(vkCreateDescriptorPool(device,&pool,nullptr,&target->descriptorPool),"vkCreateDescriptorPool(focus image)");
            VkDescriptorSetAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocation.descriptorPool=target->descriptorPool;allocation.descriptorSetCount=1;allocation.pSetLayouts=&descriptorLayout;
            vkRequired(vkAllocateDescriptorSets(device,&allocation,&target->descriptor),"vkAllocateDescriptorSets(focus image)");
            const VkDescriptorImageInfo image{target->sampler,half ? target->color.view:color.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet writes[3]{};
            for(std::uint32_t i=0;i<3;++i) {
                auto& write=writes[i];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet=target->descriptor;write.dstBinding=i;write.descriptorCount=1;
                write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
            }
            UpdateDescriptors(3,writes);
        }
    }catch(...) {Destroy(focusBlur);Destroy(focusSource);throw;}
}

void EcstasyVulkanDevice::Storage::BlurFocus(const VkDrawBatch& batch,VkPipeline pipeline)
{
    // The caller has ended the source pass, so color is its resolved,
    // transfer-readable image. The tap pass writes ONLY focusBlur, so its
    // unchanged source pixels can be sampled directly without a full-size
    // duplicate. The descriptor borrows color.view; no image owner aliases it.
    Barrier(color.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    VkClearValue clear{};clear.color.float32[3]=1;
    VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass=focusBlurRenderPass;pass.framebuffer=focusBlur.framebuffer;
    pass.renderArea={{0,0},{focusBlur.width,focusBlur.height}};pass.clearValueCount=1;pass.pClearValues=&clear;
    vkCmdBeginRenderPass(commands,&pass,VK_SUBPASS_CONTENTS_INLINE);
    VkDrawBatch tap;tap.target=-1;tap.blend=0x01020200u;tap.depth=tap.depthWrite=false;tap.textureFunction=7;
    tap.postProcess=true;
    tap.viewport={0,0,static_cast<float>(focusBlur.width),static_cast<float>(focusBlur.height),0,1};
    tap.scissor={{0,0},{focusBlur.width,focusBlur.height}};
    vkCmdBindPipeline(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    const VkDeviceSize vertexOffset=0;
    vkCmdBindVertexBuffers(commands,0,1,&vertices.buffer,&vertexOffset);
    vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,1,1,&frameDescriptor,0,nullptr);
    vkCmdSetViewport(commands,0,1,&tap.viewport);
    vkCmdSetScissor(commands,0,1,&tap.scissor);
    const float blend[]={batch.focusBlend,batch.focusBlend,batch.focusBlend,batch.focusBlend};
    vkCmdSetBlendConstants(commands,blend);
    vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&focusSource.descriptor,0,nullptr);

    VkNativeDrawPush push{};
    push.function=7;push.test=0;push.scale=1.0f;push.minification=-1;
    vkCmdPushConstants(commands,pipelineLayout,VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_VERTEX_BIT,
        0,sizeof(push),&push);
    // TRIANGLE_LIST preserves the same ordered three-vertex primitives.
    // Each triangle still blends/stores separately, retaining intermediate
    // RGBA8 quantization; no tap colors or filtering operations are combined.
    if(batch.focusPassCount>0)
        vkCmdDraw(commands,static_cast<std::uint32_t>(batch.focusPassCount)*3u,1,batch.focusFirst,0);
    vkCmdEndRenderPass(commands);
    Barrier(focusBlur.color.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    Barrier(color.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void EcstasyVulkanDevice::Storage::CopyHistory(std::int32_t id,VkImage current,bool readableLayout)
{
    auto& target=renderedTargets.at(id);
    Require(target.previous.image,"History reader has no retained GPU image");
    if(readableLayout)Barrier(current,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    Barrier(target.previous.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.dstSubresource=copy.srcSubresource;
    copy.extent={target.width,target.height,1};
    vkCmdCopyImage(commands,current,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target.previous.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    Barrier(target.previous.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if(readableLayout)Barrier(current,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void EcstasyVulkanDevice::Storage::CreateFeedback(std::int32_t id,const VkDrawTexture* source)
{
    auto& target=renderedTargets.at(id);
    if(!target.feedback.image) {
        try {
            CreateImage(target.feedback,target.width,target.height,VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,6};
            VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool.maxSets=2;pool.poolSizeCount=1;pool.pPoolSizes=&size;
            vkRequired(vkCreateDescriptorPool(device,&pool,nullptr,&target.feedbackPool),"vkCreateDescriptorPool(current colour feedback)");
            const VkDescriptorSetLayout layouts[]={descriptorLayout,descriptorLayout};
            VkDescriptorSetAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocation.descriptorPool=target.feedbackPool;allocation.descriptorSetCount=2;allocation.pSetLayouts=layouts;
            VkDescriptorSet sets[2]{};
            vkRequired(vkAllocateDescriptorSets(device,&allocation,sets),"vkAllocateDescriptorSets(current colour feedback)");
            target.feedbackDescriptor=sets[0];target.filterFeedbackDescriptor=sets[1];
            const VkDescriptorImageInfo image{target.magnifiedSampler,target.feedback.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet writes[3]{};
            for(std::uint32_t index=0;index<3;++index) {
                auto& write=writes[index];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet=target.feedbackDescriptor;write.dstBinding=index;write.descriptorCount=1;
                write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
            }
            UpdateDescriptors(3,writes);
        }catch(...) {
            if(target.feedbackPool)vkDestroyDescriptorPool(device,target.feedbackPool,nullptr);
            target.feedbackPool=VK_NULL_HANDLE;target.feedbackDescriptor=target.filterFeedbackDescriptor=VK_NULL_HANDLE;
            Destroy(target.feedback);throw;
        }
    }
    if(source) {
        Require(source->image.image && source->sampler && source->magnifiedSampler,
            "Destination filter lost its original FilterGrey upload");
        const VkDescriptorImageInfo images[]={
            {source->sampler,source->image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {source->magnifiedSampler,source->image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {target.magnifiedSampler,target.feedback.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[3]{};
        for(std::uint32_t index=0;index<3;++index) {
            auto& write=writes[index];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet=target.filterFeedbackDescriptor;write.dstBinding=index;write.descriptorCount=1;
            write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=images+index;
        }
        UpdateDescriptors(3,writes);
    }
}

void EcstasyVulkanDevice::Storage::CopyFeedback(std::int32_t id)
{
    auto& target=renderedTargets.at(id);
    const auto current=id ? target.color.image:color.image;
    Require(current && target.feedback.image,"Current colour snapshot lost its independent image owner");
    // EndRenderPass resolves MSAA before this copy. LOAD resumes the same
    // multisampled scene/depth afterwards; history images never participate.
    Barrier(current,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    Barrier(target.feedback.image,target.feedbackInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,target.feedbackInitialized ? VK_ACCESS_SHADER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT,
        target.feedbackInitialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.dstSubresource=copy.srcSubresource;
    copy.extent={target.width,target.height,1};
    vkCmdCopyImage(commands,current,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target.feedback.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    Barrier(target.feedback.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    target.feedbackInitialized=true;
}

void EcstasyVulkanDevice::Storage::SavePipelineCache() noexcept
{
    if(options.pipelineCachePath.empty())return;
    try {
        std::size_t bytes{};
        if(vkGetPipelineCacheData(device,pipelineCache,&bytes,nullptr)!=VK_SUCCESS ||
            bytes<32 || bytes>16*1024*1024)return;
        std::vector<char> data(bytes);
        if(vkGetPipelineCacheData(device,pipelineCache,&bytes,data.data())!=VK_SUCCESS)return;
        std::ofstream cache(options.pipelineCachePath,std::ios::binary|std::ios::trunc);
        cache.write(data.data(),static_cast<std::streamsize>(bytes));
    } catch(...) {} // The driver cache is optional.
}

void EcstasyVulkanDevice::Storage::RunPipelineWorker()
{
    ConfigureNativeSceneWorker();
#ifdef __SWITCH__
    s32 priority{};
    if(R_SUCCEEDED(svcGetThreadPriority(&priority,CUR_THREAD_HANDLE)))
        (void)svcSetThreadPriority(CUR_THREAD_HANDLE,std::min(priority+4,63));
#endif
    for(;;) {
        std::packaged_task<VkPipeline()> job;
        {
            std::unique_lock<std::mutex> lock(pipelineMutex);
            pipelineWake.wait(lock,[this] {return stopPipelineWorker || !pipelineJobs.empty();});
            if(stopPipelineWorker)return;
            job=std::move(pipelineJobs.front());
            pipelineJobs.pop_front();
        }
        job();
    }
}

void EcstasyVulkanDevice::Storage::StopPipelineWorker() noexcept
{
    if(!pipelineWorker.joinable())return;
    {
        std::lock_guard<std::mutex> lock(pipelineMutex);
        stopPipelineWorker=true;
        pipelineJobs.clear();
    }
    pipelineWake.notify_one();
    pipelineWorker.join();
    for(auto& entry:pendingPipelines) {
        try {vkDestroyPipeline(device,entry.second.get(),nullptr);}
        catch(...) {} // Failed or cancelled jobs own no Vulkan handle.
    }
    pendingPipelines.clear();
}

VkPipeline EcstasyVulkanDevice::Storage::Pipeline(const VkDrawBatch& batch,bool samplingDepth)
{
    Require(!samplingDepth || (!batch.target && !batch.depth && !batch.depthWrite && !batch.postProcess),
        "Depth sampling pass cannot attach or modify its sampled source depth");
    const bool multisampled=batch.target==0 && !batch.postProcess && samples!=VK_SAMPLE_COUNT_1_BIT;
    // Source state is already constant over a batch. Keep only the actual
    // material filter enum in the pipeline key: fog uses minification as a
    // per-frame palette-table offset, and the other image passes ignore it.
    const std::int32_t materialMinification=batch.textureFunction<4 || batch.textureFunction==10 ? batch.minification:0;
    const auto key=std::make_tuple(batch.blend,batch.depth,batch.depthWrite,batch.postProcess,multisampled,samplingDepth,
        batch.textureFunction,batch.alphaTest,materialMinification,batch.wireframe,batch.untextured,batch.clampTopLeft,
        batch.showcaseEnvironmentBlend);
    if(lastPipeline && lastPipelineKey==key)return lastPipeline;
    const auto found=pipelines.find(key);
    if(found!=pipelines.end()) {lastPipelineKey=key;lastPipeline=found->second;return lastPipeline;}
    VkPipeline result{};
    if(options.asyncPipelines) {
        const auto pending=pendingPipelines.find(key);
        if(pending==pendingPipelines.end()) {
            if(!pipelineWorker.joinable())
                pipelineWorker=std::thread([this] {RunPipelineWorker();});
            std::packaged_task<VkPipeline()> job([this,batch,samplingDepth,multisampled,materialMinification] {
                return CompilePipeline(batch,samplingDepth,multisampled,materialMinification);
            });
            auto future=job.get_future();
            pendingPipelines.emplace(key,std::move(future));
            {
                std::lock_guard<std::mutex> lock(pipelineMutex);
                pipelineJobs.push_back(std::move(job));
            }
            pipelineWake.notify_one();
        } else if(pending->second.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            auto ready=std::move(pending->second);
            pendingPipelines.erase(pending);
            result=ready.get();
        }
        if(!result)return VK_NULL_HANDLE;
    } else result=CompilePipeline(batch,samplingDepth,multisampled,materialMinification);
    ++statistics.pipelineCreations;
    try {pipelines.emplace(key,result);}catch (...) {vkDestroyPipeline(device,result,nullptr);throw;}
    lastPipelineKey=key;lastPipeline=result;return result;
}

VkPipeline EcstasyVulkanDevice::Storage::CompilePipeline(const VkDrawBatch& batch,bool samplingDepth,
    bool multisampled,std::int32_t materialMinification)
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module=vertexShader;stages[0].pName="main";
    stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module=fragmentShader;stages[1].pName="main";
    const std::array<std::int32_t,6> materialState{{batch.textureFunction,batch.alphaTest,materialMinification,
        batch.untextured ? 0:1,batch.clampTopLeft ? 1:0,batch.showcaseEnvironmentBlend ? 1:0}};
    const VkSpecializationMapEntry materialEntries[]={{0,0,sizeof(std::int32_t)},
        {1,sizeof(std::int32_t),sizeof(std::int32_t)},{2,sizeof(std::int32_t)*2,sizeof(std::int32_t)},
        {3,sizeof(std::int32_t)*3,sizeof(std::int32_t)},{4,sizeof(std::int32_t)*4,sizeof(std::int32_t)},
        {5,sizeof(std::int32_t)*5,sizeof(std::int32_t)}};
    const VkSpecializationInfo materialSpecialization{6,materialEntries,sizeof(materialState),materialState.data()};
    stages[1].pSpecializationInfo=&materialSpecialization;
    VkVertexInputBindingDescription vertexBinding{0,sizeof(VkVertex),VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[]={{0,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(VkVertex,position)},
        {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(VkVertex,uvq)},
        {2,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(VkVertex,color)},
        {3,0,VK_FORMAT_R32G32_SFLOAT,offsetof(VkVertex,lod)},
        {4,0,VK_FORMAT_R32_UINT,offsetof(VkVertex,glyphBoundsIndex)},
        {5,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(VkVertex,normal)}};
    VkPipelineVertexInputStateCreateInfo vertexInput{};vertexInput.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount=1;vertexInput.pVertexBindingDescriptions=&vertexBinding;
    vertexInput.vertexAttributeDescriptionCount=6;vertexInput.pVertexAttributeDescriptions=attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology=batch.wireframe ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};viewport.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount=1;viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode=VK_POLYGON_MODE_FILL;
    raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo multisample{};multisample.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples=multisampled ? samples:VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthState{};depthState.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthState.depthTestEnable=batch.depth ? VK_TRUE:VK_FALSE;depthState.depthWriteEnable=batch.depthWrite ? VK_TRUE:VK_FALSE;
    depthState.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    // Exact algebraic reduction of the source ALPHA equation (A-B)*C+D.
    // Reject an unsupported equation rather than silently replacing it with
    // conventional transparency. All original Flailer equations are retained.
    const int a=static_cast<int>(batch.blend&255u),b=static_cast<int>((batch.blend>>8)&255u),
        c=static_cast<int>((batch.blend>>16)&255u),d=static_cast<int>(batch.blend>>24);
    Require(a<=2 && b<=2 && c<=2 && d<=2,"Source ALPHA selector is outside its defined domain");
    const VkBlendFactor factor=c==0 ? VK_BLEND_FACTOR_SRC_ALPHA : c==1 ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_CONSTANT_ALPHA;
    const VkBlendFactor inverse=c==0 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : c==1 ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    auto coefficient=[&](int lane,bool& negative) {
        const int variable=(a==lane ? 1:0)-(b==lane ? 1:0),constant=d==lane ? 1:0;
        negative=variable<0 && constant==0;
        if(!variable)return constant ? VK_BLEND_FACTOR_ONE:VK_BLEND_FACTOR_ZERO;
        if(variable==1 && !constant)return factor;
        if(variable==-1)return constant ? inverse:factor;
        throw std::runtime_error("Source ALPHA equation requires a destination-feedback render path");
    };
    bool sourceNegative{},destinationNegative{};
    blend.srcColorBlendFactor=coefficient(0,sourceNegative);blend.dstColorBlendFactor=coefficient(1,destinationNegative);
    Require(!(sourceNegative && destinationNegative),"Source ALPHA equation has two negative terms");
    blend.colorBlendOp=sourceNegative ? VK_BLEND_OP_REVERSE_SUBTRACT : destinationNegative ? VK_BLEND_OP_SUBTRACT:VK_BLEND_OP_ADD;
    // An exact source overwrite has no destination term. Avoid enabling
    // the native read/modify/write blend stage for that same equation;
    // destination-alpha and all other source ALPHA equations still blend.
    blend.blendEnable=blend.srcColorBlendFactor==VK_BLEND_FACTOR_ONE &&
        blend.dstColorBlendFactor==VK_BLEND_FACTOR_ZERO && blend.colorBlendOp==VK_BLEND_OP_ADD ? VK_FALSE:VK_TRUE;
    // The GS stores the fragment alpha independently of its RGB ALPHA blend.
    // GS ALPHA_2=0x48: (Cs - 0)*As + Cd. Vulkan SRC_ALPHA saturates
    // As to [0,1], while retail GS can use 0..255/128. The frontend
    // fragment shader folds its quantized Cs*As into RGB, so the fixed
    // Vulkan attachment only sums ONE+ONE. This is not a GS emulator.
    if(batch.showcaseEnvironmentBlend) {
        Require(batch.blend==0x01000200u && batch.textureFunction==0 &&
            !batch.untextured && batch.alphaTest==0 && !batch.postProcess,
            "Frontend environment material lost PAL additive source-alpha state");
        blend.srcColorBlendFactor=VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor=VK_BLEND_FACTOR_ONE;
        blend.colorBlendOp=VK_BLEND_OP_ADD;
        blend.blendEnable=VK_TRUE;
    }
    blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;blend.alphaBlendOp=VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blending{};blending.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount=1;blending.pAttachments=&blend;
    const VkDynamicState dynamicStates[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{};dynamic.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount=3;dynamic.pDynamicStates=dynamicStates;
    VkGraphicsPipelineCreateInfo pipeline{};pipeline.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount=2;pipeline.pStages=stages;pipeline.pVertexInputState=&vertexInput;pipeline.pInputAssemblyState=&assembly;
    pipeline.pViewportState=&viewport;pipeline.pRasterizationState=&raster;pipeline.pMultisampleState=&multisample;
    pipeline.pDepthStencilState=&depthState;pipeline.pColorBlendState=&blending;pipeline.pDynamicState=&dynamic;
    pipeline.layout=pipelineLayout;pipeline.renderPass=samplingDepth ? depthSamplingRenderPass:batch.postProcess ? postProcessRenderPass:
        multisampled ? multisampleRenderPass:renderPass;pipeline.subpass=0;
    VkPipeline result{};
    const auto status=vkCreateGraphicsPipelines(device,pipelineCache,1,&pipeline,nullptr,&result);
    if(status!=VK_SUCCESS && result)vkDestroyPipeline(device,result,nullptr);
    vkRequired(status,"vkCreateGraphicsPipelines");
    return result;
}

void EcstasyVulkanDevice::Storage::UploadTexture(const TextureInfo& texture,std::size_t index,bool repeatedPadding)
{
    Require(!nativeSubmissionPending,"Texture upload cannot replace an in-flight native image");

    Require(index<textures.size() && textures.size()<=4096,"Frame exceeds the Vulkan descriptor allocation capacity");
    // Decode and validate before destroying the previous cache entry. A
    // missing/truncated resource is still a strict error, never a cache hit.
    const auto* wire=texture.descriptor ? texture.descriptor->authoredRecord:nullptr;
    Require(wire && texture.descriptor->authoredRecordSize>=164,"Vulkan mip upload lost its TextureInfo");
    const auto levels=static_cast<std::uint32_t>(wire[0x6e])+1u;
    Require(levels<=7 && wire[0x6f]<=1 && wire[0x70]<=5 && wire[0x71]<=3,
        "TextureInfo contains unsupported original mip/filter fields");
    const auto baseBytes=static_cast<std::size_t>(ReadU32(wire+0x44));
    Require(baseBytes,"Original texture downloader has an empty base level");
    std::uint32_t width{},height{};std::size_t sourceBytes{};
    std::vector<std::uint8_t> pixels;std::vector<VkBufferImageCopy> copies;copies.reserve(levels);
    for(std::uint32_t level=0;level<levels;++level) {
        // FillVramWithTextures109C38/109EAC advances through the exact
        // released mip tails. Never synthesize smaller images from level0.
        const auto bytes=baseBytes>>(2u*level);
        std::uint32_t levelWidth{},levelHeight{};
        auto rgba=DecodeTextureMipRgba(texture,level,sourceBytes,bytes,levelWidth,levelHeight,repeatedPadding);
        if(!level) {
            width=levelWidth;height=levelHeight;
#ifdef __SWITCH__
            nx::OverrideControllerTexture(texture.descriptor->key,width,height,rgba);
#endif
        }
        VkBufferImageCopy copy{};copy.bufferOffset=pixels.size();
        copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,1};copy.imageExtent={levelWidth,levelHeight,1};
        copies.push_back(copy);pixels.insert(pixels.end(),rgba.begin(),rgba.end());sourceBytes+=bytes;
    }
    auto& target=textures[index];
    // A decoded movie frame or changed CLUT is new content in the same
    // image shape. The preceding fence completed before Gather; update its
    // pixels in place instead of destroying image/memory/samplers and
    // descriptor every frame. Descriptor changes still rebuild all state.
    const bool reuse=target.uploadCompleted && target.image.image && target.width==width &&
        target.height==height && target.levels==levels &&
        std::memcmp(wire,target.sourceDescriptor.data(),target.sourceDescriptor.size())==0;
    if(!reuse) {
        Destroy(target);target.width=width;target.height=height;target.levels=levels;
        CreateImage(target.image,width,height,VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT,levels);

    }
    target.source=&texture;target.repeatedPadding=repeatedPadding;target.uploadCompleted=false;
    const auto upload=StageTexture(pixels);
    for(auto& copy:copies)copy.bufferOffset+=upload.offset;
    ++statistics.textureUploads;statistics.textureUploadBytes+=pixels.size();
    Barrier(target.image.image,reuse ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,reuse ? VK_ACCESS_SHADER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT,
        reuse ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_IMAGE_ASPECT_COLOR_BIT,levels);
    vkCmdCopyBufferToImage(commands,upload.buffer,target.image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,levels,copies.data());
    Barrier(target.image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_IMAGE_ASPECT_COLOR_BIT,levels);
    if(!reuse) {
    VkSamplerCreateInfo sampler{};sampler.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // MMIN3 is nearest texel with linear mip interpolation, whereas MMIN4
    // is bilinear texel with nearest mip. Bit0 alone is not the texel filter.
    const auto minification=wire[0x70];
    sampler.magFilter=sampler.minFilter=minification==1 || minification>=4 ? VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    sampler.mipmapMode=minification==3 || minification==5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // PAL108CC0 constructs CLAMP from the inverses of descriptor4C bits0/1.
    sampler.addressModeU=(wire[0x4c]&1u) ? VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV=(wire[0x4c]&2u) ? VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler.maxLod=static_cast<float>(levels-1u);
    if(options.textureFilter!=EcstasyTextureFilter::Authored) {
        sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
        sampler.mipmapMode=options.textureFilter==EcstasyTextureFilter::Bilinear ?
            VK_SAMPLER_MIPMAP_MODE_NEAREST:VK_SAMPLER_MIPMAP_MODE_LINEAR;
        if(options.textureFilter==EcstasyTextureFilter::Anisotropic && actualAnisotropy>1.0f) {
            sampler.anisotropyEnable=VK_TRUE;sampler.maxAnisotropy=actualAnisotropy;
        }
    }
    vkRequired(vkCreateSampler(device,&sampler,nullptr,&target.sampler),"vkCreateSampler");
    sampler.magFilter=sampler.minFilter=wire[0x6f] ? VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler.maxLod=0;sampler.anisotropyEnable=VK_FALSE;
    vkRequired(vkCreateSampler(device,&sampler,nullptr,&target.magnifiedSampler),"vkCreateSampler(magnification)");
    sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
    vkRequired(vkCreateSampler(device,&sampler,nullptr,&target.uiSampler),"vkCreateSampler(PC UI)");
    VkDescriptorSetAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool=descriptorPool;allocation.descriptorSetCount=1;allocation.pSetLayouts=&descriptorLayout;
    vkRequired(vkAllocateDescriptorSets(device,&allocation,&target.descriptor),"vkAllocateDescriptorSets");
    const VkDescriptorImageInfo images[]={{target.sampler,target.image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {target.magnifiedSampler,target.image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {target.uiSampler,target.image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[3]{};
    for(std::uint32_t i=0;i<3;++i) {
        auto& write=writes[i];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=target.descriptor;
        write.dstBinding=i;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo=images+i;
    }
    UpdateDescriptors(3,writes);
    }
    // Only the bytes actually consumed by DecodeTextureMipRgba are retained.
    // Include every authored mip; in-place changes to a tail must invalidate
    // the cache even when its base remains byte-identical.
    const auto psm=wire[0x4a];
    target.sourcePixels.assign(texture.pixels.Data(),texture.pixels.Data()+sourceBytes);
    target.sourcePaletteBytes=texture.paletteBytes;
    if(psm==0x13 || psm==0x14)
        target.sourcePalette.assign(texture.palette.Data(),texture.palette.Data()+static_cast<std::size_t>(texture.paletteBytes));
    std::copy_n(wire,target.sourceDescriptor.size(),target.sourceDescriptor.begin());
    target.CaptureOwners(texture);
    target.lastReferencedFrame=completedFrameSerial;
    Require(nextTextureGeneration!=UINT64_MAX,"Native texture generation exhausted");
    target.generation=++nextTextureGeneration;

}

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
    for (const auto& draw:frame.Commands()) {
        categoryTimer.Enter(draw);
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
            batch.nativeShadingIndex=nativeFrameShading.Capture(job,gatheredGlyphBounds);
            bool prepare=false;auto* resident=residentGeometry.Acquire(job,prepare);
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
            // A Vulkan draw is only a transport boundary. Concatenate adjacent
            // triangles when every consumed GPU state value agrees. Original
            // packet order, texture versions, alpha/depth, and target changes
            // are untouched; no sorting or transparency approximation occurs.
            appendBatch(batch);
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

void EcstasyVulkanDevice::Storage::RecordDepthTransfers(const std::vector<EcstasyDepthTransfer>& requests,
    const std::array<double,2>& displayTranslation)
{
    // Copy this scene's requested samples after its depth writes. Publish
    // them only at the next completed-frame boundary, as before; CPU scene
    // construction has already consumed the preceding sample generation.
    if(requests.empty())return;
    Require(requests.size()<=MaximumDepthReadbacks && depthReadbacks.empty(),
        "Native depth requests exceed their source target bound or were not retired");
    Require(depth.image!=VK_NULL_HANDLE,"Source depth request has no native attachment");
    const auto pixels=static_cast<VkDeviceSize>(options.width)*options.height;
    Require(pixels<=std::numeric_limits<VkDeviceSize>::max()/(MaximumDepthReadbacks*sizeof(float)),
        "Native depth attachment/request byte bound overflow");
    const auto maximum=pixels*MaximumDepthReadbacks*sizeof(float);
    VkDeviceSize totalBytes=0;
    depthReadbacks.reserve(MaximumDepthReadbacks);
    const auto sx=static_cast<float>(options.width)/static_cast<float>(options.logicalWidth);
    const auto sy=static_cast<float>(options.height)/static_cast<float>(options.logicalHeight);
    for(const auto& request:requests) {
        DepthReadback copy;copy.request=request;copy.scaleX=sx;copy.scaleY=sy;
        // Calibration moves the PC colour/depth viewport. Read the same
        // translated pixels; the original DISPLAY offset did not move VRAM.
        // Retain the translation until this submission's fence completes.
        copy.translationX=displayTranslation[0]*options.width;
        copy.translationY=displayTranslation[1]*options.height;
        const auto clipCoordinate=[](double coordinate,std::uint32_t extent) {
            return static_cast<std::int32_t>(std::clamp(coordinate,0.0,static_cast<double>(extent)));
        };
        copy.left=clipCoordinate(std::floor(copy.SampleX(0)),options.width);
        copy.top=clipCoordinate(std::floor(copy.SampleY(0)),options.height);
        const auto right=clipCoordinate(std::floor(copy.SampleX(request.width-1))+1.0,options.width);
        const auto bottom=clipCoordinate(std::floor(copy.SampleY(request.height-1))+1.0,options.height);
        copy.width=right-copy.left;copy.height=bottom-copy.top;
        Require(copy.width>=0 && copy.height>=0,
            "Translated source depth sample rectangle is inverted");
        const auto bytes=static_cast<VkDeviceSize>(copy.width)*static_cast<VkDeviceSize>(copy.height)*sizeof(float);
        Require(bytes<=maximum-totalBytes,"Native depth request extents exceed their attachment bound");
        copy.byteOffset=totalBytes;totalBytes+=bytes;
        depthReadbacks.push_back(copy);
    }
    // Entirely cropped requests still publish an occluded tile at the next
    // completion boundary, without issuing a zero-sized Vulkan image copy.
    if(!totalBytes)return;
    EnsureDepthReadbackCapacity(totalBytes,maximum);
    Barrier(depth.image,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    for(const auto& copy:depthReadbacks) {
        if(!copy.width || !copy.height)continue;
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
        region.bufferOffset=copy.byteOffset;
        region.imageOffset={copy.left,copy.top,0};
        region.imageExtent={static_cast<std::uint32_t>(copy.width),static_cast<std::uint32_t>(copy.height),1};
        vkCmdCopyImageToBuffer(commands,depth.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            depthStaging.buffer,1,&region);
    }
    Barrier(depth.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
    VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&host,0,nullptr,0,nullptr);
}

void EcstasyVulkanDevice::Storage::PublishDepthTransfers()
{
    // Only a successful GPU completion reaches this method. World, ePoly
    // and particles share the original target Z range in native D32. Read
    // that retained scale directly; clear depth1 is the original zero Z.
    for(const auto& copy:depthReadbacks) {
        const auto& request=copy.request;
        const auto bytes=static_cast<VkDeviceSize>(copy.width)*copy.height*sizeof(float);
        const float* source=nullptr;
        if(bytes) {
            Require(depthStaging.mapped && copy.byteOffset<=depthStaging.size && bytes<=depthStaging.size-copy.byteOffset,
                "Completed source depth tile leaves its retained readback buffer");
            source=static_cast<const float*>(depthStaging.mapped)+copy.byteOffset/sizeof(float);
        }
        bool valid=true;
        for(std::int32_t y=0;y<request.height;++y)for(std::int32_t x=0;x<request.width;++x) {
            const auto sampleX=std::floor(copy.SampleX(x))-copy.left;
            const auto sampleY=std::floor(copy.SampleY(y))-copy.top;
            // Pixels cropped out of the PC attachment cannot count as clear
            // sky. Do not clamp them onto an unrelated border pixel.
            auto value=request.maximumDepth;
            if(source && sampleX>=0 && sampleY>=0 && sampleX<copy.width && sampleY<copy.height) {
                const float sampleDepth=source[static_cast<std::size_t>(sampleY)*
                    static_cast<std::size_t>(copy.width)+static_cast<std::size_t>(sampleX)];
                if(!std::isfinite(sampleDepth) || sampleDepth<0.0f || sampleDepth>1.0f) {valid=false;continue;}
                value=static_cast<std::uint32_t>(std::llround((1.0-static_cast<double>(sampleDepth))*request.maximumDepth));
            }
            request.destination->values[static_cast<std::size_t>(y)*static_cast<std::size_t>(request.width)+
                static_cast<std::size_t>(x)]=value;
        }
        Require(valid,"Completed source depth tile has a singular projection");
    }
    depthReadbacks.clear();
}

EcstasyNativeMirrorPresentation EcstasyVulkanDevice::Storage::MirrorPresentation(const EcstasyFrame& frame) const
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
        options.width,options.height,properties.limits.maxImageDimension2D);
}

bool EcstasyVulkanDevice::Storage::CanGatherBeforeCompletion(const EcstasyFrame& frame,
    const EcstasyNativeMirrorPresentation& mirror,const VkNativeFrameTextureReferences& frameTextures)
{
    // Gather touches no native allocation or mapped GPU bytes when every
    // image already matches the sealed source and all sampled extents exist.
    // Texture replacement and target creation remain after the prior fence.
    const auto targetReady=[&](std::int32_t id,std::uint32_t width,std::uint32_t height) {
        const auto found=renderedTargets.find(id);
        return found!=renderedTargets.end() && found->second.width==width && found->second.height==height;
    };
    if(!targetReady(0,options.width,options.height))return false;
    for(const auto& draw:frame.Commands()) {
        if(!draw.target)return false;
        for(const auto* target:{draw.target,draw.sampledTarget})if(target && target->id>=7) {
            if(!targetReady(target->id,target->id==7 ? mirror.rasterWidth:static_cast<std::uint32_t>(target->width),
                target->id==7 ? mirror.rasterHeight:static_cast<std::uint32_t>(target->height)))return false;
        }
        if(draw.sampledTarget) {
            const auto id=draw.sampledTarget->id>=7 ? draw.sampledTarget->id:0;
            const auto found=renderedTargets.find(id);
            if(found==renderedTargets.end() ||
                (draw.sampledPreviousFrame ? found->second.previousDescriptor:found->second.descriptor)==VK_NULL_HANDLE)return false;
        }
    }
    const auto& referenced=frameTextures.Get();
    if(referenced.size()>4096 || textures.size()>4096)return false;
    for(const auto& key:referenced) {
        const auto found=textureIndices.find(key);
        if(found==textureIndices.end())return false;
        auto& texture=textures[found->second];
        ++statistics.textureValidations;
        if(!texture.Matches(*key.source))return false;
        texture.validatedGather=gatherSerial;
    }
    return true;
}

void EcstasyVulkanDevice::Storage::RemapGatheredTextures(std::vector<VkDrawBatch>& batches) const
{
    // Fence-gated retirement may compact the texture vector after CPU
    // preparation. Batches store indices, so repair them by their exact old
    // image/context key. Target/fog/focus passes do not consume this index.
    for(auto& batch:batches) {
        if(batch.sampledTarget>=0 ||
            !((batch.textureFunction>=0 && batch.textureFunction<=3) || batch.textureFunction==10))continue;
        Require(batch.texture<gatheredTextureKeys.size(),"Prepared draw lost its cached texture key");
        const auto found=textureIndices.find(gatheredTextureKeys[batch.texture]);
        Require(found!=textureIndices.end(),"Texture retirement removed a referenced prepared image");
        batch.texture=found->second;
    }
}

bool EcstasyVulkanDevice::Storage::Render(EcstasyFrame* frame)
{
    const bool reuse=frame==nullptr;
    if(reuse) {
        if(!CanRenderPrepared())return false;
    } else {
        Require(frame->State()==EcstasyFrame::Phase::Ready,"Vulkan accepts only a sealed original frame");
        CapturePreparedInputs(*frame);
    }
    using Clock=std::chrono::steady_clock;
    const auto renderBegin=Clock::now();
    const auto milliseconds=[](Clock::duration duration) {return std::chrono::duration<double,std::milli>(duration).count();};
    const auto lastGpu=statistics.gpuMilliseconds;
    const auto lastGpuFrame=statistics.completedGpuFrame;
    const auto lastGpuAvailable=statistics.gpuTimeAvailable;
    const auto lastGpuProfile=statistics.gpuProfile;
    const auto lastRenderProfile=statistics.renderProfile;
    statistics={};statistics.gpuMilliseconds=lastGpu;statistics.completedGpuFrame=lastGpuFrame;
    statistics.gpuTimeAvailable=lastGpuAvailable;statistics.gpuProfile=lastGpuProfile;
    if(reuse) {
        statistics.renderProfile.shadowStrips=lastRenderProfile.shadowStrips;
        statistics.renderProfile.shadowTriangles=lastRenderProfile.shadowTriangles;
    }
    auto submissionPhaseStarted=renderBegin;
    const auto submissionCheckpoint=[&](EcstasySubmitPhase phase) {
        const auto now=Clock::now();
        statistics.renderProfile.submitPhaseMilliseconds[static_cast<std::size_t>(phase)]+=
            milliseconds(now-submissionPhaseStarted);
        submissionPhaseStarted=now;
    };
    statistics.presentationTarget=swapchain!=VK_NULL_HANDLE;
    statistics.availableCpuThreads=NativeSceneEffectiveHardwareConcurrency();
    statistics.reusedPreparation=reuse;
    if(reuse)statistics.particles=preparedFrame.particles;
    else BeginGather();

    auto& data=gatheredVertices;auto& elementIndices=gatheredIndices;auto& batches=gatheredBatches;
    std::optional<VkNativeFrameTextureReferences> frameTextures;
    if(frame)frameTextures.emplace(frame->Commands(),referencedTextures);
    bool gatherAttempted=false;
    const auto prepare=[&] {
        if(gatherAttempted)BeginGather();
        gatherAttempted=true;
        data.clear();elementIndices.clear();batches.clear();gatheredGlyphBounds.clear();gatheredGlyphBounds.push_back({});
        const auto begin=Clock::now();
        Gather(*frame,data,elementIndices,batches);
        statistics.gatherMilliseconds+=milliseconds(Clock::now()-begin);
    };
    auto mirrorPixels=frame ? MirrorPresentation(*frame):EcstasyNativeMirrorPresentation{};
    bool prepared=reuse;
    if(reuse) {
        // The native render tail appends FXAA vertices and optional transform
        // records. Reuse the exact source preparation, not those appendages.
        data.resize(preparedFrame.vertexCount);
        gatheredGlyphBounds.resize(preparedFrame.glyphCount);
    }
    if(nativeSubmissionPending) {
        const auto status=vkGetFenceStatus(device,fence);
        if(status==VK_NOT_READY) {
            if(frame && CanGatherBeforeCompletion(*frame,mirrorPixels,*frameTextures)) {
                gatheredTextureKeys.clear();gatheredTextureKeys.reserve(textures.size());
                for(const auto& texture:textures)gatheredTextureKeys.push_back({texture.source,texture.repeatedPadding});
                prepare();prepared=true;
            }
        } else vkRequired(status,"vkGetFenceStatus(previous native submission)");
    }
    bool completedSubmission=false;
    if(nativeSubmissionPending) {
        const auto waitBegin=Clock::now();
        vkRequired(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),
             "vkWaitForFences(previous native submission)");
        statistics.fenceWaitMilliseconds=milliseconds(Clock::now()-waitBegin);
        PublishGpuTime();
        nativeSubmissionPending=false;
        ++completedFrameSerial;
        completedSubmission=true;
    }
    submissionCheckpoint(EcstasySubmitPhase::Preflight);
    auto& preflight=statistics.renderProfile.submitPhaseMilliseconds[static_cast<std::size_t>(EcstasySubmitPhase::Preflight)];
    preflight=std::max(0.0,preflight-statistics.gatherMilliseconds-statistics.fenceWaitMilliseconds);
    std::uint32_t swapIndex{};
    if(swapchain) {

        auto acquiredResult=vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&swapIndex);
        if(acquiredResult==VK_ERROR_OUT_OF_DATE_KHR) {
            vkRequired(vkDeviceWaitIdle(device),"vkDeviceWaitIdle(recreate)");CloseTargets();CreateTargets();
            // Surface recreation can also change the native render extent.
            // Rebuild through the same Gather after its new targets exist.
            if(reuse)return false; // No image was acquired or frame submitted.
            prepared=false;mirrorPixels=MirrorPresentation(*frame);
            acquiredResult=vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&swapIndex);
        }
        if(acquiredResult!=VK_SUBOPTIMAL_KHR)vkRequired(acquiredResult,"vkAcquireNextImageKHR");
    }
    submissionCheckpoint(EcstasySubmitPhase::Acquire);
    // Acquiring an obsolete swapchain can retire its depth target. Preserve
    // that cancellation before publishing samples from the completed image.
    if(completedSubmission)PublishDepthTransfers();
    // The previous GPU submission is complete, so its mapped/staging memory
    // and command buffer can now be reused without an in-flight hazard.
    ClearFrameAllocations();
    vkRequired(vkResetCommandBuffer(commands,0),"vkResetCommandBuffer");
    bool reuseRecording=reuse && recordedCommandsValid;
    // The primary is now reset and the previous real fence is complete.
    // Keep executable secondary recordings only for this unchanged plan.
    if(!reuseRecording)ResetRecordingLanes();
    submissionCheckpoint(EcstasySubmitPhase::Reclaim);
    if(frame) {
        const bool texturesReindexed=RetireUnusedTextures(*frameTextures);
        if(prepared && texturesReindexed)RemapGatheredTextures(batches);
        preparedFrame.textureKeys=frameTextures->Get().size();
    }
    submissionCheckpoint(EcstasySubmitPhase::Textures);
    // With no source mutation, every cached native descriptor remains live.
    // Retirement runs at the next genuine source capture, after its fence;
    // repeated submissions never dereference retained TextureInfo tokens.
    CreateSampledTarget(0,options.width,options.height);
    // Frames without a rear-view owner do not need a captured source extent.
    // Only the mirror uses its selected native raster size; the environment
    // targets and their texture coordinates keep their authored extents.
    auto& destinations=preparedFrame.destinations;
    if(frame)for(const auto& draw:frame->Commands()) {
        Require(draw.target,"Vulkan draw has no retained destination");
        if(draw.kind==EcstasyDrawKind::DepthOfFieldPass)CreateFocusTargets();
        const auto destination=draw.target->id>=7 ? draw.target->id:0;destinations.insert(destination);
        if(destination)CreateSampledTarget(destination,
            destination==7 ? mirrorPixels.rasterWidth : static_cast<std::uint32_t>(draw.target->width),
            destination==7 ? mirrorPixels.rasterHeight : static_cast<std::uint32_t>(draw.target->height));
        if(draw.sampledTarget && draw.sampledTarget->id>=7) {
            const auto source=draw.sampledTarget->id;
            CreateSampledTarget(source,
                source==7 ? mirrorPixels.rasterWidth : static_cast<std::uint32_t>(draw.sampledTarget->width),
                source==7 ? mirrorPixels.rasterHeight : static_cast<std::uint32_t>(draw.sampledTarget->height));
        }
    }
    destinations.insert(0); // The display is cleared/presented even without geometry.
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkRequired(vkBeginCommandBuffer(commands,&begin),"vkBeginCommandBuffer");
    // Sparse pass timestamps limit command and synchronization overhead.
    const bool sampleGpuTargets=platform::nativeDiagnosticsEnabled && timestampQueries && (nextSubmission%16u==0);
    std::uint32_t timestampCount=2;
    if(timestampQueries) {
        vkCmdResetQueryPool(commands,timestampQueries,0,sampleGpuTargets ? TimestampQueryCapacity:2);
        vkCmdWriteTimestamp(commands,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timestampQueries,0);
    }

    InitializeSampledTargets();
    // Scene captures publish history before the HUD. Copying the completed
    // display here would feed gauges/maps into the RT/LT camera blur.
    // Keep the old path only for callers without an explicit scene boundary.
    const auto historyReaders=preparedFrame.historyReaders;
    const bool advancesPresentation=frame && frame->AdvancesPresentation();
    // Display history follows newly rendered images, independently of the
    // 60Hz logical clock. A capped presentation often captures between ticks.
    const bool updatesDisplayHistory=frame!=nullptr;
    if(historyReaders.display && updatesDisplayHistory) {
        statistics.renderProfile.historyFrames=1;
        statistics.renderProfile.historyFramesWithoutTick=advancesPresentation ? 0:1;
        statistics.renderProfile.historySeeds=preparedFrame.useDisplayHistory ? 0:1;
    }
    if(historyReaders.display && !preparedFrame.sceneHistoryBoundary && hasFrame && updatesDisplayHistory) {
        CopyHistory(0,color.image,false);

    }
    const auto environmentPolicy=EcstasyEnvironmentHistoryPolicy(hasFrame,frame!=nullptr,
        frame && frame->AdvancesPresentation(),frame ? frame->CapturedEnvironmentFaces():0);
    if(historyReaders.environment && environmentPolicy.snapshotPrevious) {
        auto previousEnvironment=renderedTargets.find(14);
        if(previousEnvironment!=renderedTargets.end() && previousEnvironment->second.initialized) {
            CopyHistory(14,previousEnvironment->second.color.image,true);

        }
    }
    submissionCheckpoint(EcstasySubmitPhase::Targets);
    const auto& nextDepthTransfers=preparedFrame.depthTransfers;
    // Staging belongs to the native presenter, not to a gameplay/resource
    // owner. Reuse its capacity after each completed frame; source geometry
    // and captured draw values are rebuilt for each changed source state.
    // An unchanged presentation still records every pass and submits it.
    if(!prepared)prepare();
    submissionPhaseStarted=Clock::now(); // Gather is already timed separately.
    if(frame) {
        preparedFrame.vertexCount=data.size();preparedFrame.glyphCount=gatheredGlyphBounds.size();
        preparedFrame.particles=statistics.particles;
    }
    // Descriptor contents are prepared once before submission, never changed
    // between two recorded draws that might still refer to the same set.
    // All feedback descriptors were installed by the successful source
    // render. Rewriting an identical ordinary set would invalidate retained
    // secondary command buffers; source changes always take the full path.
    if(!reuse)for(const auto& batch:batches)if(batch.feedbackGroup) {
        const auto* source=batch.textureFunction==10 ? &textures.at(batch.texture):nullptr;
        CreateFeedback(batch.target,source);
    }
    if(historyReaders.display && preparedFrame.sceneHistoryBoundary)CreateFeedback(0,nullptr);
    VkDrawBatch antialiasing;
    if(options.antialiasing==EcstasyAntialiasing::Fxaa) {
        CreateFeedback(0,nullptr);
        antialiasing.first=static_cast<std::uint32_t>(data.size());antialiasing.count=3;
        antialiasing.postProcess=true;antialiasing.blend=0x00000202u;antialiasing.textureFunction=4;antialiasing.minification=-1;
        antialiasing.viewport={0,0,static_cast<float>(options.width),static_cast<float>(options.height),0,1};
        antialiasing.scissor={{0,0},{options.width,options.height}};
        data.push_back({{{-1,-1,0,1}},{{0,0,1}},{{1,1,1,1}},{{0,0}}});
        data.push_back({{{3,-1,0,1}},{{2,0,1}},{{1,1,1,1}},{{0,0}}});
        data.push_back({{{-1,3,0,1}},{{0,2,1}},{{1,1,1,1}},{{0,0}}});
    }

    const auto vertexDataBytes=static_cast<VkDeviceSize>(data.size()*sizeof(VkVertex));
    const auto vertexBytes=std::max<VkDeviceSize>(vertexDataBytes,4);
    const auto hostFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const bool staged=geometryMemory==GeometryMemory::StagedDeviceLocal;
    const auto directFlags=geometryMemory==GeometryMemory::LocalCoherent ?
        hostFlags|VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT:hostFlags;
    residentPendingUploads.clear();residentCopies.clear();
    VkDeviceSize residentUploadBytes=0;
    if(!residentGeometry.Active().empty()) {
        const auto capacity=static_cast<VkDeviceSize>(residentGeometry.Capacity()*VkResidentGeometryCache::SlotBytes);
        if(residentVertices.size<capacity ||
            (staged ? residentStaging.size<capacity || !residentStaging.mapped:!residentVertices.mapped)) {
            Destroy(residentVertices);Destroy(residentStaging);residentGeometry.InvalidateUploads();
            CreateBuffer(residentVertices,capacity,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|
                (staged ? VK_BUFFER_USAGE_TRANSFER_DST_BIT:0),
                staged ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT:directFlags,!staged);
            if(staged)CreateBuffer(residentStaging,capacity,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,hostFlags,true);

        }
        for(const auto slot:residentGeometry.Active()) {
            const auto& entry=residentGeometry.Entry(slot);
            Require(entry.prepared,"Resident geometry upload has unfinished vertex preparation");
            if(entry.uploaded)continue;
            const auto offset=static_cast<VkDeviceSize>(slot)*VkResidentGeometryCache::SlotBytes;
            const auto bytes=static_cast<VkDeviceSize>(entry.count)*sizeof(VkVertex);
            auto* mapped=static_cast<std::byte*>(staged ? residentStaging.mapped:residentVertices.mapped);
            std::memcpy(mapped+static_cast<std::size_t>(offset),entry.shadow.data(),static_cast<std::size_t>(bytes));
            residentCopies.push_back({offset,offset,bytes});residentPendingUploads.push_back(slot);
            residentUploadBytes+=bytes;
        }
        if(staged && !residentCopies.empty()) {
            // One recording command carries all dirty slots. No Vulkan buffer,
            // allocation or pipeline barrier is created for an individual strip.
            vkCmdCopyBuffer(commands,residentStaging.buffer,residentVertices.buffer,
                static_cast<std::uint32_t>(residentCopies.size()),residentCopies.data());
        }
    }
    residentIndexPendingUploads.clear();residentIndexCopies.clear();
    VkDeviceSize residentIndexUploadBytes=0;
    if(!residentIndexCache.Active().empty()) {
        const auto capacity=static_cast<VkDeviceSize>(residentIndexCache.Capacity())*sizeof(std::uint32_t);
        if(residentIndices.size<capacity ||
            (staged ? residentIndexStaging.size<capacity || !residentIndexStaging.mapped:!residentIndices.mapped)) {
            Destroy(residentIndices);Destroy(residentIndexStaging);residentIndexCache.InvalidateUploads();
            CreateBuffer(residentIndices,capacity,VK_BUFFER_USAGE_INDEX_BUFFER_BIT|
                (staged ? VK_BUFFER_USAGE_TRANSFER_DST_BIT:0),
                staged ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT:directFlags,!staged);
            if(staged)CreateBuffer(residentIndexStaging,capacity,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,hostFlags,true);

        }
        for(auto* entry:residentIndexCache.Active()) {
            if(entry->uploaded)continue;
            const auto offset=static_cast<VkDeviceSize>(entry->first)*sizeof(std::uint32_t);
            const auto bytes=static_cast<VkDeviceSize>(entry->shadow.size())*sizeof(std::uint32_t);
            auto* mapped=static_cast<std::byte*>(staged ? residentIndexStaging.mapped:residentIndices.mapped);
            std::memcpy(mapped+static_cast<std::size_t>(offset),entry->shadow.data(),static_cast<std::size_t>(bytes));
            residentIndexCopies.push_back({offset,offset,bytes});residentIndexPendingUploads.push_back(entry);
            residentIndexUploadBytes+=bytes;
        }
        if(staged && !residentIndexCopies.empty())
            vkCmdCopyBuffer(commands,residentIndexStaging.buffer,residentIndices.buffer,
                static_cast<std::uint32_t>(residentIndexCopies.size()),residentIndexCopies.data());
    }
    if(vertices.size<vertexBytes) {
        const auto capacity=std::max(vertexBytes,vertices.size+vertices.size/2);
        Require(capacity<=std::numeric_limits<VkDeviceSize>::max()-65535,"Native vertex capacity overflow");
        const auto aligned=(capacity+65535)&~VkDeviceSize(65535);
        Destroy(vertices);Destroy(vertexStaging);
        CreateBuffer(vertices,aligned,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|
            (staged ? VK_BUFFER_USAGE_TRANSFER_DST_BIT:0),staged ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT:directFlags,!staged);
        if(staged)CreateBuffer(vertexStaging,aligned,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,hostFlags,true);

    }
    if(vertexDataBytes) {
        std::memcpy(staged ? vertexStaging.mapped:vertices.mapped,data.data(),
            static_cast<std::size_t>(vertexDataBytes));
        if(staged) {
            const VkBufferCopy copy{0,0,vertexDataBytes};
            vkCmdCopyBuffer(commands,vertexStaging.buffer,vertices.buffer,1,&copy);
        }
    }
    const auto indexBytes=static_cast<VkDeviceSize>(elementIndices.size()*sizeof(std::uint32_t));
    if(indices.size<indexBytes) {
        const auto capacity=std::max(indexBytes,indices.size+indices.size/2);
        Require(capacity<=std::numeric_limits<VkDeviceSize>::max()-65535,"Native index capacity overflow");
        const auto aligned=(capacity+65535)&~VkDeviceSize(65535);
        Destroy(indices);Destroy(indexStaging);
        CreateBuffer(indices,aligned,VK_BUFFER_USAGE_INDEX_BUFFER_BIT|
            (staged ? VK_BUFFER_USAGE_TRANSFER_DST_BIT:0),staged ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT:directFlags,!staged);
        if(staged)CreateBuffer(indexStaging,aligned,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,hostFlags,true);

    }
    if(indexBytes) {
        std::memcpy(staged ? indexStaging.mapped:indices.mapped,elementIndices.data(),
            static_cast<std::size_t>(indexBytes));
        if(staged) {
            const VkBufferCopy copy{0,0,indexBytes};
            vkCmdCopyBuffer(commands,indexStaging.buffer,indices.buffer,1,&copy);
        }
    }
    if(staged && (vertexDataBytes || indexBytes || residentUploadBytes || residentIndexUploadBytes)) {
        VkBufferMemoryBarrier barriers[4]{};
        std::uint32_t count{};
        const auto addBarrier=[&](VkBuffer buffer,VkDeviceSize bytes,VkAccessFlags access) {
            auto& barrier=barriers[count++];barrier.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=access;
            barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer=buffer;barrier.offset=0;barrier.size=bytes;
        };
        if(vertexDataBytes)addBarrier(vertices.buffer,vertexDataBytes,VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
        if(indexBytes)addBarrier(indices.buffer,indexBytes,VK_ACCESS_INDEX_READ_BIT);
        if(residentUploadBytes)addBarrier(residentVertices.buffer,residentVertices.size,VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
        if(residentIndexUploadBytes)addBarrier(residentIndices.buffer,residentIndices.size,VK_ACCESS_INDEX_READ_BIT);
        vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            0,0,nullptr,count,barriers,0,nullptr);
    }
    submissionCheckpoint(EcstasySubmitPhase::Upload);
    for(auto& list:targetDraws)list.clear();
    for(auto& list:targetDependencies)list.clear();
    for(auto& batch:batches) {
        Require(batch.target>=0 && batch.target<static_cast<std::int32_t>(targetDraws.size()),
            "Vulkan draw has an invalid target owner");
        targetDraws[static_cast<std::size_t>(batch.target)].push_back(&batch);
        if(batch.sampledTarget>=0 && !batch.sampledPreviousFrame &&
            destinations.count(batch.sampledTarget)) {
            auto& dependencies=targetDependencies[static_cast<std::size_t>(batch.target)];
            if(std::find(dependencies.begin(),dependencies.end(),batch.sampledTarget)==dependencies.end())
                dependencies.push_back(batch.sampledTarget);
        }
    }
    bool hasIndirectDraws=false;
    if(nativeMultiDraw) {
        // Exact float-bit keys retain the producer's matrix and projected Q. This
        // table is frame-owned and shares the already fenced native data buffer.
        // Resident vertex/cache bytes never contain these per-frame table offsets.
        std::unordered_map<std::array<std::uint32_t,22>,std::uint32_t,VkNativeTransformKeyHash> streamedTransforms;
        streamedTransforms.reserve(std::min<std::size_t>(batches.size(),1024));
        std::array<std::uint32_t,22> lastTransformKey{};
        std::uint32_t lastTransformIndex=UINT32_MAX;
        const auto captureTransform=[&](const VkDrawBatch& batch) {
            std::array<std::uint32_t,22> key{};
            std::memcpy(key.data(),batch.nativeLocalToClip.data(),sizeof(batch.nativeLocalToClip));
            std::memcpy(key.data()+16,batch.nativeClipMapping.data(),sizeof(batch.nativeClipMapping));
            std::memcpy(key.data()+20,&batch.nativeTextureQScale,sizeof(float));
            key[21]=batch.nativeShadingIndex;
            // Adjacent strips commonly share an exact transform. Avoid repeating
            // the tree lookup while retaining the same full bitwise key and index.
            if(lastTransformIndex!=UINT32_MAX && key==lastTransformKey)return lastTransformIndex;
            const auto found=streamedTransforms.find(key);
            if(found!=streamedTransforms.end()) {
                lastTransformKey=key;lastTransformIndex=found->second;return lastTransformIndex;
            }
            Require(gatheredGlyphBounds.size()<=std::numeric_limits<std::uint32_t>::max()-6,
                "Native frame transform table exceeds32-bit index range");
            const auto first=static_cast<std::uint32_t>(gatheredGlyphBounds.size());
            for(std::size_t column=0;column<4;++column) {
                std::array<float,4> value{};
                std::copy_n(batch.nativeLocalToClip.data()+column*4,4,value.data());
                gatheredGlyphBounds.push_back(value);
            }
            gatheredGlyphBounds.push_back(batch.nativeClipMapping);
            std::array<float,4> parameters{{batch.nativeTextureQScale,0,0,0}};
            std::memcpy(&parameters[1],&batch.nativeShadingIndex,sizeof(batch.nativeShadingIndex));
            gatheredGlyphBounds.push_back(parameters);
            streamedTransforms.emplace(key,first);
            lastTransformKey=key;lastTransformIndex=first;return first;
        };
        for(auto& packets:targetDraws) {
            for(std::size_t first=0;first<packets.size();) {
                std::size_t end=first+1;
                const auto& candidate=*packets[first];
                if(candidate.indexed && candidate.nativeGpuPosition) {
                    while(end<packets.size() && packets[end]->SameDrawState(candidate))++end;
                }
                // Discover the entire group before changing its state flags.
                // Singletons retain the original push-constant transform path.
                if(end-first>1) {
                    hasIndirectDraws=true;
                    for(std::size_t index=first;index<end;++index) {
                        auto& batch=*packets[index];
                        batch.nativeTransformIndex=captureTransform(batch);
                        batch.nativeIndirectTransform=true;
                    }
                }
                first=end;
            }
        }
    }
    const auto glyphBytes=static_cast<VkDeviceSize>(gatheredGlyphBounds.size()*sizeof(gatheredGlyphBounds[0]));
    Require(glyphBytes<=properties.limits.maxStorageBufferRange,
        "Native frame shading/transform/glyph data exceeds the physical device storage-buffer range");
    const bool grewGlyphTable=glyphBounds.size<glyphBytes;
    if(grewGlyphTable) {
        Destroy(glyphBounds);CreateBuffer(glyphBounds,(glyphBytes+4095)&~VkDeviceSize(4095),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,hostFlags,true);
        const VkDescriptorBufferInfo info{glyphBounds.buffer,0,glyphBounds.size};
        VkWriteDescriptorSet write{};write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=frameDescriptor;
        write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&info;
        UpdateDescriptors(1,&write);
    }
    if(grewGlyphTable || gatheredGlyphBounds.size()>1) {
        std::memcpy(glyphBounds.mapped,gatheredGlyphBounds.data(),static_cast<std::size_t>(glyphBytes));
    }
    vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,1,1,&frameDescriptor,0,nullptr);

    VkClearValue clear[2]{};clear[0].color.float32[3]=1;clear[1].depthStencil.depth=1;
    // Gather has finished resolving resident index spans and any capacity
    // fallback. Build indirect records from these final absolute ranges only.
    // The shared host-coherent allocation is reused after the normal fence.
    std::array<VkDeviceSize,15> indirectOffsets{};
    if(hasIndirectDraws) {
        nativeDrawCommands.clear();nativeDrawCommands.reserve(batches.size());
        for(std::size_t targetIndex=0;targetIndex<targetDraws.size();++targetIndex) {
            indirectOffsets[targetIndex]=static_cast<VkDeviceSize>(nativeDrawCommands.size())*sizeof(VkDrawIndexedIndirectCommand);
            for(const auto* packet:targetDraws[targetIndex])
                nativeDrawCommands.push_back({packet->count,1,packet->first,0,packet->nativeTransformIndex});
        }
        const auto bytes=static_cast<VkDeviceSize>(nativeDrawCommands.size())*sizeof(VkDrawIndexedIndirectCommand);
        if(bytes>indirectCommands.size) {
            VkNativeBuffer replacement;
            try {CreateBuffer(replacement,(bytes+65535u)&~VkDeviceSize(65535u),
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,hostFlags,true);}
            catch(...) {Destroy(replacement);throw;}
            Destroy(indirectCommands);indirectCommands=replacement;
        }
        if(bytes)std::memcpy(indirectCommands.mapped,nativeDrawCommands.data(),static_cast<std::size_t>(bytes));
    }
    // Complete resource creation and any recording invalidation BEFORE the
    // first secondary is linked into this primary. A later lane reset would
    // also invalidate the primary that already references those buffers.
    const auto mainFogPlan=reuseRecording && recordedCommandsValid ?
        recordedPasses[0].fog:PlanFogDepth(targetDraws[0]);
    if(mainFogPlan.present)CreateFogDepth(mainFogPlan.direct);
    if(reuseRecording && (!recordedCommandsValid || recordedBuffers!=RecordedBufferHandles() ||
        recordedFrameDescriptor!=frameDescriptor || recordedPipelineLayout!=pipelineLayout)) {
        ResetRecordingLanes();reuseRecording=false;
    }
    std::uint64_t executedDrawCommands{};
    std::set<std::thread::id> recordingThreads;
    const auto recordPass=[&](std::int32_t id) {
        const auto width=id ? renderedTargets.at(id).width:options.width;
        const auto height=id ? renderedTargets.at(id).height:options.height;
        const auto& packets=targetDraws[static_cast<std::size_t>(id)];
        const auto fogPlan=id==0 ? mainFogPlan:VkFogDepthPlan{};
        auto& recordedPass=recordedPasses[static_cast<std::size_t>(id)];
        auto& preparedPipelines=preparedTargetPipelines[static_cast<std::size_t>(id)];
        if(reuseRecording) {
            Require(recordedPass.packetCount==packets.size() && preparedPipelines.size()==packets.size(),
                "Retained recording lost its exact native packet range");
        } else {
            recordedPass.packetCount=packets.size();recordedPass.fog=fogPlan;
            preparedPipelines.clear();preparedPipelines.reserve(packets.size());
        }
        // Compile only the actually referenced complete pipeline states,
        // before entering the render pass. Depth sampling is the same exact
        // interval selected below; no speculative variants are manufactured.
        VkPipeline focusTapPipeline{};
        const auto pipelineLookupStarted=Clock::now();
        if(!reuseRecording)for(std::size_t index=0;index<packets.size();++index) {
            const auto& batch=*packets[index];
            // On effect entry there is no HUD-free previous scene yet. Seed
            // it below and omit just the first historical blend, not the effect.
            if(batch.textureFunction==11 || batch.textureFunction==12 || (batch.sampledPreviousFrame && batch.sampledTarget==0 &&
                !preparedFrame.useDisplayHistory)) {preparedPipelines.push_back(VK_NULL_HANDLE);continue;}
            const bool sample=fogPlan.present && fogPlan.direct && index>=fogPlan.first && index<=fogPlan.last;
            VkPipeline pipeline{};
            if(batch.cachedOpaqueScenery && !sample && samples==VK_SAMPLE_COUNT_1_BIT) {
                const unsigned variant=static_cast<unsigned>(batch.wireframe) |
                    (static_cast<unsigned>(batch.untextured)<<1) |
                    (static_cast<unsigned>(batch.clampTopLeft)<<2) |
                    (static_cast<unsigned>(batch.depthWrite)<<3) |
                    (static_cast<unsigned>(batch.alphaTest!=0)<<4);
                auto& cached=textures[batch.texture].scenery.pipelines[variant];
                if(cached)++statistics.sceneryPipelineHits;
                else cached=Pipeline(batch);
                pipeline=cached;
            } else pipeline=Pipeline(batch,sample);
            if(batch.textureFunction==8) {
                VkDrawBatch tap;tap.target=-1;tap.blend=0x01020200u;tap.depth=tap.depthWrite=false;
                tap.textureFunction=7;tap.postProcess=true;focusTapPipeline=Pipeline(tap);
                if(!focusTapPipeline)pipeline=VK_NULL_HANDLE;
            }
            if(!pipeline)++statistics.pendingPipelineDraws;
            preparedPipelines.push_back(pipeline);
        }
        statistics.renderProfile.pipelineLookupMilliseconds+=milliseconds(Clock::now()-pipelineLookupStarted);
        const auto recordRange=[&](VkCommandBuffer recordCommands,
            std::size_t firstPacket,std::size_t lastPacket)->std::uint64_t {
            VkBuffer activeVertexBuffer=VK_NULL_HANDLE,activeIndexBuffer=VK_NULL_HANDLE;
            VkPipeline activePipeline{};VkDescriptorSet activeDescriptor{};
            const VkDrawBatch* previous{};
            VkNativeDrawPush previousPush{};
            std::uint64_t drawsRecorded{};
            // Secondary command buffers inherit no draw state. Bind every
            // required set and dynamic state locally, including frame data.
            vkCmdBindDescriptorSets(recordCommands,VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipelineLayout,1,1,&frameDescriptor,0,nullptr);
            for(std::size_t packetIndex=firstPacket;packetIndex<lastPacket;++packetIndex) {
                const auto pipeline=preparedPipelines[packetIndex];
                if(!pipeline)continue;
                const auto& batch=*packets[packetIndex];
                const auto vertexBuffer=batch.residentGeometry ? residentVertices.buffer:vertices.buffer;
                Require(vertexBuffer!=VK_NULL_HANDLE,"Vulkan draw lost its selected vertex storage");
                if(vertexBuffer!=activeVertexBuffer) {
                    const VkDeviceSize offset=0;vkCmdBindVertexBuffers(recordCommands,0,1,&vertexBuffer,&offset);
                    activeVertexBuffer=vertexBuffer;

                }
                if(pipeline!=activePipeline) {
                    vkCmdBindPipeline(recordCommands,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
                    activePipeline=pipeline;
                }
                if(!previous || std::memcmp(&previous->viewport,&batch.viewport,sizeof(batch.viewport))) {
                    vkCmdSetViewport(recordCommands,0,1,&batch.viewport);
                }
                if(!previous || std::memcmp(&previous->scissor,&batch.scissor,sizeof(batch.scissor))) {
                    vkCmdSetScissor(recordCommands,0,1,&batch.scissor);
                }
                if(!previous || previous->blendConstant!=batch.blendConstant) {
                    const float constants[]={batch.blendConstant,batch.blendConstant,batch.blendConstant,batch.blendConstant};
                    vkCmdSetBlendConstants(recordCommands,constants);
                }
                VkDescriptorSet descriptor;
                if(batch.textureFunction==9)descriptor=renderedTargets.at(id).feedbackDescriptor;
                else if(batch.textureFunction==10)descriptor=renderedTargets.at(id).filterFeedbackDescriptor;
                else if(batch.textureFunction==5 || batch.textureFunction==6)
                    descriptor=fogPlan.direct ? fogSourceDescriptor:fogDepth.descriptor;
                else if(batch.textureFunction==8)descriptor=focusBlur.descriptor;
                else if(batch.sampledTarget<0)descriptor=textures[batch.texture].descriptor;
                else {
                    const auto& source=renderedTargets.at(batch.sampledTarget);
                    descriptor=batch.sampledPreviousFrame ? source.previousDescriptor:source.descriptor;
                }
                if(descriptor!=activeDescriptor) {
                    vkCmdBindDescriptorSets(recordCommands,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&descriptor,0,nullptr);
                    activeDescriptor=descriptor;
                }
                VkNativeDrawPush push{};
                push.function=batch.textureFunction;push.test=batch.alphaTest;
                push.scale=batch.lodScale;push.minification=batch.minification;
                if(batch.nativeIndirectTransform)push.nativePositionMode=2;
                else if(batch.nativeGpuPosition) {
                    push.localToClip=batch.nativeLocalToClip;
                    push.clipMapping=batch.nativeClipMapping;
                    push.nativePositionMode=1;
                    push.textureQScale=batch.nativeTextureQScale;
                push.shadingIndex=batch.nativeShadingIndex;
                }
                if(!previous || std::memcmp(&previousPush,&push,sizeof(push))) {
                    vkCmdPushConstants(recordCommands,pipelineLayout,VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_VERTEX_BIT,
                        0,sizeof(push),&push);
                    previousPush=push;
                }
                if(batch.indexed) {
                    const auto indexBuffer=batch.residentIndices ? residentIndices.buffer:indices.buffer;
                    Require(indexBuffer!=VK_NULL_HANDLE,"Vulkan draw lost its selected index storage");
                    if(indexBuffer!=activeIndexBuffer) {
                        vkCmdBindIndexBuffer(recordCommands,indexBuffer,0,VK_INDEX_TYPE_UINT32);activeIndexBuffer=indexBuffer;

                    }
                    std::uint32_t groupCount=1;
                    if(batch.nativeIndirectTransform) {
                        while(groupCount<properties.limits.maxDrawIndirectCount && packetIndex+groupCount<lastPacket &&
                            preparedPipelines[packetIndex+groupCount]==pipeline &&
                            packets[packetIndex+groupCount]->CanBatchIndirectly(batch))++groupCount;
                    }
                    if(groupCount>1) {
                        const auto offset=indirectOffsets[static_cast<std::size_t>(id)]+
                            static_cast<VkDeviceSize>(packetIndex)*sizeof(VkDrawIndexedIndirectCommand);
                        Require(offset+VkDeviceSize(groupCount)*sizeof(VkDrawIndexedIndirectCommand)<=indirectCommands.size,
                            "Native indirect draw exceeds its captured command buffer");
                        vkCmdDrawIndexedIndirect(recordCommands,indirectCommands.buffer,offset,groupCount,sizeof(VkDrawIndexedIndirectCommand));
                        packetIndex+=groupCount-1;
                    } else vkCmdDrawIndexed(recordCommands,batch.count,1,batch.first,0,
                        batch.nativeIndirectTransform ? batch.nativeTransformIndex:0);
                } else vkCmdDraw(recordCommands,batch.count,1,batch.first,0);
                ++drawsRecorded;
                previous=packets[packetIndex];
            }
            return drawsRecorded;
        };
        // The whole subpass uses one content mode, as required by Vulkan1.1.
        // Small runs stay inline; substantial targets use ordered secondary
        // buffers, including their short effect-adjacent runs.
        const bool secondary=reuseRecording ? recordedPass.secondary:
            options.parallelCommandRecording && statistics.availableCpuThreads>1 && packets.size()>=128;
        if(!reuseRecording)recordedPass.secondary=secondary;
        std::size_t segmentIndex{};
        const auto contents=secondary ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS:VK_SUBPASS_CONTENTS_INLINE;
        VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass=id==14 ? preserveRenderPass:!id && multisampleRenderPass ? multisampleRenderPass:renderPass;
        const auto sceneFramebuffer=id ? renderedTargets.at(id).framebuffer:framebuffer;
        pass.framebuffer=sceneFramebuffer;
        pass.renderArea={{0,0},{width,height}};pass.clearValueCount=2;pass.pClearValues=clear;
        vkCmdBeginRenderPass(commands,&pass,contents);
        bool copiedFogDepth=false,samplingDepth=false,passActive=true;
        std::uint32_t feedbackGroup{};
        const auto endPass=[&] {
            if(passActive) {vkCmdEndRenderPass(commands);passActive=false;}
        };
        const auto resume=[&] {
            pass.renderPass=samplingDepth ? depthSamplingRenderPass:
                !id && multisampleRenderPass ? resumeMultisampleRenderPass:resumeRenderPass;
            pass.framebuffer=samplingDepth ? depthSamplingFramebuffer:sceneFramebuffer;
            pass.clearValueCount=0;pass.pClearValues=nullptr;
            vkCmdBeginRenderPass(commands,&pass,contents);passActive=true;
        };
        const auto recordSegment=[&](std::size_t firstPacket,std::size_t lastPacket) {
            if(!secondary) {
                executedDrawCommands+=recordRange(commands,firstPacket,lastPacket);
                recordingThreads.insert(std::this_thread::get_id());
                return;
            }
            if(reuseRecording) {
                Require(segmentIndex<recordedPass.segments.size(),"Retained native pass lost a segment");
                const auto& segment=recordedPass.segments[segmentIndex++];
                Require(segment.first==firstPacket && segment.last==lastPacket &&
                    segment.renderPass==pass.renderPass && segment.framebuffer==pass.framebuffer &&
                    !segment.buffers.empty(),"Retained native secondary inheritance or range changed");
                Require(segment.buffers.size()<=UINT32_MAX-statistics.reusedCommandBuffers,
                    "Reused native command-buffer counter overflow");
                vkCmdExecuteCommands(commands,static_cast<std::uint32_t>(segment.buffers.size()),segment.buffers.data());
                executedDrawCommands+=segment.draws;
                statistics.reusedCommandBuffers+=static_cast<std::uint32_t>(segment.buffers.size());
                return;
            }
            const auto packetCount=lastPacket-firstPacket;
            // Granularity is a work threshold, not a CPU limit. A machine
            // with more available processors gets more useful chunks.
            const auto chunks=std::min<std::size_t>(statistics.availableCpuThreads,
                1+(packetCount-1)/64);
            auto buffers=AcquireRecordingBuffers(chunks);
            std::vector<std::uint64_t> drawCounts(chunks);
            std::vector<std::thread::id> threads(chunks);
            VkCommandBufferInheritanceInfo inheritance{};
            inheritance.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
            inheritance.renderPass=pass.renderPass;inheritance.subpass=0;
            inheritance.framebuffer=pass.framebuffer;
            RunNativeSceneJobs(chunks,[&](std::size_t chunk) {
                VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                // A completed secondary can be executed again. The real
                // frame fence and primary reset make simultaneous use unnecessary.
                begin.flags=VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
                begin.pInheritanceInfo=&inheritance;
                vkRequired(vkBeginCommandBuffer(buffers[chunk],&begin),"vkBeginCommandBuffer(recording)");
                const auto first=firstPacket+packetCount*chunk/chunks;
                const auto last=firstPacket+packetCount*(chunk+1)/chunks;
                drawCounts[chunk]=recordRange(buffers[chunk],first,last);
                threads[chunk]=std::this_thread::get_id();
                vkRequired(vkEndCommandBuffer(buffers[chunk]),"vkEndCommandBuffer(recording)");
            },2);
            // Completion is synchronous even on an exception: no borrowed
            // packet, descriptor or inheritance data can outlive this scope.
            vkCmdExecuteCommands(commands,static_cast<std::uint32_t>(buffers.size()),buffers.data());
            NativeRecordedSegment segment;
            segment.first=firstPacket;segment.last=lastPacket;
            segment.renderPass=pass.renderPass;segment.framebuffer=pass.framebuffer;
            for(std::size_t index=0;index<chunks;++index) {
                segment.draws+=drawCounts[index];recordingThreads.insert(threads[index]);
            }
            executedDrawCommands+=segment.draws;segment.buffers=std::move(buffers);
            recordedPass.segments.push_back(std::move(segment));
            statistics.commandRecordingChunks+=static_cast<std::uint32_t>(chunks);
        };
        bool capturedCleanScene=false;
        const auto boundary=[&](std::size_t index) {
            const auto& batch=*packets[index];
            return (samplingDepth && index>fogPlan.last) ||
                (batch.feedbackGroup && feedbackGroup!=batch.feedbackGroup) ||
                ((batch.textureFunction==5 || batch.textureFunction==6) && !copiedFogDepth) ||
                batch.textureFunction==8 || batch.textureFunction==11 || batch.textureFunction==12;
        };
        for(std::size_t packetIndex=0;packetIndex<packets.size();) {
            const auto& batch=*packets[packetIndex];
            if(batch.textureFunction==11 || batch.textureFunction==12) {
                Require(id==0,"Scene history boundary targets an offscreen image");
                endPass();
                if(samplingDepth) {SetFogDepthReadable(false);samplingDepth=false;}
                if(batch.textureFunction==12) {
                    CopyFeedback(0);capturedCleanScene=true;
                } else if(updatesDisplayHistory) {
                    Require(capturedCleanScene,"Camera history has no clean pre-blur scene");
                    // Publish the raw scene, not the history-composited output:
                    // fully opaque mask areas must never feed themselves back.
                    CopyHistory(0,renderedTargets.at(0).feedback.image,true);
                    ++statistics.renderProfile.historyCopies;
                }
                ++packetIndex;continue;
            }
            if(samplingDepth && packetIndex>fogPlan.last) {
                endPass();SetFogDepthReadable(false);samplingDepth=false;
            }
            if(batch.feedbackGroup && feedbackGroup!=batch.feedbackGroup) {
                endPass();CopyFeedback(id);feedbackGroup=batch.feedbackGroup;
            }
            if((batch.textureFunction==5 || batch.textureFunction==6) && !copiedFogDepth) {
                Require(id==0 && fogPlan.present,"Fog depth has no current main attachment");
                endPass();
                if(fogPlan.direct) {SetFogDepthReadable(true);samplingDepth=true;}
                else CopyFogDepth();
                copiedFogDepth=true;
            }
            if(batch.textureFunction==8 && preparedPipelines[packetIndex]) {
                if(reuseRecording) {
                    VkDrawBatch tap;tap.target=-1;tap.blend=0x01020200u;
                    tap.textureFunction=7;tap.postProcess=true;focusTapPipeline=Pipeline(tap);
                }
                Require(focusTapPipeline && id==0 && focusSource.descriptor && focusBlur.color.image,
                    "Focus composite lost its current scratch images");
                endPass();BlurFocus(batch,focusTapPipeline);
            }
            if(!passActive)resume();
            const auto first=packetIndex++;
            while(packetIndex<packets.size() && !boundary(packetIndex))++packetIndex;
            recordSegment(first,packetIndex);
        }
        if(reuseRecording && secondary)Require(segmentIndex==recordedPass.segments.size(),
            "Retained native pass has an unexecuted segment");
        endPass();
        if(samplingDepth)SetFogDepthReadable(false);
        if(id)Barrier(renderedTargets.at(id).color.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    };
    // Preserve packet order within each original target, and satisfy actual
    // image dependencies between targets. Previous-frame sampling is a
    // different image and must never introduce a current-frame feedback loop.
    std::array<std::uint8_t,15> visited{};
    if(sampleGpuTargets)
        vkCmdWriteTimestamp(commands,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestampQueries,timestampCount++);
    const auto visit=[&](const auto& self,std::int32_t id)->void {
        Require(id>=0 && id<static_cast<std::int32_t>(visited.size()),"Vulkan target graph lost its target range");
        // Upstream 136ac47d: do not blend the persistent environment image
        // again on render-only frames. Fresh pending faces may also be
        // captured between ticks when gameplay presentation is capped.
        if(id==14 && !environmentPolicy.remap)return;
        const auto index=static_cast<std::size_t>(id);
        if(visited[index]==2)return;
        Require(visited[index]!=1,"Original render-target graph has a current-frame sampling cycle");
        visited[index]=1;
        for(const auto dependency:targetDependencies[index])self(self,dependency);
        recordPass(id);visited[index]=2;
        if(sampleGpuTargets) {
            Require(timestampCount<timestampTargetIds.size(),"GPU target timing exceeds its query pool");
            timestampTargetIds[timestampCount]=id;
            vkCmdWriteTimestamp(commands,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestampQueries,timestampCount++);
        }
    };
    submissionCheckpoint(EcstasySubmitPhase::DrawPlan);
    const auto sceneRecordStarted=Clock::now();
    for(const auto id:destinations)visit(visit,id);
    statistics.renderProfile.recordSceneMilliseconds=std::max(0.0,
        milliseconds(Clock::now()-sceneRecordStarted)-statistics.renderProfile.pipelineLookupMilliseconds);
    submissionPhaseStarted=Clock::now();
    // FXAA samples the CURRENT image through the existing history descriptor,
    // so it still needs a same-frame transfer. Ordinary race frames do not.
    // Camera history has already been captured before the HUD; FXAA uses
    // its separate feedback image and cannot overwrite that history.
    const auto antialiasingPipeline=antialiasing.count?Pipeline(antialiasing):VK_NULL_HANDLE;
    if(antialiasing.count && !antialiasingPipeline)++statistics.pendingPipelineDraws;
    if(antialiasingPipeline) {
        CopyFeedback(0);

        VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass=postProcessRenderPass;pass.framebuffer=postProcessFramebuffer;
        pass.renderArea={{0,0},{options.width,options.height}};
        vkCmdBeginRenderPass(commands,&pass,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,antialiasingPipeline);
        vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,1,1,&frameDescriptor,0,nullptr);

        const VkDeviceSize offset=0;vkCmdBindVertexBuffers(commands,0,1,&vertices.buffer,&offset);
        vkCmdSetViewport(commands,0,1,&antialiasing.viewport);vkCmdSetScissor(commands,0,1,&antialiasing.scissor);

        const float constants[4]{};vkCmdSetBlendConstants(commands,constants);
        const auto descriptor=renderedTargets.at(0).feedbackDescriptor;
        vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&descriptor,0,nullptr);

        VkNativeDrawPush push{};
        push.function=antialiasing.textureFunction;push.test=antialiasing.alphaTest;
        push.scale=antialiasing.lodScale;push.minification=antialiasing.minification;
        vkCmdPushConstants(commands,pipelineLayout,VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_VERTEX_BIT,
            0,sizeof(push),&push);

        vkCmdDraw(commands,antialiasing.count,1,antialiasing.first,0);vkCmdEndRenderPass(commands);

    }
    if(swapchain) {
        Barrier(swapImages[swapIndex],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstSubresource=blit.srcSubresource;
        blit.srcOffsets[1]={static_cast<std::int32_t>(options.width),static_cast<std::int32_t>(options.height),1};
        const double presentationScale=std::min(static_cast<double>(presentationExtent.width)/options.width,
            static_cast<double>(presentationExtent.height)/options.height);
        const auto presentWidth=std::min(presentationExtent.width,static_cast<std::uint32_t>(std::lround(options.width*presentationScale)));
        const auto presentHeight=std::min(presentationExtent.height,static_cast<std::uint32_t>(std::lround(options.height*presentationScale)));
        const auto presentLeft=static_cast<std::int32_t>((presentationExtent.width-presentWidth)/2);
        const auto presentTop=static_cast<std::int32_t>((presentationExtent.height-presentHeight)/2);
        blit.dstOffsets[0]={presentLeft,presentTop,0};
        blit.dstOffsets[1]={presentLeft+static_cast<std::int32_t>(presentWidth),presentTop+static_cast<std::int32_t>(presentHeight),1};
        if(presentWidth!=presentationExtent.width || presentHeight!=presentationExtent.height) {
            const VkClearColorValue black{{0,0,0,1}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(commands,swapImages[swapIndex],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
            // The image-wide clear and the centered blit overlap. Preserve
            // their write order even when the driver executes transfers in
            // parallel; the completed black borders remain outside the blit.
            Barrier(swapImages[swapIndex],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        vkCmdBlitImage(commands,color.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,swapImages[swapIndex],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,
            presentWidth==options.width && presentHeight==options.height ? VK_FILTER_NEAREST:VK_FILTER_LINEAR);
        Barrier(swapImages[swapIndex],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    RecordDepthTransfers(nextDepthTransfers,preparedFrame.displayTranslation);
    if(timestampQueries)vkCmdWriteTimestamp(commands,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestampQueries,1);
    submissionCheckpoint(EcstasySubmitPhase::PostProcess);
    vkRequired(vkEndCommandBuffer(commands),"vkEndCommandBuffer");
    VkSubmitInfo submission{};submission.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submission.commandBufferCount=1;submission.pCommandBuffers=&commands;
    const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    if(swapchain) {
        submission.waitSemaphoreCount=1;submission.pWaitSemaphores=&acquired;submission.pWaitDstStageMask=&waitStage;
        submission.signalSemaphoreCount=1;submission.pSignalSemaphores=&presentationSemaphores[swapIndex];
    }
    vkRequired(vkResetFences(device,1,&fence),"vkResetFences");
    submissionCheckpoint(EcstasySubmitPhase::Finalize);
    const auto queueSubmitStarted=Clock::now();
    vkRequired(vkQueueSubmit(queue,1,&submission,fence),"vkQueueSubmit");
    statistics.renderProfile.queueSubmitMilliseconds=milliseconds(Clock::now()-queueSubmitStarted);
    statistics.submittedFrame=++nextSubmission;statistics.drawBatches=executedDrawCommands;
    statistics.commandRecordingThreads=static_cast<std::uint32_t>(recordingThreads.size());
    if(timestampQueries) {
        querySubmission=statistics.submittedFrame;
        submittedTimestampCount=timestampCount;
    }
    statistics.recordSubmitMilliseconds=std::max(0.0,milliseconds(Clock::now()-renderBegin)-
        statistics.gatherMilliseconds-statistics.fenceWaitMilliseconds);

    const auto frameRetirementStarted=Clock::now();
    const auto journalTicks=platform::TicksNow();
    if(platform::nativeDiagnosticsEnabled && (!journalWorkReported || journalTicks-journalWorkTicks>=platform::kTickerFrequency*5u)) {
        journalWorkReported=true;journalWorkTicks=journalTicks;
        std::uint64_t mainTriangles=0,mirrorTriangles=0,otherTriangles=0,lineIndices=0;
        std::size_t mainBatches=0,mirrorBatches=0,otherBatches=0;
        for(const auto& batch:batches) {
            auto& count=batch.target==0 ? mainBatches:batch.target==7 ? mirrorBatches:otherBatches;
            ++count;
            if(batch.wireframe)lineIndices+=batch.count;
            else {
                auto& triangles=batch.target==0 ? mainTriangles:batch.target==7 ? mirrorTriangles:otherTriangles;
                triangles+=batch.count/3u;
            }
        }
        char detail[1024]{};
        std::snprintf(detail,sizeof(detail),
            "gather=%llu extent=%ux%u samples=%u commands=%zu scene_batches=(%zu,%zu,%zu) "
            "scene_triangles=(%llu,%llu,%llu) line_indices=%llu "
            "transient_vertex_bytes=%llu transient_index_bytes=%llu resident_vertex_upload_bytes=%llu "
            "resident_index_upload_bytes=%llu resident_vertex_slots=%zu texture_keys=%zu resident_textures=%zu pipelines=%zu "
            "available_cpus=%u recording_threads=%u recording_chunks=%u native_draw_calls=%llu resident_validation_jobs=%zu prepared=%u secondary_reused=%u",
            static_cast<unsigned long long>(gatherSerial),options.width,options.height,static_cast<unsigned>(samples),
            preparedFrame.sourceCommands,mainBatches,mirrorBatches,otherBatches,
            static_cast<unsigned long long>(mainTriangles),static_cast<unsigned long long>(mirrorTriangles),
            static_cast<unsigned long long>(otherTriangles),static_cast<unsigned long long>(lineIndices),
            static_cast<unsigned long long>(vertexDataBytes),static_cast<unsigned long long>(indexBytes),
            static_cast<unsigned long long>(residentUploadBytes),static_cast<unsigned long long>(residentIndexUploadBytes),
            residentGeometry.Active().size(),preparedFrame.textureKeys,textures.size(),pipelines.size(),
            statistics.availableCpuThreads,statistics.commandRecordingThreads,statistics.commandRecordingChunks,
            static_cast<unsigned long long>(statistics.drawBatches),reuse ? std::size_t{0}:residentGeometry.PreparedValidationCount(),
            static_cast<unsigned>(reuse),static_cast<unsigned>(statistics.reusedCommandBuffers));
        platform::RecordSessionEvent("renderer.work",detail);
    }
    residentGeometry.PublishUploads(residentPendingUploads);
    residentIndexCache.PublishUploads(residentIndexPendingUploads);
    nativeSubmissionPending=true;
    if(frame)frame->MarkSubmitted();
    // The source frame no longer owns any pixels referenced by Vulkan: all
    // vertex/index data and texture uploads have been captured into native
    // Vulkan storage before QueueSubmit. Releasing source strips here allows
    // World/Frontend to prepare the next frame while this GPU frame runs.
    // Actual *GPU* buffer/texture retirement is still fence-gated above.
    if(frame)frame->Complete();
    if(updatesDisplayHistory)displaySceneHistoryValid=
        preparedFrame.sceneHistoryBoundary && historyReaders.display;
    hasFrame=true;preparedFrame.valid=true;
    recordedBuffers=RecordedBufferHandles();recordedFrameDescriptor=frameDescriptor;
    recordedPipelineLayout=pipelineLayout;recordedCommandsValid=statistics.pendingPipelineDraws==0;

    for(auto& entry:renderedTargets)entry.second.initialized=true;
    // Successful QueueSubmit publishes captured image content for the next
    // CPU frame. Actual reuse/replacement still waits for its GPU fence.
    // A Gather/recording failure leaves new entries invalid for a retry.
    for(auto& texture:textures)texture.uploadCompleted=true;
    statistics.renderProfile.frameRetirementMilliseconds=milliseconds(Clock::now()-frameRetirementStarted);

    if(swapchain) {
        VkPresentInfoKHR present{};present.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount=1;present.pWaitSemaphores=&presentationSemaphores[swapIndex];
        present.swapchainCount=1;present.pSwapchains=&swapchain;present.pImageIndices=&swapIndex;
        VkResult result;
        const auto presentBegin=Clock::now();
        result=vkQueuePresentKHR(queue,&present);
        statistics.presentMilliseconds=milliseconds(Clock::now()-presentBegin);
        statistics.presentAccepted=result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR;

        if(result!=VK_SUCCESS && result!=VK_SUBOPTIMAL_KHR && result!=VK_ERROR_OUT_OF_DATE_KHR)
            vkRequired(result,"vkQueuePresentKHR");
        // This is queue submission, not proof of a vertical interval.
        // Source-clock cadence is published from measured native QPC time.
        if(result==VK_SUBOPTIMAL_KHR || result==VK_ERROR_OUT_OF_DATE_KHR) {
            vkRequired(vkDeviceWaitIdle(device),"vkDeviceWaitIdle(swapchain)");CloseTargets();CreateTargets();
        }
    }
    return true;
}

EcstasyVulkanColorReadback EcstasyVulkanDevice::Storage::ReadColorTarget(std::int32_t targetId)
{
    Require(hasFrame && (targetId==0 || (targetId>=7 && targetId<=14)),
        "Colour readback requires a completed native render target");
    const auto found=renderedTargets.find(targetId);
    Require(found!=renderedTargets.end() && found->second.initialized,
        "Colour readback target has not been rendered");
    const auto& target=found->second;
    EcstasyVulkanColorReadback output;output.width=target.width;output.height=target.height;
    const VkDeviceSize byteCount=VkDeviceSize(output.width)*output.height*4u;
    Require(byteCount && byteCount<=std::numeric_limits<std::size_t>::max(),
        "Colour readback exceeds host address space");
    output.rgba.resize(static_cast<std::size_t>(byteCount));
    // This diagnostic is deliberately separate from Render and its fence,
    // timestamps, depth readback requests and queue-presentation accounting.
    // It has no allocations, copies or waits in a normal gameplay frame.
    vkRequired(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"vkWaitForFences(colour capture)");
    const auto source=targetId ? target.color.image:color.image;
    Require(source!=VK_NULL_HANDLE,"Colour readback lost its actual image");
    const auto retainedLayout=targetId ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkNativeBuffer staging;
    VkCommandBuffer copyCommands{};
    VkFence copyFence{};
    bool submitted{};
    const auto close=[&]() noexcept {
        if(submitted)(void)vkWaitForFences(device,1,&copyFence,VK_TRUE,UINT64_MAX);
        if(copyFence)vkDestroyFence(device,copyFence,nullptr);
        if(copyCommands)vkFreeCommandBuffers(device,commandPool,1,&copyCommands);
        Destroy(staging);
    };
    try {
        CreateBuffer(staging,byteCount,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,true);
        VkCommandBufferAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool=commandPool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
        vkRequired(vkAllocateCommandBuffers(device,&allocate,&copyCommands),"vkAllocateCommandBuffers(colour capture)");
        VkFenceCreateInfo createFence{};createFence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkRequired(vkCreateFence(device,&createFence,nullptr,&copyFence),"vkCreateFence(colour capture)");
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkRequired(vkBeginCommandBuffer(copyCommands,&begin),"vkBeginCommandBuffer(colour capture)");
        VkImageMemoryBarrier imageBarrier{};imageBarrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imageBarrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
        imageBarrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.oldLayout=retainedLayout;imageBarrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imageBarrier.srcQueueFamilyIndex=imageBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.image=source;imageBarrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(copyCommands,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,nullptr,0,nullptr,1,&imageBarrier);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageExtent={output.width,output.height,1};
        vkCmdCopyImageToBuffer(copyCommands,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,staging.buffer,1,&copy);
        imageBarrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.dstAccessMask=targetId ? VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;imageBarrier.newLayout=retainedLayout;
        VkBufferMemoryBarrier hostBarrier{};hostBarrier.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        hostBarrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;hostBarrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        hostBarrier.srcQueueFamilyIndex=hostBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        hostBarrier.buffer=staging.buffer;hostBarrier.size=byteCount;
        vkCmdPipelineBarrier(copyCommands,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT|(targetId ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT),
            0,0,nullptr,1,&hostBarrier,1,&imageBarrier);
        vkRequired(vkEndCommandBuffer(copyCommands),"vkEndCommandBuffer(colour capture)");
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount=1;submit.pCommandBuffers=&copyCommands;
        vkRequired(vkQueueSubmit(queue,1,&submit,copyFence),"vkQueueSubmit(colour capture)");submitted=true;
        vkRequired(vkWaitForFences(device,1,&copyFence,VK_TRUE,UINT64_MAX),"vkWaitForFences(colour copy)");
        submitted=false;
        std::memcpy(output.rgba.data(),staging.mapped,output.rgba.size());
    }catch(...) {close();throw;}
    close();return output;
}


EcstasyVulkanDevice::EcstasyVulkanDevice(const EcstasyVulkanOptions& options):storage_(std::make_unique<Storage>(options)){}
EcstasyVulkanDevice::~EcstasyVulkanDevice()=default;
void EcstasyVulkanDevice::Render(EcstasyFrame& frame)
{
    try {(void)storage_->Render(&frame);}
    catch(const std::exception& error) {
        platform::RecordSessionEvent("renderer.frame.failed",error.what());throw;
    } catch(...) {
        platform::RecordSessionEvent("renderer.frame.failed","Unknown native renderer failure");throw;
    }
}
bool EcstasyVulkanDevice::CanRenderPrepared() const noexcept
{
    return storage_->CanRenderPrepared();
}
bool EcstasyVulkanDevice::RenderPrepared()
{
    try {return storage_->Render(nullptr);}
    catch(const std::exception& error) {
        storage_->InvalidatePrepared();
        platform::RecordSessionEvent("renderer.frame.failed",error.what());throw;
    } catch(...) {
        storage_->InvalidatePrepared();
        platform::RecordSessionEvent("renderer.frame.failed","Unknown native renderer failure");throw;
    }
}
void EcstasyVulkanDevice::InvalidatePrepared() noexcept
{
    storage_->InvalidatePrepared();
}
void EcstasyVulkanDevice::Resize(std::uint32_t width,std::uint32_t height)
{
    Require(width && height,"Cannot resize a Vulkan target to zero");auto& s=*storage_;
    if(width==s.options.width && height==s.options.height)return;
    vkRequired(vkDeviceWaitIdle(s.device),"vkDeviceWaitIdle(resize)");s.CloseTargets();
    s.options.width=width;s.options.height=height;s.CreateTargets();
}
void EcstasyVulkanDevice::ConfigurePresentation(bool vsync)
{
    auto& s=*storage_;if(s.options.vsync==vsync)return;
    vkRequired(vkDeviceWaitIdle(s.device),"vkDeviceWaitIdle(presentation settings)");
    s.CloseTargets();s.options.vsync=vsync;s.CreateTargets();
}
void EcstasyVulkanDevice::SetLogicalSize(std::uint32_t width,std::uint32_t height)
{
    Require(width && height,"Logical Vulkan viewport dimensions are zero");
    if(width!=storage_->options.logicalWidth || height!=storage_->options.logicalHeight)
        storage_->InvalidatePrepared();
    storage_->options.logicalWidth=width;storage_->options.logicalHeight=height;
}
std::uint32_t EcstasyVulkanDevice::Width() const noexcept{return storage_->options.width;}
std::uint32_t EcstasyVulkanDevice::Height() const noexcept{return storage_->options.height;}
const EcstasyVulkanFrameStatistics& EcstasyVulkanDevice::LastFrameStatistics() const noexcept{return storage_->statistics;}
EcstasyVulkanColorReadback EcstasyVulkanDevice::ReadColorTarget(std::int32_t target)
{
    return storage_->ReadColorTarget(target);
}
}
#endif // HP2_ENABLE_VULKAN

/* Individually qualified coordinator and cinematic view owners. */
int32_t carViewCrossesNearPlane(const CarCinematicView* view,CarMovement* movement,
    uint16_t projectionAngle)
{
    float localToWorld[16],combined[16],minimum[4],maximum[4];
    const VehicleState* state=movement->bindings.state;
    memcpy(localToWorld,state->orientation,sizeof(localToWorld));
    memcpy(localToWorld+12,state->position,3*sizeof(float));localToWorld[15]=1.0f;
    vectorMatrixMultiply(combined,view->current.matrix,localToWorld);
    DriverPhysicsVehicle physics{};physics.movement=movement;
    driverCollisionBounds(&physics,minimum,maximum);
    const uint16_t referenceHalf=(uint16_t)(projectionAngle>>1);
    const float referenceTangent=finiteRatio(rotationSineAngle(referenceHalf),vectorCosineAngle(referenceHalf));
    const uint16_t viewHalf=(uint16_t)(view->current.fieldOfView>>1);
    const float viewTangent=finiteRatio(rotationSineAngle(viewHalf),vectorCosineAngle(viewHalf));
    const float plane=finiteRatio(view->current.nearClip*referenceTangent,viewTangent);
    float previous=0.0f;
    for (int corner=0;corner<8;++corner) {
        const float point[4]={corner&4?maximum[0]:minimum[0],corner&2?maximum[1]:minimum[1],
            corner&1?maximum[2]:minimum[2],0};
        float transformed[4];vectorMatrixTransformPoint(transformed,combined,point);
        const float distance=transformed[2]-plane;
        if (corner>0 && distance*previous<0.0f) return 1;
        previous=distance;
    }
    return 0;
}
