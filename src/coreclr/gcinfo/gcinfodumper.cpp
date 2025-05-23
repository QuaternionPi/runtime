// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef SOS_INCLUDE
#include "common.h"
#endif

#include "gcinfohelpers.h"
#include "gcinfodumper.h"
#include "gcinfodecoder.h"

// Stolen from gc.h.
#define GC_CALL_INTERIOR            0x1
#define GC_CALL_PINNED              0x2


#ifdef HOST_64BIT
// All stack offsets are INT32's, so this guarantees a disjoint range of
// addresses for each register.
#define ADDRESS_SPACING UI64(0x100000000)
#elif defined(TARGET_ARM)
#define ADDRESS_SPACING 0x100000
#else
#error pick suitable ADDRESS_SPACING for platform
#endif

GcInfoDumper::GcInfoDumper (GCInfoToken gcInfoToken)
{
    m_gcTable = gcInfoToken;
    m_pRecords = NULL;
    m_gcInfoSize = 0;
}


GcInfoDumper::~GcInfoDumper ()
{
    FreePointerRecords(m_pRecords);
}
size_t GcInfoDumper::GetGCInfoSize()
{
    return m_gcInfoSize;
}


//static*
void GcInfoDumper::LivePointerCallback (
        LPVOID          hCallback,      // callback data
        OBJECTREF*      pObject,        // address of object-reference we are reporting
        uint32_t        flags           // is this a pinned and/or interior pointer
        DAC_ARG(DacSlotLocation loc))   // the location of the slot
{
    GcInfoDumper *pDumper = (GcInfoDumper*)hCallback;
    LivePointerRecord **ppRecords = &pDumper->m_pRecords;
    LivePointerRecord *pRecord = new LivePointerRecord();
    if (!pRecord)
    {
        pDumper->m_Error = OUT_OF_MEMORY;
        return;
    }

    pRecord->ppObject = pObject;
    pRecord->flags = flags;
    pRecord->marked = -1;

    pRecord->pNext = *ppRecords;
    *ppRecords = pRecord;
}


//static
void GcInfoDumper::FreePointerRecords (LivePointerRecord *pRecords)
{
    while (pRecords)
    {
        LivePointerRecord *trash = pRecords;
        pRecords = pRecords->pNext;
        delete trash;
    }
}

//This function tries to find the address of the managed object in the registers of the current function's context,
//failing which it checks if it is present in the stack of the current function. IF it finds one it reports appropriately
//
//For Amd64, this additionally tries to probe in the stack for  the caller.
//This behavior largely seems to be present for legacy x64 jit and is not likely to be used anywhere else
BOOL GcInfoDumper::ReportPointerRecord (
        UINT32 CodeOffset,
        BOOL fLive,
        REGDISPLAY *pRD,
        LivePointerRecord *pRecord)
{
    //
    // Convert the flags passed to the GC into flags used by GcInfoEncoder.
    //

    int EncodedFlags = 0;

    if (pRecord->flags & GC_CALL_INTERIOR)
        EncodedFlags |= GC_SLOT_INTERIOR;

    if (pRecord->flags & GC_CALL_PINNED)
        EncodedFlags |= GC_SLOT_PINNED;

    //
    // Compare the reported pointer against the REGIDISPLAY pointers to
    // figure out the register or register-relative location.
    //

    struct RegisterInfo
    {
        SIZE_T cbContextOffset;
    };

    static RegisterInfo rgRegisters[] = {
#define REG(reg, field) { offsetof(T_CONTEXT, field) }

#ifdef TARGET_AMD64
        REG(rax, Rax),
        REG(rcx, Rcx),
        REG(rdx, Rdx),
        REG(rbx, Rbx),
        REG(rsp, Rsp),
        REG(rbp, Rbp),
        REG(rsi, Rsi),
        REG(rdi, Rdi),
        REG(r8, R8),
        REG(r9, R9),
        REG(r10, R10),
        REG(r11, R11),
        REG(r12, R12),
        REG(r13, R13),
        REG(r14, R14),
        REG(r15, R15),
#elif defined(TARGET_ARM)
#undef REG
#define REG(reg, field) { offsetof(ArmVolatileContextPointer, field) }
        REG(r0, R0),
        REG(r1, R1),
        REG(r2, R2),
        REG(r3, R3),
#undef REG
#define REG(reg, field) { offsetof(T_KNONVOLATILE_CONTEXT_POINTERS, field) }
        REG(r4, R4),
        REG(r5, R5),
        REG(r6, R6),
        REG(r7, R7),
        REG(r8, R8),
        REG(r9, R9),
        REG(r10, R10),
        REG(r11, R11),
        { offsetof(ArmVolatileContextPointer, R12) },
        { offsetof(T_CONTEXT, Sp) },
        { offsetof(T_KNONVOLATILE_CONTEXT_POINTERS, Lr) },
        { offsetof(T_CONTEXT, Sp) },
        { offsetof(T_KNONVOLATILE_CONTEXT_POINTERS, R7) },
#elif defined(TARGET_ARM64)
#undef REG
#define REG(reg, field) { offsetof(Arm64VolatileContextPointer, field) }
        REG(x0, X0),
        REG(x1, X1),
        REG(x2, X2),
        REG(x3, X3),
        REG(x4, X4),
        REG(x5, X5),
        REG(x6, X6),
        REG(x7, X7),
        REG(x8, X8),
        REG(x9, X9),
        REG(x10, X10),
        REG(x11, X11),
        REG(x12, X12),
        REG(x13, X13),
        REG(x14, X14),
        REG(x15, X15),
        REG(x16, X16),
        REG(x17, X17),
#undef REG
#define REG(reg, field) { offsetof(T_KNONVOLATILE_CONTEXT_POINTERS, field) }
        REG(x19, X19),
        REG(x20, X20),
        REG(x21, X21),
        REG(x22, X22),
        REG(x23, X23),
        REG(x24, X24),
        REG(x25, X25),
        REG(x26, X26),
        REG(x27, X27),
        REG(x28, X28),
        REG(Fp,  Fp),
        REG(Lr,  Lr),
        { offsetof(T_CONTEXT, Sp) },
#undef REG
#elif defined(TARGET_LOONGARCH64)
#undef REG
#define REG(reg, field) { offsetof(T_KNONVOLATILE_CONTEXT_POINTERS, field) }
#define vREG(reg, field) { offsetof(LoongArch64VolatileContextPointer, field) }
        vREG(zero, R0),
        REG(ra, Ra),
        { offsetof(T_CONTEXT, Sp) },
        vREG(a0, A0),
        vREG(a1, A1),
        vREG(a2, A2),
        vREG(a3, A3),
        vREG(a4, A4),
        vREG(a5, A5),
        vREG(a6, A6),
        vREG(a7, A7),
        vREG(t0, T0),
        vREG(t1, T1),
        vREG(t2, T2),
        vREG(t3, T3),
        vREG(t4, T4),
        vREG(t5, T5),
        vREG(t6, T6),
        vREG(t7, T7),
        vREG(t8, T8),
        vREG(x0, X0),
        REG(fp, Fp),
        REG(s0, S0),
        REG(s1, S1),
        REG(s2, S2),
        REG(s3, S3),
        REG(s4, S4),
        REG(s5, S5),
        REG(s6, S6),
        REG(s7, S7),
        REG(s8, S8),
#undef vREG
#undef REG
#elif defined(TARGET_RISCV64)
#undef REG
#define REG(reg, field) { offsetof(T_KNONVOLATILE_CONTEXT_POINTERS, field) }
#define vREG(reg, field) { offsetof(RiscV64VolatileContextPointer, field) }
        vREG(zero, R0),
        REG(Ra, Ra),
        { offsetof(T_CONTEXT, Sp) },
        REG(Gp, Gp),
        REG(Tp, Tp),
        vREG(t0, T0),
        vREG(t1, T1),
        vREG(t2, T2),
        REG(Fp, Fp),
        REG(s1, S1),
        vREG(a0, A0),
        vREG(a1, A1),
        vREG(a2, A2),
        vREG(a3, A3),
        vREG(a4, A4),
        vREG(a5, A5),
        vREG(a6, A6),
        vREG(a7, A7),
        REG(s2, S2),
        REG(s3, S3),
        REG(s4, S4),
        REG(s5, S5),
        REG(s6, S6),
        REG(s7, S7),
        REG(s8, S8),
        REG(s9, S9),
        REG(s10, S10),
        REG(s11, S11),
        vREG(t3, T3),
        vREG(t4, T4),
        vREG(t5, T5),
        vREG(t6, T6),
#undef vREG
#undef REG
#else
PORTABILITY_ASSERT("GcInfoDumper::ReportPointerRecord is not implemented on this platform.")
#endif

    };

    const UINT nCONTEXTRegisters = sizeof(rgRegisters)/sizeof(rgRegisters[0]);

    UINT iFirstRegister;
    UINT iSPRegister;
    UINT nRegisters;

    iFirstRegister = 0;
    nRegisters = nCONTEXTRegisters;
#ifdef TARGET_AMD64
    iSPRegister = (offsetof(CONTEXT, Rsp) - offsetof(CONTEXT, Rax)) / sizeof(ULONGLONG);
#elif defined(TARGET_ARM64)
    iSPRegister = (offsetof(T_CONTEXT, Sp) - offsetof(T_CONTEXT, X0)) / sizeof(ULONGLONG);
#elif defined(TARGET_ARM)
    iSPRegister = (offsetof(T_CONTEXT, Sp) - offsetof(T_CONTEXT, R0)) / sizeof(ULONG);
    UINT iBFRegister = m_StackBaseRegister;
#elif defined(TARGET_LOONGARCH64)
    iSPRegister = (offsetof(T_CONTEXT, Sp) - offsetof(T_CONTEXT, R0)) / sizeof(ULONGLONG);
#elif defined(TARGET_RISCV64)
    iSPRegister = (offsetof(T_CONTEXT, Sp) - offsetof(T_CONTEXT, R0)) / sizeof(ULONGLONG);
#endif

#if defined(TARGET_ARM) || defined(TARGET_ARM64) || defined(TARGET_RISCV64) || defined(TARGET_LOONGARCH64)
    BYTE* pContext = (BYTE*)&(pRD->volatileCurrContextPointers);
#else
    BYTE* pContext = (BYTE*)pRD->pCurrentContext;
#endif

    for (int ctx = 0; ctx < 2; ctx++)
    {
        SIZE_T *pReg = NULL;

        for (UINT iReg = 0; iReg < nRegisters; iReg++)
        {
            UINT iEncodedReg = iFirstRegister + iReg;
#ifdef TARGET_ARM
            if (ctx == 1)
            {
                if ((iReg < 4 || iReg == 12))   // skip volatile registers for second context
                {
                    continue;
                }
                // Force StackRegister and BaseRegister at the end (r15, r16)
                if (iReg == iSPRegister || iReg == m_StackBaseRegister)
                {
                    continue;
                }
                if (iReg == 15)
                {
                    if (iBFRegister != NO_STACK_BASE_REGISTER)
                    {
                        iEncodedReg = iBFRegister;
                    }
                    else
                    {
                        continue;
                    }
                }
                if (iReg == 16)
                {
                    iEncodedReg = iSPRegister;
                }
            }
            if (ctx == 0 && iReg == 4)  //ArmVolatileContextPointer 5th register is R12
            {
                iEncodedReg = 12;
            }
            else if (ctx == 0 && iReg > 4)
            {
                break;
            }
#elif defined (TARGET_ARM64)
            iEncodedReg = iEncodedReg + ctx; //We have to compensate for not tracking x18
            if (ctx == 1)
            {
                if (iReg < 18 )   // skip volatile registers for second context
                {
                    continue;
                }

                if (iReg == 30)
                {
                    iEncodedReg = iSPRegister;
                }
            }

            if (ctx == 0 && iReg > 17)
            {
                break;
            }
#elif defined(TARGET_LOONGARCH64)
            if (iEncodedReg > 1)
            {
                iEncodedReg++; // We have to compensate for not tracking tp
            }
            bool isVolatile = (iReg == 0 || (iReg >= 3 && iReg <= 20));
            if (ctx == 0)
            {
                if (!isVolatile)
                {
                    continue;
                }
            }
            else if (isVolatile) // skip volatile registers for second context
            {
                continue;
            }
#elif defined(TARGET_RISCV64)
            bool isVolatile = (iReg == 0 || (iReg >= 5 && iReg <= 7) || (iReg >= 10 && iReg <= 17) || iReg >= 28);
            if (ctx == 0)
            {
                if (!isVolatile)
                {
                    continue;
                }
            }
            else if (isVolatile) // skip volatile registers for second context
            {
                continue;
            }
#endif
            {
                _ASSERTE(iReg < nCONTEXTRegisters);
#ifdef TARGET_ARM
                pReg = *(SIZE_T**)(pContext + rgRegisters[iReg].cbContextOffset);
                if (iEncodedReg == 12)
                {
                    pReg = *(SIZE_T**)((BYTE*)&pRD->volatileCurrContextPointers + rgRegisters[iEncodedReg].cbContextOffset);
                }
                if (iEncodedReg == iSPRegister)
                {
                    pReg = (SIZE_T*)((BYTE*)pRD->pCurrentContext + rgRegisters[iEncodedReg].cbContextOffset);
                }
                if (iEncodedReg == iBFRegister)
                {
                    pReg = *(SIZE_T**)((BYTE*)pRD->pCurrentContextPointers + rgRegisters[iEncodedReg].cbContextOffset);
                }

#elif defined(TARGET_ARM64) || defined(TARGET_RISCV64) || defined(TARGET_LOONGARCH64)
                pReg = *(SIZE_T**)(pContext + rgRegisters[iReg].cbContextOffset);
                if (iEncodedReg == iSPRegister)
                {
                    pReg = (SIZE_T*)((BYTE*)pRD->pCurrentContext + rgRegisters[iReg].cbContextOffset);
                }
#else
                pReg = (SIZE_T*)(pContext + rgRegisters[iReg].cbContextOffset);
#endif

            }

            SIZE_T ptr = (SIZE_T)pRecord->ppObject;


            //
            // Is it reporting the register?
            //
            if (ptr == (SIZE_T)pReg)
            {
                // Make sure the register is in the current frame.
#if defined(TARGET_AMD64)
                if (0 != ctx)
                {
                    m_Error = REPORTED_REGISTER_IN_CALLERS_FRAME;
                    return TRUE;
                }
#endif
                // Make sure the register isn't sp or the frame pointer.
                if (   iSPRegister == iEncodedReg
                    || m_StackBaseRegister == iEncodedReg)
                {
                    m_Error = REPORTED_FRAME_POINTER;
                    return TRUE;
                }

                if (m_pfnRegisterStateChange(
                        CodeOffset,
                        iEncodedReg,
                        (GcSlotFlags)EncodedFlags,
                        fLive ? GC_SLOT_LIVE : GC_SLOT_DEAD,
                        m_pvCallbackData))
                {
                    return TRUE;
                }

                return FALSE;
            }

            //
            // Is it reporting an address relative to the register's value?
            //

            SIZE_T regVal = *pReg;

            if (   ptr >= regVal - ADDRESS_SPACING/2
                && ptr <  regVal + ADDRESS_SPACING/2)
            {
                //
                // The register must be sp, caller's sp, or the frame register.
                // The GcInfoEncoder interface doesn't have a way to express
                // anything else.
                //

                if (!(   iSPRegister == iEncodedReg
                      || m_StackBaseRegister == iEncodedReg))
                {
                    continue;
                }

                GcStackSlotBase base;
                if (iSPRegister == iEncodedReg)
                {
#if defined(TARGET_ARM) || defined(TARGET_ARM64) || defined(TARGET_RISCV64) || defined(TARGET_LOONGARCH64)
                    base = GC_SP_REL;
#else
                    if (0 == ctx)
                        base = GC_SP_REL;
                    else
                        base = GC_CALLER_SP_REL;
#endif //defined(TARGET_ARM) || defined(TARGET_ARM64) || defined(TARGET_RISCV64) || defined(TARGET_LOONGARCH64)
                }
                else
                {
                    base = GC_FRAMEREG_REL;
                }

                if (m_pfnStackSlotStateChange(
                        CodeOffset,
                        (GcSlotFlags)EncodedFlags,
                        base,
                        ptr - regVal,
                        fLive ? GC_SLOT_LIVE : GC_SLOT_DEAD,
                        m_pvCallbackData))
                {
                    return TRUE;
                }

                return FALSE;
            }
        }

#if defined(TARGET_ARM) || defined(TARGET_ARM64) || defined(TARGET_RISCV64) || defined(TARGET_LOONGARCH64)
        pContext = (BYTE*)pRD->pCurrentContextPointers;
#else
        pContext = (BYTE*)pRD->pCallerContext;
#endif

    }

    m_Error = REPORTED_INVALID_POINTER;
    return TRUE;
}


BOOL GcInfoDumper::ReportPointerDifferences (
        UINT32 offset,
        REGDISPLAY *pRD,
        LivePointerRecord *pPrevState)
{
    LivePointerRecord *pNewRecord;
    LivePointerRecord *pOldRecord;

    //
    // Match up old and new records
    //

    for (pNewRecord = m_pRecords; pNewRecord; pNewRecord = pNewRecord->pNext)
    {
        for (LivePointerRecord *pOldRecord = pPrevState; pOldRecord; pOldRecord = pOldRecord->pNext)
        {
            if (   pOldRecord->flags == pNewRecord->flags
                && pOldRecord->ppObject == pNewRecord->ppObject)
            {
                pOldRecord->marked = offset;
                pNewRecord->marked = offset;
            }
        }
    }

    //
    // Report out any old records that were not marked as dead pointers.
    //

    for (pOldRecord = pPrevState; pOldRecord; pOldRecord = pOldRecord->pNext)
    {
        if (pOldRecord->marked != offset)
        {
            if (   ReportPointerRecord(offset, FALSE, pRD, pOldRecord)
                || m_Error)
            {
                return TRUE;
            }
        }
    }

    //
    // Report any new records that were not marked as new pointers.
    //

    for (pNewRecord = m_pRecords; pNewRecord; pNewRecord = pNewRecord->pNext)
    {
        if (pNewRecord->marked != offset)
        {
            if (   ReportPointerRecord(offset, TRUE, pRD, pNewRecord)
                || m_Error)
            {
                return TRUE;
            }
        }
    }

    return FALSE;
}


GcInfoDumper::EnumerateStateChangesResults GcInfoDumper::EnumerateStateChanges (
        InterruptibleStateChangeProc *pfnInterruptibleStateChange,
        RegisterStateChangeProc *pfnRegisterStateChange,
        StackSlotStateChangeProc *pfnStackSlotStateChange,
        OnSafePointProc *pfnSafePointFunc,
        PVOID pvData)
{
    m_Error = SUCCESS;

    //
    // Save callback functions for use by helper functions
    //

    m_pfnRegisterStateChange = pfnRegisterStateChange;
    m_pfnStackSlotStateChange = pfnStackSlotStateChange;
    m_pvCallbackData = pvData;

    //
    // Decode header information
    //
    GcInfoDecoder hdrdecoder(m_gcTable,
                             (GcInfoDecoderFlags)(  DECODE_SECURITY_OBJECT
                                                  | DECODE_CODE_LENGTH
                                                  | DECODE_GC_LIFETIMES
                                                  | DECODE_VARARG),
                             0);

    UINT32 cbEncodedMethodSize = hdrdecoder.GetCodeLength();
    m_StackBaseRegister = hdrdecoder.GetStackBaseRegister();

    //
    // Set up a bogus REGDISPLAY to pass to EnumerateLiveSlots.  This will
    // allow us to later identify registers or stack offsets passed to the
    // callback.
    //

    REGDISPLAY regdisp;

    ZeroMemory(&regdisp, sizeof(regdisp));

    regdisp.pContext = &regdisp.ctxOne;
    regdisp.IsCallerContextValid = TRUE;
    regdisp.pCurrentContext = &regdisp.ctxOne;
    regdisp.pCallerContext = &regdisp.ctxTwo;

#define NEXT_ADDRESS() (UniqueAddress += ADDRESS_SPACING)

    UINT iReg;

#ifdef HOST_64BIT
    ULONG64 UniqueAddress = ADDRESS_SPACING*2;
    ULONG64 *pReg;
#else
    DWORD UniqueAddress = ADDRESS_SPACING*2;
    DWORD *pReg;
#endif

#define FILL_REGS(start, count)                                             \
    do {                                                                    \
        for (iReg = 0, pReg = &regdisp.start; iReg < count; iReg++, pReg++) \
        {                                                                   \
            *pReg = NEXT_ADDRESS();                                         \
        }                                                                   \
    } while (0)

#ifdef TARGET_AMD64
    FILL_REGS(pCurrentContext->Rax, 16);
    FILL_REGS(pCallerContext->Rax, 16);

    regdisp.pCurrentContextPointers = &regdisp.ctxPtrsOne;
    regdisp.pCallerContextPointers = &regdisp.ctxPtrsTwo;

    ULONGLONG **ppCurrentRax = &regdisp.pCurrentContextPointers->Rax;
    ULONGLONG **ppCallerRax  = &regdisp.pCallerContextPointers ->Rax;

    for (iReg = 0; iReg < 16; iReg++)
    {
        *(ppCurrentRax + iReg) = &regdisp.pCurrentContext->Rax + iReg;
        *(ppCallerRax  + iReg) = &regdisp.pCallerContext ->Rax + iReg;
    }
#elif defined(TARGET_ARM)
    FILL_REGS(pCurrentContext->R0, 16);
    FILL_REGS(pCallerContext->R0, 16);

    regdisp.pCurrentContextPointers = &regdisp.ctxPtrsOne;
    regdisp.pCallerContextPointers = &regdisp.ctxPtrsTwo;

    ULONG **ppCurrentReg = &regdisp.pCurrentContextPointers->R4;
    ULONG **ppCallerReg  = &regdisp.pCallerContextPointers->R4;

    for (iReg = 0; iReg < 8; iReg++)
    {
        *(ppCurrentReg + iReg) = &regdisp.pCurrentContext->R4 + iReg;
        *(ppCallerReg  + iReg) = &regdisp.pCallerContext->R4 + iReg;
    }
    /// Set Lr
    *(ppCurrentReg + 8) = &regdisp.pCurrentContext->R4 + 10;
    *(ppCallerReg + 8) = &regdisp.pCallerContext->R4 + 10;
    ULONG **ppVolatileReg = &regdisp.volatileCurrContextPointers.R0;
    for (iReg = 0; iReg < 4; iReg++)
    {
        *(ppVolatileReg+iReg) = &regdisp.pCurrentContext->R0 + iReg;
    }
    /// Set R12
    *(ppVolatileReg+4) = &regdisp.pCurrentContext->R0+12;

#elif defined(TARGET_ARM64)
    FILL_REGS(pCurrentContext->X0, 33);
    FILL_REGS(pCallerContext->X0, 33);

    regdisp.pCurrentContextPointers = &regdisp.ctxPtrsOne;
    regdisp.pCallerContextPointers = &regdisp.ctxPtrsTwo;

    ULONG64 **ppCurrentReg = &regdisp.pCurrentContextPointers->X19;
    ULONG64 **ppCallerReg  = &regdisp.pCallerContextPointers->X19;

    for (iReg = 0; iReg < 11; iReg++)
    {
        *(ppCurrentReg + iReg) = &regdisp.pCurrentContext->X19 + iReg;
        *(ppCallerReg  + iReg) = &regdisp.pCallerContext->X19 + iReg;
    }

    /// Set Lr
    *(ppCurrentReg + 11) = &regdisp.pCurrentContext->Lr;
    *(ppCallerReg +  11) = &regdisp.pCallerContext->Lr;

    ULONG64 **ppVolatileReg = &regdisp.volatileCurrContextPointers.X0;
    for (iReg = 0; iReg < 18; iReg++)
    {
        *(ppVolatileReg+iReg) = &regdisp.pCurrentContext->X0 + iReg;
    }
#elif defined(TARGET_LOONGARCH64)
    FILL_REGS(pCurrentContext->R0, 33);
    FILL_REGS(pCallerContext->R0, 33);

    regdisp.pCurrentContextPointers = &regdisp.ctxPtrsOne;
    regdisp.pCallerContextPointers = &regdisp.ctxPtrsTwo;

    ULONG64 **ppCurrentReg = &regdisp.pCurrentContextPointers->S0;
    ULONG64 **ppCallerReg  = &regdisp.pCallerContextPointers->S0;

    // Set S0-S8
    for (iReg = 0; iReg < 9; iReg++)
    {
        *(ppCurrentReg + iReg) = &regdisp.pCurrentContext->S0 + iReg;
        *(ppCallerReg  + iReg) = &regdisp.pCallerContext->S0 + iReg;
    }

    // Set Ra, Fp
    regdisp.pCurrentContextPointers->Ra = &regdisp.pCurrentContext->Ra;
    regdisp.pCallerContextPointers->Ra  = &regdisp.pCallerContext->Ra;
    regdisp.pCurrentContextPointers->Fp = &regdisp.pCurrentContext->Fp;
    regdisp.pCallerContextPointers->Fp  = &regdisp.pCallerContext->Fp;

    ULONG64 **ppVolatileReg = &regdisp.volatileCurrContextPointers.A0;
    for (iReg = 0; iReg < 18; iReg++)
    {
        *(ppVolatileReg+iReg) = &regdisp.pCurrentContext->A0 + iReg;
    }
    regdisp.volatileCurrContextPointers.R0 = &regdisp.pCurrentContext->R0;
#elif defined(TARGET_RISCV64)
    FILL_REGS(pCurrentContext->R0, 33);
    FILL_REGS(pCallerContext->R0, 33);

    regdisp.pCurrentContextPointers = &regdisp.ctxPtrsOne;
    regdisp.pCallerContextPointers = &regdisp.ctxPtrsTwo;

    // Set S1
    regdisp.pCurrentContextPointers->S1 = &regdisp.pCurrentContext->S1;
    regdisp.pCallerContextPointers ->S1 = &regdisp.pCallerContext ->S1;

    ULONG64 **ppCurrentReg = &regdisp.pCurrentContextPointers->S2;
    ULONG64 **ppCallerReg  = &regdisp.pCallerContextPointers ->S2;
    // Set  S2-S11
    for (iReg = 0; iReg < 10; iReg++)
    {
        *(ppCurrentReg + iReg) = &regdisp.pCurrentContext->S2 + iReg;
        *(ppCallerReg  + iReg) = &regdisp.pCallerContext ->S2 + iReg;
    }

    // Set Fp
    regdisp.pCurrentContextPointers->Fp = &regdisp.pCurrentContext->Fp;
    regdisp.pCallerContextPointers ->Fp = &regdisp.pCallerContext ->Fp;

    // Set Gp
    regdisp.pCurrentContextPointers->Gp = &regdisp.pCurrentContext->Gp;
    regdisp.pCallerContextPointers ->Gp = &regdisp.pCallerContext ->Gp;

    // Set Tp
    regdisp.pCurrentContextPointers->Tp = &regdisp.pCurrentContext->Tp;
    regdisp.pCallerContextPointers ->Tp = &regdisp.pCallerContext ->Tp;

    // Set Ra
    regdisp.pCurrentContextPointers->Ra = &regdisp.pCurrentContext->Ra;
    regdisp.pCallerContextPointers ->Ra = &regdisp.pCallerContext ->Ra;

    regdisp.volatileCurrContextPointers.R0 = &regdisp.pCurrentContext->R0;
    regdisp.volatileCurrContextPointers.A0 = &regdisp.pCurrentContext->A0;
    regdisp.volatileCurrContextPointers.A1 = &regdisp.pCurrentContext->A1;
    regdisp.volatileCurrContextPointers.A2 = &regdisp.pCurrentContext->A2;
    regdisp.volatileCurrContextPointers.A3 = &regdisp.pCurrentContext->A3;
    regdisp.volatileCurrContextPointers.A4 = &regdisp.pCurrentContext->A4;
    regdisp.volatileCurrContextPointers.A5 = &regdisp.pCurrentContext->A5;
    regdisp.volatileCurrContextPointers.A6 = &regdisp.pCurrentContext->A6;
    regdisp.volatileCurrContextPointers.A7 = &regdisp.pCurrentContext->A7;
    regdisp.volatileCurrContextPointers.T0 = &regdisp.pCurrentContext->T0;
    regdisp.volatileCurrContextPointers.T1 = &regdisp.pCurrentContext->T1;
    regdisp.volatileCurrContextPointers.T2 = &regdisp.pCurrentContext->T2;
    regdisp.volatileCurrContextPointers.T3 = &regdisp.pCurrentContext->T3;
    regdisp.volatileCurrContextPointers.T4 = &regdisp.pCurrentContext->T4;
    regdisp.volatileCurrContextPointers.T5 = &regdisp.pCurrentContext->T5;
    regdisp.volatileCurrContextPointers.T6 = &regdisp.pCurrentContext->T6;
#else
PORTABILITY_ASSERT("GcInfoDumper::EnumerateStateChanges is not implemented on this platform.");
#endif

#undef FILL_REGS
#undef NEXT_ADDRESS

    SyncRegDisplayToCurrentContext(&regdisp);

    //
    // Enumerate pointers at every possible offset.
    //

#ifdef PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED
    GcInfoDecoder safePointDecoder(m_gcTable, (GcInfoDecoderFlags)0, 0);
#endif

    {
        GcInfoDecoder untrackedDecoder(m_gcTable, DECODE_GC_LIFETIMES, 0);
        untrackedDecoder.EnumerateUntrackedSlots(&regdisp,
                    0,
                    &LivePointerCallback,
                    this);

        BOOL fStop = ReportPointerDifferences(
                    -2,
                    &regdisp,
                    NULL);

        FreePointerRecords(m_pRecords);
        m_pRecords = NULL;

        if (fStop || m_Error)
            return m_Error;
    }

    LivePointerRecord *pLastState = NULL;
    BOOL fPrevInterruptible = FALSE;

    for (UINT32 offset = 0; offset <= cbEncodedMethodSize; offset++)
    {
        BOOL fNewInterruptible = FALSE;

        GcInfoDecoder decoder1(m_gcTable,
                               (GcInfoDecoderFlags)(  DECODE_SECURITY_OBJECT
                                                    | DECODE_CODE_LENGTH
                                                    | DECODE_VARARG
#if defined(TARGET_ARM) || defined(TARGET_ARM64) || defined(TARGET_LOONGARCH64) || defined(TARGET_RISCV64)
                                                    | DECODE_HAS_TAILCALLS
#endif // TARGET_ARM || TARGET_ARM64 || TARGET_LOONGARCH64 || TARGET_RISCV64

                                                    | DECODE_INTERRUPTIBILITY),
                               offset);

        fNewInterruptible = decoder1.IsInterruptible();

        if (fNewInterruptible != fPrevInterruptible)
        {
            if (pfnInterruptibleStateChange(offset, fNewInterruptible, pvData))
                break;

            fPrevInterruptible = fNewInterruptible;
        }

        unsigned flags = ActiveStackFrame;

#ifdef PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED
        UINT32 safePointOffset = offset;
#if defined(TARGET_AMD64) || defined(TARGET_ARM) || defined(TARGET_ARM64) || defined(TARGET_RISCV64) || defined(TARGET_LOONGARCH64)
        safePointOffset++;
#endif
        if(safePointDecoder.IsSafePoint(safePointOffset))
        {
            _ASSERTE(!fNewInterruptible);
            if (pfnSafePointFunc(safePointOffset, pvData))
                break;

            flags = 0;
        }
#endif

        GcInfoDecoder decoder2(m_gcTable,
                               (GcInfoDecoderFlags)(  DECODE_SECURITY_OBJECT
                                                    | DECODE_CODE_LENGTH
                                                    | DECODE_VARARG
                                                    | DECODE_GC_LIFETIMES
                                                    | DECODE_NO_VALIDATION),
                               offset);

        _ASSERTE(!m_pRecords);

        if(!fNewInterruptible && (flags == ActiveStackFrame))
        {
            // Decoding at non-interruptible offsets is only
            //  valid in the ExecutionAborted case
            flags |= ExecutionAborted;
        }

        if (!decoder2.EnumerateLiveSlots(
                    &regdisp,
                    true,
                    flags | NoReportUntracked,
                    &LivePointerCallback,
                    this))
        {
            m_Error = DECODER_FAILED;
        }

        if (m_Error)
            break;

        if (ReportPointerDifferences(
                offset,
                &regdisp,
                pLastState))
        {
            break;
        }

        if (m_Error)
            break;

        FreePointerRecords(pLastState);

        pLastState = m_pRecords;
        m_pRecords = NULL;

        size_t tempSize = decoder2.GetNumBytesRead();
        if( m_gcInfoSize < tempSize )
            m_gcInfoSize = tempSize;
    }

    FreePointerRecords(pLastState);

    FreePointerRecords(m_pRecords);
    m_pRecords = NULL;

    return m_Error;
}
