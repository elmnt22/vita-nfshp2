#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <thread>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace hp2::nx {
// The application only formats/enqueues bytes. All write/flush calls after
// opening are owned by the low-priority worker. A full queue drops diagnostics
// rather than waiting for SD I/O; shutdown drains the retained records.
class AsyncLog final:public std::ostream {
    class Buffer final:public std::streambuf {
    public:
        Buffer():queue_(256*1024) {setp(producer_.data(),producer_.data()+producer_.size());}
        ~Buffer() override {Close();}
        void Open(const std::filesystem::path& path,std::ios::openmode mode) {
            Close();file_.open(path,mode|std::ios::out);
            if(file_.is_open())Start(file_);
        }
        void Start(std::ostream& destination) {
            destination_=&destination;stopping_=false;head_=size_=0;
            dropped_=0;pendingDropped_=0;failed_=false;open_=true;
            try {worker_=std::thread([this]{Run();});}
            catch(...) {open_=false;throw;}
        }
        void Close() {
            if(worker_.joinable()) {
                sync();
                {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;}
                wake_.notify_one();worker_.join();
            }
            open_=false;destination_=nullptr;
            if(file_.is_open())file_.close();
        }
        bool IsOpen() const noexcept {return open_.load();}
        std::uint64_t DroppedBytes() const noexcept {return dropped_.load();}
        bool Failed() const noexcept {return failed_.load();}
    protected:
        int sync() override {Publish();return 0;}
        int_type overflow(int_type value) override {
            Publish();
            if(!traits_type::eq_int_type(value,traits_type::eof())) {
                *pptr()=traits_type::to_char_type(value);pbump(1);
            }
            return traits_type::not_eof(value);
        }
        std::streamsize xsputn(const char* bytes,std::streamsize count) override {
            const auto requested=count;
            while(count>0) {
                if(pptr()==epptr())Publish();
                const auto copied=std::min<std::streamsize>(count,epptr()-pptr());
                std::memcpy(pptr(),bytes,static_cast<std::size_t>(copied));
                pbump(static_cast<int>(copied));bytes+=copied;count-=copied;
            }
            return requested;
        }
    private:
        void Publish() {
            const auto count=static_cast<std::size_t>(pptr()-pbase());
            if(!count)return;
            if(open_.load()) {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto accepted=std::min(count,queue_.size()-size_);
                const auto tail=(head_+size_)%queue_.size();
                const auto first=std::min(accepted,queue_.size()-tail);
                std::memcpy(queue_.data()+tail,pbase(),first);
                std::memcpy(queue_.data(),pbase()+first,accepted-first);size_+=accepted;
                const auto lost=count-accepted;dropped_+=lost;pendingDropped_+=lost;
            }
            setp(producer_.data(),producer_.data()+producer_.size());wake_.notify_one();
        }
        void Run() noexcept {
#ifdef __SWITCH__
            s32 priority{};
            if(R_SUCCEEDED(svcGetThreadPriority(&priority,CUR_THREAD_HANDLE)))
                (void)svcSetThreadPriority(CUR_THREAD_HANDLE,std::min<s32>(priority+8,63));
#endif
            try {
                std::vector<char> batch(32*1024);
                for(;;) {
                    std::size_t count{};std::uint64_t lost{};bool stop{};
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        wake_.wait_for(lock,std::chrono::milliseconds(50),[this]{return stopping_ || size_>=16384;});
                        count=std::min(batch.size(),size_);
                        const auto first=std::min(count,queue_.size()-head_);
                        std::memcpy(batch.data(),queue_.data()+head_,first);
                        std::memcpy(batch.data()+first,queue_.data(),count-first);
                        head_=(head_+count)%queue_.size();size_-=count;
                        lost=pendingDropped_;pendingDropped_=0;stop=stopping_ && !size_;
                    }
                    // Never hold the queue mutex while a filesystem call can block.
                    if(count)destination_->write(batch.data(),static_cast<std::streamsize>(count));
                    if(lost)*destination_<<"log.dropped bytes="<<lost<<'\n';
                    if(count || lost || stop)destination_->flush();
                    if(!*destination_)failed_=true;
                    if(stop)break;
                }
            } catch(...) {failed_=true;}
        }
        std::array<char,4096> producer_{};
        std::vector<char> queue_;
        std::size_t head_{},size_{};
        std::uint64_t pendingDropped_{};
        std::mutex mutex_;
        std::condition_variable wake_;
        std::thread worker_;
        std::ofstream file_;
        std::ostream* destination_{};
        std::atomic<bool> open_{},failed_{};
        std::atomic<std::uint64_t> dropped_{};
        bool stopping_{};
    } buffer_;
public:
    AsyncLog():std::ostream(nullptr) {rdbuf(&buffer_);}
    explicit AsyncLog(std::ostream& destination):AsyncLog() {buffer_.Start(destination);}
    ~AsyncLog() override {buffer_.Close();rdbuf(nullptr);}
    void open(const std::filesystem::path& path,std::ios::openmode mode=std::ios::trunc) {
        buffer_.Open(path,mode);if(!buffer_.IsOpen())setstate(std::ios::failbit);else clear();
    }
    void close() {buffer_.Close();}
    bool is_open() const noexcept {return buffer_.IsOpen();}
    std::uint64_t DroppedBytes() const noexcept {return buffer_.DroppedBytes();}
    bool WriteFailed() const noexcept {return buffer_.Failed();}
};
}
