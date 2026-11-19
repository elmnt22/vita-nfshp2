// Crash records through kubridge exception handlers; see crash.hpp.
// The handler runs on the faulting thread with about 0x400 bytes of stack to
// spare, so it formats into a static buffer with its own hex writer (no
// snprintf) and makes as few calls as it can: one sceIoWrite, then exit.
#include "crash.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>

extern "C" {
#include "kubridge/kubridge_min.h"
}

namespace vita_crash {
namespace {
SceUID crashFd = -1;
int crashApi;          // 2: v0.3 exception handlers, 1: pre-0.3 abort handler, 0: none
volatile int claimed;
volatile int passthrough;
bool watchdogDump;
char record[4096];
int recordSize;
SceKernelThreadInfo threadInfo;

void PutStr(const char* s) { while (*s && recordSize < static_cast<int>(sizeof(record)) - 1) record[recordSize++] = *s++; }
void PutHex(std::uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    char text[9];
    for (int i = 7; i >= 0; --i, v >>= 4) text[i] = digits[v & 15];
    text[8] = 0;
    PutStr(text);
}
void PutReg(const char* name, std::uint32_t v) { PutStr(name); PutStr("="); PutHex(v); PutStr(" "); }

void ReleaseAll()
{
    if (crashApi == 2) {
        kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT);
        kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT);
        kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION);
    } else if (crashApi == 1) {
        kuKernelReleaseAbortHandler();
    }
    crashApi = 0;
}

// Name and stack range of the calling thread; returns the range.
void PutThread(std::uint32_t& low, std::uint32_t& high)
{
    const SceUID id = sceKernelGetThreadId();
    PutReg("thread", static_cast<std::uint32_t>(id));
    std::memset(&threadInfo, 0, sizeof(threadInfo));
    threadInfo.size = sizeof(threadInfo);
    low = high = 0;
    if (sceKernelGetThreadInfo(id, &threadInfo) >= 0) {
        PutStr(threadInfo.name);
        low = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(threadInfo.stack));
        high = low + static_cast<std::uint32_t>(threadInfo.stackSize);
        PutStr(" stack "); PutHex(low); PutStr("-"); PutHex(high);
    }
    PutStr("\n");
}

[[noreturn]] void WriteAndExit()
{
    PutStr("end\n");
    if (crashFd >= 0) { sceIoWrite(crashFd, record, recordSize); sceIoClose(crashFd); crashFd = -1; }
    ReleaseAll();
    sceKernelExitProcess(0);
    for (;;) sceKernelDelayThread(1000000);
}

void Claim()
{
    // The first thread to fail writes the record; any other waits for the exit.
    if (__atomic_exchange_n(&claimed, 1, __ATOMIC_ACQ_REL))
        for (;;) sceKernelDelayThread(1000000);
}

void Handler(KuKernelExceptionContext* c)
{
    if (passthrough) {
        // The watchdog's trap: unhook and return; the instruction faults
        // again and the system writes its core dump of every thread.
        ReleaseAll();
        return;
    }
    Claim();
    static const char* const kinds[] = {"data abort", "prefetch abort", "undefined instruction"};
    recordSize = 0;
    PutStr("crash: ");
    PutStr(c->exceptionType < 3 ? kinds[c->exceptionType] : "exception");
    PutStr("\n");
    PutReg("pc", c->pc); PutReg("lr", c->lr); PutReg("sp", c->sp);
    PutReg("far", c->FAR); PutReg("fsr", c->FSR); PutReg("spsr", c->SPSR); PutReg("fpscr", c->FPSCR);
    PutStr("\n");
    const SceUInt32* r = &c->r0;
    static const char* const names[] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12"};
    for (int i = 0; i < 13; ++i) { PutReg(names[i], r[i]); if (i == 6) PutStr("\n"); }
    PutStr("\n");
    // Link-time address of a known function: symbolise pc as
    // pc - (anchor - its ELF address), even if the module was relocated.
    PutReg("anchor vita_crash::Init", static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&Init)));
    PutStr("\n");
    std::uint32_t low, high;
    PutThread(low, high);
    // Stack words, only inside the thread's own stack (a bad sp must not fault again here).
    const std::uint32_t sp = c->sp & ~3u;
    if (sp >= low && sp < high) {
        for (int i = 0; i < 256 && sp + 4 * i + 4 <= high; ++i) {
            if (i % 8 == 0) { PutHex(sp + 4 * i); PutStr(":"); }
            PutStr(" ");
            PutHex(reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(sp))[i]);
            if (i % 8 == 7) PutStr("\n");
        }
        PutStr("\n");
    } else {
        PutStr("sp outside the thread's stack: no stack words\n");
    }
    WriteAndExit();
}

void Terminate()
{
    // An uncaught C++ exception (e.g. a renderer Require): record its message.
    const char* message = "std::terminate without an active exception";
    static char what[512];
    if (const auto active = std::current_exception()) {
        try { std::rethrow_exception(active); }
        catch (const std::exception& error) { std::snprintf(what, sizeof(what), "uncaught exception: %s", error.what()); message = what; }
        catch (...) { message = "uncaught non-standard exception"; }
    }
    Fatal(message);
}
} // namespace

void Fatal(const char* msg)
{
    Claim();
    recordSize = 0;
    PutStr("fatal: "); PutStr(msg); PutStr("\n");
    std::uint32_t low, high;
    PutThread(low, high);
    WriteAndExit();
}

void Stalled(const char* msg)
{
    if (watchdogDump && crashApi != 0) {
        passthrough = 1;
        *static_cast<volatile int*>(nullptr) = 0;   // the system writes its core dump
    }
    if (watchdogDump) *static_cast<volatile int*>(nullptr) = 0;
    Claim();
    recordSize = 0;
    PutStr("stalled: "); PutStr(msg); PutStr("\n");
    WriteAndExit();
}

bool Init(const char* dir, LogFn log)
{
    char path[256];
    std::snprintf(path, sizeof(path), "%s/crash.txt", dir);
    crashFd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    std::snprintf(path, sizeof(path), "%s/watchdog_dump.txt", dir);
    SceIoStat stat{};
    watchdogDump = sceIoGetstat(path, &stat) >= 0;
    std::set_terminate(Terminate);
    char detail[192];
    KuKernelExceptionHandlerOpt opt{sizeof(opt)};
    const int e0 = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, Handler, nullptr, &opt);
    if (e0 >= 0) {
        const int e1 = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT, Handler, nullptr, &opt);
        const int e2 = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION, Handler, nullptr, &opt);
        crashApi = 2;
        std::snprintf(detail, sizeof(detail), "kubridge handlers data=%08x prefetch=%08x undef=%08x anchor=%p watchdog_dump=%d",
            static_cast<unsigned>(e0), static_cast<unsigned>(e1), static_cast<unsigned>(e2),
            reinterpret_cast<void*>(&Init), watchdogDump ? 1 : 0);
        if (log) log("vita.crash", detail);
        return true;
    }
    const int ea = kuKernelRegisterAbortHandler(Handler, nullptr, nullptr);
    if (ea >= 0) {
        crashApi = 1;
        std::snprintf(detail, sizeof(detail), "old kubridge (%08x): abort handler only, anchor=%p",
            static_cast<unsigned>(e0), reinterpret_cast<void*>(&Init));
        if (log) log("vita.crash", detail);
        return true;
    }
    std::snprintf(detail, sizeof(detail), "kubridge not loaded (%08x/%08x): faults give the system dump",
        static_cast<unsigned>(e0), static_cast<unsigned>(ea));
    if (log) log("vita.crash", detail);
    return false;
}
} // namespace vita_crash
