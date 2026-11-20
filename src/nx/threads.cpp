// Switch 2 pthreads, including driver workers, may use all application cores.
// Shader compiler threads also need more than the default small pthread stack.
#include <switch.h>
#include <pthread.h>
#include <cerrno>
#include <new>

extern "C" int __real_pthread_create(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
namespace {
struct Start {void*(*entry)(void*);void* argument;};
void* Run(void* opaque) {
    const auto start=*static_cast<Start*>(opaque);
    delete static_cast<Start*>(opaque);
    static const u64 cores=[] {
        u64 value{};
        return R_SUCCEEDED(svcGetInfo(&value,InfoType_CoreMask,CUR_PROCESS_HANDLE,0)) ? value:0;
    }();
    if(cores)(void)svcSetThreadCoreMask(CUR_THREAD_HANDLE,-1,static_cast<u32>(cores));
    return start.entry(start.argument);
}
}
extern "C" int __wrap_pthread_create(pthread_t* thread,const pthread_attr_t* attributes,
    void*(*entry)(void*),void* argument) {
    pthread_attr_t local;
    int error=0;
    if(attributes)local=*attributes;
    else if((error=pthread_attr_init(&local)))return error;
    void* suppliedStack=nullptr;size_t size=0;
    error=pthread_attr_getstack(&local,&suppliedStack,&size);
    if(!error && !suppliedStack && size<2*1024*1024)
        error=pthread_attr_setstacksize(&local,2*1024*1024);
    Start* start=nullptr;
    if(!error) {
        start=new(std::nothrow) Start{entry,argument};
        if(!start)error=ENOMEM;
    }
    if(!error)error=__real_pthread_create(thread,&local,Run,start);
    if(error)delete start;
    if(!attributes)pthread_attr_destroy(&local);
    return error;
}
