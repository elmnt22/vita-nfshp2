#pragma once
namespace vita_census {
void Enable();
bool Enabled();
void NextEpoch();
void Dump(const char* tag,void(*line)(const char*,const char*));
}
#include <cstddef>
namespace vita_heap {
// Kernel big-block allocator counters (one log line).
void Stats(char* out,std::size_t size);
}
