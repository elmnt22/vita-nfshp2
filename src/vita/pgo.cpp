// Vita GCC PGO training (HP2_VITA_PGO=generate): a background thread
// writes the embedded gcov counters as one gcov-tool merge-stream file,
// ux0:data/NFSHP2/pgo.stream, every 20 s (counters are cumulative, so the
// last write holds the whole session). Split and merge it on the PC with
// tools/vita/gcov_stream_split.py + gcov-tool merge (see cmake/Vita.cmake).
#ifdef HP2_PGO_GENERATE
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" {
#include <gcov.h>
extern const gcov_info* const __start_hp2_gcov_info[];
extern const gcov_info* const __stop_hp2_gcov_info[];
}
namespace {
std::vector<unsigned char>* buffer=nullptr;
void WriteBytes(const void* data,unsigned size,void*) {
    const auto* bytes=static_cast<const unsigned char*>(data);buffer->insert(buffer->end(),bytes,bytes+size);
}
void WriteFilename(const char* name,void* context) {__gcov_filename_to_gcfn(name,WriteBytes,context);}
void* AllocateBytes(unsigned size,void*) {return std::malloc(size);}
void Note(const char* text) {
    const SceUID file=sceIoOpen("ux0:data/NFSHP2/pgo.log",SCE_O_WRONLY|SCE_O_CREAT|SCE_O_APPEND,0666);
    if(file>=0) {sceIoWrite(file,text,std::strlen(text));sceIoClose(file);}
}
void* PgoThread(void*)
{
    std::vector<unsigned char> bytes;buffer=&bytes;
    char text[96];
    std::snprintf(text,sizeof(text),"pgo start infos=%d\n",static_cast<int>(+__stop_hp2_gcov_info-+__start_hp2_gcov_info));Note(text);
    for(;;) {
        sceKernelDelayThread(20u*1000u*1000u);
        bytes.clear();
        for(auto it=+__start_hp2_gcov_info;it!=+__stop_hp2_gcov_info;++it)
            __gcov_info_to_gcda(*it,WriteFilename,WriteBytes,AllocateBytes,nullptr);
        const SceUID file=sceIoOpen("ux0:data/NFSHP2/pgo.stream.tmp",SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);
        if(file<0) {Note("pgo open failed\n");continue;}
        const int written=sceIoWrite(file,bytes.data(),bytes.size());sceIoClose(file);
        if(written!=static_cast<int>(bytes.size())) {Note("pgo short write\n");continue;}
        sceIoRemove("ux0:data/NFSHP2/pgo.stream");sceIoRename("ux0:data/NFSHP2/pgo.stream.tmp","ux0:data/NFSHP2/pgo.stream");
        std::snprintf(text,sizeof(text),"pgo saved bytes=%u\n",static_cast<unsigned>(bytes.size()));Note(text);
    }
    return nullptr;
}
}
namespace hp2 {
void StartVitaPgo()
{
    pthread_t thread;pthread_attr_t attributes;pthread_attr_init(&attributes);pthread_attr_setstacksize(&attributes,256*1024);
    if(pthread_create(&thread,&attributes,PgoThread,nullptr)==0)pthread_detach(thread); else Note("pgo thread failed\n");
    pthread_attr_destroy(&attributes);
}
}
#else
namespace hp2 {void StartVitaPgo() {}}
#endif
