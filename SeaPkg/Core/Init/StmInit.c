/** @file
  STM initialization

  Copyright (c) 2015 - 2016, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php.

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <Uefi.h>
#include <SeaResponder.h>
#include <Library/LocalApicLib.h>
#include <SmmSecurePolicy.h>
#include <IndustryStandard/Tpm20.h>
#include <Library/PcdLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MtrrLib.h>
#include <Library/SecurePolicyLib.h>
#include <x64/CpuArchSpecific.h>

#include "StmInit.h"
#include "Runtime/StmRuntimeUtil.h"

SEA_HOST_CONTEXT_COMMON   mHostContextCommon;
SEA_GUEST_CONTEXT_COMMON  mGuestContextCommonNormal;

volatile BOOLEAN  mIsBspInitialized;

/*++
  STM runtime:

                           +------------+
                           | SMM handler|
  +-------+                +------------+
  | Guest | --                  ^ |
  +-------+  |       (2)VMResume| |(3)RSM
             |(1) SMI           | v
  +-------+  |-----------> +------------+
  |       |  |(4) VMResume |SMI-H  SMM-H|
  | MVMM  |  -<----------- |   STM      |
  |       | (0) Init       |STM-Init    |
  +-------+ -------------> +------------+

  Memory layout:
                        +--------------------+ --
                        | SMM VMCS           |  |
                        +--------------------+  |-> Per-Processor VMCS
                        | SMI VMCS           |  |
                        +--------------------+ --
                        | SMM VMCS           |  |
                        +--------------------+  |-> Per-Processor VMCS
                        | SMI VMCS           |  |
                        +--------------------+ --
                        | Stack              |  |-> Per-Processor Dynamic
                        +--------------------+ --
                        | Stack              |  |-> Per-Processor Dynamic
                  RSP-> +--------------------+ --
                        | Heap               |  |
                        +--------------------+  |-> Additional Dynamic
                        | Page Table (24K)   |  |
                  CR3-> +--------------------+ --
                  RIP-> | STM Code           |  |
                        +--------------------+  |
                        | GDT (4K)           |  |-> Static Image
                  GDT-> +--------------------+  |
                        | STM Header (4K)    |  |
                 MSEG-> +--------------------+ --
--*/

/**

  This function return 4K page aligned VMCS size.

  @return 4K page aligned VMCS size

**/
UINT32
GetVmcsSize (
  VOID
  )
{
  UINT64  Data64;
  UINT32  VmcsSize;

  Data64   = AsmReadMsr64 (IA32_VMX_BASIC_MSR_INDEX);
  VmcsSize = (UINT32)(RShiftU64 (Data64, 32) & 0xFFFF);
  VmcsSize = STM_PAGES_TO_SIZE (STM_SIZE_TO_PAGES (VmcsSize));

  return VmcsSize;
}

/**

  This function return if Senter executed.

  @retval TRUE  Senter executed
  @retval FALSE Sexit not executed

**/
BOOLEAN
IsSentryEnabled (
  VOID
  )
{
  UINT32  TxtStatus;

  TxtStatus = TxtPubRead32 (TXT_STS);
  if (((TxtStatus & TXT_STS_SENTER_DONE) != 0) &&
      ((TxtStatus & TXT_STS_SEXIT_DONE) == 0))
  {
    return TRUE;
  } else {
    return FALSE;
  }
}

/**

  This function get CPU number in TXT heap region.

  @return CPU number in TXT heap region

**/
UINT32
GetCpuNumFromTxt (
  VOID
  )
{
  TXT_BIOS_TO_OS_DATA  *BiosToOsData;

  BiosToOsData = GetTxtBiosToOsData ();

  return BiosToOsData->NumLogProcs;
}

#define EBDA_BASE_ADDRESS  0x40E

/**

  This function find ACPI RSDPTR in TXT heap region.

  @return ACPI RSDPTR in TXT heap region

**/
VOID *
FindTxtAcpiRsdPtr (
  VOID
  )
{
  TXT_OS_TO_SINIT_DATA  *OsSinitData;

  OsSinitData = GetTxtOsToSinitData ();
  if (OsSinitData->Version < 5) {
    return NULL;
  }

  return (VOID *)(UINTN)OsSinitData->RsdpPtr;
}

/**

  This function find ACPI RSDPTR in UEFI or legacy region.

  @return ACPI RSDPTR in UEFI or legacy region

**/
VOID *
FindAcpiRsdPtr (
  VOID
  )
{
  if (mHostContextCommon.AcpiRsdp != 0) {
    return (VOID *)(UINTN)mHostContextCommon.AcpiRsdp;
  } else {
    UINTN  Address;

    //
    // Search EBDA
    //
    Address = (*(UINT16 *)(UINTN)(EBDA_BASE_ADDRESS)) << 4;
    for ( ; Address < 0xA0000; Address += 0x10) {
      if (*(UINT64 *)(Address) == EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER_SIGNATURE) {
        return (VOID *)(Address);
      }
    }

    //
    // First Seach 0x0e0000 - 0x0fffff for RSD Ptr
    //
    for (Address = 0xe0000; Address < 0xfffff; Address += 0x10) {
      if (*(UINT64 *)(Address) == EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER_SIGNATURE) {
        return (VOID *)Address;
      }
    }

    //
    // Not found
    //
    return NULL;
  }
}

/**

  This function scan ACPI table in RSDT.

  @param Rsdt      ACPI RSDT
  @param Signature ACPI table signature

  @return ACPI table

**/
VOID *
ScanTableInRSDT (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Rsdt,
  IN UINT32                       Signature
  )
{
  UINTN                        Index;
  UINT32                       EntryCount;
  UINT32                       *EntryPtr;
  EFI_ACPI_DESCRIPTION_HEADER  *Table;

  EntryCount = (Rsdt->Length - sizeof (EFI_ACPI_DESCRIPTION_HEADER)) / sizeof (UINT32);

  EntryPtr = (UINT32 *)(Rsdt + 1);
  for (Index = 0; Index < EntryCount; Index++, EntryPtr++) {
    Table = (EFI_ACPI_DESCRIPTION_HEADER *)((UINTN)(*EntryPtr));
    if (Table->Signature == Signature) {
      return Table;
    }
  }

  return NULL;
}

/**

  This function scan ACPI table in XSDT.

  @param Xsdt      ACPI XSDT
  @param Signature ACPI table signature

  @return ACPI table

**/
VOID *
ScanTableInXSDT (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Xsdt,
  IN UINT32                       Signature
  )
{
  UINTN                        Index;
  UINT32                       EntryCount;
  UINT64                       EntryPtr;
  UINTN                        BasePtr;
  EFI_ACPI_DESCRIPTION_HEADER  *Table;

  EntryCount = (Xsdt->Length - sizeof (EFI_ACPI_DESCRIPTION_HEADER)) / sizeof (UINT64);

  BasePtr = (UINTN)(Xsdt + 1);
  for (Index = 0; Index < EntryCount; Index++) {
    CopyMem (&EntryPtr, (VOID *)(BasePtr + Index * sizeof (UINT64)), sizeof (UINT64));
    Table = (EFI_ACPI_DESCRIPTION_HEADER *)((UINTN)(EntryPtr));
    if (Table->Signature == Signature) {
      return Table;
    }
  }

  return NULL;
}

/**

  This function find ACPI table according to signature.

  @param RsdPtr    ACPI RSDPTR
  @param Signature ACPI table signature

  @return ACPI table

**/
VOID *
FindAcpiPtr (
  VOID    *RsdPtr,
  UINT32  Signature
  )
{
  EFI_ACPI_DESCRIPTION_HEADER                   *AcpiTable;
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp;
  EFI_ACPI_DESCRIPTION_HEADER                   *Rsdt;
  EFI_ACPI_DESCRIPTION_HEADER                   *Xsdt;

  if (RsdPtr == NULL) {
    return NULL;
  }

  AcpiTable = NULL;

  Rsdp = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)RsdPtr;
  Rsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->RsdtAddress;
  Xsdt = NULL;
  if ((Rsdp->Revision >= 2) && (Rsdp->XsdtAddress < (UINT64)(UINTN)-1)) {
    Xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
  }

  //
  // Check Xsdt
  //
  if (Xsdt != NULL) {
    AcpiTable = ScanTableInXSDT (Xsdt, Signature);
  }

  //
  // Check Rsdt
  //
  if ((AcpiTable == NULL) && (Rsdt != NULL)) {
    AcpiTable = ScanTableInRSDT (Rsdt, Signature);
  }

  return AcpiTable;
}

/**

  This function return CPU number from MADT.

  @return CPU number

**/
UINT32
GetCpuNumFromAcpi (
  VOID
  )
{
  UINT32                                               Index;
  EFI_ACPI_2_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER  *Madt;
  UINTN                                                Length;
  EFI_ACPI_2_0_PROCESSOR_LOCAL_APIC_STRUCTURE          *LocalApic;

  Madt = FindAcpiPtr (FindAcpiRsdPtr (), EFI_ACPI_2_0_MULTIPLE_APIC_DESCRIPTION_TABLE_SIGNATURE);
  if (Madt == NULL) {
    return 1;
  }

  Index     = 0;
  Length    = Madt->Header.Length;
  LocalApic = (EFI_ACPI_2_0_PROCESSOR_LOCAL_APIC_STRUCTURE *)(Madt + 1);
  while ((UINTN)LocalApic < (UINTN)Madt + Length) {
    if (LocalApic->Type == EFI_ACPI_2_0_PROCESSOR_LOCAL_APIC) {
      if ((LocalApic->Flags & EFI_ACPI_2_0_LOCAL_APIC_ENABLED) != 0) {
        Index++;
      }
    } else if (LocalApic->Type == EFI_ACPI_4_0_PROCESSOR_LOCAL_X2APIC) {
      if ((((EFI_ACPI_4_0_PROCESSOR_LOCAL_X2APIC_STRUCTURE *)LocalApic)->Flags & EFI_ACPI_4_0_LOCAL_APIC_ENABLED) != 0) {
        Index++;
      }
    }

    LocalApic = (EFI_ACPI_2_0_PROCESSOR_LOCAL_APIC_STRUCTURE *)((UINTN)LocalApic + LocalApic->Length);
  }

  return Index;
}

/**

  This function return minimal MSEG size required by STM.

  @param StmHeader  Stm header

  @return minimal MSEG size

**/
UINTN
GetMinMsegSize (
  IN STM_HEADER  *StmHeader
  )
{
  UINTN  MinMsegSize;

  MinMsegSize = (STM_PAGES_TO_SIZE (STM_SIZE_TO_PAGES (StmHeader->SwStmHdr.StaticImageSize)) +
                 StmHeader->SwStmHdr.AdditionalDynamicMemorySize +
                 (StmHeader->SwStmHdr.PerProcDynamicMemorySize + GetVmcsSize () * 2) * mHostContextCommon.CpuNum);

  return MinMsegSize;
}

/**

  This function return the index of CPU according to stack.

  @param Register  stack value of this CPU

  @return CPU index

**/
UINT32
GetIndexFromStack (
  IN X86_REGISTER  *Register
  )
{
  STM_HEADER  *StmHeader;
  UINTN       ThisStackTop;
  UINTN       StackBottom;
  UINTN       Index;

  StmHeader = (STM_HEADER *)(UINTN)((UINT32)AsmReadMsr64 (IA32_SMM_MONITOR_CTL_MSR_INDEX) & 0xFFFFF000);

  DEBUG ((DEBUG_ERROR, "[%a][L%d] - StmHeader at 0x%p.\n", __func__, __LINE__, StmHeader));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - ApicId is 0x%x.\n", __func__, __LINE__, GetApicId ()));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - GetIndexFromStack (this function) is at 0x%p.\n", __func__, __LINE__, GetIndexFromStack));

  //
  // Stack top of this CPU
  //
  ThisStackTop = ((UINTN)Register + SIZE_4KB - 1) & ~(SIZE_4KB - 1);
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - ThisStackTop = 0x%lx.\n", __func__, __LINE__, ThisStackTop));

  //
  // EspOffset pointer to bottom of 1st CPU
  //
  StackBottom = (UINTN)StmHeader + StmHeader->HwStmHdr.EspOffset;
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - StackBottom = 0x%lx.\n", __func__, __LINE__, StackBottom));

  Index = (ThisStackTop - StackBottom) / StmHeader->SwStmHdr.PerProcDynamicMemorySize;
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Index = 0x%lx.\n", __func__, __LINE__, Index));

  //
  // Need minus one for 0-based CPU index
  //
  return (UINT32)(Index - 1);
}

/**

  This function initialize STM heap.

  @param StmHeader MSEG STM header

**/
VOID
InitHeap (
  IN STM_HEADER  *StmHeader
  )
{
  mHostContextCommon.HeapBottom = (UINT64)((UINTN)StmHeader +
                                           StmHeader->HwStmHdr.Cr3Offset +
                                           STM_PAGES_TO_SIZE (6)); // reserve 6 page for page table
  mHostContextCommon.HeapTop = (UINT64)((UINTN)StmHeader +
                                        STM_PAGES_TO_SIZE (STM_SIZE_TO_PAGES (StmHeader->SwStmHdr.StaticImageSize)) +
                                        StmHeader->SwStmHdr.AdditionalDynamicMemorySize);
}

/**

  This function initialize basic context for STM.

**/
VOID
InitBasicContext (
  VOID
  )
{
  mHostContextCommon.HostContextPerCpu         = AllocatePages (STM_SIZE_TO_PAGES (sizeof (SEA_HOST_CONTEXT_PER_CPU)) * mHostContextCommon.CpuNum);
  DEBUG ((DEBUG_INFO, "[%a] - (CpuNum = %d) mHostContextCommon.HostContextPerCpu = 0x%p.\n", __func__, mHostContextCommon.CpuNum, mHostContextCommon.HostContextPerCpu));
  mGuestContextCommonNormal.GuestContextPerCpu = AllocatePages (STM_SIZE_TO_PAGES (sizeof (SEA_GUEST_CONTEXT_PER_CPU)) * mHostContextCommon.CpuNum);
  DEBUG ((DEBUG_INFO, "[%a] - (CpuNum = %d) mGuestContextCommonNormal.GuestContextPerCpu = 0x%p.\n", __func__, mHostContextCommon.CpuNum, mGuestContextCommonNormal.GuestContextPerCpu));
}

/**

  This function initialize BSP.

  @param Register X86 register context

**/
VOID
BspInit (
  IN X86_REGISTER  *Register
  )
{
  STM_HEADER                    *StmHeader;
  TXT_PROCESSOR_SMM_DESCRIPTOR  *TxtProcessorSmmDescriptor;
  X86_REGISTER                  *Reg;
  IA32_IDT_GATE_DESCRIPTOR      *IdtGate;
  UINT32                        SubIndex;
  UINTN                         XStateSize;
  UINT32                        RegEax;
  IA32_VMX_MISC_MSR             VmxMisc;

  StmHeader = (STM_HEADER *)(UINTN)((UINT32)AsmReadMsr64 (IA32_SMM_MONITOR_CTL_MSR_INDEX) & 0xFFFFF000);

  InitHeap (StmHeader);
  // after that we can use mHostContextCommon

  InitializeSpinLock (&mHostContextCommon.DebugLock);
  // after that we can use DEBUG

  DEBUG ((EFI_D_INFO, "!!!STM build time - %a %a!!!\n", (CHAR8 *)__DATE__, (CHAR8 *)__TIME__));
  DEBUG ((EFI_D_INFO, "!!!STM Relocation DONE!!!\n"));
  DEBUG ((EFI_D_INFO, "!!!Enter StmInit (BSP)!!! - %d (%x)\n", (UINTN)0, (UINTN)ReadUnaligned32 ((UINT32 *)&Register->Rax)));

  // Check Signature and size
  VmxMisc.Uint64 = AsmReadMsr64 (IA32_VMX_MISC_MSR_INDEX);
  if ((VmxMisc.Uint64 & BIT15) != 0) {
    TxtProcessorSmmDescriptor = (TXT_PROCESSOR_SMM_DESCRIPTOR *)(UINTN)(AsmReadMsr64 (IA32_SMBASE_INDEX) + SMM_TXTPSD_OFFSET);
  } else {
    TxtProcessorSmmDescriptor = (TXT_PROCESSOR_SMM_DESCRIPTOR *)(UINTN)(VmRead32 (VMCS_32_GUEST_SMBASE_INDEX) + SMM_TXTPSD_OFFSET);
  }

  // We have to know CpuNum, or we do not know where VMCS will be.
  if (IsSentryEnabled ()) {
    mHostContextCommon.CpuNum = GetCpuNumFromTxt ();
    DEBUG ((EFI_D_INFO, "CpuNumber from TXT Region - %d\n", (UINTN)mHostContextCommon.CpuNum));
  } else {
    {
      EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp;
      EFI_ACPI_DESCRIPTION_HEADER                   *Rsdt;
      EFI_ACPI_DESCRIPTION_HEADER                   *Xsdt;

      mHostContextCommon.AcpiRsdp = TxtProcessorSmmDescriptor->AcpiRsdp;
      Rsdp                        = FindAcpiRsdPtr ();
      DEBUG ((EFI_D_INFO, "Rsdp - %08x\n", Rsdp));
      if (Rsdp == NULL) {
        CpuDeadLoop ();
      }

      Rsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->RsdtAddress;
      DEBUG ((EFI_D_INFO, "Rsdt - %08x\n", Rsdt));
      DEBUG ((EFI_D_INFO, "RsdtLen - %08x\n", Rsdt->Length));
      if ((Rsdp->Revision >= 2) && (Rsdp->XsdtAddress < (UINT64)(UINTN)-1)) {
        Xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
        DEBUG ((EFI_D_INFO, "Xsdt - %016lx\n", Xsdt));
        DEBUG ((EFI_D_INFO, "XsdtLen - %08x\n", Xsdt->Length));
      }
    }

    mHostContextCommon.CpuNum = GetCpuNumFromAcpi ();
    DEBUG ((EFI_D_INFO, "CpuNumber from ACPI MADT - %d\n", (UINTN)mHostContextCommon.CpuNum));
  }

  InitializeSpinLock (&mHostContextCommon.MemoryLock);
  InitializeSpinLock (&mHostContextCommon.SmiVmcallLock);
  InitializeSpinLock (&mHostContextCommon.ResponderLock);

  DEBUG ((EFI_D_INFO, "HeapBottom - %08x\n", mHostContextCommon.HeapBottom));
  DEBUG ((EFI_D_INFO, "HeapTop    - %08x\n", mHostContextCommon.HeapTop));

  DEBUG ((EFI_D_INFO, "TxtProcessorSmmDescriptor     - %08x\n", (UINTN)TxtProcessorSmmDescriptor));
  DEBUG ((EFI_D_INFO, "  Signature                   - %016lx\n", TxtProcessorSmmDescriptor->Signature));
  DEBUG ((EFI_D_INFO, "  Size                        - %04x\n", (UINTN)TxtProcessorSmmDescriptor->Size));
  DEBUG ((EFI_D_INFO, "  SmmDescriptorVerMajor       - %02x\n", (UINTN)TxtProcessorSmmDescriptor->SmmDescriptorVerMajor));
  DEBUG ((EFI_D_INFO, "  SmmDescriptorVerMinor       - %02x\n", (UINTN)TxtProcessorSmmDescriptor->SmmDescriptorVerMinor));
  DEBUG ((EFI_D_INFO, "  LocalApicId                 - %08x\n", (UINTN)TxtProcessorSmmDescriptor->LocalApicId));
  DEBUG ((EFI_D_INFO, "  ExecutionDisableOutsideSmrr - %02x\n", (UINTN)TxtProcessorSmmDescriptor->SmmEntryState.ExecutionDisableOutsideSmrr));
  DEBUG ((EFI_D_INFO, "  Intel64Mode                 - %02x\n", (UINTN)TxtProcessorSmmDescriptor->SmmEntryState.Intel64Mode));
  DEBUG ((EFI_D_INFO, "  Cr4Pae                      - %02x\n", (UINTN)TxtProcessorSmmDescriptor->SmmEntryState.Cr4Pae));
  DEBUG ((EFI_D_INFO, "  Cr4Pse                      - %02x\n", (UINTN)TxtProcessorSmmDescriptor->SmmEntryState.Cr4Pse));
  DEBUG ((EFI_D_INFO, "  SmramToVmcsRestoreRequired  - %02x\n", (UINTN)TxtProcessorSmmDescriptor->SmmResumeState.SmramToVmcsRestoreRequired));
  DEBUG ((EFI_D_INFO, "  ReinitializeVmcsRequired    - %02x\n", (UINTN)TxtProcessorSmmDescriptor->SmmResumeState.ReinitializeVmcsRequired));
  DEBUG ((EFI_D_INFO, "  DomainType                  - %02x\n", (UINTN)TxtProcessorSmmDescriptor->StmSmmState.DomainType));
  DEBUG ((EFI_D_INFO, "  XStatePolicy                - %02x\n", (UINTN)TxtProcessorSmmDescriptor->StmSmmState.XStatePolicy));
  DEBUG ((EFI_D_INFO, "  EptEnabled                  - %02x\n", (UINTN)TxtProcessorSmmDescriptor->StmSmmState.EptEnabled));
  DEBUG ((EFI_D_INFO, "  SmmCs                       - %04x\n", (UINTN)TxtProcessorSmmDescriptor->SmmCs));
  DEBUG ((EFI_D_INFO, "  SmmDs                       - %04x\n", (UINTN)TxtProcessorSmmDescriptor->SmmDs));
  DEBUG ((EFI_D_INFO, "  SmmSs                       - %04x\n", (UINTN)TxtProcessorSmmDescriptor->SmmSs));
  DEBUG ((EFI_D_INFO, "  SmmOtherSegment             - %04x\n", (UINTN)TxtProcessorSmmDescriptor->SmmOtherSegment));
  DEBUG ((EFI_D_INFO, "  SmmTr                       - %04x\n", (UINTN)TxtProcessorSmmDescriptor->SmmTr));
  DEBUG ((EFI_D_INFO, "  SmmCr3                      - %016lx\n", TxtProcessorSmmDescriptor->SmmCr3));
  DEBUG ((EFI_D_INFO, "  SmmStmSetupRip              - %016lx\n", TxtProcessorSmmDescriptor->SmmStmSetupRip));
  DEBUG ((EFI_D_INFO, "  SmmStmTeardownRip           - %016lx\n", TxtProcessorSmmDescriptor->SmmStmTeardownRip));
  DEBUG ((EFI_D_INFO, "  SmmSmiHandlerRip            - %016lx\n", TxtProcessorSmmDescriptor->SmmSmiHandlerRip));
  DEBUG ((EFI_D_INFO, "  SmmSmiHandlerRsp            - %016lx\n", TxtProcessorSmmDescriptor->SmmSmiHandlerRsp));
  DEBUG ((EFI_D_INFO, "  SmmGdtPtr                   - %016lx\n", TxtProcessorSmmDescriptor->SmmGdtPtr));
  DEBUG ((EFI_D_INFO, "  SmmGdtSize                  - %08x\n", (UINTN)TxtProcessorSmmDescriptor->SmmGdtSize));
  DEBUG ((EFI_D_INFO, "  RequiredStmSmmRevId         - %08x\n", (UINTN)TxtProcessorSmmDescriptor->RequiredStmSmmRevId));
  DEBUG ((EFI_D_INFO, "  StmProtectionExceptionHandler:\n"));
  DEBUG ((EFI_D_INFO, "    SpeRip                    - %016lx\n", TxtProcessorSmmDescriptor->StmProtectionExceptionHandler.SpeRip));
  DEBUG ((EFI_D_INFO, "    SpeRsp                    - %016lx\n", TxtProcessorSmmDescriptor->StmProtectionExceptionHandler.SpeRsp));
  DEBUG ((EFI_D_INFO, "    SpeSs                     - %04x\n", (UINTN)TxtProcessorSmmDescriptor->StmProtectionExceptionHandler.SpeSs));
  DEBUG ((EFI_D_INFO, "    PageViolationException    - %04x\n", (UINTN)TxtProcessorSmmDescriptor->StmProtectionExceptionHandler.PageViolationException));
  DEBUG ((EFI_D_INFO, "    MsrViolationException     - %04x\n", (UINTN)TxtProcessorSmmDescriptor->StmProtectionExceptionHandler.MsrViolationException));
  DEBUG ((EFI_D_INFO, "    RegisterViolationException- %04x\n", (UINTN)TxtProcessorSmmDescriptor->StmProtectionExceptionHandler.RegisterViolationException));
  DEBUG ((EFI_D_INFO, "    IoViolationException      - %04x\n", (UINTN)TxtProcessorSmmDescriptor->StmProtectionExceptionHandler.IoViolationException));
  DEBUG ((EFI_D_INFO, "    PciViolationException     - %04x\n", (UINTN)TxtProcessorSmmDescriptor->StmProtectionExceptionHandler.PciViolationException));
  DEBUG ((EFI_D_INFO, "  BiosHwResourceRequirements  - %016lx\n", TxtProcessorSmmDescriptor->BiosHwResourceRequirementsPtr));
  DEBUG ((EFI_D_INFO, "  AcpiRsdp                    - %016lx\n", TxtProcessorSmmDescriptor->AcpiRsdp));
  DEBUG ((EFI_D_INFO, "  PhysicalAddressBits         - %02x\n", (UINTN)TxtProcessorSmmDescriptor->PhysicalAddressBits));

  if (TxtProcessorSmmDescriptor->Signature != TXT_PROCESSOR_SMM_DESCRIPTOR_SIGNATURE) {
    DEBUG ((EFI_D_INFO, "TXT Descriptor Signature ERROR - %016lx!\n", TxtProcessorSmmDescriptor->Signature));
    CpuDeadLoop ();
  }

  if (TxtProcessorSmmDescriptor->Size != sizeof (TXT_PROCESSOR_SMM_DESCRIPTOR)) {
    DEBUG ((EFI_D_INFO, "TXT Descriptor Size ERROR - %08x!\n", TxtProcessorSmmDescriptor->Size));
    CpuDeadLoop ();
  }

  InitBasicContext ();

  DEBUG ((EFI_D_INFO, "Register(%d) - %08x\n", (UINTN)0, Register));
  Reg           = &mGuestContextCommonNormal.GuestContextPerCpu[0].Register;
  Register->Rsp = VmReadN (VMCS_N_GUEST_RSP_INDEX);
  CopyMem (Reg, Register, sizeof (X86_REGISTER));

  mHostContextCommon.StmHeader = StmHeader;
  DEBUG ((EFI_D_INFO, "StmHeader                     - %08x\n", (UINTN)mHostContextCommon.StmHeader));
  DEBUG ((EFI_D_INFO, "Hardware field:\n"));
  DEBUG ((EFI_D_INFO, "  MsegHeaderRevision          - %08x\n", (UINTN)StmHeader->HwStmHdr.MsegHeaderRevision));
  DEBUG ((EFI_D_INFO, "  MonitorFeatures             - %08x\n", (UINTN)StmHeader->HwStmHdr.MonitorFeatures));
  DEBUG ((EFI_D_INFO, "  GdtrLimit                   - %08x\n", (UINTN)StmHeader->HwStmHdr.GdtrLimit));
  DEBUG ((EFI_D_INFO, "  GdtrBaseOffset              - %08x\n", (UINTN)StmHeader->HwStmHdr.GdtrBaseOffset));
  DEBUG ((EFI_D_INFO, "  CsSelector                  - %08x\n", (UINTN)StmHeader->HwStmHdr.CsSelector));
  DEBUG ((EFI_D_INFO, "  EipOffset                   - %08x\n", (UINTN)StmHeader->HwStmHdr.EipOffset));
  DEBUG ((EFI_D_INFO, "  EspOffset                   - %08x\n", (UINTN)StmHeader->HwStmHdr.EspOffset));
  DEBUG ((EFI_D_INFO, "  Cr3Offset                   - %08x\n", (UINTN)StmHeader->HwStmHdr.Cr3Offset));
  DEBUG ((EFI_D_INFO, "Software field:\n"));
  DEBUG ((EFI_D_INFO, "  StmSpecVerMajor             - %02x\n", (UINTN)StmHeader->SwStmHdr.StmSpecVerMajor));
  DEBUG ((EFI_D_INFO, "  StmSpecVerMinor             - %02x\n", (UINTN)StmHeader->SwStmHdr.StmSpecVerMinor));
  DEBUG ((EFI_D_INFO, "  StaticImageSize             - %08x\n", (UINTN)StmHeader->SwStmHdr.StaticImageSize));
  DEBUG ((EFI_D_INFO, "  PerProcDynamicMemorySize    - %08x\n", (UINTN)StmHeader->SwStmHdr.PerProcDynamicMemorySize));
  DEBUG ((EFI_D_INFO, "  AdditionalDynamicMemorySize - %08x\n", (UINTN)StmHeader->SwStmHdr.AdditionalDynamicMemorySize));
  DEBUG ((EFI_D_INFO, "  Intel64ModeSupported        - %08x\n", (UINTN)StmHeader->SwStmHdr.StmFeatures.Intel64ModeSupported));
  DEBUG ((EFI_D_INFO, "  EptSupported                - %08x\n", (UINTN)StmHeader->SwStmHdr.StmFeatures.EptSupported));
  DEBUG ((EFI_D_INFO, "  NumberOfRevIDs              - %08x\n", (UINTN)StmHeader->SwStmHdr.NumberOfRevIDs));
  for (SubIndex = 0; SubIndex < StmHeader->SwStmHdr.NumberOfRevIDs; SubIndex++) {
    DEBUG ((EFI_D_INFO, "  StmSmmRevID(%02d)             - %08x\n", (UINTN)SubIndex, (UINTN)StmHeader->SwStmHdr.StmSmmRevID[SubIndex]));
  }

  mHostContextCommon.AcpiRsdp = TxtProcessorSmmDescriptor->AcpiRsdp;

  //
  // Check MSEG BASE/SIZE in TXT region
  //
  mHostContextCommon.StmSize = GetMinMsegSize (StmHeader);
  DEBUG ((EFI_D_INFO, "MinMsegSize - %08x!\n", (UINTN)mHostContextCommon.StmSize));

  mHostContextCommon.PhysicalAddressBits = TxtProcessorSmmDescriptor->PhysicalAddressBits;
  AsmCpuid (CPUID_EXTENDED_INFORMATION, &RegEax, NULL, NULL, NULL);
  if (RegEax >= CPUID_EXTENDED_ADDRESS_SIZE) {
    AsmCpuid (CPUID_EXTENDED_ADDRESS_SIZE, &RegEax, NULL, NULL, NULL);
    RegEax = (UINT8)RegEax;
    DEBUG ((EFI_D_INFO, "CPUID - PhysicalAddressBits - 0x%02x\n", (UINT8)RegEax));
  } else {
    RegEax = 36;
  }

  if ((mHostContextCommon.PhysicalAddressBits == 0) || (mHostContextCommon.PhysicalAddressBits > (UINT8)RegEax)) {
    mHostContextCommon.PhysicalAddressBits = (UINT8)RegEax;
  }

  if (sizeof (UINTN) == sizeof (UINT32)) {
    if (mHostContextCommon.PhysicalAddressBits > 32) {
      mHostContextCommon.PhysicalAddressBits = 32;
    }
  }
  DEBUG ((EFI_D_INFO, "mHostContextCommon.PhysicalAddressBits - 0x%08x!\n", (UINT8)mHostContextCommon.PhysicalAddressBits));

  mHostContextCommon.MaximumSupportAddress = (LShiftU64 (1, mHostContextCommon.PhysicalAddressBits) - 1);
  DEBUG ((EFI_D_INFO, "mHostContextCommon.MaximumSupportAddress - 0x%lx!\n", (UINT8)mHostContextCommon.MaximumSupportAddress));

  mHostContextCommon.PageTable = AsmReadCr3 ();
  AsmReadGdtr (&mHostContextCommon.Gdtr);

  //
  // Set up STM host IDT to catch exception
  //
  mHostContextCommon.Idtr.Limit = (UINT16)(STM_MAX_IDT_NUM * sizeof (IA32_IDT_GATE_DESCRIPTOR) - 1);
  mHostContextCommon.Idtr.Base  = (UINTN)AllocatePages (STM_SIZE_TO_PAGES (mHostContextCommon.Idtr.Limit + 1));
  IdtGate                       = (IA32_IDT_GATE_DESCRIPTOR *)mHostContextCommon.Idtr.Base;
  InitializeExternalVectorTablePtr (IdtGate);

  //
  // Allocate XState buffer
  //
  XStateSize                                 = CalculateXStateSize ();
  mGuestContextCommonNormal.ZeroXStateBuffer = (UINTN)AllocatePages (STM_SIZE_TO_PAGES (XStateSize));
  for (SubIndex = 0; SubIndex < mHostContextCommon.CpuNum; SubIndex++) {
    mGuestContextCommonNormal.GuestContextPerCpu[SubIndex].XStateBuffer = (UINTN)AllocatePages (STM_SIZE_TO_PAGES (XStateSize));
  }

  for (SubIndex = 0; SubIndex < mHostContextCommon.CpuNum; SubIndex++) {
    mHostContextCommon.HostContextPerCpu[SubIndex].HostMsrEntryCount          = 1;
    mGuestContextCommonNormal.GuestContextPerCpu[SubIndex].GuestMsrEntryCount = 1;
  }

  mHostContextCommon.HostContextPerCpu[0].HostMsrEntryAddress          = (UINT64)(UINTN)AllocatePages (STM_SIZE_TO_PAGES (sizeof (VM_EXIT_MSR_ENTRY) * mHostContextCommon.HostContextPerCpu[0].HostMsrEntryCount * mHostContextCommon.CpuNum));
  mGuestContextCommonNormal.GuestContextPerCpu[0].GuestMsrEntryAddress = (UINT64)(UINTN)AllocatePages (STM_SIZE_TO_PAGES (sizeof (VM_EXIT_MSR_ENTRY) * mGuestContextCommonNormal.GuestContextPerCpu[0].GuestMsrEntryCount * mHostContextCommon.CpuNum));
  for (SubIndex = 0; SubIndex < mHostContextCommon.CpuNum; SubIndex++) {
    mHostContextCommon.HostContextPerCpu[SubIndex].HostMsrEntryAddress          = mHostContextCommon.HostContextPerCpu[0].HostMsrEntryAddress + sizeof (VM_EXIT_MSR_ENTRY) * mHostContextCommon.HostContextPerCpu[0].HostMsrEntryCount * SubIndex;
    mGuestContextCommonNormal.GuestContextPerCpu[SubIndex].GuestMsrEntryAddress = mGuestContextCommonNormal.GuestContextPerCpu[0].GuestMsrEntryAddress + sizeof (VM_EXIT_MSR_ENTRY) * mGuestContextCommonNormal.GuestContextPerCpu[0].GuestMsrEntryCount * SubIndex;
  }

  //
  // Add more paging for Host CR3.
  //
  CreateHostPaging ();

  STM_PERF_INIT;

  //
  // Initialization done
  //
  mIsBspInitialized = TRUE;

  return;
}

/**

  This function initialize AP.

  @param Index    CPU index
  @param Register X86 register context

**/
VOID
ApInit (
  IN UINT32        Index,
  IN X86_REGISTER  *Register
  )
{
  X86_REGISTER  *Reg;

  DEBUG ((EFI_D_INFO, "!!!Enter StmInit (AP done)!!! - %d (%x)\n", (UINTN)Index, (UINTN)ReadUnaligned32 ((UINT32 *)&Register->Rax)));

  DEBUG ((DEBUG_ERROR, "[%a] - Index Given = %d.\n", __func__, Index));
  DEBUG ((DEBUG_ERROR, "[%a] - Register at 0x%lx.\n", __func__, Register));

  if (Index >= mHostContextCommon.CpuNum) {
    DEBUG ((EFI_D_INFO, "!!!Index(0x%x) >= mHostContextCommon.CpuNum(0x%x)\n", (UINTN)Index, (UINTN)mHostContextCommon.CpuNum));
    CpuDeadLoop ();
    Index = GetIndexFromStack (Register);
  }

  InterlockedIncrement (&mHostContextCommon.JoinedCpuNum);

  DEBUG ((EFI_D_INFO, "Register(%d) - %08x\n", (UINTN)Index, Register));
  Reg           = &mGuestContextCommonNormal.GuestContextPerCpu[Index].Register;
  Register->Rsp = VmReadN (VMCS_N_GUEST_RSP_INDEX);
  CopyMem (Reg, Register, sizeof (X86_REGISTER));

  if (mHostContextCommon.JoinedCpuNum > mHostContextCommon.CpuNum) {
    DEBUG ((DEBUG_ERROR, "JoinedCpuNum(%d) > CpuNum(%d)\n", (UINTN)mHostContextCommon.JoinedCpuNum, (UINTN)mHostContextCommon.CpuNum));
    // Reset system
    CpuDeadLoop ();
  }

  return;
}

/**

  This function initialize common part for BSP and AP.

  @param Index    CPU index

**/
VOID
CommonInit (
  IN UINT32  Index
  )
{
  UINTN              StackBase;
  UINTN              StackSize;
  STM_HEADER         *StmHeader;
  UINT32             RegEdx;
  IA32_VMX_MISC_MSR  VmxMisc;

  AsmWriteCr4 (AsmReadCr4 () | CR4_OSFXSR | CR4_OSXMMEXCPT);
  if (IsXStateSupported ()) {
    AsmWriteCr4 (AsmReadCr4 () | CR4_OSXSAVE);
  }

  VmxMisc.Uint64 = AsmReadMsr64 (IA32_VMX_MISC_MSR_INDEX);
  RegEdx         = ReadUnaligned32 ((UINT32 *)&mGuestContextCommonNormal.GuestContextPerCpu[Index].Register.Rdx);
  if ((RegEdx & STM_CONFIG_SMI_UNBLOCKING_BY_VMX_OFF) != 0) {
    if (VmxMisc.Bits.VmxOffUnblockSmiSupport != 0) {
      AsmWriteMsr64 (IA32_SMM_MONITOR_CTL_MSR_INDEX, AsmReadMsr64 (IA32_SMM_MONITOR_CTL_MSR_INDEX) | IA32_SMM_MONITOR_SMI_UNBLOCKING_BY_VMX_OFF);
    }
  }

  mHostContextCommon.HostContextPerCpu[Index].Index  = Index;
  mHostContextCommon.HostContextPerCpu[Index].ApicId = ReadLocalApicId ();

  StmHeader = mHostContextCommon.StmHeader;
  StackBase = (UINTN)StmHeader +
              STM_PAGES_TO_SIZE (STM_SIZE_TO_PAGES (StmHeader->SwStmHdr.StaticImageSize)) +
              StmHeader->SwStmHdr.AdditionalDynamicMemorySize;
  StackSize                                         = StmHeader->SwStmHdr.PerProcDynamicMemorySize;
  DEBUG ((EFI_D_INFO, "%a - Stack(%d) - StackSize = 0x%lx\n", __func__, (UINTN)Index, StackSize));
  mHostContextCommon.HostContextPerCpu[Index].Stack = (UINTN)(StackBase + StackSize * (Index + 1)); // Stack Top

  if ((VmxMisc.Uint64 & BIT15) != 0) {
    mHostContextCommon.HostContextPerCpu[Index].Smbase = (UINT32)AsmReadMsr64 (IA32_SMBASE_INDEX);
  } else {
    mHostContextCommon.HostContextPerCpu[Index].Smbase = VmRead32 (VMCS_32_GUEST_SMBASE_INDEX);
  }

  mHostContextCommon.HostContextPerCpu[Index].TxtProcessorSmmDescriptor = (TXT_PROCESSOR_SMM_DESCRIPTOR *)(UINTN)(mHostContextCommon.HostContextPerCpu[Index].Smbase + SMM_TXTPSD_OFFSET);

  DEBUG ((EFI_D_INFO, "SMBASE(%d) - %08x\n", (UINTN)Index, (UINTN)mHostContextCommon.HostContextPerCpu[Index].Smbase));
  DEBUG ((EFI_D_INFO, "TxtProcessorSmmDescriptor(%d) - %08x\n", (UINTN)Index, mHostContextCommon.HostContextPerCpu[Index].TxtProcessorSmmDescriptor));
  DEBUG ((EFI_D_INFO, "Stack(%d) - %08x\n", (UINTN)Index, (UINTN)mHostContextCommon.HostContextPerCpu[Index].Stack));
}

/**

  This function launch back to MLE.

  @param Index    CPU index
  @param Register X86 register context
**/
VOID
LaunchBack (
  IN UINT32        Index,
  IN X86_REGISTER  *Register
  )
{
  UINTN  Rflags;

  //
  // Indicate operation status from caller.
  //
  VmWriteN (VMCS_N_GUEST_RFLAGS_INDEX, VmReadN (VMCS_N_GUEST_RFLAGS_INDEX) & ~RFLAGS_CF);

  DEBUG ((DEBUG_ERROR, "Register @ LaunchBack: 0x%lx\n", (UINTN)Register));

  DEBUG ((EFI_D_INFO, "!!!LaunchBack (%d)!!!\n", (UINTN)Index));
  DEBUG ((DEBUG_ERROR, "VMCS_32_CONTROL_VMEXIT_CONTROLS_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_CONTROL_VMEXIT_CONTROLS_INDEX)));
  DEBUG ((DEBUG_ERROR, "VMCS_32_CONTROL_VMENTRY_CONTROLS_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_CONTROL_VMENTRY_CONTROLS_INDEX)));
  DEBUG ((DEBUG_ERROR, "CR0: %08x\n", (UINTN)AsmReadCr0 ()));
  DEBUG ((DEBUG_ERROR, "CR3: %08x\n", (UINTN)AsmReadCr3 ()));
  DEBUG ((DEBUG_ERROR, "CR4: %08x\n", (UINTN)AsmReadCr4 ()));
  DEBUG ((DEBUG_ERROR, "IA32_EFER_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_EFER_MSR_INDEX)));
  DEBUG ((DEBUG_ERROR, "IA32_SYSENTER_ESP_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_SYSENTER_ESP_MSR_INDEX)));
  DEBUG ((DEBUG_ERROR, "IA32_SYSENTER_EIP_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_SYSENTER_EIP_MSR_INDEX)));
  DEBUG ((DEBUG_ERROR, "IA32_PERF_GLOBAL_CTRL_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_PERF_GLOBAL_CTRL_MSR_INDEX)));
  DEBUG ((DEBUG_ERROR, "IA32_CR_PAT_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_CR_PAT_MSR_INDEX)));
  DEBUG ((DEBUG_ERROR, "IA32_S_CET: %08x\n", (UINTN)AsmReadMsr64 (0x6A2)));
  DEBUG ((DEBUG_ERROR, "IA32_PKRS: %08x\n", (UINTN)AsmReadMsr64 (0x6E1)));

  DEBUG ((DEBUG_ERROR, "Host-state CR0: %08x\n", (UINTN)VmReadN (VMCS_N_HOST_CR0_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state CR3: %08x\n", (UINTN)VmReadN (VMCS_N_HOST_CR3_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state CR4: %08x\n", (UINTN)VmReadN (VMCS_N_HOST_CR4_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_64_HOST_IA32_EFER_INDEX: %08x\n", (UINTN)VmReadN (VMCS_64_HOST_IA32_EFER_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_N_HOST_IA32_SYSENTER_ESP_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_HOST_IA32_SYSENTER_ESP_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_N_HOST_IA32_SYSENTER_EIP_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_HOST_IA32_SYSENTER_EIP_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_64_HOST_IA32_PERF_GLOBAL_CTRL_INDEX: %08x\n", (UINTN)VmRead64 (VMCS_64_HOST_IA32_PERF_GLOBAL_CTRL_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_64_HOST_IA32_PAT_INDEX: %08x\n", (UINTN)VmRead64 (VMCS_64_HOST_IA32_PAT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_N_HOST_RIP_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_HOST_RIP_INDEX)));

  DEBUG ((DEBUG_ERROR, "Host-state VMCS_16_HOST_ES_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_HOST_ES_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_16_HOST_CS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_HOST_CS_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_16_HOST_SS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_HOST_SS_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_16_HOST_DS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_HOST_DS_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_16_HOST_FS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_HOST_FS_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_16_HOST_GS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_HOST_GS_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_16_HOST_TR_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_HOST_TR_INDEX)));

  DEBUG ((DEBUG_ERROR, "Host-state VMCS_N_HOST_FS_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_HOST_FS_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_N_HOST_GS_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_HOST_GS_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_N_HOST_TR_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_HOST_TR_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_N_HOST_GDTR_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_HOST_GDTR_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Host-state VMCS_N_HOST_IDTR_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_HOST_IDTR_BASE_INDEX)));

  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rax = 0x%lx.\n", __func__, __LINE__, Register->Rax));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rcx = 0x%lx.\n", __func__, __LINE__, Register->Rcx));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rdx = 0x%lx.\n", __func__, __LINE__, Register->Rdx));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rbx = 0x%lx.\n", __func__, __LINE__, Register->Rbx));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rsp = 0x%lx.\n", __func__, __LINE__, Register->Rsp));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rbp = 0x%lx.\n", __func__, __LINE__, Register->Rbp));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rsi = 0x%lx.\n", __func__, __LINE__, Register->Rsi));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rdi = 0x%lx.\n", __func__, __LINE__, Register->Rdi));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R8  = 0x%lx.\n", __func__, __LINE__, Register->R8));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R9  = 0x%lx.\n", __func__, __LINE__, Register->R9));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R10 = 0x%lx.\n", __func__, __LINE__, Register->R10));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R11 = 0x%lx.\n", __func__, __LINE__, Register->R11));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R12 = 0x%lx.\n", __func__, __LINE__, Register->R12));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R13 = 0x%lx.\n", __func__, __LINE__, Register->R13));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R14 = 0x%lx.\n", __func__, __LINE__, Register->R14));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R15 = 0x%lx.\n", __func__, __LINE__, Register->R15));

  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_CR0_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_CR0_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_CR3_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_CR3_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_CR4_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_CR4_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_DR7_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_DR7_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_RSP_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_RSP_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_RIP_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_RIP_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_RFLAGS_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_RFLAGS_INDEX)));

  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_64_GUEST_IA32_DEBUGCTL_INDEX: %08x\n", (UINTN)VmRead64 (VMCS_64_GUEST_IA32_DEBUGCTL_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_IA32_SYSENTER_ESP_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_IA32_SYSENTER_ESP_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_IA32_SYSENTER_EIP_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_IA32_SYSENTER_EIP_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_64_GUEST_IA32_EFER_INDEX: %08x\n", (UINTN)VmRead64 (VMCS_64_GUEST_IA32_EFER_INDEX)));

  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_16_GUEST_ES_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_ES_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_16_GUEST_CS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_CS_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_16_GUEST_SS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_SS_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_16_GUEST_DS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_DS_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_16_GUEST_FS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_FS_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_16_GUEST_GS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_GS_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_16_GUEST_LDTR_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_LDTR_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_16_GUEST_TR_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_TR_INDEX)));

  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_ES_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_ES_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_CS_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_CS_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_SS_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_SS_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_DS_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_DS_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_FS_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_FS_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_GS_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_GS_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_LDTR_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_LDTR_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_TR_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_TR_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_GDTR_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_GDTR_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_IDTR_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_IDTR_LIMIT_INDEX)));

  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_ES_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_ES_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_CS_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_CS_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_SS_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_SS_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_DS_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_DS_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_FS_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_FS_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_GS_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_GS_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_LDTR_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_LDTR_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_TR_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_TR_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_GDTR_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_GDTR_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_IDTR_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_IDTR_BASE_INDEX)));

  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_ES_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_ES_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_CS_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_CS_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_SS_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_SS_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_DS_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_DS_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_FS_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_FS_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_GS_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_GS_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_LDTR_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_LDTR_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_TR_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_TR_ACCESS_RIGHT_INDEX)));

  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_CONTROL_PROCESSOR_BASED_VM_EXECUTION_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_CONTROL_PROCESSOR_BASED_VM_EXECUTION_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_CONTROL_2ND_PROCESSOR_BASED_VM_EXECUTION_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_CONTROL_2ND_PROCESSOR_BASED_VM_EXECUTION_INDEX)));

  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_INTERRUPTIBILITY_STATE_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_INTERRUPTIBILITY_STATE_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_32_GUEST_ACTIVITY_STATE_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_ACTIVITY_STATE_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_N_GUEST_PENDING_DEBUG_EXCEPTIONS_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_PENDING_DEBUG_EXCEPTIONS_INDEX)));
  DEBUG ((DEBUG_ERROR, "On exit Guest-state VMCS_64_GUEST_VMCS_LINK_PTR_INDEX: %08x\n", (UINTN)VmRead64 (VMCS_64_GUEST_VMCS_LINK_PTR_INDEX)));

  // Clear CR4 fixed bit 13 for VMXE
  AsmWriteMsr64 (IA32_VMX_CR4_FIXED0_MSR_INDEX, AsmReadMsr64 (IA32_VMX_CR4_FIXED0_MSR_INDEX) & (~BIT13));
  DEBUG ((DEBUG_ERROR, "On Exit MSR IA32_VMX_CR0_FIXED0_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_VMX_CR0_FIXED0_MSR_INDEX)));
  DEBUG ((DEBUG_ERROR, "On Exit MSR IA32_VMX_CR0_FIXED1_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_VMX_CR0_FIXED1_MSR_INDEX)));
  DEBUG ((DEBUG_ERROR, "On Exit MSR IA32_VMX_CR4_FIXED0_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_VMX_CR4_FIXED0_MSR_INDEX)));
  DEBUG ((DEBUG_ERROR, "On Exit MSR IA32_VMX_CR4_FIXED1_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_VMX_CR4_FIXED1_MSR_INDEX)));

  DEBUG ((DEBUG_ERROR, "Register @ LaunchBack Before AsmVmLaunch: 0x%lx\n", (UINTN)Register));
  Rflags = AsmVmLaunch (Register);

  AcquireSpinLock (&mHostContextCommon.DebugLock);
  DEBUG ((DEBUG_ERROR, "!!!LaunchBack FAIL!!!\n"));
  DEBUG ((DEBUG_ERROR, "Rflags: %08x\n", Rflags));
  DEBUG ((DEBUG_ERROR, "VMCS_32_RO_VM_INSTRUCTION_ERROR: %08x\n", (UINTN)VmRead32 (VMCS_32_RO_VM_INSTRUCTION_ERROR_INDEX)));
  ReleaseSpinLock (&mHostContextCommon.DebugLock);

  CpuDeadLoop ();
}

/**

  This function return if 2 resource overlap.

  @param Address1   Address of 1st resource
  @param Length1    Length of 1st resource
  @param Address2   Address of 2nd resource
  @param Length2    Length of 2nd resource

  @retval TRUE  overlap
  @retval FALSE no overlap

**/
BOOLEAN
IsOverlap (
  IN UINT64  Address1,
  IN UINT64  Length1,
  IN UINT64  Address2,
  IN UINT64  Length2
  )
{
  if ((Address1 + Length1 > Address2) && (Address1 < Address2 + Length2)) {
    // Overlap
    return TRUE;
  } else {
    return FALSE;
  }
}

/**

  This function initialize VMCS.

  @param Index    CPU index

**/
VOID
VmcsInit (
  IN UINT32  Index
  )
{
  UINT64      CurrentVmcs;
  UINTN       VmcsBase;
  UINT32      VmcsSize;
  STM_HEADER  *StmHeader;
  UINTN       Rflags;

  StmHeader = mHostContextCommon.StmHeader;
  VmcsBase  = (UINTN)StmHeader +
              STM_PAGES_TO_SIZE (STM_SIZE_TO_PAGES (StmHeader->SwStmHdr.StaticImageSize)) +
              StmHeader->SwStmHdr.AdditionalDynamicMemorySize +
              StmHeader->SwStmHdr.PerProcDynamicMemorySize * mHostContextCommon.CpuNum;
  VmcsSize = GetVmcsSize ();

  mGuestContextCommonNormal.GuestContextPerCpu[Index].Vmcs = (UINT64)(VmcsBase + VmcsSize * (Index * 2));

  DEBUG ((EFI_D_INFO, "SmiVmcsPtr(%d) - %016lx\n", (UINTN)Index, mGuestContextCommonNormal.GuestContextPerCpu[Index].Vmcs));

  AsmVmPtrStore (&CurrentVmcs);
  DEBUG ((EFI_D_INFO, "CurrentVmcs(%d) - %016lx\n", (UINTN)Index, CurrentVmcs));
  if (IsOverlap (CurrentVmcs, VmcsSize, mHostContextCommon.TsegBase, mHostContextCommon.TsegLength)) {
    // Overlap TSEG
    DEBUG ((DEBUG_ERROR, "CurrentVmcs violation - %016lx\n", CurrentVmcs));
    CpuDeadLoop ();
  }

  Rflags = AsmVmClear (&CurrentVmcs);
  if ((Rflags & (RFLAGS_CF | RFLAGS_ZF)) != 0) {
    DEBUG ((DEBUG_ERROR, "ERROR: AsmVmClear(%d) - %016lx : %08x\n", (UINTN)Index, CurrentVmcs, Rflags));
    CpuDeadLoop ();
  }

  CopyMem (
    (VOID *)(UINTN)mGuestContextCommonNormal.GuestContextPerCpu[Index].Vmcs,
    (VOID *)(UINTN)CurrentVmcs,
    (UINTN)VmcsSize
    );

  AsmWbinvd ();

  Rflags = AsmVmPtrLoad (&mGuestContextCommonNormal.GuestContextPerCpu[Index].Vmcs);
  if ((Rflags & (RFLAGS_CF | RFLAGS_ZF)) != 0) {
    DEBUG ((DEBUG_ERROR, "ERROR: AsmVmPtrLoad(%d) - %016lx : %08x\n", (UINTN)Index, mGuestContextCommonNormal.GuestContextPerCpu[Index].Vmcs, Rflags));
    CpuDeadLoop ();
  }

  InitializeNormalVmcs (Index, &mGuestContextCommonNormal.GuestContextPerCpu[Index].Vmcs);
}

/**
  Function for caller to query the capabilities of SEA core.

  @param[in, out]  Register  The registers of the context of current VMCALL request.

  @retval EFI_SUCCESS               The function completed successfully.
  @retval EFI_INVALID_PARAMETER     The supplied buffer has NULL physical address.
  @retval EFI_SECURITY_VIOLATION    The system status tripped on security violation.
**/
EFI_STATUS
EFIAPI
GetCapabilities (
  IN OUT X86_REGISTER  *Register
  )
{
  STM_STATUS               StmStatus;
  EFI_STATUS               Status;
  UINT64                   BufferBase;
  UINT64                   BufferSize;
  SEA_CAPABILITIES_STRUCT  RetStruct;

  if (Register == NULL) {
    Status = EFI_INVALID_PARAMETER;
    DEBUG ((DEBUG_ERROR, "%a Incoming register being NULL!\n", __func__));
    goto Done;
  }

  // Check the buffer not null requirement
  BufferBase = Register->Rbx;
  BufferSize = EFI_PAGES_TO_SIZE (Register->Rdx);
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - BufferBase = 0x%LX. BufferSize = 0x%LX.\n", __func__, __LINE__, BufferBase, BufferSize));
  if (BufferBase == 0) {
    StmStatus = ERROR_INVALID_PARAMETER;
    WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);
    Status = EFI_SECURITY_VIOLATION;
    DEBUG ((DEBUG_ERROR, "%a Incoming buffer being NULL!\n", __func__));
    goto Done;
  }

  // Check the minimal size requirement
  if (BufferSize < sizeof (SEA_CAPABILITIES_STRUCT)) {
    StmStatus = ERROR_STM_BUFFER_TOO_SMALL;
    WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);
    // Populate rdx with the number of pages required
    WriteUnaligned32 ((UINT32 *)&Register->Rdx, EFI_SIZE_TO_PAGES (sizeof (SEA_CAPABILITIES_STRUCT)));
    Status = EFI_SECURITY_VIOLATION;
    DEBUG ((DEBUG_ERROR, "%a Incoming buffer too small: 0x%x bytes!\n", __func__, BufferSize));
    goto Done;
  }

  // Check the buffer alignment requirement
  if (!IS_ALIGNED (BufferBase, EFI_PAGE_SIZE)) {
    StmStatus = ERROR_SMM_BAD_BUFFER;
    WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);
    Status = EFI_SECURITY_VIOLATION;
    DEBUG ((DEBUG_ERROR, "%a Incoming buffer not page size aligned: 0x%x bytes!\n", __func__, BufferBase));
    goto Done;
  }

  // Check the buffer supplied is not in the MSEG or TSEG.
  if (IsBufferInsideMmram (BufferBase, BufferSize)) {
    StmStatus = ERROR_STM_PAGE_NOT_FOUND;
    WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);
    Status = EFI_SECURITY_VIOLATION;
    DEBUG ((DEBUG_ERROR, "%a Incoming buffer is inside MMRAM: Base: 0x%x, Size: 0x%x !\n", __func__, BufferBase, BufferSize));
    goto Done;
  }

  // Enough complaints, get to work now.
  RetStruct.SeaSpecVerMajor = SEA_SPEC_VERSION_MAJOR;
  RetStruct.SeaSpecVerMinor = SEA_SPEC_VERSION_MINOR;
  RetStruct.Reserved        = 0;
  RetStruct.SeaHeaderSize   = OFFSET_OF (SEA_CAPABILITIES_STRUCT, SeaFeatures);
  RetStruct.SeaTotalSize    = sizeof (SEA_CAPABILITIES_STRUCT);

  RetStruct.SeaFeatures.VerifyMmiEntry = TRUE;
  RetStruct.SeaFeatures.VerifyMmPolicy = TRUE;
  RetStruct.SeaFeatures.VerifyMmSupv   = TRUE;
  RetStruct.SeaFeatures.HashAlg        = HASH_ALG_SHA256;
  RetStruct.SeaFeatures.Reserved       = 0;

  CopyMem ((VOID *)(UINTN)BufferBase, &RetStruct, RetStruct.SeaTotalSize);
  Status    = EFI_SUCCESS;
  StmStatus = STM_SUCCESS;
  WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);

Done:
  return Status;
}

/**
  Function for caller to query the resources of SMM environment.

  @param[in]  Register  The registers of the context of current VMCALL request.

  @retval EFI_SUCCESS               The function completed successfully.
  @retval EFI_INVALID_PARAMETER     The supplied buffer has NULL physical address.
  @retval EFI_SECURITY_VIOLATION    The system status tripped on security violation.
**/
EFI_STATUS
EFIAPI
GetResources (
  IN OUT X86_REGISTER  *Register
  )
{
  STM_STATUS                        StmStatus;
  EFI_STATUS                        Status;
  UINT64                            BufferBase;
  UINT64                            BufferSize;
  SMM_SUPV_SECURE_POLICY_DATA_V1_0  *PolicyBuffer = NULL;
  TPML_DIGEST_VALUES                DigestList[SUPPORTED_DIGEST_COUNT];
  UINTN                             CpuIndex;

  if (Register == NULL) {
    Status = EFI_INVALID_PARAMETER;
    DEBUG ((DEBUG_ERROR, "%a Incoming register being NULL!\n", __func__));
    goto Done;
  }

  // Check the buffer not null requirement
  BufferBase = Register->Rbx;
  BufferSize = EFI_PAGES_TO_SIZE (Register->Rdx);
  DEBUG ((DEBUG_ERROR, "[%a] - BufferBase 0x%lx.\n", __func__, BufferBase));
  DEBUG ((DEBUG_ERROR, "[%a] - BufferSize 0x%lx.\n", __func__, BufferSize));

  if ((BufferBase == 0) && (BufferSize != 0)) {
    StmStatus = ERROR_INVALID_PARAMETER;
    WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);
    Status = EFI_SECURITY_VIOLATION;
    DEBUG ((DEBUG_ERROR, "%a Incoming buffer being NULL!\n", __func__));
    goto Done;
  }

  // Check the buffer alignment requirement
  if (!IS_ALIGNED (BufferBase, EFI_PAGE_SIZE)) {
    StmStatus = ERROR_SMM_BAD_BUFFER;
    WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);
    Status = EFI_SECURITY_VIOLATION;
    DEBUG ((DEBUG_ERROR, "%a Incoming buffer not page size aligned: 0x%x bytes!\n", __func__, BufferBase));
    goto Done;
  }

  // Check the buffer supplied is not in the MSEG or TSEG.
  if ((BufferBase != 0) && IsBufferInsideMmram (BufferBase, BufferSize)) {
    StmStatus = ERROR_STM_PAGE_NOT_FOUND;
    WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);
    Status = EFI_SECURITY_VIOLATION;
    DEBUG ((DEBUG_ERROR, "%a Incoming buffer is inside MMRAM: Base: 0x%x, Size: 0x%x !\n", __func__, BufferBase, BufferSize));
    goto Done;
  }

  //
  // Get information about the image being loaded
  //
  ZeroMem (DigestList, sizeof (DigestList));
  DigestList[MMI_ENTRY_DIGEST_INDEX].digests[0].hashAlg = TPM_ALG_SHA256;
  DigestList[MMI_ENTRY_DIGEST_INDEX].count              = 1;
  CopyMem (DigestList[MMI_ENTRY_DIGEST_INDEX].digests[0].digest.sha256, PcdGetPtr (PcdMmiEntryBinHash), SHA256_DIGEST_SIZE);

  DigestList[MM_SUPV_DIGEST_INDEX].digests[0].hashAlg = TPM_ALG_SHA256;
  DigestList[MM_SUPV_DIGEST_INDEX].count              = 1;
  CopyMem (DigestList[MM_SUPV_DIGEST_INDEX].digests[0].digest.sha256, PcdGetPtr (PcdMmSupervisorCoreHash), SHA256_DIGEST_SIZE);

  CpuIndex = GetIndexFromStack (Register);
  AcquireSpinLock (&mHostContextCommon.ResponderLock);
  Status = SeaResponderReport (
             CpuIndex,
             (EFI_PHYSICAL_ADDRESS)(UINTN)PcdGetPtr (PcdAuxBinFile),
             PcdGetSize (PcdAuxBinFile),
             PcdGet64 (PcdMmiEntryBinSize),
             DigestList,
             SUPPORTED_DIGEST_COUNT,
             (VOID **)&PolicyBuffer
             );
  if (EFI_ERROR (Status)) {
    ReleaseSpinLock (&mHostContextCommon.ResponderLock);
    StmStatus = ERROR_STM_SECURITY_VIOLATION;
    WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);
    Status = EFI_SECURITY_VIOLATION;
    DEBUG ((DEBUG_ERROR, "%a Validation routine failed: %r!\n", __func__, Status));
    goto Done;
  } else if (BufferSize < PolicyBuffer->Size) {
    DEBUG ((DEBUG_ERROR, "[%a] - PolicyBuffer->Size 0x%lx.\n", __func__, PolicyBuffer->Size));
    ReleaseSpinLock (&mHostContextCommon.ResponderLock);
    StmStatus = ERROR_STM_BUFFER_TOO_SMALL;
    WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);
    // Populate rdx with the number of pages required
    WriteUnaligned32 ((UINT32 *)&Register->Rdx, EFI_SIZE_TO_PAGES (PolicyBuffer->Size));
    Status = EFI_SECURITY_VIOLATION;
    DEBUG ((DEBUG_ERROR, "%a Policy returned (0x%x) cannot fit into provided buffer (0x%x)!\n", __func__, PolicyBuffer->Size, BufferSize));
    goto Done;
  } else if (IsZeroBuffer ((VOID *)(UINTN)BufferBase, BufferSize)) {
    // First time being here, populate the content
    CopyMem ((VOID *)(UINTN)BufferBase, PolicyBuffer, PolicyBuffer->Size);
    ReleaseSpinLock (&mHostContextCommon.ResponderLock);
    StmStatus = STM_SUCCESS;
    WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);
    Status = EFI_SUCCESS;
  } else if (
             (CompareMemoryPolicy (PolicyBuffer, (SMM_SUPV_SECURE_POLICY_DATA_V1_0 *)(UINTN)BufferBase) == FALSE) ||
             (ComparePolicyWithType (PolicyBuffer, (SMM_SUPV_SECURE_POLICY_DATA_V1_0 *)(UINTN)BufferBase, SMM_SUPV_SECURE_POLICY_DESCRIPTOR_TYPE_IO) == FALSE) ||
             (ComparePolicyWithType (PolicyBuffer, (SMM_SUPV_SECURE_POLICY_DATA_V1_0 *)(UINTN)BufferBase, SMM_SUPV_SECURE_POLICY_DESCRIPTOR_TYPE_MSR) == FALSE) ||
             (ComparePolicyWithType (PolicyBuffer, (SMM_SUPV_SECURE_POLICY_DATA_V1_0 *)(UINTN)BufferBase, SMM_SUPV_SECURE_POLICY_DESCRIPTOR_TYPE_INSTRUCTION) == FALSE) ||
             (ComparePolicyWithType (PolicyBuffer, (SMM_SUPV_SECURE_POLICY_DATA_V1_0 *)(UINTN)BufferBase, SMM_SUPV_SECURE_POLICY_DESCRIPTOR_TYPE_SAVE_STATE) == FALSE))
  {
    // Not the first time, making sure the validation routine is giving us the same policy buffer output
    ReleaseSpinLock (&mHostContextCommon.ResponderLock);
    StmStatus = ERROR_STM_SECURITY_VIOLATION;
    WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);
    Status = EFI_SECURITY_VIOLATION;
    DEBUG ((DEBUG_ERROR, "%a Memory policy changed from one core the next!!!\n", __func__));
    goto Done;
  } else {
    ReleaseSpinLock (&mHostContextCommon.ResponderLock);
    StmStatus = STM_SUCCESS;
    WriteUnaligned32 ((UINT32 *)&Register->Rax, StmStatus);
    Status = EFI_SUCCESS;
  }

Done:
  if (PolicyBuffer != NULL) {
    FreePages (PolicyBuffer, EFI_SIZE_TO_PAGES (PolicyBuffer->Size));
  }

  return Status;
}

VOID
EFIAPI
ProcessLibraryConstructorList (
  VOID
  );

VOID
DumpMtrrsInStm (
  VOID
  )
{
  MTRR_SETTINGS  LocalMtrrs;
  MTRR_SETTINGS  *Mtrrs;
  UINTN          Index;
  UINTN          VariableMtrrCount;

  DEBUG ((DEBUG_ERROR, "[%a] - Enter\n", __func__));

  MtrrGetAllMtrrs (&LocalMtrrs);
  Mtrrs = &LocalMtrrs;
  DEBUG ((DEBUG_ERROR, "MTRR Default Type: %016lx\n", Mtrrs->MtrrDefType));
  for (Index = 0; Index < MTRR_NUMBER_OF_FIXED_MTRR; Index++) {
    DEBUG ((DEBUG_ERROR, "Fixed MTRR[%02d]   : %016lx\n", Index, Mtrrs->Fixed.Mtrr[Index]));
  }

  VariableMtrrCount = GetVariableMtrrCount ();
  for (Index = 0; Index < VariableMtrrCount; Index++) {
    DEBUG ((
      DEBUG_ERROR,
      "Variable MTRR[%02d]: Base=%016lx Mask=%016lx\n",
      Index,
      Mtrrs->Variables.Mtrr[Index].Base,
      Mtrrs->Variables.Mtrr[Index].Mask
      ));
  }

  DEBUG ((DEBUG_ERROR, "\n"));
  DEBUG ((DEBUG_ERROR, "[%a] - Exit\n", __func__));
}

/**

  This function handles VMCalls into SEA module in C code.

  @param Register X86 register context

**/
VOID
SeaVmcallDispatcher (
  IN X86_REGISTER  *Register
  )
{
  UINT32      CpuIndex;
  UINT32      ServiceId;
  EFI_STATUS  Status;

  DEBUG ((DEBUG_ERROR, "[%a] - Enter\n", __func__));

  if (Register == NULL) {
    ASSERT (Register != NULL);
    return;
  }

  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Register at 0x%p.\n", __func__, __LINE__, Register));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - ServiceId (local stack var) at 0x%p.\n", __func__, __LINE__, &ServiceId));

  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rax = 0x%lx.\n", __func__, __LINE__, Register->Rax));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rcx = 0x%lx.\n", __func__, __LINE__, Register->Rcx));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rdx = 0x%lx.\n", __func__, __LINE__, Register->Rdx));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rbx = 0x%lx.\n", __func__, __LINE__, Register->Rbx));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rsp = 0x%lx.\n", __func__, __LINE__, Register->Rsp));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rbp = 0x%lx.\n", __func__, __LINE__, Register->Rbp));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rsi = 0x%lx.\n", __func__, __LINE__, Register->Rsi));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Rdi = 0x%lx.\n", __func__, __LINE__, Register->Rdi));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R8  = 0x%lx.\n", __func__, __LINE__, Register->R8));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R9  = 0x%lx.\n", __func__, __LINE__, Register->R9));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R10 = 0x%lx.\n", __func__, __LINE__, Register->R10));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R11 = 0x%lx.\n", __func__, __LINE__, Register->R11));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R12 = 0x%lx.\n", __func__, __LINE__, Register->R12));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R13 = 0x%lx.\n", __func__, __LINE__, Register->R13));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R14 = 0x%lx.\n", __func__, __LINE__, Register->R14));
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - R15 = 0x%lx.\n", __func__, __LINE__, Register->R15));

  DEBUG ((DEBUG_ERROR, "[%a][L%d] - VMCS_32_RO_EXIT_REASON_INDEX = 0x%lx.\n", __func__, __LINE__, VmRead32 (VMCS_32_RO_EXIT_REASON_INDEX)));

  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_CR0_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_CR0_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_CR3_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_CR3_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_CR4_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_CR4_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_DR7_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_DR7_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_RSP_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_RSP_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_RIP_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_RIP_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_RFLAGS_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_RFLAGS_INDEX)));

  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_16_GUEST_ES_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_ES_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_16_GUEST_CS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_CS_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_16_GUEST_SS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_SS_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_16_GUEST_DS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_DS_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_16_GUEST_FS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_FS_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_16_GUEST_GS_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_GS_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_16_GUEST_LDTR_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_LDTR_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_16_GUEST_TR_INDEX: %04x\n", (UINTN)VmRead16 (VMCS_16_GUEST_TR_INDEX)));

  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_ES_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_ES_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_CS_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_CS_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_SS_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_SS_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_DS_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_DS_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_FS_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_FS_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_GS_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_GS_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_LDTR_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_LDTR_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_TR_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_TR_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_GDTR_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_GDTR_LIMIT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_IDTR_LIMIT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_IDTR_LIMIT_INDEX)));

  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_ES_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_ES_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_CS_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_CS_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_SS_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_SS_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_DS_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_DS_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_FS_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_FS_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_GS_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_GS_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_LDTR_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_LDTR_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_TR_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_TR_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_GDTR_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_GDTR_BASE_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_N_GUEST_IDTR_BASE_INDEX: %08x\n", (UINTN)VmReadN (VMCS_N_GUEST_IDTR_BASE_INDEX)));

  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_ES_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_ES_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_CS_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_CS_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_SS_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_SS_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_DS_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_DS_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_FS_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_FS_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_GS_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_GS_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_LDTR_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_LDTR_ACCESS_RIGHT_INDEX)));
  DEBUG ((DEBUG_ERROR, "Guest-state VMCS_32_GUEST_TR_ACCESS_RIGHT_INDEX: %08x\n", (UINTN)VmRead32 (VMCS_32_GUEST_TR_ACCESS_RIGHT_INDEX)));

  DEBUG ((DEBUG_ERROR, "MSR IA32_VMX_CR0_FIXED0_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_VMX_CR0_FIXED0_MSR_INDEX)));
  DEBUG ((DEBUG_ERROR, "MSR IA32_VMX_CR0_FIXED1_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_VMX_CR0_FIXED1_MSR_INDEX)));
  DEBUG ((DEBUG_ERROR, "MSR IA32_VMX_CR4_FIXED0_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_VMX_CR4_FIXED0_MSR_INDEX)));
  DEBUG ((DEBUG_ERROR, "MSR IA32_VMX_CR4_FIXED1_MSR_INDEX: %08x\n", (UINTN)AsmReadMsr64 (IA32_VMX_CR4_FIXED1_MSR_INDEX)));

  DumpMtrrsInStm ();

  CpuIndex = GetIndexFromStack (Register);
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - CpuIndex (From Stack) = %d\n", __func__, __LINE__, CpuIndex));
  ServiceId = ReadUnaligned32 ((UINT32 *)&Register->Rax);
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - ServiceId = 0x%x\n", __func__, __LINE__, ServiceId));

  switch (ServiceId) {
    case SEA_API_GET_CAPABILITIES:
      DEBUG ((DEBUG_ERROR, "[%a][L%d] - SEA_API_GET_CAPABILITIES entered.\n", __func__, __LINE__));
      if (CpuIndex == 0) {
        DEBUG ((DEBUG_ERROR, "[%a][L%d] - CpuIndex == 0.\n", __func__, __LINE__));
        // The build process should make sure "virtual address" is same as "file pointer to raw data",
        // in final PE/COFF image, so that we can let StmLoad load binary to memory directly.
        // If no, GenStm tool will "load image". So here, we just need "relocate image"
        RelocateStmImage (FALSE);

        DEBUG ((DEBUG_ERROR, "[%a][L%d] - After RelocateStmImage().\n", __func__, __LINE__));

        DEBUG ((DEBUG_ERROR, "[%a][L%d] - Before ProcessLibraryConstructorList().\n", __func__, __LINE__));
        ProcessLibraryConstructorList ();
        DEBUG ((DEBUG_ERROR, "[%a][L%d] - After ProcessLibraryConstructorList().\n", __func__, __LINE__));

        BspInit (Register);

        DEBUG ((DEBUG_ERROR, "[%a][L%d] - After BspInit() call.\n", __func__, __LINE__));
      }

      if (mHostContextCommon.HostContextPerCpu[CpuIndex].Stack == 0) {
        DEBUG ((DEBUG_INFO, "[%a] - Performing common init for CPU %d for the first time.\n", __func__, CpuIndex));
        DEBUG ((DEBUG_ERROR, "[%a][L%d] - Calling CommonInit()...\n", __func__, __LINE__));
        CommonInit (CpuIndex);
        DEBUG ((DEBUG_ERROR, "[%a][L%d] - Returned from CommonInit().\n", __func__, __LINE__));
      }
      DEBUG ((DEBUG_ERROR, "[%a][L%d] - Calling GetCapabilities()...\n", __func__, __LINE__));
      Status = GetCapabilities (Register);
      DEBUG ((DEBUG_ERROR, "[%a][L%d] - Returned from GetCapabilities(). Status = %r.\n", __func__, __LINE__, Status));
      break;
    case SEA_API_GET_RESOURCES:
      DEBUG ((DEBUG_ERROR, "[%a][L%d] - SEA_API_GET_RESOURCES entered.\n", __func__, __LINE__));
      if (!mIsBspInitialized) {
        DEBUG ((DEBUG_ERROR, "[%a][L%d] - !mIsBspInitialized.\n", __func__, __LINE__));
        Status = EFI_NOT_STARTED;
        break;
      }

      DEBUG ((DEBUG_ERROR, "[%a][L%d] - mIsBspInitialized.\n", __func__, __LINE__));
      // CpuIndex 0 is the BSP structure
      if (ReadLocalApicId () != mHostContextCommon.HostContextPerCpu[0].ApicId) {
        DEBUG ((DEBUG_ERROR, "[%a][L%d] - Performing AP stack init for CPU index %d.\n", __func__, __LINE__, CpuIndex));
        ApInit (CpuIndex, Register);
      }

      if (mHostContextCommon.HostContextPerCpu[CpuIndex].Stack == 0) {
        DEBUG ((DEBUG_INFO, "[%a] - Performing common init for CPU %d for the first time.\n", __func__, CpuIndex));
        DEBUG ((DEBUG_ERROR, "[%a][L%d] - Calling CommonInit()...\n", __func__, __LINE__));
        CommonInit (CpuIndex);
        DEBUG ((DEBUG_ERROR, "[%a][L%d] - Returned from CommonInit().\n", __func__, __LINE__));
      }

      Status = GetResources (Register);
      DEBUG ((DEBUG_ERROR, "[%a][L%d] - Returned from GetResources(). Status = %r.\n", __func__, __LINE__, Status));
      break;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ServiceId(0x%x) error - %r\n", (UINTN)ServiceId, Status));
    // ASSERT_EFI_ERROR (Status);
  }

  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Calling VmcsInit()...\n", __func__, __LINE__));
  VmcsInit (CpuIndex);
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Calling VmcsInit()...\n", __func__, __LINE__));

  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Calling LaunchBack()...\n", __func__, __LINE__));
  LaunchBack (CpuIndex, Register);
  DEBUG ((DEBUG_ERROR, "[%a][L%d] - Returned from LaunchBack().\n", __func__, __LINE__));

  return;
}
