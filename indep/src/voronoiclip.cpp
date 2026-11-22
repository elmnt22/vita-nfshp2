#include <atomic>
#include <vector>
#include <new>
#include <mutex>
#include "indep/inc/VoronoiClip.hpp"
#include "indep/inc/Contact.hpp"
#include "indep/inc/Polyhedra.hpp"
#include "bware/inc/bMath.hpp"
#include "bware/inc/bMath.hpp"
#include "indep/inc/VoronoiClip.hpp"

#include <chrono>
#include <math.h>
#include <stddef.h>
#include <string.h>
#include <cstdio>
#include <cstring>

CollisionWitness* collisionWitnessInitialize(CollisionWitness* witness, CollisionGeometry* first, CollisionGeometry* second)
{
    for(int lane=0;lane<3;++lane) witness->firstPoint[lane]=witness->secondPoint[lane]=0.0f;
    witness->first=first->vertices;
    witness->second=second->vertices;
    witness->firstGeometry=first;
    witness->secondGeometry=second;
    witness->side=0;
    witness->distance=0.0f;
    witness->firstDisplay=witness->secondDisplay=NULL;
    return witness;
}

float collisionPointDistance(const float first[4], const float second[4])
{
    float delta[4];
    for (int lane = 0; lane < 3; ++lane) delta[lane] = first[lane] - second[lane];
    return sqrtf(collisionGeometryDot3(delta, delta));
}

void collisionTransformDirection(float output[4], const float frame[16], const float value[4])
{
    float result[4];
    for (int row = 0; row < 4; ++row) {
        float sum = frame[row] * value[0];
        const float second = frame[4 + row] * value[1];
        sum = sum + second;
        const float third = frame[8 + row] * value[2];
        result[row] = sum + third;
    }
    memcpy(output, result, sizeof(result));
}

void collisionTransformPoint(float output[4], const float frame[16], const float value[4])
{
    vectorMatrixTransformPoint(output, frame, value);
}

void collisionTransformVertex(CollisionTransformedVertex* output, const float frame[16],
                               const CollisionFeature* vertex)
{
    const float point[4] = {vertex->vector[0], vertex->vector[1], vertex->vector[2], 0.0f};
    output->feature = vertex;
    vectorMatrixTransformPoint(output->point, frame, point);
}

void collisionTransformEdge(CollisionTransformedEdge* output, const float frame[16],
                             const CollisionFeature* edge)
{
    output->feature = edge;
    const CollisionFeature* first = edge->adjacent[1].feature;
    const CollisionFeature* second = edge->adjacent[0].feature;
    const float firstPoint[4] = {first->vector[0], first->vector[1], first->vector[2], 0.0f};
    const float secondPoint[4] = {second->vector[0], second->vector[1], second->vector[2], 0.0f};
    vectorMatrixTransformPoint(output->first, frame, firstPoint);
    vectorMatrixTransformPoint(output->second, frame, secondPoint);
    memcpy(output->direction, output->first, sizeof(output->direction));
    for (int lane = 0; lane < 3; ++lane) output->direction[lane] = output->first[lane] - output->second[lane];
}

// Per-query transformed-feature reuse (exact). Each scratch slot of a query
// is only ever filled from one frame (firstVertex/firstEdge: firstToSecond,
// secondVertex/secondEdge: secondToFirst) and the frames and geometry are
// fixed for the query, so a slot that already holds a feature holds exactly
// what transforming it again would compute. Vita witness_cache_off.txt: 0;
// PC HP2_VERIFY_WITNESS_CACHE=1 recomputes every reuse and counts mismatches.
int collisionWitnessTransformCache=1;
std::atomic<uint64_t> collisionWitnessCacheChecks{0},collisionWitnessCacheMismatches{0};
static const bool verifyWitnessCache=[] {
#if !defined(__vita__) && !defined(__SWITCH__)
    const char* value=getenv("HP2_VERIFY_WITNESS_CACHE");return value && *value=='1';
#else
    return false;
#endif
}();
static inline void cachedTransformVertex(CollisionTransformedVertex* slot,const float frame[16],const CollisionFeature* vertex)
{
    if(collisionWitnessTransformCache && slot->feature==vertex) {
        if(verifyWitnessCache) {
            CollisionTransformedVertex fresh;collisionTransformVertex(&fresh,frame,vertex);
            ++collisionWitnessCacheChecks;
            if(memcmp(&fresh,slot,sizeof(fresh))!=0) ++collisionWitnessCacheMismatches;
        }
        return;
    }
    collisionTransformVertex(slot,frame,vertex);
}
static inline void cachedTransformEdge(CollisionTransformedEdge* slot,const float frame[16],const CollisionFeature* edge)
{
    if(collisionWitnessTransformCache && slot->feature==edge) {
        if(verifyWitnessCache) {
            CollisionTransformedEdge fresh;collisionTransformEdge(&fresh,frame,edge);
            ++collisionWitnessCacheChecks;
            if(memcmp(&fresh,slot,sizeof(fresh))!=0) ++collisionWitnessCacheMismatches;
        }
        return;
    }
    collisionTransformEdge(slot,frame,edge);
}

CollisionClip* collisionClipInitialize(CollisionClip* clip)
{
    clip->lower = 0.0f;
    clip->upper = 1.0f;
    clip->lowerFeature = clip->upperFeature = NULL;
    clip->boundary = 0;
    return clip;
}

int32_t collisionClipPlanes(CollisionClip* clip, const CollisionTransformedEdge* edge,
                            const CollisionPlane* planes, int32_t count)
{
    for (int32_t index = 0; index < count; ++index) {
        const CollisionPlane* plane = &planes[index];
        const float first = collisionGeometryDot3(plane->normal, edge->second) - plane->offset;
        const float second = collisionGeometryDot3(plane->normal, edge->first) - plane->offset;
        if (first < 0.0f) {
            if (second < 0.0f) {
                clip->lowerFeature = clip->upperFeature = plane->feature;
                return 0;
            }
            const float denominator = first - second;
            const float fraction = finiteRatio(first, denominator);
            if (clip->lower < fraction) {
                clip->lower = fraction;
                clip->lowerFeature = plane->feature;
                if (clip->upper < fraction) return 0;
            }
        } else if (second < 0.0f) {
            const float denominator = first - second;
            const float fraction = finiteRatio(first, denominator);
            if (fraction < clip->upper) {
                clip->upper = fraction;
                clip->upperFeature = plane->feature;
                if (fraction < clip->lower) return 0;
            }
        }
    }
    return 1;
}

void collisionVertexSetPlanes(CollisionFeature* vertex)
{
    for (int index = 0; index < vertex->adjacentCount; ++index) {
        CollisionPlane* plane = &vertex->adjacent[index];
        CollisionFeature* edge = plane->feature;
        for (int lane = 0; lane < 3; ++lane) {
            float direction = (float)edge->encodedDirection[lane] * 0.0001220703125f;
            if (edge->adjacent[1].feature != vertex) direction = 0.0f - direction;
            plane->normal[lane] = direction;
        }
        plane->offset = collisionGeometryDot3(vertex->vector, plane->normal);
    }
}

CollisionFeature* collisionNextEdgeInFace(const CollisionFeature* vertex,
                                         const CollisionFeature* previousEdge,
                                         const CollisionFeature* face)
{
    for (int index = 0; index < vertex->adjacentCount; ++index) {
        CollisionFeature* edge = vertex->adjacent[index].feature;
        if (edge != previousEdge)
            for (int other = 0; other < face->adjacentCount; ++other)
                if (edge == face->adjacent[other].feature) return edge;
    }
    return NULL;
}

void collisionVertexMove(CollisionFeature* vertex, const float delta[4])
{
    for (int lane = 0; lane < 3; ++lane) vertex->vector[lane] = vertex->vector[lane] + delta[lane];
}

void collisionEdgeSetPlanes(CollisionFeature* edge)
{
    float direction[3];
    for (int lane = 0; lane < 3; ++lane) direction[lane] = (float)edge->encodedDirection[lane] * 0.0001220703125f;
    CollisionPlane* entries = edge->adjacent;
    for (int lane = 0; lane < 3; ++lane) {
        entries[0].normal[lane] = direction[lane];
        entries[1].normal[lane] = 0.0f - direction[lane];
    }
    entries[0].offset = collisionGeometryDot3(direction, entries[0].feature->vector);
    entries[1].offset = collisionGeometryDot3(entries[1].normal, entries[1].feature->vector);
    for (int side = 2; side < 4; ++side) {
        CollisionFeature* face = entries[side].feature;
        for (int index = 0; index < face->adjacentCount; ++index) {
            const CollisionPlane* facePlane = &face->adjacent[index];
            if (facePlane->feature == edge) {
                for (int lane = 0; lane < 3; ++lane) entries[side].normal[lane] = 0.0f - facePlane->normal[lane];
                entries[side].offset = collisionGeometryDot3(entries[side].normal, entries[1].feature->vector);
                break;
            }
        }
    }
}

void collisionFaceSetPlanes(CollisionFeature* face)
{
    for (int index = 0; index < face->adjacentCount; ++index) {
        CollisionPlane* plane = &face->adjacent[index];
        CollisionFeature* edge = plane->feature;
        CollisionFeature* endpoint = edge->adjacent[0].feature;
        float direction[3];
        for (int lane = 0; lane < 3; ++lane) direction[lane] = (float)edge->encodedDirection[lane] * 0.0001220703125f;
        for (int lane = 0; lane < 3; ++lane) {
            const int next = (lane + 1) % 3, last = (lane + 2) % 3;
            const float positive = face->vector[next] * direction[last];
            const float negative = direction[next] * face->vector[last];
            plane->normal[lane] = positive - negative;
        }
        plane->offset = collisionGeometryDot3(plane->normal, endpoint->vector);
        const CollisionFeature* nextEdge = collisionNextEdgeInFace(endpoint, edge, face);
        const CollisionFeature* other = nextEdge->adjacent[0].feature;
        if (other == endpoint) other = nextEdge->adjacent[1].feature;
        const float distance = collisionGeometryDot3(plane->normal, other->vector) - plane->offset;
        if (distance < 0.0f) {
            for (int lane = 0; lane < 3; ++lane) plane->normal[lane] = 0.0f - plane->normal[lane];
            plane->offset = collisionGeometryDot3(plane->normal, endpoint->vector);
        }
    }
}

void collisionFaceMove(CollisionFeature* face, const float delta[4])
{
    face->offset = face->offset + collisionGeometryDot3(delta, face->vector);
}

CollisionGeometry* collisionGeometryInitialize(CollisionGeometry* geometry)
{
    geometry->ownsStorage = 1;
    geometry->display = NULL;
    geometry->vertexCount = geometry->edgeCount = geometry->faceCount = geometry->planeCount = 0;
    geometry->vertexCapacity = geometry->edgeCapacity = geometry->faceCapacity = geometry->planeCapacity = 0;
    geometry->vertices = geometry->edges = geometry->faces = NULL;
    geometry->planes = NULL;
    return geometry;
}

// A slot whose geometry is rebuilt in place (terrain shapes are recycled for
// other polygons) gets a new generation, so walk memo keys never match
// across the rebuild even at the same addresses.
static std::atomic<uint32_t> collisionGeometryGenerations{0};
static void collisionGeometryTouch(CollisionGeometry* geometry)
{
    geometry->generation=collisionGeometryGenerations.fetch_add(1,std::memory_order_relaxed)+1u;
}
void collisionGeometryBind(CollisionGeometry* geometry, CollisionFeature* vertices, int16_t vertexCapacity,
                           CollisionFeature* edges, int16_t edgeCapacity,
                           CollisionFeature* faces, int16_t faceCapacity,
                           CollisionPlane* planes, int16_t planeCapacity)
{
    collisionGeometryTouch(geometry);
    geometry->vertices = vertices; geometry->edges = edges; geometry->faces = faces; geometry->planes = planes;
    geometry->vertexCapacity = vertexCapacity; geometry->edgeCapacity = edgeCapacity;
    geometry->faceCapacity = faceCapacity; geometry->planeCapacity = planeCapacity;
    geometry->ownsStorage = 0;
    geometry->vertexCount = geometry->edgeCount = geometry->faceCount = geometry->planeCount = 0;
}

CollisionPlane* collisionGeometryNewPlanes(CollisionGeometry* geometry, int32_t count)
{
    const int32_t oldCount = geometry->planeCount;
    if (geometry->planeCapacity < oldCount + count) return NULL;
    CollisionPlane* result = geometry->planes + oldCount;
    geometry->planeCount = (int16_t)(oldCount + count);
    for (int32_t index = 0; index < count; ++index) {
        result[index].normal[0] = result[index].normal[1] = 0.0f;
        result[index].normal[2] = 1.0f;
        result[index].offset = 0.0f; result[index].feature = NULL;
    }
    return result;
}

CollisionFeature* collisionGeometryNewVertex(CollisionGeometry* geometry, const float point[3],
                                            int8_t adjacentCount, CollisionPlane* adjacent)
{
    if (geometry->vertexCount >= geometry->vertexCapacity) return NULL;
    CollisionFeature* vertex = &geometry->vertices[geometry->vertexCount++];
    vertex->kind = 0; vertex->adjacentCount = adjacentCount; vertex->adjacent = adjacent;
    memcpy(vertex->vector, point, sizeof(vertex->vector));
    return vertex;
}

CollisionFeature* collisionGeometryNewEdge(CollisionGeometry* geometry, const float direction[3],
                                          int8_t adjacentCount, CollisionPlane* adjacent)
{
    if (geometry->edgeCount >= geometry->edgeCapacity) return NULL;
    CollisionFeature* edge = &geometry->edges[geometry->edgeCount++];
    edge->kind = 1; edge->adjacentCount = adjacentCount; edge->adjacent = adjacent;
    for (int lane = 0; lane < 3; ++lane) {
        const float scaled = direction[lane] * 8192.0f;
        edge->encodedDirection[lane] = (int16_t)(uint16_t)(uint32_t)(int32_t)scaled;
    }
    return edge;
}

CollisionFeature* collisionGeometryNewFace(CollisionGeometry* geometry, const float normal[3],
                                          int8_t adjacentCount, CollisionPlane* adjacent, float offset)
{
    if (geometry->faceCount >= geometry->faceCapacity) return NULL;
    CollisionFeature* face = &geometry->faces[geometry->faceCount++];
    face->kind = 2; face->adjacentCount = adjacentCount; face->adjacent = adjacent; face->offset = offset;
    memcpy(face->vector, normal, sizeof(face->vector));
    return face;
}

void collisionGeometrySetPlanes(CollisionGeometry* geometry)
{
    collisionGeometryTouch(geometry);
    for (int index = 0; index < geometry->vertexCount; ++index) collisionVertexSetPlanes(&geometry->vertices[index]);
    for (int index = 0; index < geometry->faceCount; ++index) collisionFaceSetPlanes(&geometry->faces[index]);
    for (int index = 0; index < geometry->edgeCount; ++index) collisionEdgeSetPlanes(&geometry->edges[index]);
}

uint32_t collisionGeometryFeatureCode(const CollisionGeometry* geometry, const CollisionFeature* feature)
{
    if (!feature) return UINT32_MAX;
    if (feature->kind == 0) return (uint32_t)(feature - geometry->vertices) << 4;
    if (feature->kind == 1) return ((uint32_t)(feature - geometry->edges) << 4) | 1u;
    if (feature->kind == 2) return ((uint32_t)(feature - geometry->faces) << 4) | 2u;
    return UINT32_MAX;
}

CollisionFeature* collisionGeometryFeature(const CollisionGeometry* geometry, uint32_t code)
{
    if (code == UINT32_MAX) return NULL;
    const uint32_t index = code >> 4;
    switch (code & 15u) {
    case 0: return &geometry->vertices[index];
    case 1: return &geometry->edges[index];
    case 2: return &geometry->faces[index];
    default: return NULL;
    }
}

void collisionGeometryEncodeLinks(const CollisionGeometry* geometry, CollisionGeometryLinks* links)
{
    for(int index=0;index<geometry->vertexCount;++index) links->vertexPlanes[index]=(uint32_t)(geometry->vertices[index].adjacent-geometry->planes);
    for(int index=0;index<geometry->edgeCount;++index) links->edgePlanes[index]=(uint32_t)(geometry->edges[index].adjacent-geometry->planes);
    for(int index=0;index<geometry->faceCount;++index) links->facePlanes[index]=(uint32_t)(geometry->faces[index].adjacent-geometry->planes);
    for(int index=0;index<geometry->planeCount;++index) links->planeFeatures[index]=collisionGeometryFeatureCode(geometry,geometry->planes[index].feature);
}

void collisionGeometryDecodeLinks(CollisionGeometry* geometry, const CollisionGeometryLinks* links)
{
    for(int index=0;index<geometry->vertexCount;++index) geometry->vertices[index].adjacent=geometry->planes+links->vertexPlanes[index];
    for(int index=0;index<geometry->edgeCount;++index) geometry->edges[index].adjacent=geometry->planes+links->edgePlanes[index];
    for(int index=0;index<geometry->faceCount;++index) geometry->faces[index].adjacent=geometry->planes+links->facePlanes[index];
    for(int index=0;index<geometry->planeCount;++index) geometry->planes[index].feature=collisionGeometryFeature(geometry,links->planeFeatures[index]);
}

void collisionGeometryBindIndexed(CollisionGeometry* geometry, CollisionFeature* vertices, int16_t vertexCount,
                                  CollisionFeature* edges, int16_t edgeCount,
                                  CollisionFeature* faces, int16_t faceCount,
                                  CollisionPlane* planes, int16_t planeCount,
                                  const CollisionGeometryLinks* links)
{
    collisionGeometryBind(geometry,vertices,vertexCount,edges,edgeCount,faces,faceCount,planes,planeCount);
    geometry->vertexCount=vertexCount;geometry->edgeCount=edgeCount;
    geometry->faceCount=faceCount;geometry->planeCount=planeCount;
    collisionGeometryDecodeLinks(geometry,links);
}

float* collisionRelativeTransform(float output[16], const float firstFrame[16],
                                  const float firstPosition[4], const float secondFrame[16],
                                  const float secondPosition[4])
{
    float relative[16], transposed[16];
    memcpy(relative, secondFrame, sizeof(relative));
    for (int lane = 0; lane < 4; ++lane) {
        const float translated = relative[12 + lane] + secondPosition[lane];
        relative[12 + lane] = translated - firstPosition[lane];
    }
    relative[15] = 1.0f;
    vectorMatrixTranspose(transposed, firstFrame);
    vectorMatrixMultiply(output, transposed, relative);
    return output;
}

static float dot3(const float first[3],const float second[3])
{
    const float x=first[0]*second[0],y=first[1]*second[1],z=first[2]*second[2];
    return (x+y)+z;
}

static void replaceFeature(CollisionWitness* witness,int32_t side,CollisionFeature* feature)
{
    if(side==1)witness->first=feature;else witness->second=feature;
}

int32_t collisionWitnessVertexEdge(CollisionWitness* witness,const float featureFrame[16],
                                   const float otherFrame[16],CollisionTransformedVertex* vertex,
                                   CollisionTransformedEdge* edge)
{
    CollisionFeature* first=witness->side==1?witness->first:witness->second;
    CollisionFeature* second=witness->side==2?witness->first:witness->second;
    cachedTransformVertex(vertex,featureFrame,first);
    for(int index=0;index<4;++index) {
        const CollisionPlane* plane=&second->adjacent[index];
        if(dot3(plane->normal,vertex->point)-plane->offset<0.0f) {
            replaceFeature(witness,3-witness->side,plane->feature);
            return 0;
        }
    }
    cachedTransformEdge(edge,otherFrame,second);
    CollisionClip clip;collisionClipInitialize(&clip);
    const float point[4]={first->vector[0],first->vector[1],first->vector[2],0.0f};
    CollisionFeature* changed=collisionClipVertex(&clip,edge,point,first->adjacent,first->adjacentCount);
    if(changed) {
        replaceFeature(witness,witness->side,changed);
        return 0;
    }
    float* firstPoint=witness->side==1?witness->firstPoint:witness->secondPoint;
    float* secondPoint=witness->side==2?witness->firstPoint:witness->secondPoint;
    memcpy(firstPoint,point,sizeof(point));
    if(clip.boundary) {
        witness->distance=0.0f;
        memcpy(secondPoint,vertex->point,4*sizeof(float));
        return 2;
    }
    const CollisionFeature* endpoint=second->adjacent[0].feature;
    float direction[3],delta[3];
    for(int lane=0;lane<3;++lane) {
        direction[lane]=(float)second->encodedDirection[lane]*0.0001220703125f;
        delta[lane]=vertex->point[lane]-endpoint->vector[lane];
    }
    const float fraction=dot3(delta,direction);
    for(int lane=0;lane<3;++lane) {
        const float scaled=direction[lane]*fraction;
        secondPoint[lane]=endpoint->vector[lane]+scaled;
        delta[lane]=vertex->point[lane]-secondPoint[lane];
    }
    secondPoint[3]=0.0f;
    witness->distance=sqrtf(dot3(delta,delta));
    return 1;
}

static float planeDistance(const CollisionFeature* face, const float point[4])
{
    return dot3(face->vector, point) - face->offset;
}

static void projectPlane(float output[4], const float point[4], const CollisionFeature* face, float distance)
{
    const float scale = -distance;
    for (int lane = 0; lane < 3; ++lane) {
        const float component = face->vector[lane] * scale;
        output[lane] = point[lane] + component;
    }
    output[3] = 0.0f;
}

int32_t collisionWitnessVertexFace(CollisionWitness* witness, const float featureFrame[16],
                                   CollisionTransformedVertex* transformed)
{
    CollisionFeature* vertex = witness->side == 1 ? witness->first : witness->second;
    CollisionFeature* face = witness->side == 2 ? witness->first : witness->second;
    cachedTransformVertex(transformed, featureFrame, vertex);
    float minimum = 0.0f;
    int32_t violated = -1;
    for (int index = 0; index < face->adjacentCount; ++index) {
        const CollisionPlane* plane = &face->adjacent[index];
        const float distance = dot3(plane->normal, transformed->point) - plane->offset;
        if (distance < minimum) { minimum = distance; violated = index; }
    }
    if (violated != -1) {
        replaceFeature(witness, 3 - witness->side, face->adjacent[violated].feature);
        return 0;
    }
    const float distance = planeDistance(face, transformed->point);
    float* vertexPoint = witness->side == 1 ? witness->firstPoint : witness->secondPoint;
    float* facePoint = witness->side == 2 ? witness->firstPoint : witness->secondPoint;
    if (fabsf(distance) < 0.0001f) {
        witness->distance = 0.0f;
        memcpy(vertexPoint, vertex->vector, 3 * sizeof(float));
        memcpy(facePoint, transformed->point, 4 * sizeof(float));
        return 2;
    }
    for (int index = 0; index < vertex->adjacentCount; ++index) {
        CollisionFeature* edge = vertex->adjacent[index].feature;
        CollisionFeature* neighbor = edge->adjacent[0].feature;
        if (neighbor == vertex) neighbor = edge->adjacent[1].feature;
        CollisionTransformedVertex other;
        collisionTransformVertex(&other, featureFrame, neighbor);
        const float neighborDistance = planeDistance(face, other.point);
        if ((distance < 0.0f && distance < neighborDistance) ||
            (0.0f < distance && neighborDistance < distance)) {
            replaceFeature(witness, witness->side, edge);
            return 0;
        }
    }
    memcpy(vertexPoint, vertex->vector, 3 * sizeof(float));
    if (0.0f < distance) {
        witness->distance = distance;
        projectPlane(facePoint, transformed->point, face, distance);
        return 1;
    }
    const CollisionGeometry* geometry = witness->side == 2 ? witness->firstGeometry : witness->secondGeometry;
    float maximum = distance;
    CollisionFeature* selected = face;
    for (int index = 0; index < geometry->faceCount; ++index) {
        CollisionFeature* candidate = &geometry->faces[index];
        const float candidateDistance = planeDistance(candidate, transformed->point);
        if (maximum < candidateDistance) {
            maximum = candidateDistance;
            selected = candidate;
            replaceFeature(witness, 3 - witness->side, selected);
        }
    }
    witness->distance = maximum;
    projectPlane(facePoint, transformed->point, selected, maximum);
    return maximum < 0.0f ? 2 : 1;
}

int32_t collisionWitnessEdgeRegions(CollisionWitness* witness, const CollisionTransformedEdge* segment,
                                    const CollisionFeature* feature, int32_t featureRole)
{
    CollisionClip clip;
    collisionClipInitialize(&clip);
    CollisionFeature* changed = collisionClipVertex(&clip, segment, NULL, feature->adjacent, 2);
    if (changed) {
        replaceFeature(witness, featureRole == witness->side ? 1 : 2, changed);
        return 0;
    }
    if (clip.boundary) { witness->distance = 0.0f; return 2; }
    changed = collisionClipFace(&clip, segment, NULL, feature->adjacent + 2, 2);
    if (changed) {
        replaceFeature(witness, featureRole == witness->side ? 1 : 2, changed);
        return 0;
    }
    if (clip.boundary) { witness->distance = 0.0f; return 2; }
    return 1;
}

static void edgeDirection(float output[4], const CollisionFeature* edge)
{
    for (int lane = 0; lane < 3; ++lane)
        output[lane] = (float)edge->encodedDirection[lane] * 0.0001220703125f;
    output[3] = 0.0f;
}

static float closestEdgePoints(float firstPoint[4], float secondPoint[4],
                               const CollisionFeature* firstFeature, const CollisionFeature* secondFeature,
                               const float featureFrame[16], const float otherFrame[16],
                               const CollisionTransformedEdge* transformedOther)
{
    const float* start = firstFeature->adjacent[0].feature->vector;
    const float* end = firstFeature->adjacent[1].feature->vector;
    float direction[4], otherDirection[4], transformedDirection[4], delta[4], perpendicular[4];
    edgeDirection(otherDirection, secondFeature);
    collisionTransformDirection(transformedDirection, otherFrame, otherDirection);
    edgeDirection(direction, firstFeature);
    const float cosine = dot3(transformedDirection, direction);
    const float squaredCosine = cosine * cosine;
    const float denominator = 1.0f - squaredCosine;
    for (int lane = 0; lane < 3; ++lane) {
        delta[lane] = transformedOther->second[lane] - start[lane];
        const float projected = transformedDirection[lane] * cosine;
        perpendicular[lane] = direction[lane] - projected;
    }
    const float numerator = dot3(delta, perpendicular);
    if (denominator < 0.000001f) {
        memcpy(firstPoint, 0.0f < numerator ? end : start, 3 * sizeof(float));
        firstPoint[3] = 0.0f;
    } else {
        float parameter = finiteRatio(numerator, denominator);
        if (parameter < 0.0f) parameter = 0.0f;
        else {
            const float start4[4] = {start[0], start[1], start[2], 0.0f};
            const float end4[4] = {end[0], end[1], end[2], 0.0f};
            const float length = collisionPointDistance(end4, start4);
            if (length < parameter) parameter = length;
        }
        for (int lane = 0; lane < 3; ++lane) {
            const float offset = direction[lane] * parameter;
            firstPoint[lane] = start[lane] + offset;
        }
        firstPoint[3] = 0.0f;
    }
    float transformedPoint[4];
    collisionTransformPoint(transformedPoint, featureFrame, firstPoint);
    const float* otherStart = secondFeature->adjacent[0].feature->vector;
    for (int lane = 0; lane < 3; ++lane) delta[lane] = transformedPoint[lane] - otherStart[lane];
    const float parameter = dot3(delta, otherDirection);
    for (int lane = 0; lane < 3; ++lane) {
        const float offset = otherDirection[lane] * parameter;
        secondPoint[lane] = otherStart[lane] + offset;
    }
    secondPoint[3] = 0.0f;
    return collisionPointDistance(secondPoint, transformedPoint);
}

int32_t collisionWitnessEdges(CollisionWitness* witness, const float featureFrame[16],
                              const float otherFrame[16], CollisionTransformedEdge* first,
                              CollisionTransformedEdge* second)
{
    CollisionFeature* firstFeature = witness->side == 1 ? witness->first : witness->second;
    CollisionFeature* secondFeature = witness->side == 2 ? witness->first : witness->second;
    cachedTransformEdge(first, featureFrame, firstFeature);
    int32_t result = collisionWitnessEdgeRegions(witness, first, secondFeature, 2);
    if (result != 1) return result;
    cachedTransformEdge(second, otherFrame, secondFeature);
    result = collisionWitnessEdgeRegions(witness, second, firstFeature, 1);
    if (result != 1) return result;
    float* firstPoint = witness->side == 1 ? witness->firstPoint : witness->secondPoint;
    float* secondPoint = witness->side == 2 ? witness->firstPoint : witness->secondPoint;
    witness->distance = closestEdgePoints(firstPoint, secondPoint, firstFeature, secondFeature,
                                         featureFrame, otherFrame, second);
    return result;
}

static void segmentPoint(float output[4], const CollisionTransformedEdge* segment, float parameter)
{
    for (int lane = 0; lane < 4; ++lane) {
        const float delta = segment->direction[lane] * parameter;
        output[lane] = delta + segment->second[lane];
    }
}

int32_t collisionWitnessEdgeFace(CollisionWitness* witness, const float featureFrame[16],
                                 const float otherFrame[16], CollisionTransformedEdge* edge)
{
    CollisionFeature* edgeFeature = witness->side == 1 ? witness->first : witness->second;
    CollisionFeature* face = witness->side == 2 ? witness->first : witness->second;
    cachedTransformEdge(edge, featureFrame, edgeFeature);
    CollisionClip clip;
    collisionClipInitialize(&clip);
    if (collisionClipPlanes(&clip, edge, face->adjacent, face->adjacentCount)) {
        float lower[4], upper[4];
        segmentPoint(lower, edge, clip.lower);
        segmentPoint(upper, edge, clip.upper);
        const float upperDistance = planeDistance(face, upper);
        const float lowerDistance = planeDistance(face, lower);
        const float product = lowerDistance * upperDistance;
        if (product <= 0.0f) { witness->distance = 0.0f; return 2; }
        const float derivative = dot3(face->vector, edge->direction);
        const float signedDerivative = lowerDistance * derivative;
        if (0.0f < signedDerivative) {
            if (clip.lowerFeature) replaceFeature(witness, 3 - witness->side, clip.lowerFeature);
            else replaceFeature(witness, witness->side, edgeFeature->adjacent[0].feature);
        } else {
            if (clip.upperFeature) replaceFeature(witness, 3 - witness->side, clip.upperFeature);
            else replaceFeature(witness, witness->side, edgeFeature->adjacent[1].feature);
        }
        return 0;
    }

    CollisionFeature* selected = NULL;
    CollisionFeature* changed = NULL;
    float minimum = 0.0f;
    for (int index = 0; index < face->adjacentCount; ++index) {
        CollisionFeature* candidate = face->adjacent[index].feature;
        CollisionTransformedEdge transformed;
        collisionTransformEdge(&transformed, otherFrame, candidate);
        float firstPoint[4], secondPoint[4];
        const float distance = closestEdgePoints(firstPoint, secondPoint, edgeFeature, candidate,
                                                featureFrame, otherFrame, &transformed);
        CollisionClip candidateClip;
        collisionClipInitialize(&candidateClip);
        CollisionFeature* candidateChange = collisionClipVertex(&candidateClip, edge, NULL, candidate->adjacent, 2);
        if (candidateClip.boundary) return 2;
        if (index == 0 || distance < minimum) {
            minimum = distance;
            selected = candidate;
            changed = candidateChange;
        }
    }
    replaceFeature(witness, 3 - witness->side, changed ? changed : selected);
    return 0;
}

// Diagnostic counters (logged with render.collision_solve).
uint64_t collisionWitnessQueries=0,collisionWitnessIterations=0,collisionWitnessRestarts=0,collisionWitnessExhausted=0;
double collisionWitnessMilliseconds=0,collisionWitnessWarmMilliseconds=0;
int collisionWitnessWarmProbe=0;
// Diagnostic per feature-kind split (Vita sets the clock when probing).
uint32_t (*collisionWitnessProbeClock)()=nullptr;
uint32_t (*collisionWitnessClockMicros)()=nullptr;
// Diagnostic: timer reads (approximate, racy), counted by the Vita clocks.
uint32_t diagClockReads=0;
// Fine diagnostic timers (per query, task, substep, model). Each read is a
// kernel call on Vita (~0.4 us, ~1900 per frame with all on), so Vita
// turns them off unless diag_fine.txt exists; coarse per-frame timers stay.
int diagFineTimers=1;
uint32_t diagFineClock()
{
    return diagFineTimers && collisionWitnessClockMicros ? collisionWitnessClockMicros():0u;
}
uint64_t collisionWitnessKindMicros[9]={},collisionWitnessKindSteps[9]={};
uint64_t collisionWitnessWarmMismatches=0;
static int32_t collisionWitnessQueryBody(CollisionWitness* witness, const float firstFrame[16],
                              const float firstPosition[4], const float secondFrame[16],
                              const float secondPosition[4], uint32_t* randomState,
                              uint64_t* counters=nullptr, int32_t* predictPending=nullptr,
                              const std::atomic<int>* stop=nullptr);
// RNG-free prediction (thread-safe, detached counters). *pending: 0 = the
// witness is the exact query result and no random draw happened; 1 = exact
// except the terminal restart, whose two vertex draws the caller makes from
// the real state in order; -1 = a restart before termination (the rest of
// the query depends on the random state): the witness is garbage.
int32_t collisionWitnessQueryPredict(CollisionWitness* witness, const float firstFrame[16],
                              const float firstPosition[4], const float secondFrame[16],
                              const float secondPosition[4], int32_t* pending, uint64_t counters[4])
{
    *pending=0;
    return collisionWitnessQueryBody(witness,firstFrame,firstPosition,secondFrame,secondPosition,nullptr,counters,pending);
}
// Thread-safe variant for speculative helpers: no timers, probes or global
// counters (counters[4] = queries, iterations, restarts, exhausted).
int32_t collisionWitnessQueryDetached(CollisionWitness* witness, const float firstFrame[16],
                              const float firstPosition[4], const float secondFrame[16],
                              const float secondPosition[4], uint32_t* randomState, uint64_t counters[4])
{
    return collisionWitnessQueryBody(witness,firstFrame,firstPosition,secondFrame,secondPosition,randomState,counters);
}
// Detached and abortable: returns -1 (witness garbage) once *stop is set.
// Only for speculative queries whose result the caller already knows it
// will discard.
int32_t collisionWitnessQueryAbortable(CollisionWitness* witness, const float firstFrame[16],
                              const float firstPosition[4], const float secondFrame[16],
                              const float secondPosition[4], uint32_t* randomState, uint64_t counters[4],
                              const std::atomic<int>* stop)
{
    return collisionWitnessQueryBody(witness,firstFrame,firstPosition,secondFrame,secondPosition,randomState,counters,nullptr,stop);
}
int32_t collisionWitnessQuery(CollisionWitness* witness, const float firstFrame[16],
                              const float firstPosition[4], const float secondFrame[16],
                              const float secondPosition[4], uint32_t* randomState)
{
    if(!diagFineTimers && !collisionWitnessWarmProbe)
        return collisionWitnessQueryBody(witness,firstFrame,firstPosition,secondFrame,secondPosition,randomState);
    if(collisionWitnessClockMicros && !collisionWitnessWarmProbe) {
        // Cheap host microsecond clock (Vita): ~0.4 us vs ~0.75 us per read.
        const uint32_t started=collisionWitnessClockMicros();
        const int32_t result=collisionWitnessQueryBody(witness,firstFrame,firstPosition,secondFrame,secondPosition,randomState);
        collisionWitnessMilliseconds+=(double)(collisionWitnessClockMicros()-started)*0.001;
        return result;
    }
    const CollisionWitness before=*witness;
    const uint32_t randomBefore=*randomState;
    const auto started=std::chrono::steady_clock::now();
    const int32_t result=collisionWitnessQueryBody(witness,firstFrame,firstPosition,secondFrame,secondPosition,randomState);
    const auto finished=std::chrono::steady_clock::now();
    collisionWitnessMilliseconds+=std::chrono::duration<double,std::milli>(finished-started).count();
    if(collisionWitnessWarmProbe) {
        // Diagnostic only: rerun the same query on copies with a warm cache.
        CollisionWitness copy=before;
        uint32_t random=randomBefore;
        const uint64_t q=collisionWitnessQueries,i=collisionWitnessIterations,r=collisionWitnessRestarts,e=collisionWitnessExhausted;
        const auto warmStarted=std::chrono::steady_clock::now();
        collisionWitnessQueryBody(&copy,firstFrame,firstPosition,secondFrame,secondPosition,&random);
        collisionWitnessWarmMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-warmStarted).count();
        collisionWitnessQueries=q;collisionWitnessIterations=i;collisionWitnessRestarts=r;collisionWitnessExhausted=e;
        if(memcmp(&copy,witness,sizeof(copy))!=0 || random!=*randomState) ++collisionWitnessWarmMismatches;
    }
    return result;
}
static int32_t collisionWitnessQueryWalk(CollisionWitness* witness, const float firstFrame[16],
                              const float firstPosition[4], const float secondFrame[16],
                              const float secondPosition[4], uint32_t* randomState,
                              uint64_t* counters, int32_t* predictPending, const std::atomic<int>* stop,
                              CollisionWitness* converged, int32_t* convergedPending, int32_t* steps);
// Walk memo (exact). A query's walk is a pure function of its inputs: the
// start witness (features, side, points, distance), both geometries, both
// frames and positions; only a restart reads the random state. A walk that
// converges with at most the terminal restart is stored as its converged
// witness, result and whether that terminal restart happened; a later query
// with bit-identical inputs replays it (the terminal restart's two vertex
// draws are made from the live random state, exactly as the walk does).
// Frozen contacts (countdown, parked cops) repeat their inputs every step.
// One bounded table shared by all threads (helpers run queries too). Vita walk_memo_off.txt;
// PC HP2_WALK_MEMO=0 off, 1 on, 2 on + rerun every hit and compare.
int collisionWitnessMemo=[] {
#if !defined(__vita__) && !defined(__SWITCH__)
    const char* value=getenv("HP2_WALK_MEMO");return value ? atoi(value):1;
#else
    return 1;
#endif
}();
// Bumped per collision system init: entries from an earlier world (whose
// geometry addresses may be reused) never match.
std::atomic<uint32_t> collisionWitnessMemoEpoch{1};
int collisionMemoGate=[] {
#if !defined(__vita__) && !defined(__SWITCH__)
    const char* value=getenv("HP2_MEMO_GATE");return value ? atoi(value):1;
#else
    return 1;
#endif
}();
// Diagnostic: gate flips (open<->closed) across threads.
std::atomic<uint64_t> collisionMemoGateFlips{0};
std::atomic<uint64_t> collisionMemoLookups{0},collisionMemoHits{0},collisionMemoStores{0},
    collisionMemoVerified{0},collisionMemoMismatches{0},collisionMemoSavedSteps{0};
namespace {
// Shared by all threads (helpers walk too). Every slot is a seqlock: a
// writer claims it by moving its sequence from even to odd, writes, then
// publishes the next even value; a reader copies and accepts the copy only
// if the sequence was even and unchanged. A failed claim skips the store.
//
// Poses (both frames and positions, 160 bytes) are interned: a memo key
// names its pose by (slot, sequence), and a pose slot's sequence changes on
// every rewrite, so a key only matches while that slot still holds the
// bit-identical pose.
struct PoseBlock {float values[40];};
struct PoseSlot {
    std::atomic<uint32_t> sequence;
    std::atomic<uint32_t> lastUse;
    uint32_t hash;
    PoseBlock pose;
};
struct WalkKey {
    const void* pointers[4];
    uint32_t poseSlot,poseSequence;
    uint32_t firstGeneration,secondGeneration;
    int32_t side;
    float points[8];
    float distance;
};
struct WalkValue {
    CollisionFeature* first;
    CollisionFeature* second;
    float firstPoint[4],secondPoint[4];
    float distance;
    int32_t side;
    int16_t steps;
    int8_t pending,result;
};
struct WalkEntry {
    std::atomic<uint32_t> sequence;
    uint32_t hash,epoch;
    WalkKey key;
    WalkValue value;
};
}
uint32_t collisionWitnessMemoEntries=[] {
#if !defined(__vita__) && !defined(__SWITCH__)
    const char* value=getenv("HP2_WALK_MEMO_ENTRIES");if(value) return (uint32_t)atoi(value);
#endif
    return 131072u; // ~16 MB (Vita bench: 86-92% race-start hits vs 77-81% at 65536)
}();
namespace {
uint32_t walkMemoEntries=0;
constexpr uint32_t kPoseSlots=8192u;
WalkEntry* walkMemoTable=nullptr;
PoseSlot* walkPoseTable=nullptr;
std::atomic<uint32_t> walkMemoClock{0};
std::atomic<int> walkMemoReady{0};
std::mutex walkMemoAllocate;
thread_local bool walkMemoBypass=false;
bool walkMemoEnsure()
{
    if (walkMemoReady.load(std::memory_order_acquire)) return walkMemoReady.load(std::memory_order_relaxed)>0;
    std::lock_guard<std::mutex> lock(walkMemoAllocate);
    if (!walkMemoReady.load(std::memory_order_relaxed)) {
        walkMemoEntries=collisionWitnessMemoEntries<4u ? 4u:collisionWitnessMemoEntries;
        walkMemoTable=new (std::nothrow) WalkEntry[walkMemoEntries];
        walkPoseTable=new (std::nothrow) PoseSlot[kPoseSlots];
        if (walkMemoTable && walkPoseTable) {
            for (uint32_t index=0;index<walkMemoEntries;++index) {
                walkMemoTable[index].sequence.store(0,std::memory_order_relaxed);walkMemoTable[index].epoch=0;
            }
            for (uint32_t index=0;index<kPoseSlots;++index) {
                walkPoseTable[index].sequence.store(0,std::memory_order_relaxed);
                walkPoseTable[index].lastUse.store(0,std::memory_order_relaxed);
                walkPoseTable[index].hash=0;
                memset(&walkPoseTable[index].pose,0xff,sizeof(PoseBlock)); // NaN bytes: never a real pose
            }
            walkMemoReady.store(1,std::memory_order_release);
        } else {
            delete[] walkMemoTable;delete[] walkPoseTable;walkMemoTable=nullptr;walkPoseTable=nullptr;
            walkMemoReady.store(-1,std::memory_order_release);
        }
    }
    return walkMemoReady.load(std::memory_order_relaxed)>0;
}
inline uint32_t hashWords(const void* data,size_t bytes,uint32_t hash)
{
    const unsigned char* at=static_cast<const unsigned char*>(data);
    for (size_t offset=0;offset<bytes;offset+=4) {
        uint32_t word;memcpy(&word,at+offset,4);
        hash^=word;hash*=16777619u;hash^=hash>>15;
    }
    return hash;
}
inline bool claim(std::atomic<uint32_t>& sequence,uint32_t& was)
{
    was=sequence.load(std::memory_order_relaxed);
    if (was&1u) return false;
    if (!sequence.compare_exchange_strong(was,was+1u,std::memory_order_acquire,std::memory_order_relaxed)) return false;
    std::atomic_thread_fence(std::memory_order_release);
    return true;
}
// Pose slot holding exactly this pose: its slot and (even) sequence.
bool poseFind(const PoseBlock& pose,uint32_t hash,uint32_t* slotOut,uint32_t* sequenceOut)
{
    const uint32_t base=hash&(kPoseSlots-4u);
    for (uint32_t way=0;way<4;++way) {
        PoseSlot& slot=walkPoseTable[base^way];
        const uint32_t before=slot.sequence.load(std::memory_order_acquire);
        if ((before&1u) || before==0 || slot.hash!=hash) continue;
        const bool same=memcmp(&slot.pose,&pose,sizeof(pose))==0;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!same || slot.sequence.load(std::memory_order_relaxed)!=before) continue;
        slot.lastUse.store(walkMemoClock.load(std::memory_order_relaxed),std::memory_order_relaxed);
        *slotOut=base^way;*sequenceOut=before;
        return true;
    }
    return false;
}
bool poseInsert(const PoseBlock& pose,uint32_t hash,uint32_t* slotOut,uint32_t* sequenceOut)
{
    if (poseFind(pose,hash,slotOut,sequenceOut)) return true;
    const uint32_t base=hash&(kPoseSlots-4u);
    // Replace the way used longest ago.
    uint32_t victim=base;
    uint32_t oldest=walkPoseTable[base].lastUse.load(std::memory_order_relaxed);
    const uint32_t now=walkMemoClock.load(std::memory_order_relaxed);
    for (uint32_t way=1;way<4;++way) {
        const uint32_t use=walkPoseTable[base^way].lastUse.load(std::memory_order_relaxed);
        if (now-use>now-oldest) {oldest=use;victim=base^way;}
    }
    PoseSlot& slot=walkPoseTable[victim];
    uint32_t was;
    if (!claim(slot.sequence,was)) return false;
    slot.hash=hash;slot.pose=pose;
    slot.lastUse.store(now,std::memory_order_relaxed);
    slot.sequence.store(was+2u,std::memory_order_release);
    *slotOut=victim;*sequenceOut=was+2u;
    return true;
}
inline void walkKeyFill(WalkKey& key,const CollisionWitness* witness,uint32_t poseSlot,uint32_t poseSequence)
{
    memset(&key,0,sizeof(key));
    key.pointers[0]=witness->first;key.pointers[1]=witness->second;
    key.pointers[2]=witness->firstGeometry;key.pointers[3]=witness->secondGeometry;
    key.poseSlot=poseSlot;key.poseSequence=poseSequence;
    key.firstGeneration=witness->firstGeometry->generation;key.secondGeneration=witness->secondGeometry->generation;
    key.side=witness->side;
    memcpy(key.points,witness->firstPoint,16);memcpy(key.points+4,witness->secondPoint,16);
    key.distance=witness->distance;
}
inline uint32_t walkBase(uint32_t hash) {return hash&(walkMemoEntries-4u);}
bool walkMemoFind(const WalkKey& key,uint32_t hash,uint32_t epoch,WalkValue* out)
{
    const uint32_t base=walkBase(hash);
    for (uint32_t way=0;way<4;++way) {
        WalkEntry& entry=walkMemoTable[base^way];
        const uint32_t before=entry.sequence.load(std::memory_order_acquire);
        if ((before&1u) || before==0 || entry.hash!=hash || entry.epoch!=epoch) continue;
        const bool same=memcmp(&entry.key,&key,sizeof(key))==0;
        if (same) *out=entry.value;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (same && entry.sequence.load(std::memory_order_relaxed)==before) return true;
    }
    return false;
}
void walkMemoStore(const WalkKey& key,uint32_t hash,uint32_t epoch,const WalkValue& value)
{
    const uint32_t base=walkBase(hash);
    // Prefer an empty, stale or dead-pose way; otherwise the cheapest walk.
    WalkEntry* slot=&walkMemoTable[base];
    int bestScore=1<<30;
    for (uint32_t way=0;way<4;++way) {
        WalkEntry* candidate=&walkMemoTable[base^way];
        int score;
        if (candidate->sequence.load(std::memory_order_relaxed)==0 || candidate->epoch!=epoch) score=-2;
        else if (walkPoseTable[candidate->key.poseSlot%kPoseSlots].sequence.load(std::memory_order_relaxed)!=candidate->key.poseSequence) score=-1;
        else score=candidate->value.steps;
        if (score<bestScore) {bestScore=score;slot=candidate;}
    }
    uint32_t was;
    if (!claim(slot->sequence,was)) return;
    slot->hash=hash;slot->epoch=epoch;slot->key=key;slot->value=value;
    slot->sequence.store(was+2u,std::memory_order_release);
    collisionMemoStores.fetch_add(1,std::memory_order_relaxed);
}
inline bool walkWitnessEqual(const CollisionWitness& x,const CollisionWitness& y)
{
    return x.side==y.side && x.first==y.first && x.second==y.second &&
        memcmp(x.firstPoint,y.firstPoint,16)==0 && memcmp(x.secondPoint,y.secondPoint,16)==0 &&
        memcmp(&x.distance,&y.distance,4)==0;
}
}
// Adaptive memo gate (per thread, no shared writes): sampled lookups decide
// whether the other 7 of 8 queries use the memo. Opens at >=10% sampled
// hits (measured while closed, from the sparser sample-only table), closes
// below 6% (measured while open); 64 samples per decision.
struct MemoGate {uint32_t tick=0,samples=0,hits=0;bool open=true;};
thread_local MemoGate memoGate;
inline void memoGateRecord(bool hit)
{
    ++memoGate.samples;memoGate.hits+=hit ? 1u:0u;
    if (memoGate.samples<64u) return;
    const bool open=memoGate.open ? memoGate.hits*100u>=6u*64u : memoGate.hits*100u>=10u*64u;
    if (open!=memoGate.open) collisionMemoGateFlips.fetch_add(1,std::memory_order_relaxed);
    memoGate.open=open;memoGate.samples=memoGate.hits=0;
}
// Serial verify references walk without the memo (independent reference).
void collisionWitnessMemoBypass(bool bypass) {walkMemoBypass=bypass;}
// Advanced once per world frame (pose replacement age).
void collisionWitnessMemoTick() {walkMemoClock.fetch_add(1,std::memory_order_relaxed);}
static int32_t collisionWitnessQueryBody(CollisionWitness* witness, const float firstFrame[16],
                              const float firstPosition[4], const float secondFrame[16],
                              const float secondPosition[4], uint32_t* randomState,
                              uint64_t* counters, int32_t* predictPending, const std::atomic<int>* stop)
{
    if (!collisionWitnessMemo || walkMemoBypass || !walkMemoEnsure())
        return collisionWitnessQueryWalk(witness,firstFrame,firstPosition,secondFrame,secondPosition,randomState,
            counters,predictPending,stop,nullptr,nullptr,nullptr);
    // Adaptive gate (exact either way: a memo hit replays a bit-identical
    // walk). Outside frozen phases (race start) hits are 1.5-4.5% and every
    // lookup/store misses cache in the 16 MB table, so each thread only uses
    // the memo while its sampled hit rate (1 in 8 open, 1 in 16 closed) is
    // high. Vita memo_gate_off.txt / PC HP2_MEMO_GATE=0 disables the gate.
    bool gateSample=true;
    if (collisionMemoGate) {
        // 1 in 8 while open, 1 in 16 while closed (less sampled overhead).
        gateSample=(++memoGate.tick&(memoGate.open ? 7u:15u))==0u;
        if (!memoGate.open && !gateSample) {
            return collisionWitnessQueryWalk(witness,firstFrame,firstPosition,secondFrame,secondPosition,randomState,
                counters,predictPending,stop,nullptr,nullptr,nullptr);
        }
    }
    const bool detached=counters!=nullptr;
    const uint32_t epoch=collisionWitnessMemoEpoch.load(std::memory_order_relaxed);
    PoseBlock pose;
    memcpy(pose.values,firstFrame,64);memcpy(pose.values+16,firstPosition,16);
    memcpy(pose.values+20,secondFrame,64);memcpy(pose.values+36,secondPosition,16);
    const uint32_t poseHash=hashWords(&pose,sizeof(pose),2166136261u);
    uint32_t poseSlot=0,poseSequence=0;
    bool havePose=poseFind(pose,poseHash,&poseSlot,&poseSequence);
    collisionMemoLookups.fetch_add(1,std::memory_order_relaxed);
    WalkKey key;
    uint32_t hash=0;
    WalkValue entry{};
    if (havePose) {
        walkKeyFill(key,witness,poseSlot,poseSequence);
        hash=hashWords(&key,sizeof(key),poseHash);
    }
    const bool memoHit=havePose && walkMemoFind(key,hash,epoch,&entry);
    if (collisionMemoGate && gateSample) memoGateRecord(memoHit);
    if (memoHit) {
        collisionMemoHits.fetch_add(1,std::memory_order_relaxed);
        collisionMemoSavedSteps.fetch_add((uint64_t)entry.steps,std::memory_order_relaxed);
#if !defined(__vita__) && !defined(__SWITCH__)
        CollisionWitness check=*witness;uint32_t checkState=randomState ? *randomState:0u;int32_t checkPending=0;
        uint64_t checkCounters[4]{};
        int32_t checkResult=0;
        if (collisionWitnessMemo==2) {
            walkMemoBypass=true;
            checkResult=collisionWitnessQueryWalk(&check,firstFrame,firstPosition,secondFrame,secondPosition,
                randomState ? &checkState:nullptr,checkCounters,predictPending ? &checkPending:nullptr,nullptr,nullptr,nullptr,nullptr);
            walkMemoBypass=false;
        }
#endif
        if(!detached) ++collisionWitnessQueries; else ++counters[0];
        witness->side=entry.side;witness->first=entry.first;witness->second=entry.second;
        memcpy(witness->firstPoint,entry.firstPoint,16);memcpy(witness->secondPoint,entry.secondPoint,16);
        witness->distance=entry.distance;
        uint64_t restarts=0;
        if (entry.pending==1) {
            if (predictPending) *predictPending=1;
            else {
                witness->first = &witness->firstGeometry->vertices[vectorRandomBounded((uint32_t)witness->firstGeometry->vertexCount, randomState)];
                witness->second = &witness->secondGeometry->vertices[vectorRandomBounded((uint32_t)witness->secondGeometry->vertexCount, randomState)];
                restarts=1;
            }
        }
        if (!detached) {collisionWitnessIterations+=(uint64_t)entry.steps;collisionWitnessRestarts+=restarts;}
        else {counters[1]+=(uint64_t)entry.steps;counters[2]+=restarts;}
#if !defined(__vita__) && !defined(__SWITCH__)
        if (collisionWitnessMemo==2) {
            collisionMemoVerified.fetch_add(1,std::memory_order_relaxed);
            const bool same=walkWitnessEqual(check,*witness) && checkResult==entry.result &&
                (!randomState || checkState==*randomState) && (!predictPending || checkPending==*predictPending);
            if (!same) collisionMemoMismatches.fetch_add(1,std::memory_order_relaxed);
        }
#endif
        return entry.result;
    }
    const CollisionWitness start=*witness;
    CollisionWitness converged;int32_t convergedPending=-1,steps=0;
    const int32_t result=collisionWitnessQueryWalk(witness,firstFrame,firstPosition,secondFrame,secondPosition,randomState,
        counters,predictPending,stop,&converged,&convergedPending,&steps);
    if (convergedPending>=0 && steps<32767) {
        if (!havePose) {
            havePose=poseInsert(pose,poseHash,&poseSlot,&poseSequence);
            if (havePose) {walkKeyFill(key,&start,poseSlot,poseSequence);hash=hashWords(&key,sizeof(key),poseHash);}
        }
        if (havePose) {
            WalkValue value;
            value.first=converged.first;value.second=converged.second;
            memcpy(value.firstPoint,converged.firstPoint,16);memcpy(value.secondPoint,converged.secondPoint,16);
            value.distance=converged.distance;value.side=converged.side;
            value.steps=(int16_t)steps;value.pending=(int8_t)convergedPending;value.result=(int8_t)result;
            walkMemoStore(key,hash,epoch,value);
        }
    }
    return result;
}
// The walk itself. With converged set, it reports the converged witness and
// whether the terminal restart happened (*convergedPending 0/1), or -1 when
// the walk is not replayable (a restart before termination, exhaustion, stop).
// Walk prefetch (read-only, exact): 1 = each step prefetches the current
// features' neighbours (the next step's candidates); 2 = also the two
// geometries' feature/plane arrays at query start. Vita walk_prefetch.txt "<n>".
int collisionWalkPrefetch=1;
#if defined(__GNUC__)
#define HP2_WALK_PREFETCH(address) __builtin_prefetch(address)
#else
#define HP2_WALK_PREFETCH(address) ((void)(address))
#endif
static inline void prefetchNeighbours(const CollisionFeature* feature,bool planes2)
{
    const CollisionPlane* planes=feature->adjacent;
    for(int32_t index=0;index<feature->adjacentCount;++index) {
        const CollisionFeature* next=planes[index].feature;
        HP2_WALK_PREFETCH(next);
        // The neighbour itself was prefetched by the previous step (mode 3).
        if(planes2)HP2_WALK_PREFETCH(next->adjacent);
    }
}
static inline void prefetchGeometry(const CollisionGeometry* geometry)
{
    const auto range=[](const void* begin,size_t bytes) {
        const char* at=static_cast<const char*>(begin);
        for(size_t offset=0;offset<bytes;offset+=32)HP2_WALK_PREFETCH(at+offset);
    };
    range(geometry->vertices,sizeof(CollisionFeature)*(size_t)geometry->vertexCount);
    range(geometry->edges,sizeof(CollisionFeature)*(size_t)geometry->edgeCount);
    range(geometry->faces,sizeof(CollisionFeature)*(size_t)geometry->faceCount);
    range(geometry->planes,sizeof(CollisionPlane)*(size_t)geometry->planeCount);
}
static int32_t collisionWitnessQueryWalk(CollisionWitness* witness, const float firstFrame[16],
                              const float firstPosition[4], const float secondFrame[16],
                              const float secondPosition[4], uint32_t* randomState,
                              uint64_t* counters, int32_t* predictPending, const std::atomic<int>* stop,
                              CollisionWitness* converged, int32_t* convergedPending, int32_t* steps)
{
    if(collisionWalkPrefetch==2) {prefetchGeometry(witness->firstGeometry);prefetchGeometry(witness->secondGeometry);}
    const bool detached=counters!=nullptr;
    if(!detached) ++collisionWitnessQueries; else ++counters[0];
    // Both relative transforms are pure functions of the two frames and
    // positions; consecutive walks (restarts, memo misses from other start
    // features) often share them, so keep the last pair per thread.
    struct TransformCache {bool valid;float pose[40];float firstToSecond[16],secondToFirst[16];};
    static thread_local TransformCache transformCache{};
    float pose[40];
    memcpy(pose,firstFrame,64);memcpy(pose+16,firstPosition,16);memcpy(pose+20,secondFrame,64);memcpy(pose+36,secondPosition,16);
    if (!transformCache.valid || memcmp(transformCache.pose,pose,sizeof(pose))!=0) {
        collisionRelativeTransform(transformCache.firstToSecond, secondFrame, secondPosition, firstFrame, firstPosition);
        collisionRelativeTransform(transformCache.secondToFirst, firstFrame, firstPosition, secondFrame, secondPosition);
        memcpy(transformCache.pose,pose,sizeof(pose));transformCache.valid=true;
    }
    const float* firstToSecond=transformCache.firstToSecond;
    const float* secondToFirst=transformCache.secondToFirst;
    CollisionTransformedVertex firstVertex{}, secondVertex{};
    CollisionTransformedEdge firstEdge{}, secondEdge{};
    CollisionFeature* firstHistory[80];
    CollisionFeature* secondHistory[80];
    int32_t iteration = 0, restarts = 0, result = 0;
    do {
        if (stop && stop->load(std::memory_order_relaxed)) return -1;
        const int firstKind = witness->first->kind, secondKind = witness->second->kind;
        if (collisionWalkPrefetch) {
            const bool planes2=collisionWalkPrefetch==3;
            prefetchNeighbours(witness->first,planes2);prefetchNeighbours(witness->second,planes2);
        }
        const uint32_t stepStarted = !detached && collisionWitnessProbeClock ? collisionWitnessProbeClock() : 0;
        if (firstKind == 0 && secondKind == 0) {
            witness->side = 1;
            result = collisionWitnessVertices(witness, firstToSecond, secondToFirst, &firstVertex, &secondVertex);
        } else if (firstKind == 0 && secondKind == 1) {
            witness->side = 1;
            result = collisionWitnessVertexEdge(witness, firstToSecond, secondToFirst, &firstVertex, &secondEdge);
        } else if (firstKind == 1 && secondKind == 0) {
            witness->side = 2;
            result = collisionWitnessVertexEdge(witness, secondToFirst, firstToSecond, &secondVertex, &firstEdge);
        } else if (firstKind == 0 && secondKind == 2) {
            witness->side = 1;
            result = collisionWitnessVertexFace(witness, firstToSecond, &firstVertex);
        } else if (firstKind == 2 && secondKind == 0) {
            witness->side = 2;
            result = collisionWitnessVertexFace(witness, secondToFirst, &secondVertex);
        } else if (firstKind == 1 && secondKind == 1) {
            witness->side = 1;
            result = collisionWitnessEdges(witness, firstToSecond, secondToFirst, &firstEdge, &secondEdge);
        } else if (firstKind == 1 && secondKind == 2) {
            witness->side = 1;
            result = collisionWitnessEdgeFace(witness, firstToSecond, secondToFirst, &firstEdge);
        } else if (firstKind == 2 && secondKind == 1) {
            witness->side = 2;
            result = collisionWitnessEdgeFace(witness, secondToFirst, firstToSecond, &secondEdge);
        }
        if (!detached && collisionWitnessProbeClock && firstKind < 3 && secondKind < 3) {
            collisionWitnessKindMicros[firstKind * 3 + secondKind] += collisionWitnessProbeClock() - stepStarted;
            ++collisionWitnessKindSteps[firstKind * 3 + secondKind];
        }
        if (steps) ++*steps;
        if (collisionWitnessRemember(witness, firstHistory, secondHistory, iteration)) {
            if (converged && restarts==0 && result!=0) {*converged=*witness;*convergedPending=1;}
            ++restarts;
            iteration = 0;
            if (predictPending) {
                if (result == 0) {*predictPending = -1;return 0;}
                *predictPending = 1;
                ++counters[1];
                break;
            }
            witness->first = &witness->firstGeometry->vertices[vectorRandomBounded((uint32_t)witness->firstGeometry->vertexCount, randomState)];
            witness->second = &witness->secondGeometry->vertices[vectorRandomBounded((uint32_t)witness->secondGeometry->vertexCount, randomState)];
        } else ++iteration;
        if (!detached) ++collisionWitnessIterations; else ++counters[1];
    } while (result == 0 && restarts < 3 && iteration < 80);
    if (converged && restarts==0 && result!=0) {*converged=*witness;*convergedPending=0;}
    if (!detached) {
        collisionWitnessRestarts+=(uint64_t)restarts;
        if (result == 0) ++collisionWitnessExhausted;
    } else {
        counters[2]+=(uint64_t)restarts;
        if (result == 0) ++counters[3];
    }
    return result;
}

int32_t collisionWitnessVertices(CollisionWitness* witness, const float featureFrame[16],
                                 const float otherFrame[16], CollisionTransformedVertex* feature,
                                 CollisionTransformedVertex* other)
{
    CollisionFeature* first=witness->side==1 ? witness->first : witness->second;
    CollisionFeature* second=witness->side==2 ? witness->first : witness->second;
    cachedTransformVertex(feature,featureFrame,first);
    for(int index=0;index<second->adjacentCount;++index) {
        const CollisionPlane* plane=&second->adjacent[index];
        if(dot3(plane->normal,feature->point)-plane->offset<0.0f) {
            if(witness->side==2) witness->first=plane->feature;
            else witness->second=plane->feature;
            return 0;
        }
    }
    cachedTransformVertex(other,otherFrame,second);
    for(int index=0;index<first->adjacentCount;++index) {
        const CollisionPlane* plane=&first->adjacent[index];
        if(dot3(plane->normal,other->point)-plane->offset<0.0f) {
            if(witness->side==1) witness->first=plane->feature;
            else witness->second=plane->feature;
            return 0;
        }
    }
    float difference[3];
    for(int lane=0;lane<3;++lane) difference[lane]=first->vector[lane]-other->point[lane];
    witness->distance=sqrtf(dot3(difference,difference));
    float* firstPoint=witness->side==1 ? witness->firstPoint : witness->secondPoint;
    float* secondPoint=witness->side==2 ? witness->firstPoint : witness->secondPoint;
    for(int lane=0;lane<3;++lane) { firstPoint[lane]=first->vector[lane];secondPoint[lane]=second->vector[lane]; }
    return 0.00001f<witness->distance ? 1 : 2;
}

void collisionTransformDirectionTo(float output[4], const float frame[16], const float value[4])
{
    float matrix[16];
    memcpy(matrix, frame, sizeof(matrix));
    matrix[12] = matrix[13] = matrix[14] = 0.0f;
    matrix[15] = 1.0f;
    vectorMatrixTransformPoint(output, matrix, value);
}

CollisionFeature* collisionClipVertex(CollisionClip* clip, const CollisionTransformedEdge* edge,
                                      const float* point, const CollisionPlane* planes, int32_t count)
{
    collisionClipPlanes(clip, edge, planes, count);
    if (clip->lowerFeature && clip->lowerFeature == clip->upperFeature) return clip->lowerFeature;
    CollisionFeature* limits[2] = {clip->lowerFeature, clip->upperFeature};
    const float parameters[2] = {clip->lower, clip->upper};
    for (int side = 0; side < 2; ++side) {
        CollisionFeature* feature = limits[side];
        if (feature) {
            float projected[4], reference[4], delta[4];
            collisionGeometryPointAlong(projected, edge, parameters[side]);
            if (point) memcpy(reference, point, sizeof(reference));
            else { for (int lane = 0; lane < 3; ++lane) reference[lane] = feature->vector[lane]; reference[3] = 0.0f; }
            collisionGeometrySubtract4(delta, projected, reference);
            if (collisionGeometryDot3(delta, delta) < 0.00001f) clip->boundary = 1;
            else {
                const float derivative = collisionGeometryDot3(edge->direction, delta);
                if ((side == 0 && 0.0f < derivative) || (side == 1 && derivative < 0.0f)) return feature;
            }
        }
    }
    return NULL;
}

CollisionFeature* collisionClipFace(CollisionClip* clip, const CollisionTransformedEdge* edge,
                                    const float* plane, const CollisionPlane* planes, int32_t count)
{
    const int32_t accepted = collisionClipPlanes(clip, edge, planes, count);
    CollisionFeature* first = clip->lowerFeature;
    CollisionFeature* second = clip->upperFeature;
    if (first && first == second) return first;
    int32_t oppositeKinds = 0;
    if (!accepted) {
        if (first->kind == 2 && second->kind == 1) return first;
        if (second->kind == 2 && first->kind == 1) return second;
        oppositeKinds = 1;
    }
    CollisionFeature* features[2] = {first, second};
    const float parameters[2] = {clip->lower, clip->upper};
    for (int side = 0; side < 2; ++side) {
        CollisionFeature* feature = features[side];
        if (feature && feature->kind == 2) {
            float projected[4];
            collisionGeometryPointAlong(projected, edge, parameters[side]);
            const float* normal = plane ? plane : feature->vector;
            const float offset = plane ? plane[3] : feature->offset;
            const float distance = collisionGeometryDot3(normal, projected) - offset;
            if (fabsf(distance) < 0.0001f) clip->boundary = 1;
            else {
                const float derivative = collisionGeometryDot3(normal, edge->direction);
                const float signedDerivative = distance * derivative;
                if ((side == 0 && 0.0f < signedDerivative) || (side == 1 && signedDerivative < 0.0f)) return feature;
                if (oppositeKinds) return side == 0 ? second : first;
            }
        }
    }
    return NULL;
}

void collisionFeaturePoints(const CollisionFeature* feature, float output[4][3], const float frame[16])
{
    const CollisionFeature* vertices[8];
    int32_t count = 0;
    if (feature->kind == 0) vertices[count++] = feature;
    else if (feature->kind == 1) {
        vertices[count++] = feature->adjacent[0].feature;
        vertices[count++] = feature->adjacent[1].feature;
    } else if (feature->kind == 2) {
        for (int index = 0; index < feature->adjacentCount; ++index) {
            const CollisionFeature* edge = feature->adjacent[index].feature;
            for (int end = 0; end < 2; ++end) {
                const CollisionFeature* vertex = edge->adjacent[end].feature;
                int found = 0;
                while (found < count && vertices[found] != vertex) ++found;
                if (found == count) vertices[count++] = vertex;
            }
        }
        count = feature->adjacentCount;
    }
    for (int index = 0; index < count; ++index) {
        const CollisionFeature* vertex = vertices[index];
        const float point[4] = {vertex->vector[0], vertex->vector[1], vertex->vector[2], 0.0f};
        float transformed[4];
        vectorMatrixTransformPoint(transformed, frame, point);
        for (int lane = 0; lane < 3; ++lane) output[index][lane] = transformed[lane] - frame[12 + lane];
    }
    if (count > 0) for (int index = count; index < 4; ++index) memcpy(output[index], output[index - 1], sizeof(output[index]));
}

#include "indep/inc/Replay.hpp"

// PAL1D0948..1D0A30. Feature codes are authored geometry indices; host
// pointers are reconstructed only against the same two actual polyhedra.
void collisionWitnessSnapshot(CollisionWitness* witness,ReplaySnapshot* snapshot)
{
    if(!snapshot)return;
    if(!witness || !witness->firstGeometry || !witness->secondGeometry) {
        snapshot->result=REPLAY_SNAPSHOT_INVALID_BINDING;return;
    }
    replaySnapshotBeginSection(snapshot,witness,"VCWitness");
    uint32_t first=collisionGeometryFeatureCode(witness->firstGeometry,witness->first);
    uint32_t second=collisionGeometryFeatureCode(witness->secondGeometry,witness->second);
    replaySnapshotMoveUnsigned(snapshot,&first,4);replaySnapshotMoveUnsigned(snapshot,&second,4);
    if(snapshot->restoring && snapshot->result==REPLAY_SNAPSHOT_OK) {
        const auto valid=[](const CollisionGeometry& geometry,uint32_t code) {
            if(code==UINT32_MAX)return true;
            const auto index=code>>4;
            switch(code&15u) {
            case 0:return index<static_cast<uint32_t>(geometry.vertexCount);
            case 1:return index<static_cast<uint32_t>(geometry.edgeCount);
            case 2:return index<static_cast<uint32_t>(geometry.faceCount);
            default:return false;
            }
        };
        if(!valid(*witness->firstGeometry,first) || !valid(*witness->secondGeometry,second))
            snapshot->result=REPLAY_SNAPSHOT_INVALID_TRANSFER;
        else {
            witness->first=collisionGeometryFeature(witness->firstGeometry,first);
            witness->second=collisionGeometryFeature(witness->secondGeometry,second);
        }
    }
    replaySnapshotMoveInteger(snapshot,&witness->side,4);
    replaySnapshotMoveData(snapshot,witness->firstPoint,12,-1,0);
    replaySnapshotMoveData(snapshot,witness->secondPoint,12,-1,0);
    replaySnapshotMoveFloat(snapshot,&witness->distance);
    replaySnapshotEndSection(snapshot);
}

int32_t collisionWitnessRemember(const CollisionWitness* witness, CollisionFeature** firstHistory,
                                 CollisionFeature** secondHistory, int32_t index)
{
    firstHistory[index]=witness->first;
    secondHistory[index]=witness->second;
    --index;
    for(int remaining=10;index>=0 && remaining>0;--index,--remaining)
        if(firstHistory[index]==witness->first && secondHistory[index]==witness->second) return 1;
    return 0;
}

float collisionGeometryMaximumViolation(const CollisionGeometry* geometry)
{
    float maximum=0.0f;
    for(int face=0;face<geometry->faceCount;++face)
        for(int vertex=0;vertex<geometry->vertexCount;++vertex) {
            const float distance=collisionGeometryDot3(geometry->faces[face].vector,geometry->vertices[vertex].vector)-geometry->faces[face].offset;
            if(maximum<distance) maximum=distance;
        }
    return maximum;
}

// On-device witness micro-benchmark (diagnostic, main thread): records the
// first 256 committed separation queries, then replays them on copies.
int collisionWitnessBench=0;
void (*collisionWitnessBenchLog)(const char*)=nullptr;
namespace {
struct WitnessBenchCase {CollisionWitness witness;float firstFrame[16],firstPosition[4],secondFrame[16],secondPosition[4];uint32_t state;};
WitnessBenchCase witnessBenchCases[256];
unsigned witnessBenchCount=0;
void runWitnessBench()
{
    if(!collisionWitnessClockMicros || !collisionWitnessBenchLog)return;
    char text[512];
    uint32_t passMicros[10]={};
    uint64_t counters[4]={};
    for(int pass=0;pass<10;++pass) {
        const uint32_t start=collisionWitnessClockMicros();
        for(unsigned index=0;index<witnessBenchCount;++index) {
            const auto& entry=witnessBenchCases[index];
            CollisionWitness witness=entry.witness;uint32_t state=entry.state;
            collisionWitnessQueryBody(&witness,entry.firstFrame,entry.firstPosition,entry.secondFrame,entry.secondPosition,&state,counters);
        }
        passMicros[pass]=collisionWitnessClockMicros()-start;
    }
    uint32_t warm=0;for(int pass=1;pass<10;++pass)warm+=passMicros[pass];
    std::snprintf(text,sizeof(text),"witness_bench cases=%u first_pass_us_per_query=%.2f warm_us_per_query=%.2f iterations_per_query=%.2f",
        witnessBenchCount,double(passMicros[0])/witnessBenchCount,double(warm)/9.0/witnessBenchCount,double(counters[1])/(10.0*witnessBenchCount));
    collisionWitnessBenchLog(text);
    {
        // Cold-cache walk A/B: evict (stream a 4 MB buffer) before each
        // query, time only the query, for prefetch modes 0/1/2; compare
        // every result against mode 0.
        static std::vector<uint32_t> evict(1u<<20,1u);
        volatile uint32_t sink=0;
        const int savedPrefetch=collisionWalkPrefetch;
        uint64_t modeMicros[4]={};uint32_t mismatches=0;
        for(int pass=0;pass<1;++pass)for(unsigned index=0;index<witnessBenchCount;++index) {
            const auto& entry=witnessBenchCases[index];
            CollisionWitness reference{};uint32_t referenceState=0;int32_t referenceResult=0;
            for(int mode=0;mode<4;++mode) {
                uint32_t sum=0;for(size_t at=0;at<evict.size();at+=8)sum+=evict[at];sink=sink+sum;
                collisionWalkPrefetch=mode;
                CollisionWitness witness=entry.witness;uint32_t state=entry.state;
                const uint32_t start=collisionWitnessClockMicros();
                const int32_t result=collisionWitnessQueryWalk(&witness,entry.firstFrame,entry.firstPosition,entry.secondFrame,entry.secondPosition,
                    &state,counters,nullptr,nullptr,nullptr,nullptr,nullptr);
                modeMicros[mode]+=collisionWitnessClockMicros()-start;
                if(mode==0) {reference=witness;referenceState=state;referenceResult=result;}
                else if(result!=referenceResult || state!=referenceState || memcmp(&witness,&reference,sizeof(witness))!=0)++mismatches;
            }
        }
        collisionWalkPrefetch=savedPrefetch;
        const double n=1.0*witnessBenchCount;
        std::snprintf(text,sizeof(text),"witness_bench_cold none_us=%.2f neighbours_us=%.2f geometry_us=%.2f neighbour_planes_us=%.2f mismatches=%u",
            double(modeMicros[0])/n,double(modeMicros[1])/n,double(modeMicros[2])/n,double(modeMicros[3])/n,mismatches);
        collisionWitnessBenchLog(text);
    }
    // Per kind (warm, probe clock overhead included and reported).
    const uint32_t clockStart=collisionWitnessClockMicros();
    for(int i=0;i<1000;++i)(void)collisionWitnessClockMicros();
    const double clockUs=double(collisionWitnessClockMicros()-clockStart)/1000.0;
    uint64_t kindMicros[9],kindSteps[9];
    std::memcpy(kindMicros,collisionWitnessKindMicros,sizeof(kindMicros));std::memcpy(kindSteps,collisionWitnessKindSteps,sizeof(kindSteps));
    const uint64_t q=collisionWitnessQueries,it=collisionWitnessIterations,r=collisionWitnessRestarts,e=collisionWitnessExhausted;
    auto* savedProbe=collisionWitnessProbeClock;collisionWitnessProbeClock=collisionWitnessClockMicros;
    std::memset(collisionWitnessKindMicros,0,sizeof(kindMicros));std::memset(collisionWitnessKindSteps,0,sizeof(kindSteps));
    for(int pass=0;pass<5;++pass)for(unsigned index=0;index<witnessBenchCount;++index) {
        const auto& entry=witnessBenchCases[index];
        CollisionWitness witness=entry.witness;uint32_t state=entry.state;
        collisionWitnessQueryBody(&witness,entry.firstFrame,entry.firstPosition,entry.secondFrame,entry.secondPosition,&state);
    }
    int used=std::snprintf(text,sizeof(text),"witness_bench_kinds clock_us=%.3f ",clockUs);
    for(int pair=0;pair<9;++pair)if(collisionWitnessKindSteps[pair])
        used+=std::snprintf(text+used,sizeof(text)-used,"k%d%d steps/q=%.2f us=%.3f ",pair/3,pair%3,
            double(collisionWitnessKindSteps[pair])/(5.0*witnessBenchCount),double(collisionWitnessKindMicros[pair])/collisionWitnessKindSteps[pair]);
    collisionWitnessBenchLog(text);
    collisionWitnessProbeClock=savedProbe;
    std::memcpy(collisionWitnessKindMicros,kindMicros,sizeof(kindMicros));std::memcpy(collisionWitnessKindSteps,kindSteps,sizeof(kindSteps));
    collisionWitnessQueries=q;collisionWitnessIterations=it;collisionWitnessRestarts=r;collisionWitnessExhausted=e;
}
}
void collisionWitnessBenchRecord(const CollisionWitness* witness,const float firstFrame[16],const float firstPosition[4],
    const float secondFrame[16],const float secondPosition[4],uint32_t state)
{
    if(!collisionWitnessBench || witnessBenchCount>=256)return;
    auto& entry=witnessBenchCases[witnessBenchCount];
    entry.witness=*witness;entry.state=state;
    std::memcpy(entry.firstFrame,firstFrame,sizeof(entry.firstFrame));std::memcpy(entry.secondFrame,secondFrame,sizeof(entry.secondFrame));
    std::memcpy(entry.firstPosition,firstPosition,sizeof(entry.firstPosition));std::memcpy(entry.secondPosition,secondPosition,sizeof(entry.secondPosition));
    if(++witnessBenchCount==256) {runWitnessBench();collisionWitnessBench=0;}
}
