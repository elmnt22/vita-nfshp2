#ifndef NFSHP2_ECSTASY_HPP
#define NFSHP2_ECSTASY_HPP

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include "indep/inc/Timer.hpp"
#include "indep/inc/NativeRenderSettings.hpp"
#include <array>
#include <deque>
#include <memory>
#include <string>
struct WeatherQuery;
namespace hp2 { class ResourceRegistry; class EcstasyFrame; struct TextureInfo; }
typedef hp2::ResourceRegistry NativeResourceRegistry;
typedef hp2::EcstasyFrame NativeEcstasyFrame;
typedef hp2::TextureInfo NativeTextureInfo;
extern "C" {
#else
typedef struct NativeResourceRegistry NativeResourceRegistry;
typedef struct NativeEcstasyFrame NativeEcstasyFrame;
typedef struct NativeTextureInfo NativeTextureInfo;
#endif

typedef struct GameTime GameTime;
typedef struct GameSession GameSession;

struct CarFramePlayerState;
struct RaceParameters;
struct ScreenRadialBlur;
struct Car;
typedef struct CarCinematicBase CarCinematicBase;
struct CarCinematicMethods;

typedef struct CarCinematicViewFrame {
    float matrix[16];
    float eye[4];
    float forward[4];
    float target[4];
    float focusDistance;
    float minimumVisibility;
    float maximumVisibility;
    float nearClip;
    float farClip;
    uint16_t fieldOfView;
} CarCinematicViewFrame;

typedef struct CarCinematicView {
    CarCinematicViewFrame current;
    CarCinematicViewFrame previous;
    float eyeVelocity[4];
    float forwardVelocity[4];
    float targetVelocity[4];
    float focusVelocity;
    float minimumVisibilityVelocity;
    float maximumVisibilityVelocity;
    float nearClipVelocity;
    float farClipVelocity;
    uint16_t fieldOfViewVelocity;
    float elapsed;
    int32_t resetHistory;
    const struct Car* hiddenCar;
    int32_t visibilityFlags;
} CarCinematicView;

struct CarMovement;
int32_t carViewCrossesNearPlane(const CarCinematicView* view,struct CarMovement* movement,
    uint16_t projectionAngle);

/* Frame-derived eView data: native matrices, not a serialized console
 * object or a second camera pose. Valid until this screen's next frame. */
typedef struct EcstasyProjectionPair {
    float diagonal[3];
    float translation[4];
    float inverseDiagonal[3];
    float inverseTranslation[4];
} EcstasyProjectionPair;

typedef struct EcstasyViewMatrices {
    float worldToView[16];
    float worldToClip[16];
    float worldToScreen[16];
    float worldToScreenClip[16];
    float clipToScreen[16];
    EcstasyProjectionPair screenProjection;
    EcstasyProjectionPair nearProjection;
    EcstasyProjectionPair frustumProjection;
} EcstasyViewMatrices;

typedef struct EcstasyTargetContext {
    float worldToClip[16];
    EcstasyProjectionPair projection;
    float transposedWorldToView[16];
} EcstasyTargetContext;

typedef struct EcstasyRenderTarget {
    int32_t id;
    int32_t enabled;
    int32_t left,top,width,height;
    int32_t format;
    float horizontalScale,verticalScale;
    float fieldOfViewScale;
    int32_t opaqueOnly;
    const EcstasyViewMatrices* viewMatrices;
    EcstasyTargetContext contexts[3];
} EcstasyRenderTarget;

typedef struct EcstasyViewProjectionSettings {
    uint16_t referenceFieldOfView;
    uint16_t splitFieldOfView;
    int32_t horizontalResolution;
    float horizontalClipScale,verticalClipScale;
    float horizontalClipOffset,verticalClipOffset;
    /* Native reversed depth units, selected by target format. These are
     * numeric values, never IEEE754 words carrying graphics-register bits. */
    float depthRanges[2][6];
    /* Native square-pixel display adaptation. Zero retains the exact source
     * matrix inputs for source comparisons and independently owned views. */
    int32_t nativeAspectCorrection;
    int32_t nativeSourceVideoMode;
} EcstasyViewProjectionSettings;

typedef struct CarCinematicScreen {
    CarCinematicView* view;
    CarCinematicBase* first;
    CarCinematicBase* last;
    const NativeTextureInfo* radialBlurTexture;
    // PAL eView+6 suppresses the ordinary composition while NIS overrides
    // the cars. Its separate colour request is blackAndWhite below.
    uint8_t replayComposition;
    int32_t id;
    uint8_t enabled;
    // Native storage for PAL eView+74's per-view colour effect request.
    // This field is not a serialized PS2 structure/layout.
    int32_t blackAndWhite;
    float pixelScale;
    // Native viewport pixels and authored PAL focal pixels are distinct
    // units. Calculated together; this field is not part of a PS2 layout.
    float sourcePixelScale;
    float nearClip,farClip;
    float viewScale;
    float fieldOfViewDegrees;
    float forward[4];
    EcstasyRenderTarget* target;
    EcstasyRenderTarget* auxiliaryTarget;
    EcstasyViewMatrices matrices;
    uint8_t matricesCalculated;
} CarCinematicScreen;

typedef struct CarCinematicViewOverride {
    int32_t enabled;
    float matrix[16];
} CarCinematicViewOverride;

/* The canonical ten eView records share these same Camera and mover owners.
 * Source eInitViews binds the first four; the remaining six stay unbound.
 * Screen stacks, publication gates and the optional live matrix override
 * belong to this one service owner. Other cameras borrow the same views. */
typedef struct CarCinematicServices {
    CarCinematicScreen screens[10];
    CarCinematicViewOverride override;
    int32_t suppressPublication;
    int32_t suppressOverride;
    int32_t overrideWasUsed;
    int32_t presentationLayout;
    /* Original MaybeChangeViewMode has separate requested override and
     * applied layout cells. These are renderer state, not copied cameras. */
    int32_t presentationLayoutOverride;
    int32_t appliedPresentationLayout;
    const GameTime* clock;
    struct ScreenRadialBlur* radialBlur;
    const GameSession* session;
    CarCinematicScreen* currentScreen;
    EcstasyViewProjectionSettings projection;
    /* Source eFrameCounter32F644: one increment at the end of eDisplayFrame,
     * independent of simulation time, source refreshes and GPU timestamps. */
    uint32_t renderFrameCounter;
} CarCinematicServices;

typedef struct ScreenRadialBlur {
    CarCinematicServices* cameras;
    const NativeTextureInfo* texture;
    uint8_t firstAmount[4];
    uint8_t secondAmount[4];
} ScreenRadialBlur;
CarCinematicScreen* screenRadialBlurScreen(ScreenRadialBlur* radialBlur,int32_t seat);
void screenRadialBlurStartScreen(ScreenRadialBlur* radialBlur,CarCinematicScreen* screen);
void screenRadialBlurStopScreen(CarCinematicScreen* screen);
void screenRadialBlurStart(ScreenRadialBlur* radialBlur,int32_t seat);
void screenRadialBlurStop(ScreenRadialBlur* radialBlur,int32_t seat);
void screenRadialBlurSetAmounts(ScreenRadialBlur* radialBlur,int32_t seat,float first,float second);

struct CarCinematicBase {
    CarCinematicBase* previous;
    CarCinematicBase* next;
    CarCinematicServices* services;
    CarCinematicScreen* screen;
    CarCinematicView* view;
    struct CarFramePlayerState* player;
    int32_t kind;
    int32_t viewNumber;
    int32_t active;
    const struct Car* savedHiddenCar;
    int32_t savedVisibilityFlags;
    void* ownedObject;
    const struct CarCinematicMethods* methods;
};

typedef enum CarCinematicResult {
    CAR_CINEMATIC_OK,
    CAR_CINEMATIC_INVALID_BINDING,
    CAR_CINEMATIC_ALLOCATION_FAILURE,
    CAR_CINEMATIC_NO_ROAD_PATH,
    CAR_CINEMATIC_BACKEND_CAPACITY
} CarCinematicResult;

/* Native virtual dispatch keeps the actual allocated owner. It carries no
 * console addresses or packed object-layout assumptions. */
typedef struct CarCinematicMethods {
    void (*dispose)(void* owner);
    CarCinematicResult (*advance)(void* owner);
    CarCinematicResult (*render)(void* owner,const CarCinematicScreen* screen,NativeEcstasyFrame* frame);
    struct Car* (*getCar)(const void* owner);
} CarCinematicMethods;


void ecstasyViewInitialize(CarCinematicScreen* screen);
void ecstasyViewsBind(CarCinematicServices* services,CarCinematicView cameras[4],
    EcstasyRenderTarget targets[8]);
CarCinematicScreen* ecstasyViewAt(CarCinematicServices* services,int32_t index);
CarCinematicScreen* ecstasyViewCurrent(const CarCinematicServices* services);
void ecstasyViewSetCurrent(CarCinematicServices* services,CarCinematicScreen* screen);
CarCinematicView* ecstasyViewCurrentCamera(const CarCinematicServices* services);
void ecstasyViewAttachCameraMover(CarCinematicServices* services,
    CarCinematicScreen* screen,CarCinematicBase* camera);
void ecstasyViewDetachCameraMover(CarCinematicServices* services,
    CarCinematicScreen* screen,CarCinematicBase* camera);
void ecstasyViewsUpdateMode(CarCinematicServices* services);
void ecstasyViewsApplyMode(CarCinematicServices* services);
void ecstasyViewLocalToScreenClip(const CarCinematicScreen* screen,float output[16],
    const float localToWorld[16]);
void ecstasyViewLocalToClip(const CarCinematicScreen* screen,float output[16],
    const float localToWorld[16]);
void ecstasyViewBiasMatrixForZSorting(const CarCinematicScreen* screen,
    float matrix[16],float scale);
int32_t ecstasyViewPixelSize(const CarCinematicScreen* screen,float size,float distance);
int32_t ecstasyViewPositionPixelSize(const CarCinematicScreen* screen,
    const float position[3],float radius);
/* Native PC unit boundary: authored visibility/LOD thresholds consume source
 * pixels; raster placement continues to use the output viewport's scale. */
int32_t ecstasyViewSourcePositionPixelSize(const CarCinematicScreen* screen,
    const float position[3],float radius);
int32_t ecstasyViewBoundsPixelSize(const CarCinematicScreen* screen,
    const float first[3],const float second[3]);
void ecstasyViewScreenPosition(const CarCinematicScreen* screen,float output[3],
    const float position[3]);
float ecstasyViewScreenWidth(const CarCinematicScreen* screen);
float ecstasyViewScreenHeight(const CarCinematicScreen* screen);
const char* ecstasyViewIdName(int32_t id);
/* Inputs written by the original renderer startup. Resolution is supplied
 * by the actual window owner, with no guessed viewport fallback. */
void ecstasyViewProjectionInitialize(EcstasyViewProjectionSettings* settings,
    int32_t horizontalResolution);
CarCinematicResult ecstasyViewCalculateMatrices(CarCinematicServices* services,
    CarCinematicScreen* screen,float nearOverride,float farOverride,float clipFarOverride);

typedef struct PositionMarkerRegistry PositionMarkerRegistry;

/* One decoded34013 record. The serialized console links are replaced by
 * native links in this same resource-owned node; XYZ and keys are stored once. */
typedef struct PositionMarker {
    struct PositionMarker* next;
    struct PositionMarker* previous;
    uint32_t key;
    uint32_t groupKey;
    float position[3];
    uint32_t tags;
    PositionMarkerRegistry* registry;
    const uint8_t* authoredRecord;
} PositionMarker;

struct PositionMarkerRegistry {
    PositionMarker* first;
    PositionMarker* last;
};

void positionMarkersInitialize(PositionMarkerRegistry* registry);
int32_t positionMarkersLoad(PositionMarkerRegistry* registry,uint32_t chunkKind,
    PositionMarker* records,size_t count);
int32_t positionMarkersUnload(PositionMarkerRegistry* registry,uint32_t chunkKind,
    PositionMarker* records,size_t count);
const PositionMarker* positionMarkerFind(const PositionMarkerRegistry* registry,
    uint32_t key,uint32_t groupKey,const PositionMarker* previous);

typedef struct EditorMarkerTexture {
    uint32_t key;
    const uint8_t* authoredRecord;
    size_t authoredRecordSize;
    int32_t used;
} EditorMarkerTexture;

typedef struct EditorMarkerRigidAnimation {
    int16_t frameCount;
    int8_t frameDuration;
    uint32_t key;
    const float* matrices;
    const uint16_t* labels;
    size_t labelCount;
    int8_t playbackType;
} EditorMarkerRigidAnimation;

typedef struct EditorMarkerSolidGeometry EditorMarkerSolidGeometry;
struct EditorMarkerSolid;
typedef struct EditorMarkerVertexAnimation {
    uint32_t framesPerSecond;
    uint32_t count;
    const struct EditorMarkerSolid* frames[64];
} EditorMarkerVertexAnimation;
typedef struct EditorMarkerAnimationTree {
    int32_t nodeCount;
    const uint8_t* authoredNodes;
    size_t authoredBytes;
} EditorMarkerAnimationTree;
typedef struct EditorMarkerSolid {
    uint32_t key;
    const uint8_t* authoredGeometry;
    size_t authoredGeometrySize;
    float minimum[3];
    float maximum[3];
    const EditorMarkerRigidAnimation* rigidAnimation;
    float origin[3];
    /* Original eSolid+60 matrix, exposed by eModel::GetPivotMatrix
     * PAL10CA60. Wipers compose this authored pivot before rigid animation. */
    float positionMatrix[16];
    struct EditorMarkerSolidGeometry* geometry;
    uint16_t resourceFlags;
    uint16_t renderFlags;
    char name[24];
    /* eSolid+A0/A4 is the actual34007 tree, independent of position markers.
     * Its96-byte node records remain borrowed from the resource owner. */
    EditorMarkerAnimationTree animationTree;
    /* Authored 0x10 solid variants; the containing resource owns every
     * member and this immutable sequence for the same lifetime. */
    const EditorMarkerVertexAnimation* vertexAnimation;
} EditorMarkerSolid;

typedef struct EditorMarkerReplacement {
    uint32_t originalKey;
    uint32_t replacementKey;
    EditorMarkerTexture* texture;
    int8_t textureIndices[16];
} EditorMarkerReplacement;

typedef struct EditorMarkerModel EditorMarkerModel;
// Native, decoded PC light material. The source record is retained separately;
// no PS2 floating-point words or VU exponent instructions execute per vertex.
// The response tables contain a smooth CPU approximation of the native
// specular power curve over its physically meaningful [0.1, 1] domain.
enum class EditorMarkerLightMaterialDomain : std::uint8_t { Frontend,InGame };
typedef struct EditorMarkerLightMaterial {
    static constexpr std::size_t kNativeResponseSamples=1025;
    uint32_t key{};
    float coefficients[2][16]{};
    const uint8_t* authoredRecord{};
    std::array<std::array<float,kNativeResponseSamples>,2> nativeSpecularResponse{};
    bool nativeSpecularReady{};
    // PAL10A754..10A770 selects the authored FE/IG bank from InGameFlag.
    // Each decoded variant owns its precomputed response; both original
    // coefficient arrays stay intact and no material is rewritten per frame.
    EditorMarkerLightMaterialDomain nativeDomain{EditorMarkerLightMaterialDomain::Frontend};
    std::shared_ptr<const EditorMarkerLightMaterial> inGameVariant;
    const float* NativeCoefficients() const noexcept {
        return coefficients[nativeDomain==EditorMarkerLightMaterialDomain::InGame ? 1:0];
    }
} EditorMarkerLightMaterial;
typedef struct EditorMarkerLightMaterialCatalogue EditorMarkerLightMaterialCatalogue;

/* Geometry/texture storage belongs to the decoded asset owner. These live
 * handles retain the actual resource identities and replacement bindings for
 * the native renderer; no console render object or packet is fabricated. */
typedef struct EditorMarkerResources {
    const EditorMarkerSolid* solids;
    size_t solidCount;
    EditorMarkerTexture* textures;
    size_t textureCount;
    EditorMarkerModel* first;
    EditorMarkerModel* last;
    uint64_t bindingRevision;
    size_t missingTextures;
    struct EditorMarkerResources* nextCatalogue;
    struct EditorMarkerResources* catalogueRoot;
    int32_t modelsChanged;
    int32_t texturesChanged;
    int32_t fixupsBlocked;
    NativeResourceRegistry* resourceRegistry;
    EditorMarkerLightMaterialCatalogue* lightMaterialCatalogues;
    const EditorMarkerLightMaterial* defaultLightMaterial;
    struct EditorMarkerDuplicatedSolid* duplicatedSolids;
} EditorMarkerResources;

struct EditorMarkerModel {
    EditorMarkerModel* previous;
    EditorMarkerModel* next;
    EditorMarkerResources* resources;
    uint32_t key;
    const EditorMarkerSolid* solid;
    EditorMarkerReplacement* replacement;
    uint16_t rigidAnimationStart;
    int8_t replacementCount;
    int8_t detailLevel;
};

void editorMarkerSolidBounds(const EditorMarkerSolid* solid,float minimum[3],float maximum[3]);
void editorMarkerModelBounds(const EditorMarkerModel* model,float minimum[3],float maximum[3]);
/* 0=outside one common plane, 1=intersecting, 2=all eight corners inside. */
int32_t ecstasyClassifyBounds(const float minimum[3],const float maximum[3],const float matrix[16]);
// PC visibility distance only; physical radii, camera field of view, and original
// source pixels used for lighting/animation remain unchanged. sourceFocal=1
// selects the authored focal scale; 0 retains the WorldObject display scale.
int32_t ecstasyViewNativePositionPixelSize(const CarCinematicScreen* screen,
    const float position[3],float radius,int32_t sourceFocal);

void editorMarkerModelInitialize(EditorMarkerModel* model,EditorMarkerResources* resources,uint32_t key);
void editorMarkerModelSetReplacement(EditorMarkerModel* model,EditorMarkerReplacement* replacement);
void editorMarkerModelAttachReplacementTextures(EditorMarkerModel* model,
    EditorMarkerReplacement* replacements,int32_t count,int32_t detailLevel);
int32_t editorMarkerModelRenameTexture(EditorMarkerModel* model,uint32_t originalKey,uint32_t replacementKey);
void editorMarkerSolidReplaceLightMaterial(const EditorMarkerSolid* solid,uint32_t originalKey,
    const EditorMarkerLightMaterial* replacement);
void editorMarkerSolidReplaceLightMaterialKey(const EditorMarkerSolid* solid,EditorMarkerResources* resources,
    uint32_t originalKey,uint32_t replacementKey);
void editorMarkerModelReplaceLightMaterial(EditorMarkerModel* model,uint32_t originalKey,uint32_t replacementKey);
const EditorMarkerAnimationTree* editorMarkerModelAnimationTree(const EditorMarkerModel* model);
void editorMarkerModelReconnectSolid(EditorMarkerModel* model);
void editorMarkerModelReconnectTextureTable(EditorMarkerModel* model);
void editorMarkerModelApplyReplacementTextureTable(EditorMarkerModel* model);
void editorMarkerModelsReconnectAll(EditorMarkerResources* resources);
void editorMarkerModelDispose(EditorMarkerModel* model);
int32_t editorMarkerResourcesAttach(EditorMarkerResources* root,EditorMarkerResources* catalogue);
void editorMarkerResourcesDetach(EditorMarkerResources* catalogue);
void ecstasySolidChangeName(EditorMarkerSolid* solid,const char* name);
void ecstasyDuplicateSolid(EditorMarkerResources* resources,const char* sourceName,const char* newName);
void ecstasyDeleteAllDuplicatedSolids(EditorMarkerResources* resources);
int32_t ecstasyModelNumVertices(const EditorMarkerModel* model);
void ecstasySolidSetEnvironmentMapping(const EditorMarkerSolid* solid,uint32_t textureKey,int32_t enabled);
void ecstasyModelSetEnvironmentMapping(EditorMarkerModel* model,uint32_t textureKey,int32_t enabled);
struct BodyDamageModel;
typedef int32_t (*EcstasyDamageVertexFunction)(struct BodyDamageModel*,int32_t,float*,uint16_t*,float*,int32_t);
void ecstasyCallDamageVertexFunction(EditorMarkerModel* model,struct BodyDamageModel* damage,
    EcstasyDamageVertexFunction function);
const EditorMarkerRigidAnimation* editorMarkerModelRigidAnimation(const EditorMarkerModel* model);
void editorMarkerModelSetRigidAnimationFrame(EditorMarkerModel* model,int32_t frame,int32_t worldFrames);
int32_t editorMarkerModelRigidAnimationTime(const EditorMarkerModel* model,int32_t worldFrames);
void editorMarkerModelSetupVertexAnimation(EditorMarkerModel* model,int32_t worldFrames);
int32_t editorMarkerRigidAnimationMatrixAtFrame(const EditorMarkerRigidAnimation* animation,
    float output[16],int32_t frame);
int32_t editorMarkerRigidAnimationMatrixAtTime(const EditorMarkerRigidAnimation* animation,
    float output[16],int32_t time);


typedef struct CarReplayAnimationClip {
    uint32_t key;
    float minimum[4],maximum[4];
    const uint8_t* authoredRecord;
    size_t authoredRecordSize;
    uint16_t frameCount;
    uint16_t boneCount;
    int16_t* boneRotations;
    const float* framePositions;
} CarReplayAnimationClip;
typedef struct CarReplayAnimationCatalogue {
    const CarReplayAnimationClip* clips;
    size_t count;
    struct CarReplayAnimationCatalogue* next;
} CarReplayAnimationCatalogue;
typedef struct CarReplayAnimationRegistry CarReplayAnimationRegistry;
typedef struct CarReplayAnimationController {
    const CarReplayAnimationCatalogue* catalogue;
    const EditorMarkerModel* model;
    uint32_t primaryKey,secondaryKey;
    const CarReplayAnimationClip* primary;
    const CarReplayAnimationClip* secondary;
    float phase,secondaryPhase,blend;
    CarReplayAnimationRegistry* registry;
    struct CarReplayAnimationController* next;
    struct CarReplayAnimationController* previous;
} CarReplayAnimationController;
/* One loaded-clip list and one live controller list. The root catalogue is
 * an empty list anchor, not another copy of the authored clips. */
struct CarReplayAnimationRegistry {
    CarReplayAnimationCatalogue root;
    CarReplayAnimationController* first;
    CarReplayAnimationController* last;
    int32_t reconnectRequired;
};

const CarReplayAnimationClip* carReplayAnimationFind(const CarReplayAnimationCatalogue* catalogue,uint32_t key);
void carReplayAnimationSelect(CarReplayAnimationController* controller,uint32_t key,float phase);
void carReplayAnimationBounds(const CarReplayAnimationController* controller,float minimum[4],float maximum[4]);
void carReplayAnimationRegistryInitialize(CarReplayAnimationRegistry* registry);
int32_t carReplayAnimationInitialize(CarReplayAnimationRegistry* registry,
    CarReplayAnimationController* controller, const EditorMarkerModel* model);
int32_t carReplayAnimationUnregisterController(CarReplayAnimationController* controller);
void carReplayAnimationReconnect(CarReplayAnimationController* controller);
void carReplayAnimationReconnectAll(CarReplayAnimationRegistry* registry);
int32_t carReplayAnimationAppendCatalogue(CarReplayAnimationRegistry* registry,
    CarReplayAnimationCatalogue* catalogue);
int32_t carReplayAnimationRemoveCatalogue(CarReplayAnimationRegistry* registry,
    CarReplayAnimationCatalogue* catalogue);
void carReplayAnimationResourceEndFixup(CarReplayAnimationRegistry* registry);


#ifdef __cplusplus
}

#include <cstddef>
#include <cstdint>
#include <memory>
#include <array>
#include <vector>
#include <optional>
#include "indep/inc/eLight.hpp"

struct EditorMarkerResources;
struct EditorMarkerTexture;
struct CarReplayAnimationRegistry;

namespace hp2 {
class ChopperEffectData;
class TexturePack;
class TextureAnimPack;
class LightMaterialResources;
class LightSceneRegistry;
class SunResources;
class QueuedFileService;
namespace frontend { class FlailerResources; }

// The application borrows its one model/texture bank and animation list.
// ResourceLoader and texture reads use this same CPU refresh owner.
class ResourceRegistry final {
public:
    ResourceRegistry(EditorMarkerResources& models,CarReplayAnimationRegistry& animations,
        ChopperEffectData& effects,frontend::FlailerResources& flailer);
    ~ResourceRegistry();
    ResourceRegistry(const ResourceRegistry&)=delete;
    ResourceRegistry& operator=(const ResourceRegistry&)=delete;
    EditorMarkerResources& Models() noexcept;
    CarReplayAnimationRegistry& Animations() noexcept;
    ChopperEffectData& Effects() noexcept;
    frontend::FlailerResources& Flailer() noexcept;
    const EditorMarkerTexture* FindTexture(std::uint32_t key) const noexcept;
    // Actual loaded texture lookup: retained pixels plus required palette.
    // FindTexture is the separately named metadata-only catalogue query.
    EditorMarkerTexture* ResolveTexture(std::uint32_t key,bool fallback);
    const EditorMarkerLightMaterial* FindLightMaterial(std::uint32_t key) const noexcept;
    bool IsTextureAlive(const EditorMarkerTexture* texture) const noexcept;
    void UpdateLightTextures();
    const EditorMarkerTexture* LightTexture(std::size_t index) const;
    void ResourceRefresh();
    void RefreshSpecialTextures();
    static void CreateSpecialTexturePalette(std::uint8_t* destination,std::size_t colorCount);
    // Native lifetime boundary for aliases into an unregistering catalogue.
    // This clears retired borrows even while original model fixups are blocked.
    void RetireTextureCatalogue(const EditorMarkerResources& catalogue) noexcept;
    const EditorMarkerTexture* RearViewTexture(std::size_t index) const;
    const EditorMarkerTexture* SpecialTexture(std::size_t index) const;
    SunResources& Sun() noexcept;
    LightSceneRegistry& Lights() noexcept;
    void PostLoadFixup();
    void BindQueuedFiles(QueuedFileService& files);
    QueuedFileService* QueuedFiles() const noexcept;
private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
};

/* Resource bundles borrow one live position-marker list. Several car/track
 * files can be present at once; load order and duplicate queries use that
 * same registry, independently of the eSolid/eAnimationTree catalogues. */
class PositionMarkerResources final {
public:
    PositionMarkerResources(const std::vector<std::uint8_t>& bundle,
        PositionMarkerRegistry& registry);
    ~PositionMarkerResources();
    PositionMarkerResources(const PositionMarkerResources&) = delete;
    PositionMarkerResources& operator=(const PositionMarkerResources&) = delete;
    PositionMarkerResources(PositionMarkerResources&&) = delete;
    PositionMarkerResources& operator=(PositionMarkerResources&&) = delete;
    std::size_t Count() const noexcept;
    const PositionMarker* At(std::size_t index) const noexcept;
private:
    struct Block {
        std::unique_ptr<PositionMarker[]> records;
        std::size_t count=0;
    };
    PositionMarkerRegistry& registry_;
    std::vector<std::uint8_t> authored_;
    std::vector<Block> blocks_;
};

}
namespace hp2 {

/* This owns the authored geometry and texture records for the editor's
 * marker handles. The CPU catalogue does not emulate a console renderer. */
class TrackRouteEditorResources final {
public:
    explicit TrackRouteEditorResources(const std::vector<std::uint8_t>& ingame,
        EditorMarkerResources* registry=nullptr);
    ~TrackRouteEditorResources();
    TrackRouteEditorResources(const TrackRouteEditorResources&) = delete;
    TrackRouteEditorResources& operator=(const TrackRouteEditorResources&) = delete;
    TrackRouteEditorResources(TrackRouteEditorResources&&) = delete;
    TrackRouteEditorResources& operator=(TrackRouteEditorResources&&) = delete;
    EditorMarkerResources& Native() noexcept { return native_; }

private:
    struct RigidAnimationStorage {
        EditorMarkerRigidAnimation native{};
        std::vector<float> matrices;
        std::vector<std::uint16_t> labels;
    };
    std::vector<std::uint8_t> authored_;
    std::vector<std::unique_ptr<RigidAnimationStorage>> rigidAnimations_;
    std::vector<EditorMarkerSolid> solids_;
    std::vector<std::unique_ptr<EditorMarkerVertexAnimation>> vertexAnimations_;
    std::vector<std::unique_ptr<TexturePack>> texturePacks_;
    std::vector<std::unique_ptr<TextureAnimPack>> textureAnimationPacks_;
    std::unique_ptr<LightMaterialResources> lightMaterials_;
    std::vector<std::unique_ptr<EditorMarkerSolidGeometry>> geometry_;
    EditorMarkerResources native_{};
};

} // namespace hp2

// Source strip packets become native CPU vertices. Refresh and rendering
// share the live ST array and mip attributes; no console/GPU object exists.
struct EditorMarkerSolidGeometry {
    struct TextureBinding {
        std::uint32_t key{};
        const EditorMarkerTexture* texture{};
    };
    struct Strip {
        std::int8_t textureIndex{};
        std::int8_t lightMaterialIndex{-1};
        std::uint16_t flags{};
        std::array<std::uint16_t,2> textureScales{};
        std::int16_t autoKProjection{};
        std::uint16_t autoKAngle{};
        float autoKScale{};
        std::int16_t autoK{};
        std::uint8_t mipLevelScale{}; // TextureInfo+71: TEX1 L, not MXL(+6E).
        // Original XYZF2 ADC for each vertex: suppress the triangle ending
        // at that vertex, while retaining the strip's vertex FIFO/winding.
        std::uint64_t drawDisableMask{};
        std::array<float,3> minimum{},maximum{};
        std::array<std::int16_t,2> cullDirectionXY{};
        std::vector<std::array<float,3>> positions;
        // Source VIF signed V4_8 at TOPS+5C, column-packed four vertices.
        // Skinning changes XYZ only; PAL DoBlendingOnStrip retains normals.
        std::vector<std::array<float,3>> normals;
        std::vector<std::array<float,2>> textureCoordinates;
        std::vector<std::uint16_t> colors;
        // Native, immutable strip topology prepared when a model is decoded.
        // Indices are source vertex ordinals, never decoded PS2 packet words.
        // Preserves exact strip winding, ADC suppression and first-use order.
        std::vector<std::uint8_t> nativeUniqueOrder;
        std::vector<std::uint8_t> nativeTriangleOrder;
        std::size_t nativeTopologyCount{};
        std::uint64_t nativeTopologyMask{};
        std::uint64_t nativeVertexRevision{}; // Zero retains exact byte validation.
        void PrepareNativeTopology();
        void StampNativeVertexRevision() noexcept;
    };
    struct SharedTables {
        std::vector<TextureBinding> textures;
        std::vector<std::uint32_t> lightMaterialKeys;
        std::vector<const EditorMarkerLightMaterial*> lightMaterials;
    };
    // eDuplicateSolid copies vertex/strip storage but retains the original
    // texture/material tables. Their one owner is shared by all such solids.
    std::shared_ptr<SharedTables> tables{std::make_shared<SharedTables>()};
    std::vector<TextureBinding>& textures{tables->textures};
    std::vector<Strip> strips;
    std::vector<std::uint32_t>& lightMaterialKeys{tables->lightMaterialKeys};
    std::vector<const EditorMarkerLightMaterial*>& lightMaterials{tables->lightMaterials};
    std::int32_t vertexCount{};
    void FixTextureTable(hp2::ResourceRegistry& resources);
    void FixStripSTs();
    void CalcAutoK();
    void FixLightMaterialTable(hp2::ResourceRegistry& resources);
};

struct EditorMarkerLightMaterialCatalogue {
    std::vector<EditorMarkerLightMaterial> materials;
    EditorMarkerLightMaterialCatalogue* next{};
    EditorMarkerResources* registry{};
};

struct EcstasyVisibleStrip {
    std::size_t first{};
    std::uint16_t count{};
    std::int32_t clipClass{};
};

/* One Draw caller owns this scratch list. Its entries borrow the same
 * decoded solid strips; no duplicate vertices or model state are created. */
bool ecstasyDetermineVisibleStrips(std::vector<EcstasyVisibleStrip>& output,
    const EditorMarkerModel& model,const CarCinematicScreen& screen,
    const float localToWorld[16],bool forceVisible,std::int32_t mode);

namespace hp2 {
struct Chunk;
struct TextureInfo;
class EnvironmentMap;
class AcidFXRuntime;
namespace frontend { struct FlailerPoly; struct FlailerPolyVertex; struct FlailerStrip; }

// Native draw transport for the original eDisplayFrame/ePoly/eModel callers.
// Geometry, texture metadata and pixels retain their existing resource owners;
// a command records only the draw's arguments. Console DMA packets are absent.
// Complete original eSet/eGetScreenDisplayOffsets1056D8/105728. The
// per-mode renderer slots use source VCK/raster units, independently of the
// saved FRED calibration record. The caller supplies its actual game clock.
void eSetScreenDisplayOffsets(const GameTime& clock,std::int32_t x,std::int32_t y);
void eGetScreenDisplayOffsets(const GameTime& clock,std::int32_t& x,std::int32_t& y);

// Native projected particle transport. One contiguous group retains the
// original texture, average-depth ordering key and TL,TR,BL,BR primitive order.
// Coordinates are clip-space X/Y and D32 depth, never GS words carried as float.
struct EcstasyParticleQuad {
    std::array<std::array<float,2>,4> clipXY{};
    std::array<std::array<float,2>,4> uv{};
    float depth{};
    std::uint32_t color{};
};
// The same numeric depth interval is used by world draws and particles.
// Captured projection values preserve the original foreground layer headroom.
std::array<float,2> ecstasyNativeDepthRange(const float clipToScreen[16],std::int32_t format);
struct EcstasyFogSettings {
    std::int32_t enabled{1};
    std::array<std::int32_t,2> modes{};
    float power{4.0f};
};
extern EcstasyFogSettings fogSettings;
void CreateFogPalette(const WeatherQuery& weather,std::int32_t mode,float power,
    std::array<std::uint32_t,256>& output);
struct EcstasyFocusSettings {
    std::int32_t enabled{1},printPalette{};
    float alphaMin{},alphaMax{128.0f},focalDistance{},depthOfField{};
    std::int32_t transitionSlots{24};
    std::int32_t numPasses{2},jitterBlend{64};
    std::array<std::int32_t,5> maskBlend{{0,0,0,1,128}};
    std::array<std::int32_t,5> compositeBlend{{0,1,1,1,128}};
    // Native sampling displacement in source pixels; default aligns the
    // full-resolution composite. No GS origin or12.4 coordinate units.
    std::array<float,2> compositeOffsetPixels{};
};
extern EcstasyFocusSettings focusSettings;
bool CreateFocusPalette(const CarCinematicScreen* view,const EcstasyViewProjectionSettings& projection,
    std::array<std::uint32_t,256>* output);
struct EcstasyDepthPalettePass {
    std::array<std::uint32_t,256> palette{};
    std::uint32_t blend{};
    float blendConstant{};
};
struct EcstasyScreenFilter {
    std::array<std::uint8_t,4> color{{128,128,128,255}};
    std::array<std::uint8_t,4> blend{{1,0,0,0}};
    std::uint8_t enabled{},fixedAlpha{128},textureFunction{};
};
struct EcstasyNativeFeedbackRegion {
    std::array<float,4> viewport{}; // native x,y,width,height
    std::array<std::int32_t,4> scissor{}; // native left,top,width,height
    std::array<float,4> textureRectangle{}; // clipped original0..1 texture coordinates
};
EcstasyNativeFeedbackRegion ecstasyNativeFeedbackRegion(const EcstasyRenderTarget& target,
    std::uint32_t width,std::uint32_t height,std::uint32_t logicalWidth,std::uint32_t logicalHeight,
    const std::array<double,2>& translation);
enum class EcstasyDrawKind { FlailerPolygon,SolidStrip,WorldPolygon,RenderTargetPolygon,ParticleGroup,MirrorOverlayPolygon,WorldStrip,RenderTargetStrip,SolidReflectionStrip,ScreenFilterPolygon,SolidSpecularStrip,ScreenPolygon,FogPass,FocusMaskPass,DepthOfFieldPass,BlackAndWhitePass,DestinationFilterPass,SceneHistoryBoundary,SceneHistorySource };
// Explicit native presentation policy, not a PS2 packet/layout field.
// Ordinary authored HUD retains its established edge positions. Only an
// identified layout owner can request a centred proportional canvas.
enum class EcstasyUiPlacement : std::uint8_t { OriginalCanvas,CenteredCanvas };
enum EcstasyNativeRenderEggState : std::uint32_t {
    EcstasyRenderEggTexturing=1u,
    EcstasyRenderEggWireframe=2u,
    EcstasyRenderEggClampTopLeft=4u
};
std::uint32_t ecstasyCaptureNativeRenderEggState() noexcept;
enum class EcstasyCarShadow : std::uint8_t { None,Sun,Road };

// Frame-owned immutable snapshots survive sorting and parallel capture.
struct EcstasyDrawInstance {
    std::array<float,16> localToClip{};
    std::optional<SceneLightContext> lightContext;
};
struct EcstasyDrawProjection {
    std::array<std::array<float,16>,3> matrices{};
    EcstasyProjectionPair nearSource{},frustumSource{};
};

struct EcstasyDrawCommand {
    EcstasyDrawKind kind{EcstasyDrawKind::FlailerPolygon};
    EcstasyUiPlacement uiPlacement{EcstasyUiPlacement::OriginalCanvas};
    // PAL113A90 suspends the effect before HUD and113D7C resumes it.
    // Deferred Gather must consume the state at packet construction time.
    std::uint32_t nativeRenderEggState{ecstasyCaptureNativeRenderEggState()};
    const frontend::FlailerPoly* polygon{};
    const std::vector<EcstasyParticleQuad>* particleGroup{};
    const std::vector<frontend::FlailerPolyVertex>* worldStrip{};
    const EditorMarkerSolidGeometry::Strip* strip{};
    const EditorMarkerModel* model{};
    const TextureInfo* texture{};
    const TextureInfo* orderingTexture{};
    const EcstasyDepthPalettePass* depthPalette{};
    const EcstasyFocusSettings* focus{};
    // Value capture survives SuspendRenderEggs before deferred Gather.
    EcstasyScreenFilter screenFilter;
    std::uint32_t feedbackGroup{};
    const EditorMarkerLightMaterial* lightMaterial{};
    const EcstasyDrawInstance* instance{};
    const std::array<float,16>* clipToScreen{};
    const CarCinematicScreen* view{};
    const EcstasyRenderTarget* target{};
    // DisplayRVMs borrows the real source target; this is not a TextureInfo
    // resource or an image synthesized on the CPU. The device renders it.
    const EcstasyRenderTarget* sampledTarget{};
    bool sampledPreviousFrame{};
    // Native Vulkan implementation of the retail PAL Select Car env-map
    // source-alpha additive equation; only the frontend body map sets this.
    bool showcaseEnvironmentBlend{};
    bool cachedOpaqueScenery{};
    EcstasyCarShadow carShadow{};
    std::array<std::uint32_t,4> rasterState{};
    std::int32_t layer{};
    std::int32_t clipClass{};
    float alphaScale{1.0f};
    // Source projected Q is pixelScale/viewZ. Retain the issuing view's
    // scale because sky/world projections can change before native Gather.
    // Screen/direct-data packets instead carry Q in their own UVQ lane.
    float textureQScale{};
    std::uint32_t drawFlags{};
    bool depthSorted{};
    bool zWriteEnabled{true};
    float sortDepth{};
    // One original eAttachDirectData packet can contain several quads.
    // Its alpha key moves the entire packet, preserving its internal order.
    std::uint64_t alphaGroup{};
    std::array<std::uint8_t,4> vertexOrder{{0,1,2,3}};
    const SceneLightContext* LightContext() const noexcept {
        return instance && instance->lightContext ? &*instance->lightContext:nullptr;
    }
    const std::array<float,16>& LocalToClip() const noexcept {
        static constexpr std::array<float,16> empty{};
        return instance ? instance->localToClip:empty;
    }
    const std::array<float,16>& ClipToScreen() const noexcept {
        static constexpr std::array<float,16> empty{};
        return clipToScreen ? *clipToScreen:empty;
    }
};

// Native arithmetic adapter of the released normal/reflection/lighter path.
// Returns the original quantized GS color in128-unit shader coordinates.
std::array<float,4> EcstasyLitVertexColor(const SceneLightContext& context,
    const EditorMarkerLightMaterial& material,const float position[3],const float normal[3],
    std::uint16_t color,std::uint32_t flags);
struct EcstasyReflectionVertex {
    std::array<float,2> uv{};
    std::array<float,4> color{};
};
EcstasyReflectionVertex EcstasyReflectVertex(const SceneLightContext& context,
    const EditorMarkerLightMaterial& material,const float position[3],const float normal[3],
    std::uint16_t color,std::uint32_t flags);
// Released writable EnableEnvMap32F720; shared source car pixel-size gate.
extern std::int32_t environmentMappingEnabled;
// Released writable texture-scroll controls32F740..32F74C.
extern std::array<float,2> textureAnimationScrollSpeed;
extern std::int32_t textureAnimationScrollMode;
extern float textureAnimationScrollInterval;

// Native storage for the released writable render controls. This is a host
// policy, not a copy of any console structure or graphics-register packet.
struct EcstasyNativeRenderPolicy {
    std::int32_t texturing{1},wireframe{},drawSky{1},pixelClampOverride{};
    std::array<std::int32_t,5> reflectionBlend{{0,2,1,1,16}};
    std::array<EcstasyScreenFilter,2> screenFilters;
    std::int32_t blackAndWhite{};
};
extern EcstasyNativeRenderPolicy nativeRenderPolicy;

// Original eTransferZBufferRect arguments. The destination remains owned by
// the persistent SunZBuffers equivalent; the request carries no console DMA.
struct SunDepthSamples;
struct EcstasyDepthTransfer {
    // Native ownership of the actual source tile, retained across the
    // original deferred stage. Destroying an application cannot leave a
    // borrowed destination in a still-live presentation device.
    std::shared_ptr<SunDepthSamples> destination;
    std::int32_t x{},y{},width{},height{},targetWidth{},targetHeight{};
    std::int32_t targetLeft{},targetTop{};
    // Native pixels per retained source-grid sample. The destination remains
    // the original fixed-size tile, independent of the display resolution.
    float nativeSampleScale{1.0f};
    std::uint32_t maximumDepth{65535};
    std::array<float,16> clipToScreen{};
    // StuffSun consumes the preceding tile's bounds before publishing the
    // next rectangle. Keep those actual inputs for native preparation reuse.
    std::array<std::int32_t,4> consumedBounds{};
    bool consumedBoundsValid{};
};

struct OpaqueSceneryBatch {
    struct Strip {
        const EditorMarkerSolidGeometry::Strip* geometry{};
        const TextureInfo* texture{};
        std::uint32_t flags{};
    };
    const EditorMarkerSolidGeometry* geometry{};
    std::uint64_t textureRevision{};
    bool eligible{};
    std::vector<const EditorMarkerTexture*> bindings;
    std::vector<Strip> strips;
};

class EcstasyFrame final {
public:
    enum class Phase { Building,Ready,Submitted,Complete };
    EcstasyFrame();
    ~EcstasyFrame();
    EcstasyFrame(const EcstasyFrame&)=delete;
    EcstasyFrame& operator=(const EcstasyFrame&)=delete;
    const std::vector<EcstasyDrawCommand>& Commands() const noexcept {return commands_;}
    Phase State() const noexcept {return phase_;}
    std::size_t CapturedInstanceCount() const noexcept {return liveInstances_;}
    std::size_t CapturedProjectionCount() const noexcept {return liveProjections_;}
    // Capture original eSetZWriteState in the native packet stream. The
    // returned state and bool normalization match PAL1033C8.
    std::int32_t SetZWriteState(std::int32_t enabled);
    bool SetDashboardProjection(bool enabled);
    // Snapshot the source display environment for this submitted frame;
    // later calibration changes cannot change a frame already queued.
    void CaptureDisplayOffsets(const GameTime& clock);
    void SetPresentationAdvance(bool advance) noexcept {advancesPresentation_=advance;}
    bool AdvancesPresentation() const noexcept {return advancesPresentation_;}
    std::uint32_t CapturedEnvironmentFaces() const noexcept {return environmentCameraFaces_;}
    const std::array<double,2>& DisplayTranslation() const noexcept {return displayTranslation_;}
    const std::array<std::uint32_t,2>& SourceDisplayExtent() const noexcept {return sourceDisplayExtent_;}
    float TextureTime() const noexcept {return textureTime_;}
    void CaptureEnvironment(EnvironmentMap* environment,ResourceRegistry& resources,
        std::int32_t applicationState,std::uint32_t renderFrameCounter,bool inGame=false);
    const float* EnvironmentMatrix() const noexcept;
    SceneLightContext SetupWorldLightContext(ResourceRegistry& resources,const CarCinematicScreen& view) const;
    std::uint32_t RenderFrameCounter() const noexcept {return renderFrameCounter_;}
    std::int32_t ApplicationState() const noexcept {return environmentApplicationState_;}
    // PAL118150/118498: frame-owned native strips sample the actual rendered
    // face image, with the original target14 coordinates and ALPHA context.
    void RenderTargetStrip(const std::vector<frontend::FlailerPolyVertex>& vertices,
        const EcstasyRenderTarget& source,const EcstasyRenderTarget& destination);
    // PAL113FD8 follows draw-context construction. The deferred PC endpoint
    // advances the same textures only after this frame has consumed them.
    void AdvanceTextureAnimationsAfterCompletion(ResourceRegistry& resources,const GameTime& clock);
    // PAL10EDC8 DisplayRVMs: view3's actual target, original unscaled quad,
    // horizontal UV reversal and target MaxZ. Native packet ownership only.
    void DisplayRVMs(const CarCinematicServices& views,const CarCinematicScreen& destination);
    void DisplayRadialBlur(const CarCinematicServices& views);
    // PAL113900/11A9F0 and113A44/10F6A8: stage-ordered current colour
    // consumers, independent of the retained previous-frame image.
    void AttachBlackAndWhite(const CarCinematicServices& views);
    void DisplayFilter(const CarCinematicServices& views,ResourceRegistry& resources);
    void DisplayBlendPrevious(const CarCinematicServices& views);
    void MarkSceneHistoryBoundary(const EcstasyRenderTarget& display,bool source=false);
    // PAL1138AC..1138D4 / eAttachFog119768: current scene depth, before
    // history filters and HUD. These palettes contain values, no resources.
    void AttachFog(const CarCinematicServices& views);
    // PAL119F70: focus masks on player views, then the two-tap half-size
    // image and destination-alpha composite. Camera values remain original.
    void AttachDepthOfField(const CarCinematicServices& views);
    // PAL10EC70: authored RVM and RVM_MASK on the real rear render target.
    void OverlayRVMImages(const CarCinematicServices& views,ResourceRegistry& resources);
    // PAL10BB20 emits only the solid list. Alpha packets remain pending
    // until PAL10B838; scenery and remap flushes must not expose particles
    // before the later car solids. Sorting retains its source target/key.
    void AttachSolidPolys();
    void AttachAlphaPolys();

    // Source eAttachDirectData110378: one texture and average group depth.
    // The native packet owns copies of all projected vertices until completion.
    void AttachParticleGroup(const std::vector<EcstasyParticleQuad>& quads,
        const EditorMarkerTexture& texture,const CarCinematicScreen& view,float averageDepth);
    void AddPolygon(const frontend::FlailerPoly& polygon,const EditorMarkerTexture& texture,
        const CarCinematicScreen& view,std::int32_t layer,float alphaScale,
        EcstasyUiPlacement placement=EcstasyUiPlacement::OriginalCanvas);
    // The same released ePoly::Render2D boundary with direct source Z and
    // optional literal ALPHA. Immediate caller vertices are copied once.
    void RenderScreenPolygon(const frontend::FlailerPoly& polygon,const TextureInfo* texture,
        const CarCinematicScreen& view);
    void TransferZBufferRect(std::shared_ptr<SunDepthSamples> destination,
        const CarCinematicScreen& view,std::int32_t x,std::int32_t y,std::int32_t width,std::int32_t height,
        float nativeSampleScale=1.0f,const std::array<std::int32_t,4>* consumedBounds=nullptr);
    const std::vector<EcstasyDepthTransfer>& DepthTransfers() const noexcept {return depthTransfers_;}
    // Full PAL10FF88 ePoly::Render3D. Source stack polygons/matrices are
    // copied into frame-owned transport, just as its original DMA packet.
    void RenderPolygon3D(const frontend::FlailerPoly& polygon,const TextureInfo* texture,
        const float localToWorld[16],const CarCinematicScreen& view);
    // Released exBegin/EndStrip110848/110878 and eBegin/EndStrip110488/110608.
    void RenderWorldStrip(const std::vector<frontend::FlailerPolyVertex>& vertices,
        const TextureInfo& texture,const float localToWorld[16],const CarCinematicScreen& view,
        EcstasyCarShadow shadow=EcstasyCarShadow::None);
    void RecordCarShadowCapture(double sunMilliseconds,double roadMilliseconds) noexcept {
        carShadowCaptureMilliseconds_[0]+=sunMilliseconds;
        carShadowCaptureMilliseconds_[1]+=roadMilliseconds;
    }
    const std::array<double,2>& CarShadowCaptureMilliseconds() const noexcept {return carShadowCaptureMilliseconds_;}
    // Native packet transport for PAL eModel::Render/RenderStrips. The
    // existing resource owner supplies decoded XYZ/ST/RGB555; no geometry
    // cache or texture fallback is manufactured by a rendering call.
    std::size_t RenderModel(EditorMarkerModel& model,const float localToWorld[16],
        const CarCinematicScreen& view,std::uint32_t flags=0,float alphaScale=1.0f,
        std::int32_t worldFrames=0,const SceneLightContext* lightContext=nullptr,
        const std::vector<std::array<float,16>>* boneMatrices=nullptr);
    // Static opaque scenery uses ordered bindings and GPU frustum clipping.
    bool RenderOpaqueSceneryBatch(OpaqueSceneryBatch& batch,const EditorMarkerModel& model,
        const float localToWorld[16],const CarCinematicScreen& view,const SceneLightContext* lightContext);
    std::uint64_t ReusedOpaqueSceneryStrips() const noexcept {return reusedOpaqueSceneryStrips_;}
    void RenderStrip(const EditorMarkerModel& model,const EditorMarkerSolidGeometry::Strip& strip,
        const TextureInfo& texture,const EditorMarkerLightMaterial* material,
        const float localToWorld[16],const CarCinematicScreen& view,
        std::int32_t clipClass,std::uint32_t flags,float alphaScale=1.0f,
        const SceneLightContext* lightContext=nullptr);
    // Transfer one already-retained manager submission; no second retain.
    void RetireStripAfterCompletion(frontend::FlailerStrip& strip);
    void Seal();
    void MarkSubmitted();
    // The graphics owner calls this after synchronous CPU capture. Source
    // borrowers retire here; GPU allocations retain their own fence lifetime.
    // This does not claim GPU completion or presentation.
    void Complete();
    void CancelBeforeSubmit();
    // Reuse only completed native packet capacity. Every retained borrower
    // has already been released by Complete/CancelBeforeSubmit; no submitted
    // frame, captured source state or texture-animation callback is reused.
    void BeginNextFrame();
private:
    // Only the issuing owner reads writable frame/render policy. Preparation
    // borrows immutable geometry and projection inputs until its synchronous
    // join, and writes commands/posed strips into a separate ordered chunk.
    std::uint64_t reusedOpaqueSceneryStrips_{};
    std::array<double,2> carShadowCaptureMilliseconds_{};
    const EcstasyDrawInstance& CaptureInstance(const float localToWorld[16],
        const CarCinematicScreen& view,const SceneLightContext* lightContext=nullptr);
    const EcstasyDrawProjection& CaptureProjection(const CarCinematicScreen& view);
    struct StripCaptureState;
    StripCaptureState CaptureStripState() const;
    static void CaptureStrip(std::vector<EcstasyDrawCommand>& output,
        std::uint64_t& alphaGroups,const StripCaptureState& state,
        const EditorMarkerModel& model,const EditorMarkerSolidGeometry::Strip& strip,
        const TextureInfo& texture,const EditorMarkerLightMaterial* material,
        const float localToWorld[16],const CarCinematicScreen& view,
        std::int32_t clipClass,std::uint32_t flags,float alphaScale,
        const SceneLightContext* lightContext);
    struct ModelStripSource {std::size_t index{};std::int32_t clipClass{};};
    struct ModelCaptureChunk {
        std::vector<EcstasyDrawCommand> commands;
        std::vector<std::unique_ptr<EditorMarkerSolidGeometry::Strip>> animatedStrips;
        std::uint64_t alphaGroups{};
    };
    void Release() noexcept;
    std::vector<EcstasyDrawCommand> commands_;
    // Deque growth preserves references. Value-only slots are reused after
    // every source command and preparation worker has released its borrow.
    std::deque<EcstasyDrawInstance> capturedInstances_;
    std::deque<EcstasyDrawProjection> capturedProjections_;
    std::size_t liveInstances_{},liveProjections_{};
    const EcstasyDrawProjection* lastProjection_{};
    std::uint32_t nextFeedbackGroup_{};
    std::vector<EcstasyDrawCommand> pendingAlphaCommands_;
    // Native temporary capacity only. Neither scratch list survives a call
    // with live elements, so it cannot retain a track/texture borrower.
    std::vector<EcstasyDrawCommand> orderingScratch_;
    std::vector<EcstasyVisibleStrip> visibleScratch_;
    std::vector<ModelStripSource> modelStripScratch_;
    std::vector<const TextureInfo*> modelTextureScratch_;
    std::vector<ModelCaptureChunk> modelCaptureScratch_;
    std::vector<EcstasyDepthTransfer> depthTransfers_;
    std::vector<frontend::FlailerStrip*> retainedStrips_;
    std::vector<std::unique_ptr<frontend::FlailerPoly>> worldPolygons_;
    // Capacity only survives completion. The next frame overwrites each used
    // group before publication, and no resource borrower lives in these quads.
    std::vector<std::unique_ptr<std::vector<EcstasyParticleQuad>>> particleGroups_;
    std::size_t liveParticleGroups_{};
    std::vector<std::unique_ptr<std::vector<frontend::FlailerPolyVertex>>> worldStrips_;
    std::vector<std::unique_ptr<EditorMarkerSolidGeometry::Strip>> animatedStrips_;
    std::array<EcstasyDepthPalettePass,2> fogPasses_{},focusPasses_{};
    EcstasyFocusSettings focusSettings_{};
    Phase phase_{Phase::Building};
    bool zWriteEnabled_{true};
    bool dashboardProjection_{};
    bool inGameMaterials_{};
    std::size_t pendingAlphaBegin_{};
    std::uint64_t nextAlphaGroup_{};
    std::array<double,2> displayTranslation_{};
    std::array<std::uint32_t,2> sourceDisplayExtent_{};
    float textureTime_{};
    EnvironmentMap* environment_{};
    std::uint32_t environmentCameraFaces_{};
    const TextureInfo* frontendEnvironmentTexture_{};
    const TextureInfo* specularFalloffTexture_{};
    std::int32_t environmentApplicationState_{};
    std::uint32_t renderFrameCounter_{};
    bool environmentUpdated_{};
    bool advancesPresentation_{true};
    ResourceRegistry* textureAnimationResources_{};
    std::int32_t textureAnimationWorldFrames_{},textureAnimationRealFrames_{};
};

// Native Vulkan submission policy. A previous-frame image is only needed if
// a *current* source draw actually samples that image. In particular the main
// 1920x1080 color copy is not required for ordinary (non-FXAA) race frames.
// The policy is derived from the real sealed draw list, not a guess about
// track/world state; it is also testable without a physical GPU.
// A persistent reflection image is blended once per new camera capture,
// including captures between logical ticks under a presentation cap.
inline constexpr bool EcstasyAdvanceEnvironmentImages(bool hasPrevious,bool freshFrame,
    bool logicalTick,std::uint32_t capturedFaces) noexcept {
    return !hasPrevious || (freshFrame && (logicalTick || capturedFaces!=0));
}
struct EcstasyEnvironmentImagePolicy {
    bool snapshotPrevious{},remap{};
};
inline constexpr EcstasyEnvironmentImagePolicy EcstasyEnvironmentHistoryPolicy(
    bool hasPrevious,bool freshFrame,bool logicalTick,std::uint32_t capturedFaces) noexcept {
    // Snapshot the latest completed map for every fresh source draw list.
    // A render-only capture can sample history even without new camera faces.
    // Retained replays keep their original snapshot and never re-blend it.
    return {freshFrame,EcstasyAdvanceEnvironmentImages(hasPrevious,freshFrame,logicalTick,capturedFaces)};
}
struct EcstasyNativeHistoryReaders {
    bool display{};
    bool environment{};
};
inline EcstasyNativeHistoryReaders EcstasyFindNativeHistoryReaders(
    const std::vector<EcstasyDrawCommand>& draws) noexcept
{
    EcstasyNativeHistoryReaders result;
    for(const auto& draw:draws) {
        if(!draw.sampledPreviousFrame || !draw.sampledTarget)continue;
        // Targets 0..6 share the display image; target 14 is the only
        // environment history image published by the renderer.
        const auto id=draw.sampledTarget->id;
        if(id<7)result.display=true;
        if(id==14)result.environment=true;
        if(result.display && result.environment)break;
    }
    return result;
}

// Released eAnimation::Render/PoseMatrixRecursive: frame-owned native draw
// data, using the actual resource tree and the supplied quaternion table.
extern std::int32_t nBoneScale;
extern float vBoneScale[4];
std::size_t RenderAnimationPose(CarReplayAnimationController& animation,
    const float localToWorld[16],const float rootTranslation[3],
    const float rotations[][4],std::size_t rotationCount,const SceneLightContext* lightContext,
    const CarCinematicScreen& view,EcstasyFrame& frame,std::uint32_t flags=0,
    std::vector<std::array<float,16>>* posedMatrices=nullptr);

namespace frontend {
struct FlailerUiState;
struct FlailerScreenList;
struct GameFlowTrackResources;
struct ApplicationFrontendFlow;
struct FrontendFrame;
struct FlailerRenderManager;
}

struct EcstasySceneCaptureProfile {
    // Setup, background, world, particles, solids, alpha/mirrors, effects, HUD.
    std::array<double,8> milliseconds{};
    std::uint64_t emittedCaptures{},discardedCaptures{};
};

// eDisplayFrame borrows the canonical application/track owners. The frame
// contains submissions only; simulation, cameras and resource lifetimes
// stay with their original owners.
struct EcstasySceneBindings {
    CarCinematicServices& views;
    frontend::FlailerUiState& ui;
    frontend::GameFlowTrackResources* track{};
    frontend::ApplicationFrontendFlow* flow{};
    const ::RaceParameters* parameters{};
    const std::int32_t* applicationState{};
    const GameTime* clock{};
    frontend::FrontendFrame* frontendFrame{};
    frontend::FlailerScreenList* screens{};
    EnvironmentMap* environmentMap{};
    AcidFXRuntime* effects{};
    // Additional native presentation captures keep the same logical state.
    // Original callers retain the one-update/one-capture default.
    bool advancePresentation{true};
    // The application defers these post-capture mutations until the next
    // logical update, after every presenter has released the prior borrow.
    bool deferPresentationUpdates{};
    bool emitRaceGeometry{true};
    double* carCaptureMilliseconds{};
    platform::NativeDurationStatistics* sceneryCaptureTiming{};
    platform::NativeDurationStatistics* sceneryFrameTiming{};
    EcstasySceneCaptureProfile* captureProfile{};
};
void EcstasyDisplayFrame(const EcstasySceneBindings& scene,EcstasyFrame& frame);
// Independent native UI overlay. It has no authored screen/debug object and
// is rendered regardless of applicationDebugToolsEnabled/Player debug flags.
void EcstasyDisplayFrameRate(frontend::FlailerResources& resources,
    frontend::FlailerRenderManager& render,const CarCinematicScreen& view,EcstasyFrame& frame);

// CPU durations refer to the submitted frame. GPU duration refers only to
// the explicitly identified completed submission, obtained after its fence.
// Unsupported timestamp queues leave gpuTimeAvailable false.
enum class EcstasySubmitPhase : std::size_t {
    Preflight,Acquire,Reclaim,Textures,Targets,Upload,DrawPlan,PostProcess,Finalize,Count
};
struct EcstasyVulkanRenderProfile {
    std::array<double,static_cast<std::size_t>(EcstasySubmitPhase::Count)> submitPhaseMilliseconds{};
    // Scenery cache candidates, other solids, particles, world primitives, UI, effects.
    std::array<double,6> gatherCategoryMilliseconds{};
    std::array<std::uint64_t,6> gatherCategoryCommands{};
    std::uint64_t gatherCategorySamples{},coldVertexJobs{},coldVertices{};
    double gatherVertexJobsMilliseconds{},gatherIndicesMilliseconds{};
    double frameRetirementMilliseconds{}; // After the existing record/submit interval.
    std::uint64_t historyFrames{},historyCopies{},historySeeds{},historyFramesWithoutTick{};

    double gatherValidationMilliseconds{},gatherFinishMilliseconds{};
    double pipelineLookupMilliseconds{},recordSceneMilliseconds{},queueSubmitMilliseconds{};
    std::array<std::uint64_t,2> shadowStrips{},shadowTriangles{};
    std::array<double,2> shadowGatherMilliseconds{};
    std::uint64_t shadowGatherSamples{};
};

struct EcstasyVulkanGpuProfile {
    double setupMilliseconds{},mainMilliseconds{},mirrorMilliseconds{},auxMilliseconds{},tailMilliseconds{};
    bool available{};
};

struct EcstasyVulkanFrameStatistics {
    double gatherMilliseconds{},fenceWaitMilliseconds{},recordSubmitMilliseconds{},presentMilliseconds{};
    double gpuMilliseconds{};
    EcstasyVulkanRenderProfile renderProfile;
    EcstasyVulkanGpuProfile gpuProfile;
    std::uint64_t submittedFrame{},completedGpuFrame{};
    std::uint64_t textureValidations{},textureUploads{},textureUploadBytes{},textureStagingAllocations{};
    std::uint64_t pipelineCreations{},pendingPipelineDraws{},drawBatches{},particles{},gpuWorldVertices{};
    std::uint64_t sceneryPipelineHits{};
    std::uint64_t sceneryPacketHits{},sceneryPacketMisses{},sceneryPacketFallbacks{};
    // Actual CPU budget and distinct threads that recorded draw commands.
    // These are work diagnostics, not an estimate of physical GPU speed.
    std::uint32_t availableCpuThreads{},commandRecordingThreads{},commandRecordingChunks{};
    // Actually executed retained secondary buffers; these did not record again.
    std::uint32_t reusedCommandBuffers{};
    bool gpuTimeAvailable{};
    // Queue acceptance, not monitor scan-out. Offscreen rendering has no
    // presentation target; OUT_OF_DATE never counts as an accepted present.
    bool presentationTarget{},presentAccepted{};
    // Native preparation was reused, but this frame was recorded/submitted.
    bool reusedPreparation{};
};

#ifdef HP2_ENABLE_VULKAN
// Physical graphics transport in the individually proven EcstasyE.cpp owner.
// Handles and upload allocations belong to this device; authored resources
// and gameplay state remain in their existing owners. Zero ELF-body credit.
enum class EcstasyTextureFilter { Authored,Bilinear,Trilinear,Anisotropic };
// Native final-image antialiasing and multisample coverage modes.
enum class EcstasyAntialiasing { Off=0,Fxaa=1,Msaa2=2,Msaa4=3,Msaa8=4 };
struct EcstasyVulkanOptions {
    std::uint32_t width{1280},height{720};
    std::uint32_t logicalWidth{644},logicalHeight{480};
    bool vsync{true};
    void* nativeInstance{};
    void* nativeWindow{};
    // Native PC quality controls. Authored preserves the released TEX1
    // path for source comparisons; the desktop launcher chooses its default.
    EcstasyTextureFilter textureFilter{EcstasyTextureFilter::Authored};
    EcstasyAntialiasing antialiasing{EcstasyAntialiasing::Off};
    bool fixedRenderExtent{};
    // Headless correctness fixtures may explicitly use a CPU Vulkan ICD.
    // Normal play requires an actual graphics device and never silently
    // substitutes software rasterization for hardware rendering.
    bool allowSoftwareDevice{false};
    // Maintainer serial/parallel parity control. Normal play automatically
    // partitions substantial draw runs across the available CPU budget.
    bool parallelCommandRecording{true};
    // Skip a draw until its exact pipeline has compiled on the worker.
    bool asyncPipelines{};
    std::string pipelineCachePath;
    // Optional immutable startup seed; storage must survive device creation.
    const void* pipelineCacheSeedData{};
    std::size_t pipelineCacheSeedSize{};
    // Opt-in transport experiment for maintainer parity/performance controls.
    // The measured candidate did not improve total CPU time, so normal play
    // keeps the direct path. Launcher and authored quality stay unchanged.
    bool streamTransformBatching{false};
    std::uint32_t anisotropy{16};
};
struct EcstasyVulkanColorReadback {
    std::uint32_t width{},height{};
    std::vector<std::uint8_t> rgba;
};
class EcstasyVulkanDevice final {
public:
    explicit EcstasyVulkanDevice(const EcstasyVulkanOptions& options);
    ~EcstasyVulkanDevice();
    EcstasyVulkanDevice(const EcstasyVulkanDevice&)=delete;
    EcstasyVulkanDevice& operator=(const EcstasyVulkanDevice&)=delete;
    void Render(EcstasyFrame& frame);
    // The host must prove that source/presentation inputs have not advanced.
    // This reuses owned native preparation, then renders and submits a real
    // new GPU frame. It never re-presents an old colour image or advances time.
    bool CanRenderPrepared() const noexcept;
    bool RenderPrepared();
    void InvalidatePrepared() noexcept;
    void Resize(std::uint32_t width,std::uint32_t height);
    void ConfigurePresentation(bool vsync);
    void SetLogicalSize(std::uint32_t width,std::uint32_t height);
    std::uint32_t Width() const noexcept;
    std::uint32_t Height() const noexcept;
    const EcstasyVulkanFrameStatistics& LastFrameStatistics() const noexcept;
    // Explicit diagnostics only. This waits for the submitted image and copies
    // its actual RGBA pixels; normal Render never reads colour back to the CPU.
    EcstasyVulkanColorReadback ReadColorTarget(std::int32_t target=0);
private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
};
#endif

// PC-only quality adaptation for the original 256x64 rear-view target. The
// PS2 game/view/mask and 160..352 display polygon retain exact PAL geometry.
// This typed rectangle changes physical presentation quality only; it is not
// a recovered PAL function and does not affect camera or collision logic.
struct EcstasyNativeMirrorPresentation {
    float left{},top{},right{},bottom{};
    std::uint32_t rasterWidth{},rasterHeight{};
};
EcstasyNativeMirrorPresentation ecstasyNativeMirrorPresentation(
    std::uint32_t targetWidth,std::uint32_t targetHeight,
    std::uint32_t sourceWidth,std::uint32_t sourceHeight,
    std::uint32_t physicalWidth,std::uint32_t physicalHeight,
    std::uint32_t maximumImageDimension);

// Native raster bound for the original display RVM quad. The original PAL
// vertices and UVs stay unchanged, but a GPU scissor prevents sampled rear
// camera texels from appearing outside their authored screen region.
struct EcstasyNativeMirrorScissor {std::int32_t left{},top{},right{},bottom{};};
EcstasyNativeMirrorScissor ecstasyNativeMirrorScissor(
    double viewportX,double viewportY,double viewportWidth,double viewportHeight,
    std::uint32_t sourceWidth,std::uint32_t sourceHeight,
    std::uint32_t outputWidth,std::uint32_t outputHeight,
    double translationX=0.0,double translationY=0.0);

// PC-only transparent-edge correction: PAL OverlayRVMImages draws x=1..257
// into the 256-wide target, whereas DisplayRVMs samples mirrored U=0..1.
// The unmasked source column x=0 is therefore visible at the RIGHT edge.
// Vulkan presentation remaps only the sampled UV at that edge, keeping
// original screen quad, mask, frame textures and camera unchanged.
float ecstasyNativeMirrorMaskedSampleU(float originalU) noexcept;
// The RVM_MASK's authored alpha gate (GS 128-level alpha) is used in the
// final native composite, rejecting transparent border interpolation.
std::int32_t ecstasyNativeMirrorCompositeAlphaTest() noexcept;

// Native CPU parameters consumed by sun/flare rendering. Their C++ layout
// is independent of the serialized34202 record and console render packets.
struct SunFlareParameters {
    std::int32_t kind{};
    std::int32_t enabled{};
    float scale{};
    float size{};
    std::array<float,2> offset{};
    std::array<std::uint8_t,4> color{};
    std::int32_t angle{};
    float parameter20{};
};
struct SunDepthSamples {
    std::array<std::uint32_t,1024> values{};
    std::array<std::int32_t,4> bounds{}; // left,right,top,bottom; exclusive ends.
};
// Exact observable consumed by StuffSun's retained-depth loop. A changed
// nonclear depth cannot alter its opacity; a changed clear-sample count can.
// Invalid nonempty bounds reject reuse and retain normal source validation.
inline bool EcstasyNativeSunVisibleSampleCount(const SunDepthSamples& samples,
    std::uint32_t& count) noexcept
{
    count=0;const auto& bounds=samples.bounds;
    if(bounds[0]>=bounds[1] || bounds[2]>=bounds[3])return true;
    if(bounds[0]<0 || bounds[1]>32 || bounds[2]<0 || bounds[3]>32)return false;
    for(auto y=bounds[2];y<bounds[3];++y)for(auto x=bounds[0];x<bounds[1];++x)
        if((samples.values[static_cast<std::size_t>(y*32+x)]&0x00ffffffu)<2u)++count;
    return true;
}
struct SunGlareState {
    std::array<float,2> positionOverride{};
    float distance{1.0f};
    std::int32_t start{},reset{},timer{},phase{};
};

class SunResources final {
public:
    SunResources();
    SunResources(const SunResources&)=delete;
    SunResources& operator=(const SunResources&)=delete;
    SunResources(SunResources&&)=delete;
    SunResources& operator=(SunResources&&)=delete;
    bool LoadChunk(const std::vector<std::uint8_t>& owner,const Chunk& chunk);
    void Unload() noexcept;
    void LoadTrack(ResourceRegistry& resources);
    void UnloadTrack() noexcept;
    const EditorMarkerTexture* TrackTexture(std::size_t index) const;
    const std::array<float,3>& Vector() const noexcept {return vector_;}
    const std::array<SunFlareParameters,6>& Flares() const noexcept {return flares_;}
    std::int32_t Mode() const noexcept {return mode_;}
    const std::uint8_t* Info() const noexcept {return info_;}
    std::size_t InfoSize() const noexcept {return infoSize_;}
    void StuffSun(const CarCinematicScreen& view,EcstasyFrame& frame,const GameTime& clock,bool inGame);
    void StuffSunGlare(const CarCinematicScreen& view,EcstasyFrame& frame,const GameTime& clock);
    SunGlareState& Glare() noexcept {return glare_;}
    const SunGlareState& Glare() const noexcept {return glare_;}
    SunDepthSamples& DepthSamples(std::size_t target) {return depthSamples_->at(target);}
    const SunDepthSamples& DepthSamples(std::size_t target) const {return depthSamples_->at(target);}
    std::shared_ptr<SunDepthSamples> RetainDepthSamples(std::size_t target) const {
        return std::shared_ptr<SunDepthSamples>(depthSamples_,&depthSamples_->at(target));
    }
private:
    const std::uint8_t* info_{};
    std::size_t infoSize_{};
    std::array<float,3> vector_{};
    std::array<SunFlareParameters,6> flares_{};
    std::int32_t mode_{};
    std::array<EditorMarkerTexture*,5> trackTextures_{};
    SunGlareState glare_;
    // PAL2E03D0:15 fixed zero-initialized tiles are application-owned,
    // independent of track resource loading and its borrowed SunInfo.
    std::shared_ptr<std::array<SunDepthSamples,15>> depthSamples_;
};
}

struct Scenery;
namespace hp2 {
// One native owner of the two source SkyModel objects and their original
// placement matrices. Their geometry and textures remain resource-owned.
class SkyResources final {
public:
    SkyResources(EditorMarkerResources& models,Scenery& scenery);
    ~SkyResources();
    SkyResources(const SkyResources&)=delete;
    SkyResources& operator=(const SkyResources&)=delete;
    const EditorMarkerModel& Model(std::size_t index) const;
    const float* Transform(std::size_t index) const;
    void StuffSky(const CarCinematicScreen& view,EcstasyFrame& frame,std::int32_t worldFrames,
        bool emitGeometry=true) const;
private:
    std::array<EditorMarkerModel,2> models_{};
    std::array<std::array<float,16>,2> transforms_{};
};
}

namespace hp2 {
/* Retains the original authored model/texture and animation records. This
 * resource adapter does not implement the renderer or skeletal playback. */
class CarReplayAnimationResources final {
public:
    explicit CarReplayAnimationResources(const std::vector<std::uint8_t>& bundle,
        CarReplayAnimationRegistry* registry = nullptr,EditorMarkerResources* modelRoot = nullptr,
        bool ownModelCatalogue = true);
    ~CarReplayAnimationResources();
    CarReplayAnimationResources(const CarReplayAnimationResources&) = delete;
    CarReplayAnimationResources& operator=(const CarReplayAnimationResources&) = delete;
    CarReplayAnimationResources(CarReplayAnimationResources&&) = delete;
    CarReplayAnimationResources& operator=(CarReplayAnimationResources&&) = delete;
    const CarReplayAnimationCatalogue& Animations() const noexcept { return catalogue_; }
    EditorMarkerResources& Models() noexcept { return *modelView_; }
    bool Register(CarReplayAnimationRegistry& registry) noexcept;
    void Unregister() noexcept;
private:
    std::vector<std::uint8_t> authored_;
    std::unique_ptr<TrackRouteEditorResources> models_;
    EditorMarkerResources* modelView_{};
    /* These decoded buffers are the sole CPU pose streams. Retained authored
     * bytes stay read-only; preprocessing and playback use these same buffers. */
    std::vector<std::vector<std::int16_t>> rotations_;
    std::vector<std::vector<float>> positions_;
    std::vector<CarReplayAnimationClip> clips_;
    CarReplayAnimationCatalogue catalogue_{};
    CarReplayAnimationRegistry* registry_{};
};
}


// Native transport for the original graphics eVBlankCounter. It remains
// distinct from PSX2VBlank's platform cell and GameTime loop counters. The
// host publishes measured 50/59.94 Hz source intervals; this does not signal
// GPU completion or assert physical monitor presentation.
std::uint32_t graphicsVBlankCount() noexcept;
void graphicsNotifyPresentationComplete() noexcept;

// Native PC rendering support kept at the individually observed Ecstasy
// source boundary. These are host adaptations, not additional PAL bodies.
#include <algorithm>
#include <atomic>
#include <cfenv>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP>=1)
#include <xmmintrin.h>
#endif
#include <vector>
#include "indep/inc/eMath.hpp"

namespace hp2 {
// Use the nearest scenery envelope for the high-detail guard. XY distance
// avoids distant vertical extents, and entry/exit hysteresis reduces popping.
// Beyond the guard, authored screen-space thresholds select existing meshes.



inline std::int32_t nativeDetailLodPixels(std::int32_t originalPixels,
    float distanceFactor) noexcept
{
    if(originalPixels <= 0 || !std::isfinite(distanceFactor) || distanceFactor <= 1.0f)
        return originalPixels;
    const double scaled = static_cast<double>(originalPixels) * distanceFactor;
    if(scaled >= static_cast<double>(std::numeric_limits<std::int32_t>::max()))
        return std::numeric_limits<std::int32_t>::max();
    return static_cast<std::int32_t>(scaled);
}
}

namespace hp2 {
// Native Vulkan clip-space transform for an authored solid vertex. This is
// ordinary PC matrix math, NOT a PS2/VU interpreter. Keeping the same eMath
// multiplication and float/double evaluation order preserves the established
// sequential Gather image while disjoint vertices execute on native CPU cores.
inline std::array<float,4> NativeSolidClipPosition(
    const float matrix[16],const float input[3],
    const std::array<double,2>& scale,const std::array<double,2>& offset,
    bool translated)
{
    std::array<float,4> result{};
    eMulVector3(result.data(),matrix,input);
    result[2]=(result[2]+result[3])*0.5f;
    if(translated) {
        result[0]=static_cast<float>(result[0]*scale[0]+result[3]*offset[0]);
        result[1]=static_cast<float>(result[1]*scale[1]+result[3]*offset[1]);
    }
    return result;
}
} // namespace hp2

namespace hp2 {
// Native PC Vulkan push-constant ABI, not a PlayStation packet or its
// hardware register layout. The first 16 bytes preserve fragment state;
// a solid draw additionally supplies its model->clip matrix to the GPU.
struct NativeVulkanDrawPush {
    std::int32_t function{},test{};
    float scale{1.0f};
    std::int32_t minification{};
    std::array<float,16> localToClip{};
    std::array<float,4> clipMapping{{1.0f,1.0f,0.0f,0.0f}};
    std::int32_t nativePositionMode{};
    float textureQScale{};
    std::uint32_t shadingIndex{};
};
static_assert(offsetof(NativeVulkanDrawPush,localToClip)==16 &&
    offsetof(NativeVulkanDrawPush,clipMapping)==80 &&
    offsetof(NativeVulkanDrawPush,nativePositionMode)==96 &&
    offsetof(NativeVulkanDrawPush,textureQScale)==100 &&
    offsetof(NativeVulkanDrawPush,shadingIndex)==104 &&
    sizeof(NativeVulkanDrawPush)==108,
    "Native Vulkan draw push-constant ABI differs from the compiled vertex shader");
} // namespace hp2

namespace hp2 {
// Discover the processors available to this process/thread, including OS
// affinity restrictions. Work is bounded by this count and the useful jobs,
// never by a fixed application-wide limit on the number of worker threads.
unsigned NativeSceneEffectiveHardwareConcurrency() noexcept;
#ifdef __SWITCH__
void ConfigureNativeSceneWorker() noexcept;
#else
inline void ConfigureNativeSceneWorker() noexcept {}
#endif

inline bool& NativeSceneExecutingJobs() noexcept {
    static thread_local bool active=false;
    return active;
}

struct NativeSceneFloatingPointState {
    std::fenv_t environment{};
    bool valid{};
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP>=1)
    unsigned mxcsr{};
#endif
    NativeSceneFloatingPointState() noexcept { Capture(); }
    void Capture() noexcept {
        valid=std::fegetenv(&environment)==0;
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP>=1)
        mxcsr=_mm_getcsr();
#endif
    }
    void Apply() const noexcept {
        if(valid)std::fesetenv(&environment);
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP>=1)
        _mm_setcsr(mxcsr);
#endif
    }
};

// Render workers persist for the invoking thread's lifetime. The caller also
// drains jobs. A nested dispatch executes inline in its existing worker:
// it cannot create an oversubscribed per-worker pool or re-enter an active
// batch. Every worker drains before borrowed scene data can be unwound.
class NativeSceneWorkerPool final {
public:
    NativeSceneWorkerPool()=default;
    ~NativeSceneWorkerPool() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stopping_=true;
        }
        wake_.notify_all();
        for(auto& worker:workers_)if(worker.joinable())worker.join();
    }
    NativeSceneWorkerPool(const NativeSceneWorkerPool&)=delete;
    NativeSceneWorkerPool& operator=(const NativeSceneWorkerPool&)=delete;

    template<class Task>
    void Run(std::size_t count,Task& task,unsigned workers) {
        if(!count)return;
        workers=static_cast<unsigned>(std::min<std::size_t>(count,std::max(1u,workers)));
        if(workers<2 || NativeSceneExecutingJobs()) {
            for(std::size_t index=0;index<count;++index)task(index);
            return;
        }
        // thread_local ownership prevents simultaneous outside callers.
        // A C-style trampoline avoids allocating std::function per frame.
        // Grow only when a genuinely parallel workload needs more workers.
        // Existing sleeping workers never access the vector. If creation
        // throws, this already-constructed owner still joins every thread.
        while(workers_.size()+1<workers) {
            const auto slot=static_cast<unsigned>(workers_.size());
            workers_.emplace_back([this,slot] { Worker(slot); });
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            function_=[](void* taskContext,std::size_t index) {
                (*static_cast<Task*>(taskContext))(index);
            };
            context_=&task;
            floatingPoint_.Capture();
            size_=count;
            const auto shares=static_cast<std::size_t>(workers)*8;
            chunk_=std::max<std::size_t>(1,std::min<std::size_t>(32,count/shares+(count%shares!=0)));
            next_.store(0,std::memory_order_relaxed);
            canceled_.store(false,std::memory_order_relaxed);
            failure_=nullptr;
            active_=std::min<unsigned>(workers-1,static_cast<unsigned>(workers_.size()));
            remaining_=active_;
            ++generation_;
        }
        wake_.notify_all();
        Execute();
        std::exception_ptr failure;
        {
            std::unique_lock<std::mutex> guard(mutex_);
            completed_.wait(guard,[this] {return remaining_==0;});
            failure=failure_;
            function_=nullptr;
            context_=nullptr;
        }
        if(failure)std::rethrow_exception(failure);
    }
private:
    void Execute() noexcept {
        struct ExecutionScope {
            bool previous{NativeSceneExecutingJobs()};
            NativeSceneFloatingPointState previousFloatingPoint;
            explicit ExecutionScope(const NativeSceneFloatingPointState& floatingPoint) {
                NativeSceneExecutingJobs()=true;
                floatingPoint.Apply();
            }
            ~ExecutionScope() {
                previousFloatingPoint.Apply();
                NativeSceneExecutingJobs()=previous;
            }
        } execution(floatingPoint_);
        try {
            while(!canceled_.load(std::memory_order_relaxed)) {
                const auto first=next_.fetch_add(chunk_,std::memory_order_relaxed);
                if(first>=size_)return;
                const auto last=first+std::min(chunk_,size_-first);
                for(auto index=first;index<last;++index) {
                    if(canceled_.load(std::memory_order_relaxed))return;
                    function_(context_,index);
                }
            }
        } catch(...) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if(!failure_)failure_=std::current_exception();
            }
            canceled_.store(true,std::memory_order_release);
        }
    }
    void Worker(unsigned slot) noexcept {
        ConfigureNativeSceneWorker();
        std::size_t seen=0;
        for(;;) {
            bool assigned=false;
            {
                std::unique_lock<std::mutex> guard(mutex_);
                wake_.wait(guard,[this,&seen] {return stopping_ || generation_!=seen;});
                if(stopping_)return;
                seen=generation_;
                assigned=slot<active_;
            }
            if(!assigned)continue;
            Execute();
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if(--remaining_==0)completed_.notify_one();
            }
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_,completed_;
    std::vector<std::thread> workers_;
    void (*function_)(void*,std::size_t){};
    void* context_{};
    std::size_t size_{},generation_{},chunk_{1};
    unsigned active_{},remaining_{};
    bool stopping_{};
    std::exception_ptr failure_;
    NativeSceneFloatingPointState floatingPoint_;
    std::atomic<std::size_t> next_{0};
    std::atomic<bool> canceled_{false};
};

inline NativeSceneWorkerPool& NativeSceneThreadPool() {
    static thread_local NativeSceneWorkerPool pool;
    return pool;
}

// Deterministic source order is retained in the recorded Vulkan stream. The
// workers only fill separate output slots; the main thread joins them before
// it copies shared vertices or uploads. On one core/small scenes, no native
// thread pool is constructed and there are no locks/condition variables.
template<class Task>
void RunNativeSceneJobs(std::size_t count,Task&& task,
    std::size_t minimumParallelJobs=512,unsigned requestedWorkers=0)
{
    if(!count)return;
    const unsigned effective=NativeSceneEffectiveHardwareConcurrency();
    const unsigned budget=requestedWorkers ? std::min(effective,requestedWorkers):effective;
    const unsigned workers=static_cast<unsigned>(std::min<std::size_t>(count,std::max(1u,budget)));
    if(workers<2 || count<minimumParallelJobs || NativeSceneExecutingJobs()) {
        for(std::size_t index=0;index<count;++index)task(index);
        return;
    }
    // One persistent pool per render-thread, shared by every Task type.
    NativeSceneThreadPool().Run(count,task,workers);
}
} // namespace hp2

// Stomper.obj: PAL 24B9xx-24C044, beta obj\release\Stomper.obj.
// The final PAL game has twelve render-egg modes (the beta has seven).
// Native pointers/callbacks replace the original EE 12-byte resource entries:
// no PS2 addresses or pointer bits are imported into a Windows process.
enum class StompKind : std::uint32_t {
    Byte=0, Half=1, Word=2, Float=3,
    IntegerCallback=4, FloatCallback=5, Group=6, End=7
};

struct StompEntry {
    using IntegerFn=std::int32_t (*)(std::int32_t,std::int32_t);
    using FloatFn=float (*)(std::int32_t,float);
    StompKind kind{StompKind::End};
    void* target{};
    std::uint32_t valueBits{};
    const StompEntry* group{};
    IntegerFn integerFn{};
    FloatFn floatFn{};
    // PAL StompEntry::Set/Clear save exactly one 32-bit word per leaf.
    // A group consumes as many words as its nested leaves.
    std::size_t Set(std::uint32_t* saved,std::size_t capacity) const;
    std::size_t Clear(const std::uint32_t* saved,std::size_t capacity) const;
    static std::size_t SetAll(const StompEntry* entries,std::uint32_t* saved,std::size_t capacity);
    static std::size_t ClearAll(const StompEntry* entries,const std::uint32_t* saved,std::size_t capacity);
};

class Stomper final {
public:
    static constexpr std::uint32_t FinalModeCount=12;
    static constexpr std::size_t OriginalSaveBytes=0x400;

    Stomper()=default;
    Stomper(const Stomper&)=delete;
    Stomper& operator=(const Stomper&)=delete;
    bool Enabled() const noexcept {return enabled_;}
    std::uint32_t Mode() const noexcept {return mode_;}
    bool ModeBound(std::uint32_t mode) const noexcept {
        return mode>=1 && mode<=FinalModeCount && tables_[mode]!=nullptr;
    }

    // Native-mode binding requires confirmed original renderer ownership.
    // An unbound mode cannot silently substitute a missing visual effect.
    void BindMode(std::uint32_t mode,const StompEntry* entries);
    void Set(std::uint32_t mode);
    void Clear();
    void ClearSet(std::uint32_t mode);
    void Suspend();
    void Resume();
    void Disable();
    void Enable() noexcept {enabled_=true;}

private:
    std::array<const StompEntry*,FinalModeCount+1> tables_{};
    std::array<std::uint32_t,OriginalSaveBytes/sizeof(std::uint32_t)> saved_{};
    std::uint32_t mode_{};
    std::size_t savedWords_{};
    bool suspended_{};
    bool enabled_{true};
};

Stomper* GetStomper() noexcept;
void InitStomper();
void CloseStomper();
void ResetRenderEggs();
void SuspendRenderEggs();
void ResumeRenderEggs();
void DisableRenderEggs();
void EnableRenderEggs();
void UpdateStomper() noexcept;

// Final PAL 0x10F1F8 (beta RadialBlurOnOff(bool,int)) is the verified
// callback in render-egg mode 8. The native track owns the render service;
// the global Stomper only borrows it between track activation and retirement.
struct ScreenRadialBlur;
void BindPalRadialRenderEgg(ScreenRadialBlur* radial);
void UnbindPalRadialRenderEgg(const ScreenRadialBlur* radial);

namespace hp2::frontend { struct EasterEggCode; }
// PAL 24BF18: original priority among the live, mutable EasterEgg record
// unlock lanes. A native record pointer replaces its global EE address.
// Rendering modes need their actual Vulkan descriptor bindings first.
std::uint32_t SelectedRenderEggMode(const hp2::frontend::EasterEggCode* records,std::size_t count);
void ActivateRenderEggs(const hp2::frontend::EasterEggCode* records,std::size_t count);


#endif
#endif
