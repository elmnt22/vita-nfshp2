// Forced include for HP2_ALLOC_TRACK builds only: routes the game's direct
// malloc/calloc/realloc/free through the allocation tracker.
#pragma once
#ifdef __cplusplus
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <memory>
#include <string>
#include <vector>
extern "C" {
#else
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#endif
void* hp2TrackMalloc(size_t size);
void* hp2TrackCalloc(size_t count,size_t size);
void* hp2TrackRealloc(void* p,size_t size);
void hp2TrackFree(void* p);
#ifdef __cplusplus
}
namespace std {using ::hp2TrackMalloc;using ::hp2TrackCalloc;using ::hp2TrackRealloc;using ::hp2TrackFree;}
#endif
#define malloc(n) hp2TrackMalloc(n)
#define calloc(c,n) hp2TrackCalloc(c,n)
#define realloc(p,n) hp2TrackRealloc(p,n)
#define free(p) hp2TrackFree(p)
