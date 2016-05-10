//===-- RuntimeDyldCOFFX86_64.h --- COFF/X86_64 specific code ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// COFF x86_x64 support for MC-JIT runtime dynamic linker.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_EXECUTIONENGINE_RUNTIMEDYLD_TARGETS_RUNTIMEDYLDCOFF86_64_H
#define LLVM_LIB_EXECUTIONENGINE_RUNTIMEDYLD_TARGETS_RUNTIMEDYLDCOFF86_64_H

#include "llvm/Object/COFF.h"
#include "llvm/Support/COFF.h"
#include "../RuntimeDyldCOFF.h"

#define DEBUG_TYPE "dyld"

namespace llvm {

class RuntimeDyldCOFFX86_64 : public RuntimeDyldCOFF {

private:
  // When a module is loaded we save the SectionID of the unwind
  // sections in a table until we receive a request to register all
  // unregisteredEH frame sections with the memory manager.
  SmallVector<SID, 2> UnregisteredEHFrameSections;
  SmallVector<SID, 2> RegisteredEHFrameSections;

public:
  RuntimeDyldCOFFX86_64(RuntimeDyld::MemoryManager &MM,
                        RuntimeDyld::SymbolResolver &Resolver)
    : RuntimeDyldCOFF(MM, Resolver) {}

  unsigned getMaxStubSize() override {
    return 14; // 2-byte jmp instruction + 32-bit relative address
  }

  // The target location for the relocation is described by RE.SectionID and
  // RE.Offset.  RE.SectionID can be used to find the SectionEntry.  Each
  // SectionEntry has three members describing its location.
  // SectionEntry::Address is the address at which the section has been loaded
  // into memory in the current (host) process.  SectionEntry::LoadAddress is
  // the address that the section will have in the target process.
  // SectionEntry::ObjAddress is the address of the bits for this section in the
  // original emitted object image (also in the current address space).
  //
  // Relocations will be applied as if the section were loaded at
  // SectionEntry::LoadAddress, but they will be applied at an address based
  // on SectionEntry::Address.  SectionEntry::ObjAddress will be used to refer
  // to Target memory contents if they are required for value calculations.
  //
  // The Value parameter here is the load address of the symbol for the
  // relocation to be applied.  For relocations which refer to symbols in the
  // current object Value will be the LoadAddress of the section in which
  // the symbol resides (RE.Addend provides additional information about the
  // symbol location).  For external symbols, Value will be the address of the
  // symbol in the target address space.
  void resolveRelocation(const RelocationEntry &RE, uint64_t Value) override {
    const SectionEntry &Section = Sections[RE.SectionID];
    uint8_t *Target = Section.getAddressWithOffset(RE.Offset);

    switch (RE.RelType) {

    case COFF::IMAGE_REL_AMD64_REL32:
    case COFF::IMAGE_REL_AMD64_REL32_1:
    case COFF::IMAGE_REL_AMD64_REL32_2:
    case COFF::IMAGE_REL_AMD64_REL32_3:
    case COFF::IMAGE_REL_AMD64_REL32_4:
    case COFF::IMAGE_REL_AMD64_REL32_5: {
      uint64_t FinalAddress = Section.getLoadAddressWithOffset(RE.Offset);
      // Delta is the distance from the start of the reloc to the end of the
      // instruction with the reloc.
      uint64_t Delta = 4 + (RE.RelType - COFF::IMAGE_REL_AMD64_REL32);
      Value -= FinalAddress + Delta;
      uint64_t Result = Value + RE.Addend;
      assert(((int64_t)Result <= INT32_MAX) && "Relocation overflow");
      assert(((int64_t)Result >= INT32_MIN) && "Relocation underflow");
      writeBytesUnaligned(Result, Target, 4);
      break;
    }

    case COFF::IMAGE_REL_AMD64_ADDR32NB: {
      // Note ADDR32NB requires a well-established notion of
      // image base. This address must be less than or equal
      // to every section's load address, and all sections must be
      // within a 32 bit offset from the base.
      //
      // For now we just set these to zero.
      writeBytesUnaligned(0, Target, 4);
      break;
    }

    case COFF::IMAGE_REL_AMD64_ADDR64: {
      writeBytesUnaligned(Value + RE.Addend, Target, 8);
      break;
    }

    default:
      llvm_unreachable("Relocation type not implemented yet!");
      break;
    }
  }

  uint64_t generateRelocationStub(unsigned SectionID, StringRef TargetName,
                                  uint64_t Offset, uint64_t RelType, 
                                  uint64_t Addend, StubMap &Stubs) {
    RelocationValueRef OriginalRelValueRef;
    OriginalRelValueRef.SectionID = SectionID;
    OriginalRelValueRef.Offset = Offset;
    OriginalRelValueRef.Addend = Addend;
    OriginalRelValueRef.SymbolName = TargetName.data();

    StubMap::const_iterator i = Stubs.find(OriginalRelValueRef);

    if (i == Stubs.end()) {
      DEBUG(dbgs() << " Create a new stub function for " 
                      << TargetName.data() << "\n");

      SectionEntry &Section = Sections[SectionID];
      uintptr_t StubOffset = Section.getStubOffset();

      createStubFunction(Section.getAddressWithOffset(StubOffset));
      Section.advanceStubOffset(getMaxStubSize());

      Stubs[OriginalRelValueRef] = StubOffset;
      return StubOffset;
    }
    else {
      DEBUG(dbgs() << " Stub function found for " 
                   << TargetName.data() << "\n");

      return i->second;
    }
  }

  Expected<relocation_iterator>
  relocation_iterator processRelocationRef(unsigned SectionID,
                                           relocation_iterator RelI,
                                           const ObjectFile &Obj,
                                           ObjSectionToIDMap &ObjSectionToID,
                                           StubMap &Stubs) override {
    // If possible, find the symbol referred to in the relocation,
    // and the section that contains it.
    symbol_iterator Symbol = RelI->getSymbol();
    if (Symbol == Obj.symbol_end())
      report_fatal_error("Unknown symbol in relocation");
    section_iterator SecI = *Symbol->getSection();
    // If there is no section, this must be an external reference.
    const bool IsExtern = SecI == Obj.section_end();

    // Determine the Addend used to adjust the relocation value.
    uint64_t RelType = RelI->getType();
    uint64_t Offset = RelI->getOffset();
    uint64_t Addend = 0;
    SectionEntry &Section = Sections[SectionID];
    uintptr_t ObjTarget = Section.getObjAddress() + Offset;

    ErrorOr<StringRef> TargetNameOrErr = Symbol->getName();
    if (std::error_code EC = TargetNameOrErr.getError())
      report_fatal_error(EC.message());
    StringRef TargetName = *TargetNameOrErr;

    switch (RelType) {

    case COFF::IMAGE_REL_AMD64_REL32:
    case COFF::IMAGE_REL_AMD64_REL32_1:
    case COFF::IMAGE_REL_AMD64_REL32_2:
    case COFF::IMAGE_REL_AMD64_REL32_3:
    case COFF::IMAGE_REL_AMD64_REL32_4:
    case COFF::IMAGE_REL_AMD64_REL32_5: {
      uint8_t *Displacement = (uint8_t *)ObjTarget;
      Addend = readBytesUnaligned(Displacement, 4);

      if (IsExtern)
      {
        uint64_t StubOffset = generateRelocationStub(
          SectionID, TargetName, Offset, RelType, Addend, Stubs);

        // Make the target call a call into the stub table.
        RelocationEntry RE(SectionID, Offset, RelType, Addend);
        uint8_t *StubAbsAddress = Section.getAddressWithOffset(StubOffset);
        resolveRelocation(RE, reinterpret_cast<uint64_t>(StubAbsAddress));

        // Let relocation resolution write the symbol pointer 
        // to the stub function as 64bit absolute address.
        constexpr unsigned PointerOffsetInStub = 6;
        Offset = StubOffset + PointerOffsetInStub;
        RelType = COFF::IMAGE_REL_AMD64_ADDR64;
        Addend = 0;
      }

      break;
    }

    case COFF::IMAGE_REL_AMD64_ADDR32NB: {
      uint8_t *Displacement = (uint8_t *)ObjTarget;
      Addend = readBytesUnaligned(Displacement, 4);
      break;
    }

    case COFF::IMAGE_REL_AMD64_ADDR64: {
      uint8_t *Displacement = (uint8_t *)ObjTarget;
      Addend = readBytesUnaligned(Displacement, 8);
      break;
    }

    default:
      break;
    }

    Expected<StringRef> TargetNameOrErr = Symbol->getName();
    if (!TargetNameOrErr)
      return TargetNameOrErr.takeError();
    StringRef TargetName = *TargetNameOrErr;

    DEBUG(dbgs() << "\t\tIn Section " << SectionID << " Offset " << Offset
                 << " RelType: " << RelType << " TargetName: " << TargetName
                 << " Addend " << Addend << "\n");

    if (IsExtern) {
      RelocationEntry RE(SectionID, Offset, RelType, Addend);
      addRelocationForSymbol(RE, TargetName);
    } else {
      bool IsCode = SecI->isText();
      unsigned TargetSectionID;
      if (auto TargetSectionIDOrErr =
          findOrEmitSection(Obj, *SecI, IsCode, ObjSectionToID))
        TargetSectionID = *TargetSectionIDOrErr;
      else
        return TargetSectionIDOrErr.takeError();
      uint64_t TargetOffset = getSymbolOffset(*Symbol);
      RelocationEntry RE(SectionID, Offset, RelType, TargetOffset + Addend);
      addRelocationForSection(RE, TargetSectionID);
    }

    return ++RelI;
  }

  unsigned getStubAlignment() override { return 1; }
  void registerEHFrames() override {
    for (auto const &EHFrameSID : UnregisteredEHFrameSections) {
      uint8_t *EHFrameAddr = Sections[EHFrameSID].getAddress();
      uint64_t EHFrameLoadAddr = Sections[EHFrameSID].getLoadAddress();
      size_t EHFrameSize = Sections[EHFrameSID].getSize();
      MemMgr.registerEHFrames(EHFrameAddr, EHFrameLoadAddr, EHFrameSize);
      RegisteredEHFrameSections.push_back(EHFrameSID);
    }
    UnregisteredEHFrameSections.clear();
  }
  void deregisterEHFrames() override {
    // Stub
  }
  Error finalizeLoad(const ObjectFile &Obj,
                     ObjSectionToIDMap &SectionMap) override {
    // Look for and record the EH frame section IDs.
    for (const auto &SectionPair : SectionMap) {
      const SectionRef &Section = SectionPair.first;
      StringRef Name;
      if (auto EC = Section.getName(Name))
        return errorCodeToError(EC);
      // Note unwind info is split across .pdata and .xdata, so this
      // may not be sufficiently general for all users.
      if (Name == ".xdata") {
        UnregisteredEHFrameSections.push_back(SectionPair.second);
      }
    }
    return Error::success();
  }
};

} // end namespace llvm

#undef DEBUG_TYPE

#endif
