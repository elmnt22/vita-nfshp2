/* The parts of the kubridge v0.3.1 user API (github.com/bythos14/kubridge, kubridge.h) the port
 * uses: exception handlers. Stub: kubridge_nids.yml, built by tools/build_port.py (weak). */
#ifndef KUBRIDGE_MIN_H
#define KUBRIDGE_MIN_H

#include <psp2/types.h>

#define KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT            0
#define KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT        1
#define KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION 2

/* Saved on the faulting thread's stack; the handler runs on that thread with ~0x400 bytes of
 * stack to spare. Returning resumes at pc (as changed by the handler). */
typedef struct KuKernelExceptionContext {
    SceUInt32 r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12;
    SceUInt32 sp, lr, pc;
    SceUInt64 vfpRegisters[32];
    SceUInt32 SPSR, FPSCR, FPEXC, FSR, FAR;
    SceUInt32 exceptionType;
} KuKernelExceptionContext;

typedef void (*KuKernelExceptionHandler)(KuKernelExceptionContext *);

typedef struct KuKernelExceptionHandlerOpt {
    SceSize size;
} KuKernelExceptionHandlerOpt;

int kuKernelRegisterExceptionHandler(SceUInt32 exceptionType, KuKernelExceptionHandler pHandler,
                                     KuKernelExceptionHandler *pOldHandler, KuKernelExceptionHandlerOpt *pOpt);
void kuKernelReleaseExceptionHandler(SceUInt32 exceptionType);

/* Older kubridge (before v0.3): data/prefetch aborts only, same context layout. v0.2 takes
 * two arguments; the third is ignored there. */
typedef KuKernelExceptionContext KuKernelAbortContext;
typedef void (*KuKernelAbortHandler)(KuKernelAbortContext *);
int kuKernelRegisterAbortHandler(KuKernelAbortHandler pHandler, KuKernelAbortHandler *pOldHandler, void *pOpt);
void kuKernelReleaseAbortHandler(void);

#endif
