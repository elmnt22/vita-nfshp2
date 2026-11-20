#include "pgo.hpp"

#ifdef HP2_PGO_GENERATE
#include <switch.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <ostream>
#include <stdexcept>
#include <system_error>

extern "C" {
#include <gcov.h>
extern const gcov_info* const __start_hp2_gcov_info[];
extern const gcov_info* const __stop_hp2_gcov_info[];
}
#endif

namespace hp2::nx {

const char* PgoSession::BuildMode() noexcept {return HP2_SWITCH_BUILD_MODE;}

#ifdef HP2_PGO_GENERATE
namespace {

void WriteBytes(const void* data,unsigned size,void* context)
{
    std::fwrite(data,1,size,static_cast<std::FILE*>(context));
}

void WriteFilename(const char* name,void* context)
{
    __gcov_filename_to_gcfn(name,WriteBytes,context);
}

void* AllocateBytes(unsigned size,void*) {return std::malloc(size);}

void WriteProfile(const std::filesystem::path& temporary,const std::filesystem::path& destination)
{
    if(+__start_hp2_gcov_info==+__stop_hp2_gcov_info)
        throw std::runtime_error("no embedded GCC profile counters");
    auto* file=std::fopen(temporary.c_str(),"wb");
    if(!file)throw std::system_error(errno,std::generic_category(),"open profile stream");
    errno=0;
    for(auto it=+__start_hp2_gcov_info;it!=+__stop_hp2_gcov_info;++it)
        __gcov_info_to_gcda(*it,WriteFilename,WriteBytes,AllocateBytes,file);
    int error=std::ferror(file) ? (errno ? errno:EIO):0;
    if(std::fclose(file)!=0 && !error)error=errno ? errno:EIO;
    if(!error) {
        if(std::rename(temporary.c_str(),destination.c_str())==0)return;
        error=errno;
    }
    std::remove(temporary.c_str());
    throw std::system_error(error,std::generic_category(),"save profile stream");
}

}
#endif

void PgoSession::Checkpoint(bool force) noexcept
{
#ifdef HP2_PGO_GENERATE
    using Clock=std::chrono::steady_clock;
    constexpr auto period=std::chrono::seconds(20);
    const auto now=Clock::now();
    if(nextCheckpoint_==Clock::time_point{})nextCheckpoint_=now+period;
    if(!force && now<nextCheckpoint_)return;
    nextCheckpoint_=now+period;
    try {
        const auto directory=saves_/"pgo";
        std::filesystem::create_directories(directory);
        if(session_.empty())
            session_=std::to_string(std::time(nullptr))+"-"+std::to_string(armGetSystemTick());
        const auto name="nfshp2-"+session_+"-"+std::to_string(++checkpoint_)+".stream";
        const auto destination=directory/name;
        log_<<"pgo.checkpoint "<<destination.string()<<std::endl;
        WriteProfile(directory/(name+".tmp"),destination);
        log_<<"pgo.saved "<<destination.string()<<std::endl;
    } catch(const std::exception& error) {
        log_<<"pgo.error "<<error.what()<<std::endl;
    }
#else
    (void)force;
#endif
}

}
